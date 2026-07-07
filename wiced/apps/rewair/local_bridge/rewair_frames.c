/* F103 sensor-frame TX (frame building + senders), lifted verbatim out of
 * local_bridge.c (Phase 2 Task 8, pure move). See rewair_frames.h. */

#include "rewair_frames.h"

#include <stdio.h>
#include "wiced_tcpip.h"
#include "wiced_wifi.h"
#include "internal/wiced_internal_api.h"
#include "rewair_fmt.h"
#include "rewair_walltime.h"
#include "rewair_state.h"
#include "rewair_frame_rx.h" /* SENSOR_PAYLOAD_MAX, shared frame-size limit */

wiced_mutex_t sensor_uart_tx_mutex;
volatile uint32_t sensor_uart_tx_count = 0u;
volatile uint32_t sensor_uart_tx_drop_count = 0u;
volatile uint32_t sensor_uart_wiced_tx_fail_count = 0u;
volatile uint32_t sensor_uart_wiced_tx_result = 0u;
volatile uint32_t sensor_uart_tx_sr_before = 0u;
volatile uint32_t sensor_uart_tx_sr_after = 0u;

volatile uint32_t sensor_boot_context_sent = 0u;
volatile uint32_t sensor_netw_boot_pulses = 0u;

rewair_tz_rule_t current_tz_rule;
uint32_t current_tz_rule_valid = 0u;

void sensor_set_tz_rule( const rewair_tz_rule_t* rule )
{
    current_tz_rule = *rule;
    current_tz_rule_valid = 1u;
}

uint32_t fields_payload_len( char** fields, uint32_t count )
{
    uint32_t length = 0u;
    uint32_t i;
    for ( i = 0; i < count; i++ )
    {
        length += cstr_len( fields[i] ) + 1u;
    }
    return length;
}

void frame_append( uint8_t* frame, uint32_t* frame_len, const void* data, uint32_t length )
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t i;
    for ( i = 0u; i < length; i++ )
    {
        frame[( *frame_len )++] = bytes[i];
    }
}

void sensor_uart_send_frame_bytes( const uint8_t* frame, uint32_t frame_len )
{
    wiced_result_t result;

    if ( wiced_rtos_lock_mutex( &sensor_uart_tx_mutex ) != WICED_SUCCESS )
    {
        sensor_uart_tx_drop_count++;
        sensor_uart_wiced_tx_result = WICED_ERROR;
        return;
    }

    sensor_uart_tx_sr_before = USART2->SR;
    result = wiced_uart_transmit_bytes( WICED_UART_2, frame, frame_len );
    sensor_uart_tx_sr_after = USART2->SR;
    sensor_uart_wiced_tx_result = (uint32_t)result;
    if ( result != WICED_SUCCESS )
    {
        sensor_uart_wiced_tx_fail_count++;
    }
    else
    {
        sensor_uart_tx_count++;
    }

    wiced_rtos_unlock_mutex( &sensor_uart_tx_mutex );
}

#define SENSOR_FRAME_MAX ( 1u + 4u + 8u + 1u + SENSOR_PAYLOAD_MAX + 1u )

void sensor_send_frame( const char cmd[4], char** fields, uint32_t field_count )
{
    uint32_t payload_len = fields_payload_len( fields, field_count );
    uint8_t frame[SENSOR_FRAME_MAX];
    uint32_t frame_len = 0u;
    char lenbuf[9];
    uint32_t i;

    if ( payload_len > SENSOR_PAYLOAD_MAX )
    {
        printf( "[tx drop] %.4s payload too large len=%lu\n", cmd, payload_len );
        return;
    }

    frame_len_hex( payload_len, lenbuf );

    frame[frame_len++] = '*';
    frame_append( frame, &frame_len, cmd, 4u );
    frame_append( frame, &frame_len, lenbuf, 8u );
    frame[frame_len++] = 0u;
    for ( i = 0; i < field_count; i++ )
    {
        frame_append( frame, &frame_len, fields[i], cstr_len( fields[i] ) );
        frame[frame_len++] = 0u;
    }
    frame[frame_len++] = '#';

    sensor_uart_send_frame_bytes( frame, frame_len );

    printf( "[tx] %.4s len=%lu frame=%lu fields=%lu wiced=%lu/%lu\n",
            cmd, payload_len, frame_len, field_count,
            sensor_uart_wiced_tx_result,
            sensor_uart_wiced_tx_fail_count );
}

void send_netw_up( void )
{
    wiced_bool_t link_up = wiced_network_is_up( WICED_STA_INTERFACE );
    wiced_bool_t network_ready = WICED_FALSE;
    wiced_ip_address_t ip;
    wiced_mac_t mac;
    uint32_t ipv4 = 0u;
    int32_t rssi = 0;
    char rssi_buf[12];
    char ip_buf[16] = "0.0.0.0";
    char mac_buf[18] = "00:00:00:00:00:00";
    const char* net_value;

    if ( wiced_ip_get_ipv4_address( WICED_STA_INTERFACE, &ip ) == WICED_SUCCESS )
    {
        ipv4 = GET_IPV4_ADDRESS( ip );
        ipv4_to_cstr( &ip, ip_buf );
    }
    network_ready = ( link_up == WICED_TRUE && ipv4 != 0u ) ? WICED_TRUE : WICED_FALSE;
    net_value = network_ready == WICED_TRUE ? "1" : "0";

    if ( wiced_wifi_get_mac_address( &mac ) == WICED_SUCCESS )
    {
        mac_to_cstr( &mac, mac_buf );
    }

    if ( link_up != WICED_TRUE || wwd_wifi_get_rssi( &rssi ) != WWD_SUCCESS )
    {
        rssi = 0;
    }
    int32_to_cstr( rssi, rssi_buf, sizeof( rssi_buf ) );

    char* fields[] =
    {
        "net", (char*)net_value,
        "rssi", rssi_buf,
        "ip", ip_buf,
        "mac", mac_buf,
    };

    sensor_send_frame( "NETW", fields, (uint32_t)( sizeof( fields ) / sizeof( fields[0] ) ) );
    sensor_netw_boot_pulses++;
    printf( "[netw] net=%s rssi=%s ip=%s mac=%s\n", net_value, rssi_buf, ip_buf, mac_buf );
}

void send_tinf_from_rule( const rewair_tz_rule_t* rule, uint32_t year )
{
    char dst_on[15];
    char dst_off[15];
    char offs_buf[12];
    char dst_offs_buf[12];
    wall_time_t on_wall;
    wall_time_t off_wall;

    if ( rule->has_dst == 0u )
    {
        int32_to_cstr( (int32_t)rule->std_offset_min * 60, offs_buf, sizeof( offs_buf ) );
        char* fields_fixed[] = { "offs", offs_buf, "dst_offs", "0" };
        sensor_send_frame( "TINF", fields_fixed, 4u );
        return;
    }

    on_wall.year = year;
    on_wall.month = rule->start_month;
    on_wall.day = rewair_tz_rule_day( year, rule->start_month, rule->start_week, rule->start_dow );
    on_wall.hour = rule->start_time_s / 3600u;
    on_wall.minute = ( rule->start_time_s % 3600u ) / 60u;
    on_wall.second = 0u;
    wall_time_to_compact( &on_wall, dst_on );

    off_wall.year = year;
    off_wall.month = rule->end_month;
    off_wall.day = rewair_tz_rule_day( year, rule->end_month, rule->end_week, rule->end_dow );
    off_wall.hour = rule->end_time_s / 3600u;
    off_wall.minute = ( rule->end_time_s % 3600u ) / 60u;
    off_wall.second = 0u;
    wall_time_to_compact( &off_wall, dst_off );

    int32_to_cstr( (int32_t)rule->std_offset_min * 60, offs_buf, sizeof( offs_buf ) );
    int32_to_cstr( ( (int32_t)rule->dst_offset_min - (int32_t)rule->std_offset_min ) * 60,
                   dst_offs_buf, sizeof( dst_offs_buf ) );

    {
        char* fields[] =
        {
            "offs", offs_buf,
            "dst_on", dst_on,
            "dst_off", dst_off,
            "dst_offs", dst_offs_buf,
        };
        sensor_send_frame( "TINF", fields, (uint32_t)( sizeof( fields ) / sizeof( fields[0] ) ) );
    }
}

void send_time_from_rule( const rewair_tz_rule_t* rule, uint32_t utc_seconds )
{
    char time_value[15];
    int16_t offset_min = 0;
    uint8_t dst = 0u;
    char* fields[] = { "time", time_value };

    rewair_tz_eval( rule, utc_seconds, &offset_min, &dst );
    compact_utc_time_with_offset( utc_seconds, (int32_t)offset_min * 60, time_value );
    sensor_send_frame( "TIME", fields, (uint32_t)( sizeof( fields ) / sizeof( fields[0] ) ) );
    printf( "[time] sent TIME %s offset_min=%d dst=%u\n", time_value, (int)offset_min, (unsigned)dst );
}

void send_time_context( uint32_t utc_seconds )
{
    wall_time_t utc_wall;

    epoch_utc_to_wall( utc_seconds, &utc_wall );
    send_tinf_from_rule( &current_tz_rule, utc_wall.year );
    wiced_rtos_delay_milliseconds( 20u );
    send_time_from_rule( &current_tz_rule, utc_seconds );
}

void send_disp_clock_canary( void )
{
    char* fields[] =
    {
        "mode", "clock",
    };

    sensor_send_frame( "DISP", fields, (uint32_t)( sizeof( fields ) / sizeof( fields[0] ) ) );
    printf( "[nudge] DISP clock\n" );
}

wiced_result_t sensor_send_disp_mode( const char* mode )
{
    char* fields[] = { "mode", (char*)mode };

    if ( !cstr_eq( mode, "score" ) && !cstr_eq( mode, "clock" ) && !cstr_eq( mode, "sensors" ) )
    {
        return WICED_BADARG;
    }
    sensor_send_frame( "DISP", fields, 2u );
    return WICED_SUCCESS;
}

void sensor_apply_manual_time( uint32_t epoch )
{
    wiced_utc_time_ms_t ms = (wiced_utc_time_ms_t)epoch * 1000u;

    wiced_time_set_utc_time_ms( &ms );
    send_time_context( epoch );
    rewair_state_set_time( epoch, 0u );
}

void send_sensor_boot_context( void )
{
    if ( sensor_boot_context_sent != 0u )
    {
        return;
    }

    sensor_boot_context_sent = 1u;
    send_netw_up( );
    wiced_rtos_delay_milliseconds( 20u );
    send_tinf_from_rule( &current_tz_rule, 2026u );
}

void send_scor_from_sens( const sens_values_t* sens )
{
    static char empty[] = "";
    char score_buf[12];
    char temp_index_buf[12];
    char humid_index_buf[12];
    char co2_index_buf[12];
    char voc_index_buf[12];
    char dust_index_buf[12];
    char temp_value_buf[12];
    char humid_value_buf[12];
    char co2_value_buf[12];
    char voc_value_buf[12];
    char dust_value_buf[12];
    sens_score_t computed;

    sens_compute_score( sens, &computed );

    int32_to_cstr( (int32_t)computed.score, score_buf, sizeof( score_buf ) );
    int32_to_cstr( (int32_t)computed.idx[0], temp_index_buf, sizeof( temp_index_buf ) );
    int32_to_cstr( (int32_t)computed.idx[1], humid_index_buf, sizeof( humid_index_buf ) );
    int32_to_cstr( (int32_t)computed.idx[2], co2_index_buf, sizeof( co2_index_buf ) );
    int32_to_cstr( (int32_t)computed.idx[3], voc_index_buf, sizeof( voc_index_buf ) );
    int32_to_cstr( (int32_t)computed.idx[4], dust_index_buf, sizeof( dust_index_buf ) );
    int32_to_cstr( centi_to_int( sens->temp ), temp_value_buf, sizeof( temp_value_buf ) );
    int32_to_cstr( centi_to_int( sens->humid ), humid_value_buf, sizeof( humid_value_buf ) );
    int32_to_cstr( centi_to_int( sens->co2 ), co2_value_buf, sizeof( co2_value_buf ) );
    int32_to_cstr( centi_to_int( sens->voc ), voc_value_buf, sizeof( voc_value_buf ) );
    int32_to_cstr( centi_to_int( sens->dust ), dust_value_buf, sizeof( dust_value_buf ) );

    {
        char* fields[] =
        {
            "score", score_buf,
            "color", computed.color,
            "index", empty,
            "temp", temp_index_buf,
            "humid", humid_index_buf,
            "co2", co2_index_buf,
            "voc", voc_index_buf,
            "dust", dust_index_buf,
            "sensor", empty,
            "temp", temp_value_buf,
            "humid", humid_value_buf,
            "co2", co2_value_buf,
            "voc", voc_value_buf,
            "dust", dust_value_buf,
        };

        sensor_send_frame( "SCOR", fields, (uint32_t)( sizeof( fields ) / sizeof( fields[0] ) ) );
    }

    printf( "[auto] score=%s color=%s t=%s h=%s co2=%s voc=%s dust=%s\n",
            score_buf, computed.color, temp_value_buf, humid_value_buf, co2_value_buf,
            voc_value_buf, dust_value_buf );

    {
        rewair_sens_t cache_sens;

        cache_sens.temp = sens->temp;
        cache_sens.humid = sens->humid;
        cache_sens.co2 = sens->co2;
        cache_sens.voc = sens->voc;
        cache_sens.dust = sens->dust;
        cache_sens.light = ( sens->seen & SENS_LIGHT ) != 0u ? centi_to_int( sens->light ) : 0;
        rewair_state_set_sens( &cache_sens, computed.score, computed.color, computed.idx );
    }
}
