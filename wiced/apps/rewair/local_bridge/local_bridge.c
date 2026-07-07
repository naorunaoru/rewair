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
#include "rewair_fmt.h"
#include "rewair_walltime.h"
#include "rewair_score.h"
#include "rewair_frame_rx.h"
#include "rewair_frames.h"
#include "rewair_wifi_dct.h"
#include "rewair_wifi_scan.h"
#include "rewair_wifi_join.h"
#include "rewair_console.h"
#include "web_api.h"
#include "spi_flash.h"
#include "spi_flash_internal.h" /* device_id_t + sflash_read_ID (on GLOBAL_INCLUDES via drivers/spi_flash component) */

#define SENSOR_RX_BUFFER_SIZE 1024u
#define SENSOR_THREAD_STACK_SIZE 4096u
#define CONSOLE_THREAD_STACK_SIZE 4096u
#define NETWORK_THREAD_STACK_SIZE 6144u
/* CONSOLE_LINE_MAX, CONSOLE_ARG_MAX moved to rewair_console.c (Phase 2
 * Task 11, pure move) -- only the console tokenizer/dispatcher used them. */
#define SENSOR_DIAG_REPEAT_MS 1000u
#define SENSOR_STAT_REPEAT_MS 10000u
#define SENSOR_NUDGE_START_MS 5000u
#define SENSOR_NETW_NUDGE_MS 10000u
#define SENSOR_NETW_NUDGE_MAX 5u
#define NETWORK_RETRY_MS 60000u
#define NTP_RETRY_COUNT 3u
#define NTP_SYNC_PERIOD_MS ( 12u * 60u * 60u * 1000u )
#define SENSOR_RAW_TRACE_MAX 64u

static sensor_rx_t sensor_rx;
static wiced_thread_t sensor_thread;
static wiced_thread_t console_thread;
static wiced_thread_t network_thread;
/* sensor_uart_tx_mutex now lives in rewair_frames.c (Phase 2 Task 8, primary
 * writer: sensor_uart_send_frame_bytes); declared extern in rewair_frames.h,
 * still initialized here by sensor_uart_start (UART bring-up). */
/* dct_wifi_mutex now lives in rewair_wifi_dct.c (Phase 2 Task 9, primary
 * owner: the DCT credential/list functions there); declared extern in
 * rewair_wifi_dct.h, still initialized here by application_start and
 * locked/unlocked directly here by network_after_ip_ready. */
/* Guards every external-sflash driver call (init/id/read/write/erase). The
 * SDK's spi_flash/platform_spi stack has no locking of its own, and this
 * handle is shared by the console thread and the HTTP worker thread. Scope
 * is kept tight: locked immediately around each driver call, never held
 * across console printf/parsing or HTTP response building. */
static wiced_mutex_t sflash_mutex;
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
/* sensor_boot_context_sent, sensor_netw_boot_pulses now live in
 * rewair_frames.c (Phase 2 Task 8, primary writer: send_sensor_boot_context /
 * send_netw_up); declared extern in rewair_frames.h. Reset here (by
 * sensor_reset_release/sensor_reset_cycle below and the console "context"
 * command) and read here (sensor_uart_stat). */
static volatile uint32_t sensor_sens_seen = 0u;
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
/* sensor_uart_tx_count, sensor_uart_tx_drop_count, sensor_uart_wiced_tx_fail_count,
 * sensor_uart_wiced_tx_result, sensor_uart_tx_sr_before, sensor_uart_tx_sr_after
 * now live in rewair_frames.c (Phase 2 Task 8, primary writer:
 * sensor_uart_send_frame_bytes); declared extern in rewair_frames.h, read by
 * sensor_uart_stat below. */
static volatile uint32_t sensor_sens_count = 0u;
static volatile uint32_t sensor_last_rx_ms = 0u;
static volatile uint32_t sensor_last_sens_ms = 0u;
static char sensor_last_frame_cmd[5] = "----";
static volatile uint32_t sensor_last_frame_trailer = 0u;
static uint8_t sensor_raw_trace[SENSOR_RAW_TRACE_MAX];
static volatile uint32_t sensor_raw_trace_count = 0u;
static uint32_t sensor_raw_trace_reported = 0u;
/* console_scan_count, console_scan_cache(_count), scan_done_semaphore,
 * scan_semaphore_inited, scan_in_progress, scan_busy (+ its benign-race
 * documentation comment, moved verbatim) now live in rewair_wifi_scan.c
 * (Phase 2 Task 10, pure move; all still static there -- every outside
 * reader goes through the accessors in rewair_wifi_scan.h).
 * wifi_join_in_progress now lives in rewair_wifi_join.c (volatile
 * preserved), declared extern in rewair_wifi_join.h because
 * network_thread_main below reads it directly.
 * wifi_time_synced and wifi_last_ntp_sync_ms: owned by local_bridge.c
 * (primary writers: network_after_ip_ready / network_sync_time_once,
 * both stay here), but the console "forget"/"down" commands
 * (rewair_console.c, Phase 2 Task 11) clear both directly -- `static`
 * dropped, extern-declared at that call site in rewair_console.c (no
 * accessor functions introduced, matching the wifi_join_in_progress /
 * dct_wifi_mutex convention). */
volatile uint32_t wifi_time_synced = 0u;
volatile uint32_t wifi_last_ntp_sync_ms = 0u;
static volatile uint32_t wifi_network_ready_ms = 0u;
static volatile uint32_t wifi_link_was_up = 0u;

/* current_tz_rule, current_tz_rule_valid, and sensor_set_tz_rule() now live
 * in rewair_frames.c (Phase 2 Task 8, primary writer: sensor_set_tz_rule);
 * both statics are declared extern in rewair_frames.h because
 * application_start's DST-recheck loop below reads them directly. */
static rewair_settings_t current_settings;

/* ---- External SPI flash (Macronix MX25L1606E) -- Phase 2 Task 1 bring-up ----
 * Lazily initialized on first use by either the console "sflash" commands or
 * the /api/debug/sflash route (web_api.c); both share this handle so the SPI
 * peripheral is only brought up once. Read-only for now: write_allowed is
 * SFLASH_WRITE_NOT_ALLOWED until a later task explicitly needs writes. */
static sflash_handle_t rewair_sflash_handle;
static uint32_t         rewair_sflash_inited = 0u;

/* Device capacity: MX25L1606E is 2 MiB (16 Mbit). The SDK's sflash_read/write
 * take a 24-bit device address and silently wrap past this -- addr+len must
 * be checked by every caller (console commands and the debug route) before
 * ever reaching the driver. */
#define REWAIR_SFLASH_CAPACITY 0x200000u

/* Returns 1 if [addr, addr+len) fits within the device, 0 otherwise. Also
 * catches the addr+len overflow case (len so large it wraps uint32_t). */
int rewair_sflash_bounds_ok( uint32_t addr, uint32_t len )
{
    if ( len == 0u )
    {
        return 0;
    }
    if ( addr >= REWAIR_SFLASH_CAPACITY )
    {
        return 0;
    }
    if ( len > REWAIR_SFLASH_CAPACITY - addr )
    {
        return 0;
    }
    return 1;
}

static int rewair_sflash_ensure_init_locked( void )
{
    if ( rewair_sflash_inited != 0u )
    {
        return 0;
    }
    if ( init_sflash( &rewair_sflash_handle, NULL, SFLASH_WRITE_NOT_ALLOWED ) != 0 )
    {
        printf( "[sflash] init_sflash failed\n" );
        return -1;
    }
    rewair_sflash_inited = 1u;
    return 0;
}

/* Public wrapper: takes the mutex around the lazy-init path only. Exposed
 * separately from the locked variant so callers that already hold
 * sflash_mutex (none today) could reuse the inner call; everyone else should
 * call this one. */
int rewair_sflash_ensure_init( void )
{
    int rc;

    wiced_rtos_lock_mutex( &sflash_mutex );
    rc = rewair_sflash_ensure_init_locked( );
    wiced_rtos_unlock_mutex( &sflash_mutex );
    return rc;
}

/* Returns 0 on success with *out_id filled (3 bytes: manufacturer, memory
 * type, capacity -- e.g. c2 20 15 for the MX25L1606E), nonzero on failure. */
int rewair_sflash_read_id( uint8_t out_id[3] )
{
    device_id_t id;
    int rc;

    wiced_rtos_lock_mutex( &sflash_mutex );
    rc = rewair_sflash_ensure_init_locked( );
    if ( rc == 0 )
    {
        rc = sflash_read_ID( &rewair_sflash_handle, &id );
    }
    wiced_rtos_unlock_mutex( &sflash_mutex );

    if ( rc != 0 )
    {
        return -1;
    }
    out_id[0] = id.id[0];
    out_id[1] = id.id[1];
    out_id[2] = id.id[2];
    return 0;
}

/* Returns 0 on success, nonzero on failure. Callers MUST bounds-check addr+len
 * against REWAIR_SFLASH_CAPACITY (rewair_sflash_bounds_ok) before calling --
 * this function does not re-check, since both current callers (console
 * command and the dev-gated debug route) already cap len <= 256 and validate
 * addr themselves, and the driver's 24-bit address silently wraps rather than
 * erroring. */
int rewair_sflash_read_bytes( uint32_t addr, uint8_t* out, uint32_t size )
{
    int rc;

    wiced_rtos_lock_mutex( &sflash_mutex );
    rc = rewair_sflash_ensure_init_locked( );
    if ( rc == 0 )
    {
        rc = sflash_read( &rewair_sflash_handle, (unsigned long)addr, out, (unsigned int)size );
    }
    wiced_rtos_unlock_mutex( &sflash_mutex );

    return ( rc == 0 ) ? 0 : -1;
}

/* network_after_ip_ready's forward declaration removed (Phase 2 Task 10):
 * it is now declared in rewair_wifi_join.h (included above) because the
 * moved wifi_join_command_ex calls it; the definition STAYS below, linkage
 * changed from static to external, body untouched.
 *
 * sensor_reset_cycle's forward declaration removed (Phase 2 Task 11): the
 * moved console_handle_command (rewair_console.c) calls it, so it is now
 * extern-declared at that one call site in rewair_console.c instead of a
 * same-file forward declaration; the definition STAYS below (reset
 * cluster), linkage changed from static to external, body unchanged
 * (see the Task 11 report for the sensor_reset_release/sensor_reset_cycle
 * dedup that also landed in this task). */

/* console_prompt, console_read_line and console_tokenize moved to
 * rewair_console.c (Phase 2 Task 11, pure move). Declarations now come from
 * rewair_console.h (console_thread_main only -- the rest are static to
 * rewair_console.c, since nothing outside the console cluster calls
 * them). */

/* wifi_security_name, parse_wifi_security, wifi_result_name,
 * find_best_scan_result_for_ssid, console_scan_cache_get and
 * ap_from_scan_result moved to rewair_wifi_scan.c (Phase 2 Task 10, pure
 * move). Declarations now come from rewair_wifi_scan.h. */

/* Was static; linkage changed to external (Phase 2 Task 10) so
 * rewair_wifi_join.c's moved wifi_join_command_ex can call it (declared in
 * rewair_wifi_join.h; STAYS here -- pure wiced link-state printing, its two
 * other call sites, the console "net" command and network_thread_main,
 * both stay). Body unchanged. */
void wifi_print_status( void )
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

/* console_scan_result_handler, wifi_scan_start and sensor_scan_blocking
 * moved to rewair_wifi_scan.c (Phase 2 Task 10, pure move; the handler
 * stayed static -- both of its callers moved with it). */

/* wifi_dct_has_stored_ap, wifi_dct_saved_count, wifi_print_saved_credentials,
 * wifi_clear_stored_credentials, wifi_store_ap_credentials moved to
 * rewair_wifi_dct.c (Phase 2 Task 9, pure move). Declarations now come from
 * rewair_wifi_dct.h. */

/* wifi_join_command_ex, wifi_join_command, wifi_join_test_index_command and
 * wifi_join_index_command moved to rewair_wifi_join.c (Phase 2 Task 10,
 * pure move). Declarations now come from rewair_wifi_join.h.
 * (wifi_list_add, wifi_list_remove, wifi_list_reorder, wifi_list_get
 * remain in rewair_wifi_dct.c per Task 9; rewair_wifi_dct.c now includes
 * rewair_wifi_join.h for wifi_join_command_ex instead of its former local
 * extern declaration.) */

/* SFLASH_CONSOLE_READ_MAX, parse_hex_addr, sflash_id_command and
 * sflash_read_command moved to rewair_console.c (Phase 2 Task 11, pure
 * move) -- console-only wrappers around rewair_sflash_read_id /
 * rewair_sflash_bounds_ok / rewair_sflash_read_bytes, which STAY here (the
 * debug HTTP route in web_api.c calls rewair_sflash_read_bytes directly,
 * so those three functions are shared and not console-only). All static;
 * no linkage changes. */

/* console_print_help and console_handle_command moved to rewair_console.c
 * (Phase 2 Task 11, pure move); both remain static there. */

/* console_thread_main moved to rewair_console.c (Phase 2 Task 11, pure
 * move); declaration now comes from rewair_console.h. application_start's
 * wiced_rtos_create_thread call site below is unchanged except for
 * resolving the symbol through that header. */

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

/* Was static; linkage changed to external (Phase 2 Task 10) so
 * rewair_wifi_join.c's moved wifi_join_command_ex can call it (declared in
 * rewair_wifi_join.h; STAYS here). Body unchanged. */
void network_after_ip_ready( void )
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
        wiced_rtos_lock_mutex( &dct_wifi_mutex );
        if ( wiced_dct_read_lock( (void**)&ap, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                                  OFFSETOF( platform_dct_wifi_config_t, stored_ap_list ),
                                  sizeof( *ap ) ) == WICED_SUCCESS )
        {
            uint32_t n = ap->details.SSID.length <= 32u ? ap->details.SSID.length : 32u;
            memcpy( ssid_buf, ap->details.SSID.value, n );
            ssid_buf[n] = '\0';
            wiced_dct_read_unlock( ap, WICED_FALSE );
        }
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
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

/* Phase 2 Task 11 sanctioned dedup: sensor_reset_release and
 * sensor_reset_cycle shared an identical boot-state-reset prefix (the 8
 * counter/flag clears + sensor_frame_reset + the GPIO output-mode init).
 * Extracted verbatim into this helper; everything after it (the actual
 * release-vs-pulse GPIO sequencing and the differing log message) stays in
 * the respective callers untouched -- no behavior change. */
static void sensor_boot_state_reset( void )
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
}

static void sensor_reset_release( void )
{
    sensor_boot_state_reset( );

    wiced_gpio_output_high( AWAIR_SENSOR_RESET );
    sensor_reset_released = 1u;
    printf( "[boot] sensor reset line released on PB12\n" );
}

/* Was static; linkage changed to external (Phase 2 Task 11) so the moved
 * console "sensor-reset" command (rewair_console.c) can call it. Body
 * otherwise unchanged (aside from the sanctioned dedup above). */
void sensor_reset_cycle( void )
{
    sensor_boot_state_reset( );

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

    if ( wiced_rtos_init_mutex( &dct_wifi_mutex ) != WICED_SUCCESS )
    {
        printf( "dct wifi mutex init failed\n" );
        return;
    }

    if ( wiced_rtos_init_mutex( &sflash_mutex ) != WICED_SUCCESS )
    {
        printf( "sflash mutex init failed\n" );
        return;
    }

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
