#include "rewair_mqtt.h"

#include <stdio.h>
#include <string.h>

#include "wiced.h"
#include "wiced_framework.h"
#include "wiced_tcpip.h"
#include "wiced_wifi.h"
#include "rewair_json.h"
#include "rewair_mqtt_packet.h"
#include "rewair_state.h"
#include "rewair_version.h"

#define REWAIR_MQTT_CONFIG_MAGIC       0x52574d31u /* 'RWM1' */
#define REWAIR_MQTT_DCT_OFFSET         512u
#define REWAIR_MQTT_THREAD_STACK_SIZE  6144u
#define REWAIR_MQTT_PACKET_MAX         1200u
#define REWAIR_MQTT_PAYLOAD_MAX        900u
#define REWAIR_MQTT_TOPIC_MAX          192u
#define REWAIR_MQTT_CONNECT_TIMEOUT_MS 5000u
#define REWAIR_MQTT_RX_TIMEOUT_MS      5000u
#define REWAIR_MQTT_RETRY_MS           15000u
#define REWAIR_MQTT_HEARTBEAT_MS       30000u
#define REWAIR_MQTT_KEEP_ALIVE_S       45u

typedef struct
{
    const char* object_id;
    const char* name;
    const char* state_key;
    const char* device_class;
    const char* unit;
} mqtt_sensor_definition_t;

static const mqtt_sensor_definition_t mqtt_sensors[] =
{
    { "temperature", "Temperature",       "temperature", "temperature",                       "\\u00b0C" },
    { "humidity",    "Humidity",          "humidity",    "humidity",                          "%" },
    { "co2",         "Carbon dioxide",    "co2",         "carbon_dioxide",                    "ppm" },
    { "voc",         "VOC",               "voc",         "volatile_organic_compounds_parts",  "ppb" },
    { "dust",        "Dust",              "dust",        "",                                  "\\u00b5g/m\\u00b3" },
    { "illuminance", "Illuminance",       "illuminance", "illuminance",                       "lx" },
    { "score",       "Air quality score", "score",       "",                                  "" },
};

static wiced_thread_t mqtt_thread;
static wiced_mutex_t  mqtt_status_mutex;
static wiced_tcp_socket_t mqtt_socket;
static uint8_t socket_created = 0u;
static uint8_t mqtt_connected = 0u;
static volatile uint32_t mqtt_config_generation = 1u;
static volatile uint32_t mqtt_state_dirty = 1u;
static rewair_mqtt_config_t active_config;
static rewair_mqtt_status_t mqtt_status;
static uint8_t mqtt_packet[REWAIR_MQTT_PACKET_MAX];
static char mqtt_payload[REWAIR_MQTT_PAYLOAD_MAX];
static char mqtt_base_topic[REWAIR_MQTT_TOPIC_MAX];
static char mqtt_state_topic[REWAIR_MQTT_TOPIC_MAX];
static char mqtt_availability_topic[REWAIR_MQTT_TOPIC_MAX];
static char mqtt_device_id[32];
static char mqtt_client_id[24];
static char mqtt_last_discovery_name[32];

static void copy_str( char* out, uint32_t out_size, const char* value )
{
    uint32_t i = 0u;

    if ( value != NULL )
    {
        while ( value[i] != '\0' && i + 1u < out_size )
        {
            out[i] = value[i];
            i++;
        }
    }
    out[i] = '\0';
}

static void mqtt_set_error( const char* message )
{
    wiced_rtos_lock_mutex( &mqtt_status_mutex );
    copy_str( mqtt_status.last_error, sizeof( mqtt_status.last_error ), message );
    wiced_rtos_unlock_mutex( &mqtt_status_mutex );
}

static void mqtt_set_connected( uint8_t connected )
{
    wiced_rtos_lock_mutex( &mqtt_status_mutex );
    mqtt_status.connected = connected;
    if ( connected != 0u )
    {
        mqtt_status.reconnects++;
        mqtt_status.last_error[0] = '\0';
    }
    wiced_rtos_unlock_mutex( &mqtt_status_mutex );
}

static void mqtt_note_publish( void )
{
    wiced_rtos_lock_mutex( &mqtt_status_mutex );
    mqtt_status.published++;
    wiced_rtos_unlock_mutex( &mqtt_status_mutex );
}

void rewair_mqtt_config_defaults( rewair_mqtt_config_t* config )
{
    memset( config, 0, sizeof( *config ) );
    config->magic = REWAIR_MQTT_CONFIG_MAGIC;
    config->port = 1883u;
    config->discovery = 1u;
    copy_str( config->discovery_prefix, sizeof( config->discovery_prefix ), "homeassistant" );
}

void rewair_mqtt_config_load( rewair_mqtt_config_t* config )
{
    rewair_mqtt_config_t* stored = NULL;

    if ( wiced_dct_read_lock( (void**)&stored, WICED_FALSE, DCT_APP_SECTION,
                              REWAIR_MQTT_DCT_OFFSET, sizeof( *stored ) ) != WICED_SUCCESS )
    {
        rewair_mqtt_config_defaults( config );
        return;
    }
    if ( stored->magic != REWAIR_MQTT_CONFIG_MAGIC )
    {
        rewair_mqtt_config_defaults( config );
    }
    else
    {
        *config = *stored;
        config->host[sizeof( config->host ) - 1u] = '\0';
        config->username[sizeof( config->username ) - 1u] = '\0';
        config->password[sizeof( config->password ) - 1u] = '\0';
        config->topic_prefix[sizeof( config->topic_prefix ) - 1u] = '\0';
        config->discovery_prefix[sizeof( config->discovery_prefix ) - 1u] = '\0';
        if ( config->port == 0u )
        {
            config->port = 1883u;
        }
        config->enabled = config->enabled != 0u ? 1u : 0u;
        config->discovery = config->discovery != 0u ? 1u : 0u;
    }
    wiced_dct_read_unlock( stored, WICED_FALSE );
}

int rewair_mqtt_config_save( const rewair_mqtt_config_t* config )
{
    rewair_mqtt_config_t saved = *config;
    wiced_result_t result;

    saved.magic = REWAIR_MQTT_CONFIG_MAGIC;
    saved.enabled = saved.enabled != 0u ? 1u : 0u;
    saved.discovery = saved.discovery != 0u ? 1u : 0u;
    result = wiced_dct_write( &saved, DCT_APP_SECTION, REWAIR_MQTT_DCT_OFFSET,
                              sizeof( saved ) );
    printf( "[mqtt] config save %s\n", result == WICED_SUCCESS ? "ok" : "FAILED" );
    if ( result == WICED_SUCCESS )
    {
        rewair_mqtt_reconfigure( );
        return 0;
    }
    return -1;
}

int rewair_mqtt_config_reset( void )
{
    rewair_mqtt_config_t config;

    rewair_mqtt_config_defaults( &config );
    return rewair_mqtt_config_save( &config );
}

int rewair_mqtt_topic_prefix_valid( const char* prefix, uint8_t allow_empty )
{
    uint32_t i;

    if ( prefix == NULL || prefix[0] == '\0' )
    {
        return allow_empty != 0u ? 1 : 0;
    }
    for ( i = 0u; prefix[i] != '\0'; i++ )
    {
        char c = prefix[i];
        if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
                ( c >= '0' && c <= '9' ) || c == '_' || c == '-' ||
                c == '.' || c == '/' ) )
        {
            return 0;
        }
    }
    return prefix[i - 1u] != '/' ? 1 : 0;
}

void rewair_mqtt_reconfigure( void )
{
    mqtt_config_generation++;
}

void rewair_mqtt_get_status( rewair_mqtt_status_t* status )
{
    wiced_rtos_lock_mutex( &mqtt_status_mutex );
    *status = mqtt_status;
    wiced_rtos_unlock_mutex( &mqtt_status_mutex );
}

static void mqtt_state_changed( void )
{
    mqtt_state_dirty = 1u;
}

static int mqtt_send_bytes( const uint8_t* data, uint32_t length )
{
    wiced_packet_t* packet = NULL;
    uint8_t* tx_data = NULL;
    uint16_t available = 0u;
    wiced_result_t result;

    if ( socket_created == 0u || length == 0u || length > 65535u )
    {
        return -1;
    }
    if ( wiced_packet_create_tcp( &mqtt_socket, (uint16_t)length, &packet,
                                  &tx_data, &available ) != WICED_SUCCESS || packet == NULL )
    {
        return -1;
    }
    if ( available < length )
    {
        wiced_packet_delete( packet );
        return -1;
    }
    memcpy( tx_data, data, length );
    wiced_packet_set_data_end( packet, tx_data + length );
    result = wiced_tcp_send_packet( &mqtt_socket, packet );
    if ( result != WICED_SUCCESS )
    {
        /* The LwIP WICED backend only consumes the packet after a successful
         * NETCONN_COPY.  The caller still owns it on failure. */
        wiced_packet_delete( packet );
        return -1;
    }
    /* wiced_tcp_send_packet() consumes the packet on success. */
    return 0;
}

static int mqtt_publish( const char* topic, const char* payload, uint8_t retained )
{
    int length = rewair_mqtt_packet_publish( topic, payload, retained,
                                             mqtt_packet, sizeof( mqtt_packet ) );
    if ( length < 0 || mqtt_send_bytes( mqtt_packet, (uint32_t)length ) != 0 )
    {
        return -1;
    }
    mqtt_note_publish( );
    return 0;
}

static void mqtt_build_identity_and_topics( void )
{
    wiced_mac_t mac;

    memset( &mac, 0, sizeof( mac ) );
    (void)wiced_wifi_get_mac_address( &mac );
    snprintf( mqtt_device_id, sizeof( mqtt_device_id ),
              "rewair_%02x%02x%02x%02x%02x%02x",
              mac.octet[0], mac.octet[1], mac.octet[2],
              mac.octet[3], mac.octet[4], mac.octet[5] );
    snprintf( mqtt_client_id, sizeof( mqtt_client_id ),
              "rewair-%02x%02x%02x%02x%02x%02x",
              mac.octet[0], mac.octet[1], mac.octet[2],
              mac.octet[3], mac.octet[4], mac.octet[5] );

    if ( active_config.topic_prefix[0] != '\0' )
    {
        copy_str( mqtt_base_topic, sizeof( mqtt_base_topic ), active_config.topic_prefix );
    }
    else
    {
        snprintf( mqtt_base_topic, sizeof( mqtt_base_topic ), "rewair/%s", mqtt_device_id );
    }
    snprintf( mqtt_state_topic, sizeof( mqtt_state_topic ), "%s/state", mqtt_base_topic );
    snprintf( mqtt_availability_topic, sizeof( mqtt_availability_topic ),
              "%s/availability", mqtt_base_topic );
}

static int mqtt_receive_connack( void )
{
    uint8_t response[4];
    uint32_t received = 0u;

    while ( received < sizeof( response ) )
    {
        wiced_packet_t* packet = NULL;
        uint8_t* data = NULL;
        uint16_t data_length = 0u;
        uint16_t available = 0u;
        uint32_t copy_length;

        if ( wiced_tcp_receive( &mqtt_socket, &packet, REWAIR_MQTT_RX_TIMEOUT_MS ) != WICED_SUCCESS ||
             packet == NULL )
        {
            mqtt_set_error( "MQTT CONNACK timeout" );
            return -1;
        }
        if ( wiced_packet_get_data( packet, 0, &data, &data_length, &available ) != WICED_SUCCESS )
        {
            wiced_packet_delete( packet );
            mqtt_set_error( "invalid MQTT CONNACK" );
            return -1;
        }
        copy_length = data_length;
        if ( copy_length > sizeof( response ) - received )
        {
            copy_length = sizeof( response ) - received;
        }
        memcpy( response + received, data, copy_length );
        received += copy_length;
        wiced_packet_delete( packet );
    }

    if ( response[0] != 0x20u || response[1] != 0x02u || response[3] != 0x00u )
    {
        switch ( response[3] )
        {
            case 4u: mqtt_set_error( "bad username or password" ); break;
            case 5u: mqtt_set_error( "broker authorization denied" ); break;
            default: mqtt_set_error( "broker rejected MQTT connection" ); break;
        }
        return -1;
    }
    return 0;
}

static void mqtt_close_socket( void )
{
    if ( socket_created != 0u )
    {
        /* Either disconnect or delete releases the LwIP netconn.  Calling
         * both double-deletes it in this WICED backend. */
        (void)wiced_tcp_delete_socket( &mqtt_socket );
        socket_created = 0u;
    }
    mqtt_connected = 0u;
    mqtt_set_connected( 0u );
}

static int mqtt_discovery_publish( uint8_t clear )
{
    rewair_status_t state;
    char escaped_name[64];
    char escaped_state_topic[REWAIR_MQTT_TOPIC_MAX * 2u];
    char escaped_availability_topic[REWAIR_MQTT_TOPIC_MAX * 2u];
    uint32_t i;

    rewair_state_snapshot( &state );
    (void)rewair_json_escape_string( state.name, escaped_name, sizeof( escaped_name ) );
    (void)rewair_json_escape_string( mqtt_state_topic, escaped_state_topic,
                                     sizeof( escaped_state_topic ) );
    (void)rewair_json_escape_string( mqtt_availability_topic, escaped_availability_topic,
                                     sizeof( escaped_availability_topic ) );

    for ( i = 0u; i < sizeof( mqtt_sensors ) / sizeof( mqtt_sensors[0] ); i++ )
    {
        const mqtt_sensor_definition_t* sensor = &mqtt_sensors[i];
        char topic[REWAIR_MQTT_TOPIC_MAX];
        char unique_id[64];
        char device_class_json[80] = "";
        char unit_json[64] = "";
        int length;

        snprintf( unique_id, sizeof( unique_id ), "%s_%s", mqtt_device_id, sensor->object_id );
        snprintf( topic, sizeof( topic ), "%s/sensor/%s/config",
                  active_config.discovery_prefix, unique_id );
        if ( clear != 0u )
        {
            if ( mqtt_publish( topic, "", 1u ) != 0 )
            {
                return -1;
            }
            continue;
        }
        if ( sensor->device_class[0] != '\0' )
        {
            snprintf( device_class_json, sizeof( device_class_json ),
                      ",\"device_class\":\"%s\"", sensor->device_class );
        }
        if ( sensor->unit[0] != '\0' )
        {
            snprintf( unit_json, sizeof( unit_json ),
                      ",\"unit_of_measurement\":\"%s\"", sensor->unit );
        }
        length = snprintf(
            mqtt_payload, sizeof( mqtt_payload ),
            "{\"name\":\"%s\",\"unique_id\":\"%s\","
            "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.%s }}\","
            "\"availability_topic\":\"%s\",\"state_class\":\"measurement\"%s%s,"
            "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
            "\"manufacturer\":\"Rewair\",\"model\":\"Awair Element\","
            "\"sw_version\":\"%s\"},"
            "\"origin\":{\"name\":\"Rewair\",\"sw_version\":\"%s\"}}",
            sensor->name, unique_id, escaped_state_topic, sensor->state_key,
            escaped_availability_topic, device_class_json, unit_json,
            mqtt_device_id, escaped_name, REWAIR_FW_VERSION, REWAIR_FW_VERSION );
        if ( length < 0 || (uint32_t)length >= sizeof( mqtt_payload ) ||
             mqtt_publish( topic, mqtt_payload, 1u ) != 0 )
        {
            return -1;
        }
    }

    if ( clear == 0u )
    {
        copy_str( mqtt_last_discovery_name, sizeof( mqtt_last_discovery_name ), state.name );
    }
    return 0;
}

static void mqtt_centi_string( int32_t value, char out[24] )
{
    int32_t whole = value / 100;
    int32_t fraction = value % 100;

    if ( fraction < 0 )
    {
        fraction = -fraction;
    }
    if ( value < 0 && whole == 0 )
    {
        snprintf( out, 24, "-0.%02ld", (long)fraction );
    }
    else
    {
        snprintf( out, 24, "%ld.%02ld", (long)whole, (long)fraction );
    }
}

static int mqtt_state_publish( void )
{
    rewair_status_t state;
    char temp[24], humidity[24], co2[24], voc[24], dust[24];
    int length;

    rewair_state_snapshot( &state );
    if ( state.sens_valid == 0u )
    {
        return 0;
    }
    mqtt_centi_string( state.sens.temp, temp );
    mqtt_centi_string( state.sens.humid, humidity );
    mqtt_centi_string( state.sens.co2, co2 );
    mqtt_centi_string( state.sens.voc, voc );
    mqtt_centi_string( state.sens.dust, dust );
    length = snprintf( mqtt_payload, sizeof( mqtt_payload ),
                       "{\"temperature\":%s,\"humidity\":%s,\"co2\":%s,"
                       "\"voc\":%s,\"dust\":%s,\"illuminance\":%ld,\"score\":%lu}",
                       temp, humidity, co2, voc, dust, (long)state.sens.light,
                       (unsigned long)state.score );
    if ( length < 0 || (uint32_t)length >= sizeof( mqtt_payload ) )
    {
        return -1;
    }
    return mqtt_publish( mqtt_state_topic, mqtt_payload, 1u );
}

static int mqtt_connect_broker( void )
{
    wiced_ip_address_t broker_ip;
    rewair_mqtt_connect_options_t options;
    wiced_result_t result;
    int length;

    mqtt_set_error( "" );
    mqtt_build_identity_and_topics( );
    if ( wiced_hostname_lookup( active_config.host, &broker_ip,
                                REWAIR_MQTT_CONNECT_TIMEOUT_MS ) != WICED_SUCCESS )
    {
        mqtt_set_error( "broker DNS lookup failed" );
        return -1;
    }
    if ( wiced_tcp_create_socket( &mqtt_socket, WICED_STA_INTERFACE ) != WICED_SUCCESS )
    {
        mqtt_set_error( "could not create TCP socket" );
        return -1;
    }
    socket_created = 1u;
    if ( wiced_tcp_bind( &mqtt_socket, WICED_ANY_PORT ) != WICED_SUCCESS )
    {
        mqtt_set_error( "broker TCP connection failed" );
        mqtt_close_socket( );
        return -1;
    }
    result = wiced_tcp_connect( &mqtt_socket, &broker_ip, active_config.port,
                                REWAIR_MQTT_CONNECT_TIMEOUT_MS );
    if ( result != WICED_SUCCESS )
    {
        mqtt_set_error( "broker TCP connection failed" );
        if ( result == WICED_NOTUP )
        {
            /* The link check returns before touching the allocated netconn. */
            mqtt_close_socket( );
        }
        else
        {
            /* The LwIP backend deletes the netconn after a failed connect but
             * leaves a dangling pointer in the public socket structure. */
            mqtt_socket.conn_handler = NULL;
            mqtt_socket.is_bound = WICED_FALSE;
            socket_created = 0u;
            mqtt_connected = 0u;
            mqtt_set_connected( 0u );
        }
        return -1;
    }

    memset( &options, 0, sizeof( options ) );
    options.client_id = mqtt_client_id;
    options.username = active_config.username;
    options.password = active_config.password;
    options.will_topic = mqtt_availability_topic;
    options.will_payload = "offline";
    options.keep_alive_s = REWAIR_MQTT_KEEP_ALIVE_S;
    length = rewair_mqtt_packet_connect( &options, mqtt_packet, sizeof( mqtt_packet ) );
    if ( length < 0 || mqtt_send_bytes( mqtt_packet, (uint32_t)length ) != 0 )
    {
        mqtt_set_error( "MQTT CONNECT send failed" );
        mqtt_close_socket( );
        return -1;
    }
    if ( mqtt_receive_connack( ) != 0 )
    {
        mqtt_close_socket( );
        return -1;
    }

    mqtt_connected = 1u;
    mqtt_set_connected( 1u );
    if ( mqtt_publish( mqtt_availability_topic, "online", 1u ) != 0 ||
         ( active_config.discovery != 0u && mqtt_discovery_publish( 0u ) != 0 ) ||
         mqtt_state_publish( ) != 0 )
    {
        mqtt_set_error( "initial MQTT publish failed" );
        mqtt_close_socket( );
        return -1;
    }
    mqtt_state_dirty = 0u;
    printf( "[mqtt] connected broker=%s:%u topic=%s\n",
            active_config.host, active_config.port, mqtt_base_topic );
    return 0;
}

static void mqtt_graceful_disconnect( uint8_t clear_discovery )
{
    int length;

    if ( mqtt_connected != 0u )
    {
        if ( clear_discovery != 0u && active_config.discovery != 0u )
        {
            (void)mqtt_discovery_publish( 1u );
        }
        (void)mqtt_publish( mqtt_availability_topic, "offline", 1u );
        length = rewair_mqtt_packet_disconnect( mqtt_packet, sizeof( mqtt_packet ) );
        if ( length > 0 )
        {
            (void)mqtt_send_bytes( mqtt_packet, (uint32_t)length );
        }
    }
    mqtt_close_socket( );
}

static int time_reached( uint32_t now, uint32_t deadline )
{
    return (int32_t)( now - deadline ) >= 0 ? 1 : 0;
}

static void mqtt_thread_main( uint32_t arg )
{
    uint32_t seen_generation = 0u;
    uint32_t next_connect_ms = 0u;
    uint32_t last_publish_ms = 0u;

    (void)arg;
    memset( &active_config, 0, sizeof( active_config ) );

    while ( 1 )
    {
        wiced_time_t now_time = 0u;
        uint32_t now_ms;
        uint32_t generation = mqtt_config_generation;

        (void)wiced_time_get_time( &now_time );
        now_ms = (uint32_t)now_time;

        if ( generation != seen_generation )
        {
            rewair_mqtt_config_t new_config;
            uint8_t clear_discovery = 0u;

            rewair_mqtt_config_load( &new_config );
            if ( mqtt_connected != 0u && active_config.discovery != 0u &&
                 ( new_config.enabled == 0u || new_config.discovery == 0u ||
                   strcmp( active_config.discovery_prefix,
                           new_config.discovery_prefix ) != 0 ) )
            {
                clear_discovery = 1u;
            }
            mqtt_graceful_disconnect( clear_discovery );
            active_config = new_config;
            seen_generation = generation;
            next_connect_ms = now_ms;
            mqtt_state_dirty = 1u;
            mqtt_last_discovery_name[0] = '\0';
            if ( active_config.enabled == 0u )
            {
                mqtt_set_error( "" );
            }
        }

        if ( active_config.enabled == 0u )
        {
            wiced_rtos_delay_milliseconds( 500u );
            continue;
        }
        if ( wiced_network_is_up( WICED_STA_INTERFACE ) != WICED_TRUE )
        {
            if ( mqtt_connected != 0u || socket_created != 0u )
            {
                mqtt_close_socket( );
            }
            mqtt_set_error( "waiting for Wi-Fi" );
            wiced_rtos_delay_milliseconds( 500u );
            continue;
        }

        if ( mqtt_connected == 0u )
        {
            if ( time_reached( now_ms, next_connect_ms ) != 0 )
            {
                if ( mqtt_connect_broker( ) == 0 )
                {
                    last_publish_ms = now_ms;
                }
                else
                {
                    next_connect_ms = now_ms + REWAIR_MQTT_RETRY_MS;
                }
            }
            wiced_rtos_delay_milliseconds( 500u );
            continue;
        }

        if ( mqtt_state_dirty != 0u )
        {
            rewair_status_t state;
            rewair_state_snapshot( &state );
            if ( active_config.discovery != 0u &&
                 strcmp( mqtt_last_discovery_name, state.name ) != 0 &&
                 mqtt_discovery_publish( 0u ) != 0 )
            {
                mqtt_set_error( "discovery publish failed" );
                mqtt_close_socket( );
                next_connect_ms = now_ms + REWAIR_MQTT_RETRY_MS;
                continue;
            }
            if ( mqtt_state_publish( ) != 0 )
            {
                mqtt_set_error( "state publish failed" );
                mqtt_close_socket( );
                next_connect_ms = now_ms + REWAIR_MQTT_RETRY_MS;
                continue;
            }
            mqtt_state_dirty = 0u;
            last_publish_ms = now_ms;
        }
        else if ( (uint32_t)( now_ms - last_publish_ms ) >= REWAIR_MQTT_HEARTBEAT_MS )
        {
            /* A repeated QoS-0 publish satisfies the MQTT keepalive without
             * accumulating unread PINGRESP packets in this publish-only client. */
            if ( mqtt_publish( mqtt_availability_topic, "online", 1u ) != 0 ||
                 mqtt_state_publish( ) != 0 )
            {
                mqtt_set_error( "MQTT heartbeat failed" );
                mqtt_close_socket( );
                next_connect_ms = now_ms + REWAIR_MQTT_RETRY_MS;
                continue;
            }
            last_publish_ms = now_ms;
        }
        wiced_rtos_delay_milliseconds( 500u );
    }
}

void rewair_mqtt_start( void )
{
    rewair_mqtt_config_t config;

    memset( &mqtt_status, 0, sizeof( mqtt_status ) );
    if ( wiced_rtos_init_mutex( &mqtt_status_mutex ) != WICED_SUCCESS )
    {
        printf( "[mqtt] status mutex init failed\n" );
        return;
    }
    rewair_mqtt_config_load( &config );
    if ( rewair_state_subscribe( mqtt_state_changed ) != 0 )
    {
        printf( "[mqtt] state subscription failed\n" );
    }
    if ( wiced_rtos_create_thread( &mqtt_thread, WICED_DEFAULT_LIBRARY_PRIORITY, "mqtt",
                                   mqtt_thread_main, REWAIR_MQTT_THREAD_STACK_SIZE,
                                   NULL ) != WICED_SUCCESS )
    {
        printf( "[mqtt] thread start failed\n" );
    }
}
