#include "rewair_console.h"

#include <stdint.h>
#include <stdio.h>
#include "wiced.h"
#include "internal/wiced_internal_api.h" /* wiced_leave_ap (also used by local_bridge.c) */
#include "rewair_fmt.h"
#include "rewair_frames.h"
#include "rewair_wifi_dct.h"
#include "rewair_wifi_scan.h"
#include "rewair_wifi_join.h"
#include "rewair_sflash.h"

/* ---- Cross-boundary statics owned by local_bridge.c ----
 * These file-globals stay in local_bridge.c (network/sensor-reset clusters,
 * which stay there per the task boundary) but are read/written directly by
 * the console dispatcher below. Per the established pure-move convention
 * (Task 8's rewair_frames.h, Task 9/10's dct_wifi_mutex /
 * wifi_join_in_progress), each is extern-declared at its point of use
 * rather than routed through an accessor function; `static` was removed
 * from their definitions in local_bridge.c. */

/* owned by local_bridge.c (network cluster; primary writer:
 * network_after_ip_ready / network_sync_time_once) -- cleared here by the
 * console "forget"/"down" commands. */
extern volatile uint32_t wifi_time_synced;
extern volatile uint32_t wifi_last_ntp_sync_ms;

/* owned by local_bridge.c (reset cluster) -- invoked here by the console
 * "sensor-reset" command. Declared, not defined: local_bridge.c has no
 * header of its own (same convention as network_after_ip_ready /
 * wifi_print_status in rewair_wifi_join.h), so the forward declaration
 * lives at the one call site that needs it, outside local_bridge.c. */
extern void sensor_reset_cycle( void );

#define CONSOLE_LINE_MAX 160u
#define CONSOLE_ARG_MAX 32
#define SFLASH_CONSOLE_READ_MAX 256u

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

static int parse_hex_addr( const char* text, uint32_t* out )
{
    uint32_t value = 0u;

    if ( *text == '\0' )
    {
        return 0;
    }
    while ( *text != '\0' )
    {
        uint8_t nibble = 0u;
        if ( from_hex( *text, &nibble ) == 0 )
        {
            return 0;
        }
        value = ( value << 4 ) | nibble;
        text++;
    }
    *out = value;
    return 1;
}

static void sflash_id_command( void )
{
    uint8_t id[3];

    if ( rewair_sflash_read_id( id ) != 0 )
    {
        printf( "[sflash] id read failed\n" );
        return;
    }
    printf( "[sflash] id: %02x %02x %02x\n", id[0], id[1], id[2] );
}

static void sflash_read_command( const char* addr_text, const char* len_text )
{
    uint32_t addr = 0u;
    uint32_t len = 0u;
    uint8_t buf[SFLASH_CONSOLE_READ_MAX];
    uint32_t i;

    if ( parse_hex_addr( addr_text, &addr ) == 0 )
    {
        printf( "usage: sflash read <hexaddr> <len>\n" );
        return;
    }
    if ( parse_uint32( len_text, &len ) == 0 || len == 0u || len > SFLASH_CONSOLE_READ_MAX )
    {
        printf( "[sflash] len must be 1..%lu\n", (unsigned long)SFLASH_CONSOLE_READ_MAX );
        return;
    }
    if ( rewair_sflash_bounds_ok( addr, len ) == 0 )
    {
        printf( "[sflash] addr 0x%lx len %lu beyond device (2 MiB)\n",
                (unsigned long)addr, (unsigned long)len );
        return;
    }
    if ( rewair_sflash_read_bytes( addr, buf, len ) != 0 )
    {
        printf( "[sflash] read failed at 0x%lx len %lu\n", (unsigned long)addr, (unsigned long)len );
        return;
    }
    printf( "[sflash] read 0x%lx len %lu:\n", (unsigned long)addr, (unsigned long)len );
    for ( i = 0u; i < len; i++ )
    {
        printf( "%02x%s", buf[i], ( ( i + 1u ) % 16u == 0u || i + 1u == len ) ? "\n" : " " );
    }
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
    printf( "  sflash id\n" );
    printf( "  sflash read <hexaddr> <len>\n" );
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
    else if ( cstr_eq( argv[0], "sflash" ) )
    {
        if ( argc >= 2 && cstr_eq( argv[1], "id" ) )
        {
            sflash_id_command( );
        }
        else if ( argc >= 4 && cstr_eq( argv[1], "read" ) )
        {
            sflash_read_command( argv[2], argv[3] );
        }
        else
        {
            printf( "usage: sflash id | sflash read <hexaddr> <len>\n" );
        }
    }
    else
    {
        printf( "unknown command: %s\n", argv[0] );
    }
}

void console_thread_main( uint32_t arg )
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
