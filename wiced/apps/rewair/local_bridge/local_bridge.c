/*
 * Rewair local bridge bring-up app.
 *
 * First milestone: run under WICED, keep the radio stack initialized, and speak
 * enough of the F103 UART protocol to keep the sensor/display board alive.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wiced.h"
#include "wiced_framework.h"
#include "wiced_wifi.h"
#include "wiced_management.h"
#include "wiced_tcpip.h"
#include "internal/wiced_internal_api.h"
#include "sntp.h"
#include "rewair_state.h"
#include "rewair_tz.h"
#include "rewair_settings.h"
#include "web_api.h"

#define SENSOR_PAYLOAD_MAX 512u
#define SENSOR_RX_BUFFER_SIZE 1024u
#define SENSOR_THREAD_STACK_SIZE 4096u
#define CONSOLE_THREAD_STACK_SIZE 4096u
#define NETWORK_THREAD_STACK_SIZE 6144u
#define CONSOLE_LINE_MAX 160u
#define CONSOLE_ARG_MAX 32
#define CONSOLE_SCAN_CACHE_MAX 16u
#define SENSOR_DIAG_REPEAT_MS 1000u
#define SENSOR_STAT_REPEAT_MS 10000u
#define SENSOR_NUDGE_START_MS 5000u
#define SENSOR_NETW_NUDGE_MS 10000u
#define SENSOR_NETW_NUDGE_MAX 5u
#define NETWORK_RETRY_MS 60000u
#define NTP_RETRY_COUNT 3u
#define NTP_SYNC_PERIOD_MS ( 12u * 60u * 60u * 1000u )
#define SENSOR_RAW_TRACE_MAX 64u
#define SENSOR_FRAME_MAX ( 1u + 4u + 8u + 1u + SENSOR_PAYLOAD_MAX + 1u )

#define SENS_TEMP  (1u << 0)
#define SENS_HUMID (1u << 1)
#define SENS_CO2   (1u << 2)
#define SENS_VOC   (1u << 3)
#define SENS_DUST  (1u << 4)
#define SENS_LIGHT (1u << 5)
#define SENS_ALL   (SENS_TEMP | SENS_HUMID | SENS_CO2 | SENS_VOC | SENS_DUST)

typedef struct
{
    uint8_t state;
    uint32_t pos;
    uint32_t length;
    char cmd[4];
    uint8_t header[13];
    uint8_t payload[SENSOR_PAYLOAD_MAX];
} sensor_rx_t;

typedef struct
{
    uint32_t seen;
    int32_t temp;
    int32_t humid;
    int32_t co2;
    int32_t voc;
    int32_t dust;
    int32_t light;
} sens_values_t;

static sensor_rx_t sensor_rx;
static wiced_thread_t sensor_thread;
static wiced_thread_t console_thread;
static wiced_thread_t network_thread;
static wiced_mutex_t sensor_uart_tx_mutex;
static wiced_ring_buffer_t sensor_uart_rx_buffer;
static uint8_t sensor_uart_rx_data[SENSOR_RX_BUFFER_SIZE];

#define SENSOR_UART_ERROR_MASK ( USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE )

static const wiced_uart_config_t sensor_uart_config =
{
    .baud_rate    = 115200,
    .data_width   = DATA_WIDTH_8BIT,
    .parity       = NO_PARITY,
    .stop_bits    = STOP_BITS_1,
    .flow_control = FLOW_CONTROL_DISABLED,
};

static uint32_t auto_score_enabled = 1u;
static volatile uint32_t sensor_reset_released = 0u;
static volatile uint32_t sensor_boot_context_sent = 0u;
static volatile uint32_t sensor_sens_seen = 0u;
static volatile uint32_t sensor_netw_boot_pulses = 0u;
static volatile uint32_t sensor_netw_nudge_count = 0u;
static volatile uint32_t sensor_last_nudge_ms = 0u;
static volatile uint32_t sensor_disp_clock_canary_sent = 0u;
static volatile uint32_t sensor_uart_rx_bytes = 0u;
static volatile uint32_t sensor_uart_error_flags = 0u;
static volatile uint32_t sensor_uart_error_count = 0u;
static volatile uint32_t sensor_uart_error_report_count = 0u;
static volatile uint32_t sensor_uart_error_reported_flags = 0u;
static volatile uint32_t sensor_uart_error_clear_count = 0u;
static volatile uint32_t sensor_uart_last_clear_sr = 0u;
static volatile uint32_t sensor_uart_last_clear_dr = 0u;
static volatile uint32_t sensor_uart_last_clear_after_sr = 0u;
static volatile uint32_t sensor_uart_last_rx_byte = 0xffffffffu;
static volatile uint32_t sensor_uart_last_rx_sr = 0u;
static volatile uint32_t sensor_uart_tx_count = 0u;
static volatile uint32_t sensor_uart_tx_drop_count = 0u;
static volatile uint32_t sensor_uart_wiced_tx_fail_count = 0u;
static volatile uint32_t sensor_uart_wiced_tx_result = 0u;
static volatile uint32_t sensor_uart_tx_sr_before = 0u;
static volatile uint32_t sensor_uart_tx_sr_after = 0u;
static volatile uint32_t sensor_sens_count = 0u;
static volatile uint32_t sensor_last_rx_ms = 0u;
static volatile uint32_t sensor_last_sens_ms = 0u;
static char sensor_last_frame_cmd[5] = "----";
static volatile uint32_t sensor_last_frame_trailer = 0u;
static uint8_t sensor_raw_trace[SENSOR_RAW_TRACE_MAX];
static volatile uint32_t sensor_raw_trace_count = 0u;
static uint32_t sensor_raw_trace_reported = 0u;
static volatile uint32_t console_scan_count = 0u;
static uint32_t console_scan_cache_count = 0u;
static wiced_scan_result_t console_scan_cache[CONSOLE_SCAN_CACHE_MAX];
static wiced_semaphore_t scan_done_semaphore;
static uint32_t scan_semaphore_inited = 0u;
static volatile uint32_t scan_in_progress = 0u;
/* Benign-race guard between the console "scan" command (console thread) and
 * /api/scan (HTTP worker thread): both would otherwise reset console_scan_cache
 * and console_scan_cache_count concurrently and corrupt each other's results.
 * This is not a strict mutex (no cheap atomics on this platform) -- it just
 * prevents cache clobbering from concurrent scans; a rare TOCTOU sliver where
 * both sides read 0 and proceed is acceptable since the worst case is one
 * scan's results getting overwritten, not corrupted. */
static volatile uint32_t scan_busy = 0u;
static volatile uint32_t wifi_join_in_progress = 0u;
static volatile uint32_t wifi_time_synced = 0u;
static volatile uint32_t wifi_last_ntp_sync_ms = 0u;
static volatile uint32_t wifi_network_ready_ms = 0u;
static volatile uint32_t wifi_link_was_up = 0u;

static rewair_tz_rule_t current_tz_rule;
static uint32_t current_tz_rule_valid = 0u;
static rewair_settings_t current_settings;

void sensor_set_tz_rule( const rewair_tz_rule_t* rule )
{
    current_tz_rule = *rule;
    current_tz_rule_valid = 1u;
}

static void network_after_ip_ready( void );
static void sensor_send_frame( const char cmd[4], char** fields, uint32_t field_count );
static void send_sensor_boot_context( void );
static void send_tinf_from_rule( const rewair_tz_rule_t* rule, uint32_t year );
static void send_time_from_rule( const rewair_tz_rule_t* rule, uint32_t utc_seconds );
static void send_time_context( uint32_t utc_seconds );
static void send_disp_clock_canary( void );
static void sensor_reset_cycle( void );

static uint32_t cstr_len( const char* s )
{
    uint32_t n = 0u;
    while ( s[n] != '\0' )
    {
        n++;
    }
    return n;
}

static int cstr_eq( const char* a, const char* b )
{
    while ( *a == *b )
    {
        if ( *a == '\0' )
        {
            return 1;
        }
        a++;
        b++;
    }
    return 0;
}

static int cmd4_eq( const char cmd[4], const char* text )
{
    return cmd[0] == text[0] && cmd[1] == text[1] && cmd[2] == text[2] &&
           cmd[3] == text[3] && text[4] == '\0';
}

static int ascii_space( char c )
{
    return c == ' ' || c == '\t';
}

static void console_prompt( void )
{
    printf( "awair> " );
}

static int console_read_line( char* line, uint32_t line_size )
{
    uint32_t pos = 0u;

    while ( 1 )
    {
        int c = getchar( );

        if ( c == '\r' || c == '\n' )
        {
            line[pos] = '\0';
            printf( "\n" );
            return 1;
        }

        if ( c == 0x08 || c == 0x7f )
        {
            if ( pos != 0u )
            {
                pos--;
                printf( "\b \b" );
            }
            continue;
        }

        if ( c >= 0x20 && c <= 0x7e )
        {
            if ( pos + 1u < line_size )
            {
                line[pos++] = (char)c;
                putchar( c );
            }
        }
    }
}

static int console_tokenize( char* line, char* argv[], int max_argc )
{
    char* cursor = line;
    int argc = 0;

    while ( *cursor != '\0' )
    {
        while ( ascii_space( *cursor ) )
        {
            cursor++;
        }

        if ( *cursor == '\0' )
        {
            break;
        }

        if ( argc == max_argc )
        {
            return -1;
        }

        if ( *cursor == '"' )
        {
            cursor++;
            argv[argc++] = cursor;

            while ( *cursor != '\0' && *cursor != '"' )
            {
                cursor++;
            }

            if ( *cursor == '"' )
            {
                *cursor++ = '\0';
            }
        }
        else
        {
            argv[argc++] = cursor;

            while ( *cursor != '\0' && ascii_space( *cursor ) == 0 )
            {
                cursor++;
            }

            if ( *cursor != '\0' )
            {
                *cursor++ = '\0';
            }
        }
    }

    return argc;
}

static const char* wifi_security_name( wiced_security_t security )
{
    switch ( security )
    {
        case WICED_SECURITY_OPEN:
            return "open";
        case WICED_SECURITY_WEP_PSK:
            return "wep";
        case WICED_SECURITY_WPA_TKIP_PSK:
            return "wpa-tkip";
        case WICED_SECURITY_WPA_AES_PSK:
            return "wpa-aes";
        case WICED_SECURITY_WPA_MIXED_PSK:
            return "wpa-mixed";
        case WICED_SECURITY_WPA2_AES_PSK:
            return "wpa2-aes";
        case WICED_SECURITY_WPA2_TKIP_PSK:
            return "wpa2-tkip";
        case WICED_SECURITY_WPA2_MIXED_PSK:
            return "wpa2-mixed";
        default:
            return "unknown";
    }
}

static int parse_wifi_security( const char* text, wiced_security_t* security )
{
    if ( cstr_eq( text, "open" ) )
    {
        *security = WICED_SECURITY_OPEN;
        return 1;
    }
    if ( cstr_eq( text, "wpa" ) || cstr_eq( text, "wpa-mixed" ) )
    {
        *security = WICED_SECURITY_WPA_MIXED_PSK;
        return 1;
    }
    if ( cstr_eq( text, "wpa2" ) || cstr_eq( text, "wpa2-mixed" ) )
    {
        *security = WICED_SECURITY_WPA2_MIXED_PSK;
        return 1;
    }
    if ( cstr_eq( text, "wpa2-aes" ) )
    {
        *security = WICED_SECURITY_WPA2_AES_PSK;
        return 1;
    }
    if ( cstr_eq( text, "wpa2-tkip" ) )
    {
        *security = WICED_SECURITY_WPA2_TKIP_PSK;
        return 1;
    }
    if ( cstr_eq( text, "wpa-aes" ) )
    {
        *security = WICED_SECURITY_WPA_AES_PSK;
        return 1;
    }
    if ( cstr_eq( text, "wpa-tkip" ) )
    {
        *security = WICED_SECURITY_WPA_TKIP_PSK;
        return 1;
    }
    return 0;
}

static int parse_uint32( const char* text, uint32_t* out )
{
    uint32_t value = 0u;

    if ( *text == '\0' )
    {
        return 0;
    }

    while ( *text != '\0' )
    {
        if ( *text < '0' || *text > '9' )
        {
            return 0;
        }
        value = ( value * 10u ) + (uint32_t)( *text - '0' );
        text++;
    }

    *out = value;
    return 1;
}

static const char* wifi_result_name( wiced_result_t result )
{
    switch ( result )
    {
        case WICED_SUCCESS:
            return "success";
        case WICED_TIMEOUT:
            return "timeout";
        case WICED_BADARG:
            return "badarg";
        case WICED_NOTUP:
            return "not-up";
        case WICED_NOT_FOUND:
            return "not-found";
        case WICED_NO_STORED_AP_IN_DCT:
            return "no-stored-ap";
        case WICED_STA_JOIN_FAILED:
            return "sta-join-failed";
        case WICED_WWD_NOT_AUTHENTICATED:
            return "not-authenticated";
        case WICED_WWD_INVALID_KEY:
            return "invalid-key";
        case WICED_WWD_CONNECTION_LOST:
            return "connection-lost";
        default:
            return "unknown";
    }
}

static void print_ipv4( const wiced_ip_address_t* address )
{
    uint32_t ipv4 = GET_IPV4_ADDRESS( *address );
    printf( "%lu.%lu.%lu.%lu",
            (unsigned long)( ( ipv4 >> 24 ) & 0xffu ),
            (unsigned long)( ( ipv4 >> 16 ) & 0xffu ),
            (unsigned long)( ( ipv4 >> 8 ) & 0xffu ),
            (unsigned long)( ipv4 & 0xffu ) );
}

static void ipv4_to_cstr( const wiced_ip_address_t* address, char out[16] )
{
    uint32_t ipv4 = GET_IPV4_ADDRESS( *address );
    sprintf( out, "%lu.%lu.%lu.%lu",
             (unsigned long)( ( ipv4 >> 24 ) & 0xffu ),
             (unsigned long)( ( ipv4 >> 16 ) & 0xffu ),
             (unsigned long)( ( ipv4 >> 8 ) & 0xffu ),
             (unsigned long)( ipv4 & 0xffu ) );
}

static void mac_to_cstr( const wiced_mac_t* mac, char out[18] )
{
    sprintf( out, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac->octet[0], mac->octet[1], mac->octet[2],
             mac->octet[3], mac->octet[4], mac->octet[5] );
}

static void print_ssid( const wiced_ssid_t* ssid )
{
    uint32_t i;
    for ( i = 0u; i < ssid->length && i < SSID_NAME_SIZE; i++ )
    {
        uint8_t c = ssid->value[i];
        printf( "%c", ( c >= 0x20u && c <= 0x7eu ) ? c : '.' );
    }
}

static int ssid_eq_text( const wiced_ssid_t* ssid, const char* text )
{
    uint32_t length = cstr_len( text );
    if ( length != ssid->length || length > SSID_NAME_SIZE )
    {
        return 0;
    }
    return memcmp( ssid->value, text, length ) == 0;
}

const wiced_scan_result_t* find_best_scan_result_for_ssid( const char* ssid_text )
{
    const wiced_scan_result_t* best = NULL;
    uint32_t i;

    for ( i = 0u; i < console_scan_cache_count; i++ )
    {
        const wiced_scan_result_t* record = &console_scan_cache[i];
        if ( ssid_eq_text( &record->SSID, ssid_text ) == 0 )
        {
            continue;
        }

        if ( best == NULL || record->signal_strength > best->signal_strength )
        {
            best = record;
        }
    }

    return best;
}

const wiced_scan_result_t* console_scan_cache_get( uint32_t index )
{
    if ( index >= console_scan_cache_count )
    {
        return NULL;
    }
    return &console_scan_cache[index];
}

static void ap_from_scan_result( wiced_ap_info_t* ap, const wiced_scan_result_t* scan )
{
    memset( ap, 0, sizeof( *ap ) );
    ap->SSID = scan->SSID;
    ap->BSSID = scan->BSSID;
    ap->signal_strength = scan->signal_strength;
    ap->max_data_rate = scan->max_data_rate;
    ap->bss_type = scan->bss_type;
    ap->security = scan->security;
    ap->channel = scan->channel;
    ap->band = scan->band;
}

static void wifi_print_status( void )
{
    wiced_ip_address_t ip;
    wiced_ip_address_t gateway;

    printf( "[wifi] sta network=%s\n", wiced_network_is_up( WICED_STA_INTERFACE ) == WICED_TRUE ? "up" : "down" );
    if ( wiced_ip_get_ipv4_address( WICED_STA_INTERFACE, &ip ) == WICED_SUCCESS )
    {
        printf( "[wifi] ip=" );
        print_ipv4( &ip );
        printf( "\n" );
    }
    if ( wiced_ip_get_gateway_address( WICED_STA_INTERFACE, &gateway ) == WICED_SUCCESS )
    {
        printf( "[wifi] gateway=" );
        print_ipv4( &gateway );
        printf( "\n" );
    }
}

static wiced_result_t console_scan_result_handler( wiced_scan_handler_result_t* malloced_scan_result )
{
    if ( malloced_scan_result != NULL )
    {
        malloc_transfer_to_curr_thread( malloced_scan_result );

        if ( malloced_scan_result->status == WICED_SCAN_INCOMPLETE )
        {
            wiced_scan_result_t* record = &malloced_scan_result->ap_details;
            uint32_t index = console_scan_count++;

            if ( console_scan_cache_count < CONSOLE_SCAN_CACHE_MAX )
            {
                memcpy( &console_scan_cache[console_scan_cache_count], record, sizeof( console_scan_cache[0] ) );
                index = console_scan_cache_count;
                console_scan_cache_count++;
            }

            printf( "[scan] %2lu rssi=%4d ch=%2u sec=%-11s ssid=\"",
                    (unsigned long)index,
                    (int)record->signal_strength,
                    (unsigned int)record->channel,
                    wifi_security_name( record->security ) );
            print_ssid( &record->SSID );
            printf( "\" bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    record->BSSID.octet[0],
                    record->BSSID.octet[1],
                    record->BSSID.octet[2],
                    record->BSSID.octet[3],
                    record->BSSID.octet[4],
                    record->BSSID.octet[5] );
        }
        else
        {
            printf( "[scan] complete results=%lu\n", (unsigned long)console_scan_count );

            if ( scan_in_progress != 0u )
            {
                scan_in_progress = 0u;
                wiced_rtos_set_semaphore( &scan_done_semaphore );
            }
            scan_busy = 0u;
        }

        free( malloced_scan_result );
    }

    return WICED_SUCCESS;
}

static void wifi_scan_start( void )
{
    wiced_result_t result;

    if ( scan_busy != 0u )
    {
        printf( "[scan] busy, try again\n" );
        return;
    }
    scan_busy = 1u;

    console_scan_count = 0u;
    console_scan_cache_count = 0u;
    memset( console_scan_cache, 0, sizeof( console_scan_cache ) );
    printf( "[scan] starting\n" );
    result = wiced_wifi_scan_networks( console_scan_result_handler, NULL );
    if ( result != WICED_SUCCESS )
    {
        printf( "[scan] failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        scan_busy = 0u;
    }
}

uint32_t sensor_scan_blocking( void )
{
    if ( scan_semaphore_inited == 0u )
    {
        wiced_rtos_init_semaphore( &scan_done_semaphore );
        scan_semaphore_inited = 1u;
    }

    if ( scan_busy != 0u )
    {
        /* Console "scan" already owns the cache -- return the current (possibly
         * stale) results rather than clobbering them; see scan_busy comment above. */
        return console_scan_cache_count;
    }
    scan_busy = 1u;

    /* Drain any semaphore signal left over from a prior scan that timed out
     * after its completion callback finally landed -- otherwise this scan's
     * wiced_rtos_get_semaphore below would return immediately on stale state. */
    while ( wiced_rtos_get_semaphore( &scan_done_semaphore, 0u ) == WICED_SUCCESS )
    {
    }

    console_scan_count = 0u;
    console_scan_cache_count = 0u;
    memset( console_scan_cache, 0, sizeof( console_scan_cache ) );
    scan_in_progress = 1u;
    if ( wiced_wifi_scan_networks( console_scan_result_handler, NULL ) != WICED_SUCCESS )
    {
        scan_in_progress = 0u;
        scan_busy = 0u;
        return 0u;
    }
    if ( wiced_rtos_get_semaphore( &scan_done_semaphore, 6000u ) != WICED_SUCCESS )
    {
        /* Timed out: clear scan_in_progress so a late completion callback does
         * not pre-signal the semaphore for the NEXT scan, and release the
         * busy guard so a subsequent scan is not blocked forever. On success,
         * console_scan_result_handler's completion branch already cleared
         * scan_busy. */
        scan_in_progress = 0u;
        scan_busy = 0u;
        return console_scan_cache_count;
    }
    return console_scan_cache_count;
}

static int wifi_dct_has_stored_ap( void )
{
    wiced_config_ap_entry_t* ap = NULL;
    int present = 0;

    if ( wiced_dct_read_lock( (void**)&ap, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                              OFFSETOF( platform_dct_wifi_config_t, stored_ap_list ),
                              sizeof( *ap ) ) != WICED_SUCCESS )
    {
        return 0;
    }

    present = ap->details.SSID.length != 0u && ap->details.SSID.length <= SSID_NAME_SIZE;
    wiced_dct_read_unlock( ap, WICED_FALSE );
    return present;
}

uint32_t wifi_dct_saved_count( void )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    uint32_t count = 0u;
    uint32_t i;

    if ( wiced_dct_read_lock( (void**)&wifi_config, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                              0, sizeof( *wifi_config ) ) != WICED_SUCCESS )
    {
        return 0u;
    }
    for ( i = 0u; i < CONFIG_AP_LIST_SIZE; i++ )
    {
        if ( wifi_config->stored_ap_list[i].details.SSID.length != 0u &&
             wifi_config->stored_ap_list[i].details.SSID.length <= SSID_NAME_SIZE )
        {
            count++;
        }
    }
    wiced_dct_read_unlock( wifi_config, WICED_FALSE );
    return count;
}

static void wifi_print_saved_credentials( void )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    uint32_t i;

    if ( wiced_dct_read_lock( (void**)&wifi_config, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                              0, sizeof( *wifi_config ) ) != WICED_SUCCESS )
    {
        printf( "[wifi] saved: DCT read failed\n" );
        return;
    }

    for ( i = 0u; i < CONFIG_AP_LIST_SIZE; i++ )
    {
        const wiced_config_ap_entry_t* ap = &wifi_config->stored_ap_list[i];
        if ( ap->details.SSID.length == 0u || ap->details.SSID.length > SSID_NAME_SIZE )
        {
            continue;
        }

        printf( "[wifi] saved[%lu] ssid=\"", (unsigned long)i );
        print_ssid( &ap->details.SSID );
        printf( "\" security=%s key_len=%u ch=%u\n",
                wifi_security_name( ap->details.security ),
                ap->security_key_length,
                (unsigned int)ap->details.channel );
    }

    wiced_dct_read_unlock( wifi_config, WICED_FALSE );
}

static wiced_result_t wifi_clear_stored_credentials( void )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    wiced_result_t result;

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        printf( "[wifi] DCT read failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        return result;
    }

    memset( wifi_config->stored_ap_list, 0, sizeof( wifi_config->stored_ap_list ) );
    wifi_config->device_configured = WICED_FALSE;
    result = wiced_dct_write( wifi_config, DCT_WIFI_CONFIG_SECTION, 0, sizeof( *wifi_config ) );
    wiced_dct_read_unlock( wifi_config, WICED_TRUE );

    printf( "[wifi] stored credentials %s\n", result == WICED_SUCCESS ? "cleared" : "clear failed" );
    return result;
}

static wiced_result_t wifi_store_ap_credentials( const wiced_ap_info_t* ap,
                                                const char* security_key,
                                                uint32_t security_key_length )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    wiced_config_ap_entry_t* stored;
    wiced_result_t result;

    if ( ap->SSID.length == 0u || ap->SSID.length > SSID_NAME_SIZE || security_key_length > SECURITY_KEY_SIZE )
    {
        return WICED_BADARG;
    }

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        printf( "[wifi] DCT read failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        return result;
    }

    memset( wifi_config->stored_ap_list, 0, sizeof( wifi_config->stored_ap_list ) );
    stored = &wifi_config->stored_ap_list[0];
    stored->details = *ap;
    stored->security_key_length = (uint8_t)security_key_length;
    memset( stored->security_key, 0, sizeof( stored->security_key ) );
    if ( security_key_length != 0u )
    {
        memcpy( stored->security_key, security_key, security_key_length );
    }
    wifi_config->device_configured = WICED_TRUE;

    result = wiced_dct_write( wifi_config, DCT_WIFI_CONFIG_SECTION, 0, sizeof( *wifi_config ) );
    wiced_dct_read_unlock( wifi_config, WICED_TRUE );

    if ( result == WICED_SUCCESS )
    {
        printf( "[wifi] saved credentials to DCT slot 0\n" );
    }
    else
    {
        printf( "[wifi] DCT write failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
    }
    return result;
}

static wiced_result_t wifi_join_command( const char* ssid_text, const char* pass_text,
                                         wiced_security_t security, const wiced_scan_result_t* scan,
                                         int force_security )
{
    wiced_ap_info_t ap;
    char security_key[64];
    uint32_t ssid_len = scan == NULL ? cstr_len( ssid_text ) : scan->SSID.length;
    uint32_t pass_len = cstr_len( pass_text );
    wiced_result_t result;

    wifi_join_in_progress = 1u;

    if ( ssid_len == 0u || ssid_len > SSID_NAME_SIZE )
    {
        printf( "[wifi] bad SSID length=%lu\n", (unsigned long)ssid_len );
        wifi_join_in_progress = 0u;
        return WICED_BADARG;
    }

    if ( security != WICED_SECURITY_OPEN && pass_len > 63u )
    {
        printf( "[wifi] passphrase too long length=%lu\n", (unsigned long)pass_len );
        wifi_join_in_progress = 0u;
        return WICED_BADARG;
    }

    memset( &ap, 0, sizeof( ap ) );
    memset( security_key, 0, sizeof( security_key ) );
    if ( scan != NULL )
    {
        ap_from_scan_result( &ap, scan );
        if ( force_security == 0 )
        {
            security = ap.security;
        }
        else
        {
            ap.security = security;
        }
    }
    else
    {
        ap.SSID.length = ssid_len;
        memcpy( ap.SSID.value, ssid_text, ssid_len );
        ap.bss_type = WICED_BSS_TYPE_INFRASTRUCTURE;
        ap.security = security;
        ap.band = WICED_802_11_BAND_2_4GHZ;
    }

    if ( security != WICED_SECURITY_OPEN )
    {
        memcpy( security_key, pass_text, pass_len );
    }
    else
    {
        pass_len = 0u;
    }

    printf( "[wifi] joining \"" );
    print_ssid( &ap.SSID );
    printf( "\" security=%s key_len=%lu", wifi_security_name( security ), (unsigned long)pass_len );
    if ( scan != NULL )
    {
        printf( " ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                (unsigned int)ap.channel,
                ap.BSSID.octet[0],
                ap.BSSID.octet[1],
                ap.BSSID.octet[2],
                ap.BSSID.octet[3],
                ap.BSSID.octet[4],
                ap.BSSID.octet[5] );
    }
    printf( "\n" );

    if ( wiced_network_is_up( WICED_STA_INTERFACE ) == WICED_TRUE )
    {
        wiced_network_down( WICED_STA_INTERFACE );
    }
    else
    {
        wiced_leave_ap( WICED_STA_INTERFACE );
    }

    result = wiced_join_ap_specific( &ap, (uint8_t)pass_len, security_key );
    if ( result != WICED_SUCCESS )
    {
        printf( "[wifi] join failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        wifi_join_in_progress = 0u;
        return result;
    }

    printf( "[wifi] joined; starting DHCP\n" );
    result = wiced_ip_up( WICED_STA_INTERFACE, WICED_USE_EXTERNAL_DHCP_SERVER, NULL );
    if ( result != WICED_SUCCESS )
    {
        printf( "[wifi] DHCP failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        wiced_leave_ap( WICED_STA_INTERFACE );
        wifi_join_in_progress = 0u;
        return result;
    }

    wifi_print_status( );
    wifi_store_ap_credentials( &ap, security_key, pass_len );
    wifi_join_in_progress = 0u;
    network_after_ip_ready( );
    return WICED_SUCCESS;
}

static void wifi_join_test_index_command( const char* index_text, const char* pass_text,
                                          wiced_security_t security, int force_security )
{
    const wiced_scan_result_t* scan;
    wiced_scan_result_t ap;
    char security_key[64];
    uint32_t index;
    uint32_t pass_len = cstr_len( pass_text );
    wiced_result_t result;

    if ( parse_uint32( index_text, &index ) == 0 )
    {
        printf( "[wifi] bad scan index \"%s\"\n", index_text );
        return;
    }

    scan = console_scan_cache_get( index );
    if ( scan == NULL )
    {
        printf( "[wifi] no cached scan result %lu; run scan first\n", (unsigned long)index );
        return;
    }

    memcpy( &ap, scan, sizeof( ap ) );
    if ( force_security == 0 )
    {
        security = ap.security;
    }
    else
    {
        ap.security = security;
    }

    if ( security != WICED_SECURITY_OPEN && pass_len > 63u )
    {
        printf( "[wifi] passphrase too long length=%lu\n", (unsigned long)pass_len );
        return;
    }

    memset( security_key, 0, sizeof( security_key ) );
    if ( security != WICED_SECURITY_OPEN )
    {
        memcpy( security_key, pass_text, pass_len );
    }
    else
    {
        pass_len = 0u;
    }

    printf( "[wifi-test] ssid=\"" );
    print_ssid( &ap.SSID );
    printf( "\" security=%s key_len=%lu ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
            wifi_security_name( security ),
            (unsigned long)pass_len,
            (unsigned int)ap.channel,
            ap.BSSID.octet[0],
            ap.BSSID.octet[1],
            ap.BSSID.octet[2],
            ap.BSSID.octet[3],
            ap.BSSID.octet[4],
            ap.BSSID.octet[5] );

    wiced_network_down( WICED_STA_INTERFACE );
    wiced_leave_ap( WICED_STA_INTERFACE );

    result = (wiced_result_t)wwd_wifi_join_specific( &ap, (uint8_t*)security_key, (uint8_t)pass_len,
                                                     NULL, WWD_STA_INTERFACE );
    printf( "[wifi-test] specific result=%d (%s)\n", (int)result, wifi_result_name( result ) );
    wwd_wifi_leave( WWD_STA_INTERFACE );
    wiced_rtos_delay_milliseconds( 250u );

    result = (wiced_result_t)wwd_wifi_join( &ap.SSID, security, (uint8_t*)security_key,
                                            (uint8_t)pass_len, NULL );
    printf( "[wifi-test] ssid-only result=%d (%s)\n", (int)result, wifi_result_name( result ) );
    wwd_wifi_leave( WWD_STA_INTERFACE );
}

static void wifi_join_index_command( const char* index_text, const char* pass_text,
                                     wiced_security_t security, int force_security )
{
    const wiced_scan_result_t* scan;
    uint32_t index;

    if ( parse_uint32( index_text, &index ) == 0 )
    {
        printf( "[wifi] bad scan index \"%s\"\n", index_text );
        return;
    }

    scan = console_scan_cache_get( index );
    if ( scan == NULL )
    {
        printf( "[wifi] no cached scan result %lu; run scan first\n", (unsigned long)index );
        return;
    }

    wifi_join_command( (const char*)scan->SSID.value, pass_text, security, scan, force_security );
}

static void console_print_help( void )
{
    printf( "commands:\n" );
    printf( "  help\n" );
    printf( "  scan\n" );
    printf( "  join \"ssid\" \"pass\" [wpa2|wpa2-aes|wpa2-tkip|wpa|wpa-aes|wpa-tkip|open]\n" );
    printf( "  join-index n \"pass\" [security]\n" );
    printf( "  join-test-index n \"pass\" [security]\n" );
    printf( "  join-open \"ssid\"\n" );
    printf( "  frame CMD [field ...]\n" );
    printf( "  time-now\n" );
    printf( "  tinf [year]\n" );
    printf( "  context\n" );
    printf( "  sensor-reset\n" );
    printf( "  forget\n" );
    printf( "  saved\n" );
    printf( "  down\n" );
    printf( "  net\n" );
}

static void console_handle_command( int argc, char* argv[] )
{
    wiced_security_t security = WICED_SECURITY_WPA2_MIXED_PSK;

    if ( argc == 0 )
    {
        return;
    }

    if ( cstr_eq( argv[0], "help" ) || cstr_eq( argv[0], "?" ) )
    {
        console_print_help( );
    }
    else if ( cstr_eq( argv[0], "scan" ) )
    {
        wifi_scan_start( );
    }
    else if ( cstr_eq( argv[0], "join" ) )
    {
        const wiced_scan_result_t* scan;
        int force_security = 0;
        if ( argc < 3 )
        {
            printf( "usage: join \"ssid\" \"pass\" [security]\n" );
            return;
        }
        if ( argc >= 4 && parse_wifi_security( argv[3], &security ) == 0 )
        {
            printf( "[wifi] unknown security \"%s\"\n", argv[3] );
            return;
        }
        force_security = argc >= 4;
        scan = find_best_scan_result_for_ssid( argv[1] );
        if ( scan == NULL )
        {
            printf( "[wifi] no cached scan result for \"%s\"; using guessed AP details\n", argv[1] );
        }
        wifi_join_command( argv[1], argv[2], security, scan, force_security );
    }
    else if ( cstr_eq( argv[0], "join-index" ) )
    {
        int force_security = 0;
        if ( argc < 3 )
        {
            printf( "usage: join-index n \"pass\" [security]\n" );
            return;
        }
        if ( argc >= 4 && parse_wifi_security( argv[3], &security ) == 0 )
        {
            printf( "[wifi] unknown security \"%s\"\n", argv[3] );
            return;
        }
        force_security = argc >= 4;
        wifi_join_index_command( argv[1], argv[2], security, force_security );
    }
    else if ( cstr_eq( argv[0], "join-test-index" ) )
    {
        int force_security = 0;
        if ( argc < 3 )
        {
            printf( "usage: join-test-index n \"pass\" [security]\n" );
            return;
        }
        if ( argc >= 4 && parse_wifi_security( argv[3], &security ) == 0 )
        {
            printf( "[wifi] unknown security \"%s\"\n", argv[3] );
            return;
        }
        force_security = argc >= 4;
        wifi_join_test_index_command( argv[1], argv[2], security, force_security );
    }
    else if ( cstr_eq( argv[0], "join-open" ) )
    {
        if ( argc < 2 )
        {
            printf( "usage: join-open \"ssid\"\n" );
            return;
        }
        wifi_join_command( argv[1], "", WICED_SECURITY_OPEN, find_best_scan_result_for_ssid( argv[1] ), 1 );
    }
    else if ( cstr_eq( argv[0], "frame" ) )
    {
        if ( argc < 2 || cstr_len( argv[1] ) != 4u )
        {
            printf( "usage: frame CMD [field ...]\n" );
            return;
        }
        sensor_send_frame( argv[1], &argv[2], (uint32_t)( argc - 2 ) );
    }
    else if ( cstr_eq( argv[0], "time-now" ) )
    {
        wiced_utc_time_t utc_seconds = 0u;
        if ( wiced_time_get_utc_time( &utc_seconds ) == WICED_SUCCESS && utc_seconds != 0u )
        {
            send_time_context( utc_seconds );
        }
        else
        {
            printf( "[time] UTC clock is not set yet\n" );
        }
    }
    else if ( cstr_eq( argv[0], "tinf" ) )
    {
        uint32_t year = 2026u;
        if ( argc >= 2 && parse_uint32( argv[1], &year ) == 0 )
        {
            printf( "usage: tinf [year]\n" );
            return;
        }
        send_tinf_from_rule( &current_tz_rule, year );
    }
    else if ( cstr_eq( argv[0], "context" ) )
    {
        sensor_boot_context_sent = 0u;
        send_sensor_boot_context( );
    }
    else if ( cstr_eq( argv[0], "sensor-reset" ) )
    {
        sensor_reset_cycle( );
    }
    else if ( cstr_eq( argv[0], "forget" ) )
    {
        wiced_network_down( WICED_STA_INTERFACE );
        wiced_leave_ap( WICED_STA_INTERFACE );
        wifi_time_synced = 0u;
        wifi_last_ntp_sync_ms = 0u;
        wifi_clear_stored_credentials( );
    }
    else if ( cstr_eq( argv[0], "saved" ) )
    {
        wifi_print_saved_credentials( );
    }
    else if ( cstr_eq( argv[0], "down" ) )
    {
        wiced_network_down( WICED_STA_INTERFACE );
        wiced_leave_ap( WICED_STA_INTERFACE );
        wifi_time_synced = 0u;
        wifi_last_ntp_sync_ms = 0u;
        printf( "[wifi] down\n" );
    }
    else if ( cstr_eq( argv[0], "net" ) )
    {
        wifi_print_status( );
        wifi_print_saved_credentials( );
    }
    else
    {
        printf( "unknown command: %s\n", argv[0] );
    }
}

static void console_thread_main( uint32_t arg )
{
    char line[CONSOLE_LINE_MAX];
    char* argv[CONSOLE_ARG_MAX];
    int argc;

    (void)arg;

    printf( "\nconsole ready; type help\n" );
    console_prompt( );

    while ( 1 )
    {
        if ( console_read_line( line, sizeof( line ) ) == 0 )
        {
            continue;
        }

        argc = console_tokenize( line, argv, CONSOLE_ARG_MAX );
        if ( argc < 0 )
        {
            printf( "too many args\n" );
        }
        else
        {
            console_handle_command( argc, argv );
        }
        console_prompt( );
    }
}

static int from_hex( char c, uint8_t* value )
{
    if ( c >= '0' && c <= '9' )
    {
        *value = (uint8_t)( c - '0' );
        return 1;
    }
    if ( c >= 'a' && c <= 'f' )
    {
        *value = (uint8_t)( c - 'a' + 10 );
        return 1;
    }
    if ( c >= 'A' && c <= 'F' )
    {
        *value = (uint8_t)( c - 'A' + 10 );
        return 1;
    }
    return 0;
}

static char hex_nibble( uint8_t value )
{
    value &= 0xfu;
    return (char)( value < 10u ? ( '0' + value ) : ( 'A' + value - 10u ) );
}

static uint32_t decode_hex_len( const uint8_t* text, int* ok )
{
    uint32_t value = 0u;
    uint32_t i;
    *ok = 1;
    for ( i = 0; i < 8u; i++ )
    {
        uint8_t nibble = 0u;
        if ( from_hex( (char)text[i], &nibble ) == 0 )
        {
            *ok = 0;
            return 0u;
        }
        value = ( value << 4 ) | nibble;
    }
    return value;
}

static void frame_len_hex( uint32_t value, char out[9] )
{
    int shift;
    for ( shift = 28; shift >= 0; shift -= 4 )
    {
        *out++ = hex_nibble( (uint8_t)( value >> (uint32_t)shift ) );
    }
    *out = '\0';
}

static void sensor_frame_reset( sensor_rx_t* rx )
{
    rx->state = 0u;
    rx->pos = 0u;
    rx->length = 0u;
}

static const char* payload_next_field( const uint8_t* payload, uint32_t length, uint32_t* offset )
{
    uint32_t start = *offset;
    if ( start >= length )
    {
        return NULL;
    }

    while ( *offset < length && payload[*offset] != 0u )
    {
        ( *offset )++;
    }
    if ( *offset >= length )
    {
        return NULL;
    }
    ( *offset )++;
    return (const char*)&payload[start];
}

static int parse_fixed_centi( const char* text, int32_t* out )
{
    int negative = 0;
    uint32_t whole = 0u;
    uint32_t frac = 0u;
    uint32_t frac_digits = 0u;
    int saw_digit = 0;
    int32_t value;

    if ( *text == '-' )
    {
        negative = 1;
        text++;
    }
    else if ( *text == '+' )
    {
        text++;
    }

    while ( *text >= '0' && *text <= '9' )
    {
        saw_digit = 1;
        whole = ( whole * 10u ) + (uint32_t)( *text - '0' );
        text++;
    }

    if ( *text == '.' )
    {
        text++;
        while ( *text >= '0' && *text <= '9' )
        {
            if ( frac_digits < 2u )
            {
                frac = ( frac * 10u ) + (uint32_t)( *text - '0' );
                frac_digits++;
            }
            else if ( frac_digits == 2u && *text >= '5' )
            {
                frac++;
                frac_digits++;
            }
            saw_digit = 1;
            text++;
        }
    }

    if ( saw_digit == 0 || *text != '\0' )
    {
        return 0;
    }

    while ( frac_digits < 2u )
    {
        frac *= 10u;
        frac_digits++;
    }
    if ( frac >= 100u )
    {
        whole++;
        frac -= 100u;
    }

    value = (int32_t)( ( whole * 100u ) + frac );
    if ( negative != 0 )
    {
        value = -value;
    }
    *out = value;
    return 1;
}

static int32_t centi_to_int( int32_t centi )
{
    if ( centi >= 0 )
    {
        return ( centi + 50 ) / 100;
    }
    return ( centi - 50 ) / 100;
}

static uint32_t index_outside_range( int32_t value, int32_t low, int32_t high, int32_t step )
{
    int32_t delta = 0;
    uint32_t index = 0u;
    if ( value < low )
    {
        delta = low - value;
    }
    else if ( value > high )
    {
        delta = value - high;
    }

    if ( delta > 0 )
    {
        index = (uint32_t)( ( delta + step - 1 ) / step );
    }
    return index > 4u ? 4u : index;
}

static uint32_t index_above( int32_t value, int32_t good_max, int32_t step )
{
    uint32_t index = 0u;
    if ( value > good_max )
    {
        index = (uint32_t)( ( value - good_max + step - 1 ) / step );
    }
    return index > 4u ? 4u : index;
}

static uint32_t penalty_outside_range( int32_t value, int32_t low, int32_t high,
                                       int32_t full_bad_delta, uint32_t max_penalty )
{
    int32_t delta = 0;
    if ( value < low )
    {
        delta = low - value;
    }
    else if ( value > high )
    {
        delta = value - high;
    }

    if ( delta <= 0 )
    {
        return 0u;
    }
    if ( delta >= full_bad_delta )
    {
        return max_penalty;
    }
    return (uint32_t)( ( (uint32_t)delta * max_penalty + ( (uint32_t)full_bad_delta / 2u ) ) /
                       (uint32_t)full_bad_delta );
}

static uint32_t penalty_above( int32_t value, int32_t good_max, int32_t full_bad_delta,
                               uint32_t max_penalty )
{
    int32_t delta;
    if ( value <= good_max )
    {
        return 0u;
    }

    delta = value - good_max;
    if ( delta >= full_bad_delta )
    {
        return max_penalty;
    }
    return (uint32_t)( ( (uint32_t)delta * max_penalty + ( (uint32_t)full_bad_delta / 2u ) ) /
                       (uint32_t)full_bad_delta );
}

static void int32_to_cstr( int32_t value, char* out, uint32_t out_len )
{
    char tmp[11];
    uint32_t pos = 0u;
    uint32_t uvalue;

    if ( out_len == 0u )
    {
        return;
    }

    if ( value < 0 )
    {
        *out++ = '-';
        out_len--;
        uvalue = (uint32_t)( -( value + 1 ) ) + 1u;
    }
    else
    {
        uvalue = (uint32_t)value;
    }

    if ( out_len == 0u )
    {
        return;
    }

    if ( uvalue == 0u )
    {
        *out++ = '0';
        if ( out_len > 1u )
        {
            *out = '\0';
        }
        return;
    }

    while ( uvalue != 0u && pos < sizeof( tmp ) )
    {
        tmp[pos++] = (char)( '0' + ( uvalue % 10u ) );
        uvalue /= 10u;
    }

    while ( pos != 0u && out_len > 1u )
    {
        *out++ = tmp[--pos];
        out_len--;
    }
    *out = '\0';
}

typedef struct
{
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} wall_time_t;

static int date_is_leap( uint32_t year )
{
    return ( ( year % 4u ) == 0u && ( year % 100u ) != 0u ) || ( year % 400u ) == 0u;
}

static uint32_t date_days_in_month( uint32_t year, uint32_t month )
{
    static const uint8_t days[] = { 31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u };
    if ( month == 2u && date_is_leap( year ) != 0 )
    {
        return 29u;
    }
    return days[month - 1u];
}

static void epoch_utc_to_wall( uint32_t epoch_seconds, wall_time_t* out )
{
    uint32_t days = epoch_seconds / 86400u;
    uint32_t seconds = epoch_seconds % 86400u;
    uint32_t year = 1970u;
    uint32_t month = 1u;

    while ( 1 )
    {
        uint32_t year_days = date_is_leap( year ) != 0 ? 366u : 365u;
        if ( days < year_days )
        {
            break;
        }
        days -= year_days;
        year++;
    }

    while ( 1 )
    {
        uint32_t month_days = date_days_in_month( year, month );
        if ( days < month_days )
        {
            break;
        }
        days -= month_days;
        month++;
    }

    out->year = year;
    out->month = month;
    out->day = days + 1u;
    out->hour = seconds / 3600u;
    seconds %= 3600u;
    out->minute = seconds / 60u;
    out->second = seconds % 60u;
}

static void write_dec2( char* out, uint32_t value )
{
    out[0] = (char)( '0' + ( ( value / 10u ) % 10u ) );
    out[1] = (char)( '0' + ( value % 10u ) );
}

static void write_dec4( char* out, uint32_t value )
{
    out[0] = (char)( '0' + ( ( value / 1000u ) % 10u ) );
    out[1] = (char)( '0' + ( ( value / 100u ) % 10u ) );
    out[2] = (char)( '0' + ( ( value / 10u ) % 10u ) );
    out[3] = (char)( '0' + ( value % 10u ) );
}

static void wall_time_to_compact( const wall_time_t* wall, char out[15] )
{
    write_dec4( &out[0], wall->year );
    write_dec2( &out[4], wall->month );
    write_dec2( &out[6], wall->day );
    write_dec2( &out[8], wall->hour );
    write_dec2( &out[10], wall->minute );
    write_dec2( &out[12], wall->second );
    out[14] = '\0';
}

static void compact_utc_time_with_offset( uint32_t utc_seconds, int32_t offset_seconds, char out[15] )
{
    wall_time_t wall;
    epoch_utc_to_wall( utc_seconds + (uint32_t)offset_seconds, &wall );
    wall_time_to_compact( &wall, out );
}

static uint32_t fields_payload_len( char** fields, uint32_t count )
{
    uint32_t length = 0u;
    uint32_t i;
    for ( i = 0; i < count; i++ )
    {
        length += cstr_len( fields[i] ) + 1u;
    }
    return length;
}

static void frame_append( uint8_t* frame, uint32_t* frame_len, const void* data, uint32_t length )
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t i;
    for ( i = 0u; i < length; i++ )
    {
        frame[( *frame_len )++] = bytes[i];
    }
}

static void sensor_uart_send_frame_bytes( const uint8_t* frame, uint32_t frame_len )
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

static void sensor_send_frame( const char cmd[4], char** fields, uint32_t field_count )
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

static void send_netw_up( void )
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

static void send_tinf_from_rule( const rewair_tz_rule_t* rule, uint32_t year )
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

static void send_time_from_rule( const rewair_tz_rule_t* rule, uint32_t utc_seconds )
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

static void send_time_context( uint32_t utc_seconds )
{
    wall_time_t utc_wall;

    epoch_utc_to_wall( utc_seconds, &utc_wall );
    send_tinf_from_rule( &current_tz_rule, utc_wall.year );
    wiced_rtos_delay_milliseconds( 20u );
    send_time_from_rule( &current_tz_rule, utc_seconds );
}

static void send_disp_clock_canary( void )
{
    char* fields[] =
    {
        "mode", "clock",
    };

    sensor_send_frame( "DISP", fields, (uint32_t)( sizeof( fields ) / sizeof( fields[0] ) ) );
    printf( "[nudge] DISP clock\n" );
}

static void send_sensor_boot_context( void )
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

static wiced_result_t network_sync_time_once( uint32_t* utc_seconds_out )
{
    wiced_ip_address_t ntp_server_ip;
    ntp_timestamp_t timestamp;
    wiced_result_t result = WICED_ERROR;
    uint32_t attempt;

    for ( attempt = 0u; attempt < NTP_RETRY_COUNT; attempt++ )
    {
        result = wiced_hostname_lookup( "pool.ntp.org", &ntp_server_ip, 5000u );
        if ( result != WICED_SUCCESS )
        {
            printf( "[time] NTP DNS failed attempt=%lu result=%d (%s)\n",
                    (unsigned long)( attempt + 1u ), (int)result, wifi_result_name( result ) );
        }
        else
        {
            memset( &timestamp, 0, sizeof( timestamp ) );
            result = sntp_get_time( &ntp_server_ip, &timestamp );
            if ( result == WICED_SUCCESS )
            {
                wiced_utc_time_ms_t utc_time_ms =
                    ( (uint64_t)timestamp.seconds * (uint64_t)1000u ) +
                    (uint64_t)( timestamp.microseconds / 1000u );

                wiced_time_set_utc_time_ms( &utc_time_ms );
                *utc_seconds_out = timestamp.seconds;
                printf( "[time] NTP synced utc=%lu\n", (unsigned long)timestamp.seconds );
                return WICED_SUCCESS;
            }

            printf( "[time] NTP query failed attempt=%lu result=%d (%s)\n",
                    (unsigned long)( attempt + 1u ), (int)result, wifi_result_name( result ) );
        }

        wiced_rtos_delay_milliseconds( 1000u );
    }

    return result;
}

static void network_after_ip_ready( void )
{
    wiced_time_t now_ms = 0u;
    uint32_t utc_seconds = 0u;

    if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS )
    {
        wifi_network_ready_ms = (uint32_t)now_ms;
    }

    send_netw_up( );
    wiced_rtos_delay_milliseconds( 20u );

    {
        wiced_ip_address_t addr;
        wiced_mac_t mac;
        int32_t rssi = 0;
        char ip_buf[16] = "0.0.0.0";
        char gw_buf[16] = "0.0.0.0";
        char dns_buf[16] = "0.0.0.0";
        char mac_buf[18] = "00:00:00:00:00:00";
        wiced_config_ap_entry_t* ap = NULL;
        char ssid_buf[33] = "";

        if ( wiced_ip_get_ipv4_address( WICED_STA_INTERFACE, &addr ) == WICED_SUCCESS )
        {
            ipv4_to_cstr( &addr, ip_buf );
        }
        if ( wiced_ip_get_gateway_address( WICED_STA_INTERFACE, &addr ) == WICED_SUCCESS )
        {
            ipv4_to_cstr( &addr, gw_buf );
            /* LwIP DNS getter not exposed in this SDK; gateway is the typical
             * home-router DNS relay, so report it as DNS too. */
            ipv4_to_cstr( &addr, dns_buf );
        }
        if ( wiced_wifi_get_mac_address( &mac ) == WICED_SUCCESS )
        {
            mac_to_cstr( &mac, mac_buf );
        }
        wwd_wifi_get_rssi( &rssi );
        if ( wiced_dct_read_lock( (void**)&ap, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                                  OFFSETOF( platform_dct_wifi_config_t, stored_ap_list ),
                                  sizeof( *ap ) ) == WICED_SUCCESS )
        {
            uint32_t n = ap->details.SSID.length <= 32u ? ap->details.SSID.length : 32u;
            memcpy( ssid_buf, ap->details.SSID.value, n );
            ssid_buf[n] = '\0';
            wiced_dct_read_unlock( ap, WICED_FALSE );
        }
        rewair_state_set_wifi_sta( ssid_buf, rssi, ip_buf, gw_buf, dns_buf, mac_buf,
                                   wifi_dct_saved_count( ) );
    }

    rewair_web_api_start( WICED_STA_INTERFACE );

    if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS )
    {
        if ( wifi_time_synced != 0u &&
             (uint32_t)( now_ms - wifi_last_ntp_sync_ms ) < NTP_SYNC_PERIOD_MS )
        {
            return;
        }
    }

    if ( network_sync_time_once( &utc_seconds ) == WICED_SUCCESS )
    {
        wifi_time_synced = 1u;
        if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS )
        {
            wifi_last_ntp_sync_ms = (uint32_t)now_ms;
        }
        send_time_context( utc_seconds );
        rewair_state_set_time( utc_seconds, 1u );
    }
}

static void sens_parse_pair( sens_values_t* sens, const char* key, const char* value )
{
    int32_t parsed = 0;
    if ( parse_fixed_centi( value, &parsed ) == 0 )
    {
        return;
    }

    if ( cstr_eq( key, "temp" ) )
    {
        sens->temp = parsed;
        sens->seen |= SENS_TEMP;
    }
    else if ( cstr_eq( key, "humid" ) )
    {
        sens->humid = parsed;
        sens->seen |= SENS_HUMID;
    }
    else if ( cstr_eq( key, "co2" ) )
    {
        sens->co2 = parsed;
        sens->seen |= SENS_CO2;
    }
    else if ( cstr_eq( key, "voc" ) )
    {
        sens->voc = parsed;
        sens->seen |= SENS_VOC;
    }
    else if ( cstr_eq( key, "dust" ) )
    {
        sens->dust = parsed;
        sens->seen |= SENS_DUST;
    }
    else if ( cstr_eq( key, "light" ) )
    {
        sens->light = parsed;
        sens->seen |= SENS_LIGHT;
    }
}

static char* score_color( uint32_t score )
{
    static char green[] = "green";
    static char amber[] = "amber";
    static char purple[] = "purple";

    if ( score >= 80u )
    {
        return green;
    }
    if ( score >= 60u )
    {
        return amber;
    }
    return purple;
}

typedef struct
{
    uint32_t score;
    uint8_t  idx[5];   /* temp, humid, co2, voc, dust */
    char*    color;
} sens_score_t;

static void sens_compute_score( const sens_values_t* sens, sens_score_t* out )
{
    uint32_t penalty;

    out->idx[0] = (uint8_t)index_outside_range( sens->temp, 1800, 2600, 200 );
    out->idx[1] = (uint8_t)index_outside_range( sens->humid, 3000, 6000, 1000 );
    out->idx[2] = (uint8_t)index_above( sens->co2, 100000, 40000 );
    out->idx[3] = (uint8_t)index_above( sens->voc, 33300, 33300 );
    out->idx[4] = (uint8_t)index_above( sens->dust, 1200, 1200 );

    penalty  = penalty_outside_range( sens->temp, 1800, 2600, 1200, 8u );
    penalty += penalty_outside_range( sens->humid, 3000, 6000, 4000, 8u );
    penalty += penalty_above( sens->co2, 100000, 80000, 25u );
    penalty += penalty_above( sens->voc, 33300, 100000, 25u );
    penalty += penalty_above( sens->dust, 1200, 6000, 34u );

    out->score = penalty < 100u ? 100u - penalty : 0u;
    out->color = score_color( out->score );
}

static void send_scor_from_sens( const sens_values_t* sens )
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

static void maybe_auto_score_sens( const sensor_rx_t* rx, uint8_t trailer )
{
    sens_values_t sens = { 0u, 0, 0, 0, 0, 0, 0 };
    uint32_t offset = 0u;
    const char* key;
    wiced_time_t now_ms;

    if ( auto_score_enabled == 0u || trailer != '#' || cmd4_eq( rx->cmd, "SENS" ) == 0 )
    {
        return;
    }

    sensor_sens_seen = 1u;
    sensor_sens_count++;
    if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS )
    {
        sensor_last_sens_ms = (uint32_t)now_ms;
    }

    key = payload_next_field( rx->payload, rx->length, &offset );
    while ( key != NULL )
    {
        const char* value = payload_next_field( rx->payload, rx->length, &offset );
        if ( value == NULL )
        {
            break;
        }
        sens_parse_pair( &sens, key, value );
        key = payload_next_field( rx->payload, rx->length, &offset );
    }

    if ( ( sens.seen & SENS_ALL ) != SENS_ALL )
    {
        printf( "[auto] incomplete SENS mask=0x%08lx\n", sens.seen );
        return;
    }

    send_scor_from_sens( &sens );
}

static void handle_sensor_frame( const sensor_rx_t* rx, uint8_t trailer )
{
    sensor_last_frame_cmd[0] = rx->cmd[0];
    sensor_last_frame_cmd[1] = rx->cmd[1];
    sensor_last_frame_cmd[2] = rx->cmd[2];
    sensor_last_frame_cmd[3] = rx->cmd[3];
    sensor_last_frame_cmd[4] = '\0';
    sensor_last_frame_trailer = trailer;

    if ( trailer == '#' )
    {
        if ( cmd4_eq( rx->cmd, "REDY" ) != 0 )
        {
            printf( "[boot] sensor REDY; sending boot context\n" );
            send_sensor_boot_context( );
        }
        else if ( cmd4_eq( rx->cmd, "RUNN" ) != 0 )
        {
            printf( "[boot] sensor RUNN\n" );
        }
        else if ( cmd4_eq( rx->cmd, "BTST" ) != 0 )
        {
            printf( "[boot] sensor BTST\n" );
        }
        else if ( cmd4_eq( rx->cmd, "TEST" ) != 0 )
        {
            printf( "[boot] sensor TEST; no reply, matching stock handler\n" );
        }
        else if ( cmd4_eq( rx->cmd, "NETD" ) != 0 )
        {
            printf( "[boot] sensor NETD; sending NETW up probe\n" );
            send_netw_up( );
        }
        else if ( cmd4_eq( rx->cmd, "NETR" ) != 0 )
        {
            printf( "[boot] sensor NETR; sending NETW up probe\n" );
            send_netw_up( );
        }
    }

    maybe_auto_score_sens( rx, trailer );
}

static uint32_t payload_argc( const uint8_t* payload, uint32_t length )
{
    uint32_t argc = 0u;
    uint32_t start = 0u;
    while ( start < length )
    {
        argc++;
        while ( start < length && payload[start] != 0u )
        {
            start++;
        }
        start++;
    }
    return argc;
}

static void print_sensor_frame( const sensor_rx_t* rx, uint8_t trailer )
{
    printf( "[rx] %.4s len=%lu argc=%lu trailer=0x%02x\n",
            rx->cmd, rx->length, payload_argc( rx->payload, rx->length ), trailer );
}

static void sensor_rx_byte( sensor_rx_t* rx, uint8_t value )
{
    int ok = 0;
    uint32_t i;

    switch ( rx->state )
    {
        case 0:
            if ( value == '*' )
            {
                rx->state = 1u;
                rx->pos = 0u;
            }
            break;

        case 1:
            rx->header[rx->pos++] = value;
            if ( rx->pos == sizeof( rx->header ) )
            {
                for ( i = 0; i < 4u; i++ )
                {
                    rx->cmd[i] = (char)rx->header[i];
                }
                rx->length = decode_hex_len( &rx->header[4], &ok );
                if ( ok == 0 || rx->length > SENSOR_PAYLOAD_MAX )
                {
                    printf( "[rx bad header] %.4s len=0x%08lx\n", rx->cmd, rx->length );
                    sensor_frame_reset( rx );
                }
                else if ( rx->length == 0u )
                {
                    rx->state = 3u;
                    rx->pos = 0u;
                }
                else
                {
                    rx->state = 2u;
                    rx->pos = 0u;
                }
            }
            break;

        case 2:
            rx->payload[rx->pos++] = value;
            if ( rx->pos == rx->length )
            {
                rx->state = 3u;
                rx->pos = 0u;
            }
            break;

        case 3:
            handle_sensor_frame( rx, value );
            print_sensor_frame( rx, value );
            sensor_frame_reset( rx );
            break;

        default:
            sensor_frame_reset( rx );
            break;
    }
}

static wiced_result_t sensor_uart_start( void )
{
    if ( wiced_rtos_init_mutex( &sensor_uart_tx_mutex ) != WICED_SUCCESS )
    {
        printf( "sensor uart tx mutex init failed\n" );
        return WICED_ERROR;
    }

    ring_buffer_init( &sensor_uart_rx_buffer, sensor_uart_rx_data, sizeof( sensor_uart_rx_data ) );
    if ( wiced_uart_init( WICED_UART_2, &sensor_uart_config, &sensor_uart_rx_buffer ) != WICED_SUCCESS )
    {
        printf( "sensor uart init failed\n" );
        return WICED_ERROR;
    }
    sensor_frame_reset( &sensor_rx );
    return WICED_SUCCESS;
}

static void sensor_uart_note_status( uint32_t sr )
{
    uint32_t error_bits = sr & SENSOR_UART_ERROR_MASK;

    if ( error_bits != 0u )
    {
        uint32_t should_report = 0u;

        sensor_uart_error_flags |= error_bits;
        sensor_uart_error_count++;
        if ( sensor_uart_error_report_count < 4u ||
             ( error_bits & ~sensor_uart_error_reported_flags ) != 0u ||
             ( sensor_uart_error_count % 1000u ) == 0u )
        {
            should_report = 1u;
        }

        if ( should_report != 0u )
        {
            wiced_time_t now_ms = 0u;
            uint32_t gpioa_idr = GPIOA->IDR;
            uint32_t gpiob_idr = GPIOB->IDR;

            (void)wiced_time_get_time( &now_ms );
            printf( "[uart-err] tick=%lu sr=0x%08lx bits=%c%c%c%c count=%lu "
                    "rx=%lu last_rx=0x%02lx last_rx_sr=0x%08lx state=%lu pos=%lu "
                    "pa3=%lu pb12=%lu\n",
                    (unsigned long)now_ms,
                    (unsigned long)sr,
                    ( sr & USART_SR_PE ) != 0u ? 'P' : '-',
                    ( sr & USART_SR_FE ) != 0u ? 'F' : '-',
                    ( sr & USART_SR_NE ) != 0u ? 'N' : '-',
                    ( sr & USART_SR_ORE ) != 0u ? 'O' : '-',
                    (unsigned long)sensor_uart_error_count,
                    (unsigned long)sensor_uart_rx_bytes,
                    (unsigned long)( sensor_uart_last_rx_byte & 0xffu ),
                    (unsigned long)sensor_uart_last_rx_sr,
                    (unsigned long)sensor_rx.state,
                    (unsigned long)sensor_rx.pos,
                    ( gpioa_idr >> 3 ) & 1u,
                    ( gpiob_idr >> 12 ) & 1u );
            sensor_uart_error_report_count++;
            sensor_uart_error_reported_flags |= error_bits;
        }
    }
}

static void sensor_uart_clear_latched_error( uint32_t status )
{
    if ( ( status & SENSOR_UART_ERROR_MASK ) != 0u )
    {
        uint32_t clear_sr = USART2->SR;
        uint32_t clear_dr = USART2->DR;
        uint32_t after_sr = USART2->SR;

        sensor_uart_error_clear_count++;
        sensor_uart_last_clear_sr = clear_sr;
        sensor_uart_last_clear_dr = clear_dr;
        sensor_uart_last_clear_after_sr = after_sr;

        printf( "[uart-clear] count=%lu sr=0x%08lx dr=0x%02lx after=0x%08lx bits=%c%c%c%c\n",
                (unsigned long)sensor_uart_error_clear_count,
                (unsigned long)clear_sr,
                (unsigned long)( clear_dr & 0xffu ),
                (unsigned long)after_sr,
                ( after_sr & USART_SR_PE ) != 0u ? 'P' : '-',
                ( after_sr & USART_SR_FE ) != 0u ? 'F' : '-',
                ( after_sr & USART_SR_NE ) != 0u ? 'N' : '-',
                ( after_sr & USART_SR_ORE ) != 0u ? 'O' : '-' );
    }
}

static void sensor_uart_diag( uint32_t status )
{
    uint32_t gpioa_idr = GPIOA->IDR;
    uint32_t gpiob_idr = GPIOB->IDR;

    printf( "[diag] usart2_sr=0x%08lx sr_bits=%c%c%c%c pa3=%lu pb12=%lu "
            "rx=%lu err=0x%08lx/%lu err_bits=%c%c%c%c\n",
            status,
            ( status & USART_SR_PE ) != 0u ? 'P' : '-',
            ( status & USART_SR_FE ) != 0u ? 'F' : '-',
            ( status & USART_SR_NE ) != 0u ? 'N' : '-',
            ( status & USART_SR_ORE ) != 0u ? 'O' : '-',
            ( gpioa_idr >> 3 ) & 1u,
            ( gpiob_idr >> 12 ) & 1u,
            sensor_uart_rx_bytes,
            sensor_uart_error_flags,
            sensor_uart_error_count,
            ( sensor_uart_error_flags & USART_SR_PE ) != 0u ? 'P' : '-',
            ( sensor_uart_error_flags & USART_SR_FE ) != 0u ? 'F' : '-',
            ( sensor_uart_error_flags & USART_SR_NE ) != 0u ? 'N' : '-',
            ( sensor_uart_error_flags & USART_SR_ORE ) != 0u ? 'O' : '-' );

    if ( sensor_raw_trace_reported != sensor_raw_trace_count )
    {
        uint32_t i;
        uint32_t count = sensor_raw_trace_count;
        printf( "[raw]" );
        for ( i = 0u; i < count; i++ )
        {
            printf( " %02x", sensor_raw_trace[i] );
        }
        printf( " |" );
        for ( i = 0u; i < count; i++ )
        {
            uint8_t c = sensor_raw_trace[i];
            printf( "%c", ( c >= 0x20u && c <= 0x7eu ) ? c : '.' );
        }
        printf( "\n" );
        sensor_raw_trace_reported = count;
    }
}

static void sensor_uart_stat( uint32_t status, uint32_t now_ms )
{
    uint32_t last_rx_age = sensor_last_rx_ms == 0u ? 0xffffffffu : (uint32_t)( now_ms - sensor_last_rx_ms );
    uint32_t last_sens_age = sensor_last_sens_ms == 0u ? 0xffffffffu : (uint32_t)( now_ms - sensor_last_sens_ms );

    printf( "[stat] tick=%lu rx=%lu sens=%lu last_rx_ms=%lu last_sens_ms=%lu "
            "state=%lu pos=%lu err=0x%08lx/%lu err_bits=%c%c%c%c "
            "clr=%lu/0x%08lx/0x%02lx/0x%08lx "
            "tx=%lu fail=%lu drop=%lu last_tx=%lu "
            "tx_sr=0x%08lx->0x%08lx netw=%lu nudge=%lu disp=%lu last=%.4s/0x%02lx "
            "sr=0x%08lx sr_bits=%c%c%c%c\n",
            (unsigned long)now_ms,
            (unsigned long)sensor_uart_rx_bytes,
            (unsigned long)sensor_sens_count,
            (unsigned long)last_rx_age,
            (unsigned long)last_sens_age,
            (unsigned long)sensor_rx.state,
            (unsigned long)sensor_rx.pos,
            (unsigned long)sensor_uart_error_flags,
            (unsigned long)sensor_uart_error_count,
            ( sensor_uart_error_flags & USART_SR_PE ) != 0u ? 'P' : '-',
            ( sensor_uart_error_flags & USART_SR_FE ) != 0u ? 'F' : '-',
            ( sensor_uart_error_flags & USART_SR_NE ) != 0u ? 'N' : '-',
            ( sensor_uart_error_flags & USART_SR_ORE ) != 0u ? 'O' : '-',
            (unsigned long)sensor_uart_error_clear_count,
            (unsigned long)sensor_uart_last_clear_sr,
            (unsigned long)( sensor_uart_last_clear_dr & 0xffu ),
            (unsigned long)sensor_uart_last_clear_after_sr,
            (unsigned long)sensor_uart_tx_count,
            (unsigned long)sensor_uart_wiced_tx_fail_count,
            (unsigned long)sensor_uart_tx_drop_count,
            (unsigned long)sensor_uart_wiced_tx_result,
            (unsigned long)sensor_uart_tx_sr_before,
            (unsigned long)sensor_uart_tx_sr_after,
            (unsigned long)sensor_netw_boot_pulses,
            (unsigned long)sensor_netw_nudge_count,
            (unsigned long)sensor_disp_clock_canary_sent,
            sensor_last_frame_cmd,
            (unsigned long)sensor_last_frame_trailer,
            (unsigned long)status,
            ( status & USART_SR_PE ) != 0u ? 'P' : '-',
            ( status & USART_SR_FE ) != 0u ? 'F' : '-',
            ( status & USART_SR_NE ) != 0u ? 'N' : '-',
            ( status & USART_SR_ORE ) != 0u ? 'O' : '-' );
}

static void sensor_maybe_nudge_boot( uint32_t now_ms )
{
    uint32_t ready_ms = wifi_network_ready_ms;
    uint32_t age_ms;

    if ( sensor_reset_released == 0u || sensor_sens_seen != 0u || ready_ms == 0u )
    {
        return;
    }

    age_ms = (uint32_t)( now_ms - ready_ms );
    if ( age_ms < SENSOR_NUDGE_START_MS )
    {
        return;
    }

    if ( sensor_disp_clock_canary_sent == 0u )
    {
        sensor_disp_clock_canary_sent = 1u;
        sensor_last_nudge_ms = now_ms;
        send_disp_clock_canary( );
        return;
    }

    if ( sensor_netw_nudge_count >= SENSOR_NETW_NUDGE_MAX )
    {
        return;
    }

    if ( sensor_last_nudge_ms == 0u ||
         (uint32_t)( now_ms - sensor_last_nudge_ms ) >= SENSOR_NETW_NUDGE_MS )
    {
        sensor_netw_nudge_count++;
        sensor_last_nudge_ms = now_ms;
        printf( "[nudge] NETW retry %lu/%lu\n",
                (unsigned long)sensor_netw_nudge_count,
                (unsigned long)SENSOR_NETW_NUDGE_MAX );
        send_netw_up( );
    }
}

static void sensor_thread_main( uint32_t arg )
{
    uint8_t value;
    uint32_t last_diag_ms = 0u;
    uint32_t last_stat_ms = 0u;
    uint32_t diag_count = 0u;
    uint32_t status = 0u;
    wiced_time_t now_ms;

    (void)arg;

    while ( 1 )
    {
        if ( wiced_uart_receive_bytes( WICED_UART_2, &value, 1u, 10u ) == WICED_SUCCESS )
        {
            uint32_t trace_count = sensor_raw_trace_count;
            status = USART2->SR;
            sensor_uart_last_rx_byte = value;
            sensor_uart_last_rx_sr = status;
            sensor_uart_note_status( status );
            if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS )
            {
                sensor_last_rx_ms = (uint32_t)now_ms;
            }
            if ( trace_count < SENSOR_RAW_TRACE_MAX )
            {
                sensor_raw_trace[trace_count] = value;
                sensor_raw_trace_count = trace_count + 1u;
            }
            sensor_uart_rx_bytes++;
            sensor_rx_byte( &sensor_rx, value );
        }
        else
        {
            status = USART2->SR;
            sensor_uart_note_status( status );
            sensor_uart_clear_latched_error( status );
        }

        if ( sensor_reset_released != 0u && sensor_sens_seen == 0u && diag_count < 12u )
        {
            if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS &&
                 ( last_diag_ms == 0u || (uint32_t)( now_ms - last_diag_ms ) >= SENSOR_DIAG_REPEAT_MS ) )
            {
                sensor_uart_diag( status );
                diag_count++;
                last_diag_ms = now_ms;
            }
        }

        if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS &&
             ( last_stat_ms == 0u || (uint32_t)( now_ms - last_stat_ms ) >= SENSOR_STAT_REPEAT_MS ) )
        {
            sensor_uart_stat( status, (uint32_t)now_ms );
            last_stat_ms = (uint32_t)now_ms;
        }

        if ( wiced_time_get_time( &now_ms ) == WICED_SUCCESS )
        {
            sensor_maybe_nudge_boot( (uint32_t)now_ms );
        }
    }
}

static void sensor_reset_release( void )
{
    sensor_boot_context_sent = 0u;
    sensor_sens_seen = 0u;
    sensor_netw_boot_pulses = 0u;
    sensor_netw_nudge_count = 0u;
    sensor_last_nudge_ms = 0u;
    sensor_disp_clock_canary_sent = 0u;
    sensor_raw_trace_count = 0u;
    sensor_raw_trace_reported = 0u;
    sensor_frame_reset( &sensor_rx );

    wiced_gpio_init( AWAIR_SENSOR_RESET, OUTPUT_PUSH_PULL );
    wiced_gpio_output_high( AWAIR_SENSOR_RESET );
    sensor_reset_released = 1u;
    printf( "[boot] sensor reset line released on PB12\n" );
}

static void sensor_reset_cycle( void )
{
    sensor_boot_context_sent = 0u;
    sensor_sens_seen = 0u;
    sensor_netw_boot_pulses = 0u;
    sensor_netw_nudge_count = 0u;
    sensor_last_nudge_ms = 0u;
    sensor_disp_clock_canary_sent = 0u;
    sensor_raw_trace_count = 0u;
    sensor_raw_trace_reported = 0u;
    sensor_frame_reset( &sensor_rx );

    wiced_gpio_init( AWAIR_SENSOR_RESET, OUTPUT_PUSH_PULL );
    wiced_gpio_output_low( AWAIR_SENSOR_RESET );
    wiced_rtos_delay_milliseconds( 10u );
    wiced_gpio_output_high( AWAIR_SENSOR_RESET );
    sensor_reset_released = 1u;
    printf( "[boot] sensor reset pulse on PB12\n" );
}

static void sensor_reset_release_early( void )
{
    RCC->AHB1ENR |= RCC_AHB1Periph_GPIOB;
    (void)RCC->AHB1ENR;

    GPIOB->ODR |= ( 1u << 12 );
    GPIOB->MODER = ( GPIOB->MODER & ~( 3u << 24 ) ) | ( 1u << 24 );
    GPIOB->OTYPER &= ~( 1u << 12 );
    GPIOB->OSPEEDR = ( GPIOB->OSPEEDR & ~( 3u << 24 ) ) | ( 2u << 24 );
    GPIOB->PUPDR &= ~( 3u << 24 );
}

static void network_thread_main( uint32_t arg )
{
    wiced_result_t result;

    (void)arg;

    wiced_rtos_delay_milliseconds( 1000u );

    while ( 1 )
    {
        if ( wifi_join_in_progress == 0u )
        {
            if ( wiced_network_is_up( WICED_STA_INTERFACE ) == WICED_TRUE )
            {
                wifi_link_was_up = 1u;
                network_after_ip_ready( );
            }
            else if ( wifi_dct_has_stored_ap( ) != 0 )
            {
                if ( wifi_link_was_up != 0u )
                {
                    printf( "[wifi] link dropped; will rejoin from DCT\n" );
                    rewair_state_wifi_drop( );
                    wifi_link_was_up = 0u;
                }
                printf( "[wifi] autojoin from DCT\n" );
                result = wiced_network_up( WICED_STA_INTERFACE, WICED_USE_EXTERNAL_DHCP_SERVER, NULL );
                if ( result == WICED_SUCCESS )
                {
                    printf( "[wifi] autojoin ready\n" );
                    wifi_print_status( );
                    wifi_link_was_up = 1u;
                    network_after_ip_ready( );
                }
                else
                {
                    printf( "[wifi] autojoin failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
                    wiced_network_down( WICED_STA_INTERFACE );
                    wiced_leave_ap( WICED_STA_INTERFACE );
                }
            }
        }

        wiced_rtos_delay_milliseconds( NETWORK_RETRY_MS );
    }
}

void application_start( void )
{
    wiced_result_t result;

    sensor_reset_release_early( );

    WPRINT_WICED_INFO( ( "\nStarting WICED v" WICED_VERSION "\n" ) );
    result = wiced_core_init( );
    if ( result != WICED_SUCCESS )
    {
        return;
    }

    rewair_state_init( );

    {
        rewair_tz_rule_t rule;

        rewair_settings_load( &current_settings );
        if ( rewair_tz_parse( current_settings.tz_posix, &rule ) == 0 )
        {
            sensor_set_tz_rule( &rule );
        }
        rewair_settings_apply_to_state( &current_settings );
    }

    setvbuf( stdin, NULL, _IONBF, 0 );
    setvbuf( stdout, NULL, _IONBF, 0 );
    setvbuf( stderr, NULL, _IONBF, 0 );

    printf( "\nRewair WICED local bridge\n" );
    printf( "console: USART1 PB6 TX / PA10 RX, 115200 8N1\n" );
    printf( "sensor:  USART2 PA2 TX / PA3 RX, 115200 8N1\n" );

    if ( sensor_uart_start( ) != WICED_SUCCESS )
    {
        return;
    }

    if ( wiced_rtos_create_thread( &sensor_thread, WICED_DEFAULT_LIBRARY_PRIORITY, "sensor uart",
                                   sensor_thread_main, SENSOR_THREAD_STACK_SIZE, NULL ) != WICED_SUCCESS )
    {
        printf( "sensor thread start failed\n" );
        return;
    }

    result = wiced_wlan_connectivity_init( );
    if ( result != WICED_SUCCESS )
    {
        printf( "wlan init failed: %d\n", (int)result );
    }

    if ( wiced_rtos_create_thread( &console_thread, WICED_DEFAULT_LIBRARY_PRIORITY, "console",
                                   console_thread_main, CONSOLE_THREAD_STACK_SIZE, NULL ) != WICED_SUCCESS )
    {
        printf( "console thread start failed\n" );
    }

    if ( wiced_rtos_create_thread( &network_thread, WICED_DEFAULT_LIBRARY_PRIORITY, "network",
                                   network_thread_main, NETWORK_THREAD_STACK_SIZE, NULL ) != WICED_SUCCESS )
    {
        printf( "network thread start failed\n" );
    }

    sensor_reset_release( );

    {
        int16_t last_offset_sent = 0;
        uint8_t offset_known = 0u;

        while ( 1 )
        {
            wiced_rtos_delay_milliseconds( 60000u );
            if ( current_tz_rule_valid != 0u && wifi_time_synced != 0u )
            {
                wiced_utc_time_t now = 0u;
                int16_t offset_min = 0;
                uint8_t dst = 0u;

                wiced_time_get_utc_time( &now );
                rewair_tz_eval( &current_tz_rule, (uint32_t)now, &offset_min, &dst );
                if ( offset_known == 0u || offset_min != last_offset_sent )
                {
                    send_time_context( (uint32_t)now );
                    rewair_settings_apply_to_state( &current_settings );
                    last_offset_sent = offset_min;
                    offset_known = 1u;
                }
            }
        }
    }
}
