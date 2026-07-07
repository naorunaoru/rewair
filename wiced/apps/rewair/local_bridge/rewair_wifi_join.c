/*
 * Wi-Fi join commands (console join path + web-API join entry point), lifted
 * verbatim out of local_bridge.c (Phase 2 Task 10, pure move). See
 * rewair_wifi_join.h for the module boundary and the Task 10 report for the
 * placement rationale.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wiced.h"
#include "wiced_wifi.h"
#include "wiced_management.h"
#include "wiced_tcpip.h"
#include "internal/wiced_internal_api.h"
#include "rewair_fmt.h"
#include "rewair_wifi_dct.h"
#include "rewair_wifi_scan.h"
#include "rewair_wifi_join.h"

volatile uint32_t wifi_join_in_progress = 0u;

wiced_result_t wifi_join_command_ex( const char* ssid_text, const char* pass_text,
                                            wiced_security_t security, const wiced_scan_result_t* scan,
                                            int force_security, int store_to_dct,
                                            wiced_ap_info_t* joined_ap_out, char joined_key_out[64],
                                            uint32_t* joined_key_len_out )
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
    if ( store_to_dct != 0 )
    {
        wifi_store_ap_credentials( &ap, security_key, pass_len );
    }
    if ( joined_ap_out != NULL )
    {
        *joined_ap_out = ap;
    }
    if ( joined_key_out != NULL )
    {
        memcpy( joined_key_out, security_key, sizeof( security_key ) );
    }
    if ( joined_key_len_out != NULL )
    {
        *joined_key_len_out = pass_len;
    }
    wifi_join_in_progress = 0u;
    network_after_ip_ready( );
    return WICED_SUCCESS;
}

wiced_result_t wifi_join_command( const char* ssid_text, const char* pass_text,
                                         wiced_security_t security, const wiced_scan_result_t* scan,
                                         int force_security )
{
    return wifi_join_command_ex( ssid_text, pass_text, security, scan, force_security,
                                 1 /* store_to_dct: preserve legacy console slot-0 behavior */,
                                 NULL, NULL, NULL );
}

void wifi_join_test_index_command( const char* index_text, const char* pass_text,
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

void wifi_join_index_command( const char* index_text, const char* pass_text,
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
