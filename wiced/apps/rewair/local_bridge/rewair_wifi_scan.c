/*
 * Wi-Fi scan cache + scan-diagnostics naming helpers, lifted verbatim out of
 * local_bridge.c (Phase 2 Task 10, pure move). See rewair_wifi_scan.h for the
 * module boundary and the Task 10 report for the placement rationale.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wiced.h"
#include "wiced_wifi.h"
#include "rewair_fmt.h"
#include "rewair_wifi_scan.h"

#define CONSOLE_SCAN_CACHE_MAX 16u

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

const char* wifi_security_name( wiced_security_t security )
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

int parse_wifi_security( const char* text, wiced_security_t* security )
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

const char* wifi_result_name( wiced_result_t result )
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

void ap_from_scan_result( wiced_ap_info_t* ap, const wiced_scan_result_t* scan )
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

void wifi_scan_start( void )
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
