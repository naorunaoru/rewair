#include "rewair_net_mode.h"

#include <stdio.h>
#include <string.h>
#include "wiced.h"
#include "wiced_wifi.h"
#include "wiced_framework.h"
#include "wiced_management.h"
#include "internal/wiced_internal_api.h"
#include "rewair_state.h"
#include "rewair_fmt.h"
#include "rewair_wifi_dct.h"
#include "rewair_wifi_join.h"
#include "rewair_wifi_scan.h"
#include "web_api.h"
#include "web_ui.h"
#include "dns_redirect.h"

#define AP_JOIN_ATTEMPTS_BEFORE_FALLBACK 3u
#define AP_CHANNEL                       6u

/* volatile: written only by the network thread (tick/enter/exit), but read
 * cross-thread by HTTP workers via rewair_net_mode_current (Tasks 4/5). */
static volatile rewair_net_mode_t current_mode = NET_MODE_STA_JOINING;
static uint32_t            boot_join_failures = 0u;
static volatile uint32_t   sta_requested = 0u;
/* Owns the STA link-history bit moved verbatim out of local_bridge.c's
 * network_thread_main (was `static volatile uint32_t wifi_link_was_up`). */
static volatile uint32_t   wifi_link_was_up = 0u;
static dns_redirector_t    dns_redirector;
/* Set only when wiced_dns_redirector_start succeeds. A failed start returns
 * BEFORE the SDK creates dns_redirector.dns_thread (dns_redirect.c:170), so an
 * unconditional stop would force-awake/join/delete an uninitialized (first
 * cycle) or stale (later cycles) thread handle -- probable hard fault. */
static uint8_t             dns_started = 0u;

static const wiced_ip_setting_t ap_ip_settings =
{
    INITIALISER_IPV4_ADDRESS( .ip_address, MAKE_IPV4_ADDRESS( 192, 168, 0, 1 ) ),
    INITIALISER_IPV4_ADDRESS( .netmask,    MAKE_IPV4_ADDRESS( 255, 255, 255, 0 ) ),
    INITIALISER_IPV4_ADDRESS( .gateway,    MAKE_IPV4_ADDRESS( 192, 168, 0, 1 ) ),
};

static void ap_ssid_build( char out[33] )
{
    wiced_mac_t mac;
    memset( &mac, 0, sizeof( mac ) );
    if ( wiced_wifi_get_mac_address( &mac ) != WICED_SUCCESS )
    {
        printf( "[net-mode] mac read failed\n" );
    }
    snprintf( out, 33, "rewair-setup-%02x%02x", mac.octet[4], mac.octet[5] );
}

/* Writes soft_ap_settings into the wifi DCT section so the SDK's
 * wiced_network_up( WICED_AP_INTERFACE, ... ) can bring the soft-AP up. Per the
 * Step-1 SDK review: wiced_network_up reads soft_ap_settings.{SSID,security,
 * security_key,security_key_length,channel} and calls wwd_wifi_start_ap; it does
 * NOT check details_valid for the AP interface (only for the CONFIG interface),
 * but we set it anyway for completeness. Whole-section read-modify-write (offset
 * 0) mirrors rewair_wifi_dct.c and preserves stored_ap_list -- required so
 * AP_FALLBACK does not lose the saved credentials it is falling back from. Held
 * under dct_wifi_mutex like every other writer of this section. */
static wiced_result_t ap_write_soft_ap_dct( const char* ssid )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    wiced_config_soft_ap_t* soft_ap;
    wiced_result_t result;
    uint32_t ssid_len = cstr_len( ssid );

    if ( ssid_len == 0u || ssid_len > SSID_NAME_SIZE )
    {
        return WICED_BADARG;
    }

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        return result;
    }

    soft_ap = &wifi_config->soft_ap_settings;
    memset( soft_ap, 0, sizeof( *soft_ap ) );
    soft_ap->SSID.length = (uint8_t)ssid_len;
    memcpy( soft_ap->SSID.value, ssid, ssid_len );
    soft_ap->security = WICED_SECURITY_OPEN;
    soft_ap->channel = AP_CHANNEL;
    soft_ap->security_key_length = 0u;
    soft_ap->details_valid = CONFIG_VALIDITY_VALUE;

    result = wiced_dct_write( wifi_config, DCT_WIFI_CONFIG_SECTION, 0, sizeof( *wifi_config ) );
    wiced_dct_read_unlock( wifi_config, WICED_TRUE );
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );
    return result;
}

rewair_net_mode_t rewair_net_mode_current( void )
{
    return current_mode;
}

void rewair_net_mode_request_sta( void )
{
    sta_requested = 1u;
}

int rewair_net_mode_sta_requested( void )
{
    return sta_requested != 0u ? 1 : 0;
}

wiced_result_t rewair_net_mode_enter_ap( rewair_net_mode_t which )
{
    char ssid[33];
    wiced_result_t result;

    ap_ssid_build( ssid );
    if ( ap_write_soft_ap_dct( ssid ) != WICED_SUCCESS )
    {
        printf( "[net-mode] soft-ap dct write failed\n" );
    }

    rewair_web_api_stop( );
    result = wiced_network_up( WICED_AP_INTERFACE, WICED_USE_INTERNAL_DHCP_SERVER,
                               &ap_ip_settings );
    if ( result != WICED_SUCCESS )
    {
        printf( "[net-mode] ap up failed result=%d, retrying once\n", (int)result );
        wiced_network_down( WICED_AP_INTERFACE );
        result = wiced_network_up( WICED_AP_INTERFACE, WICED_USE_INTERNAL_DHCP_SERVER,
                                   &ap_ip_settings );
        if ( result != WICED_SUCCESS )
        {
            printf( "[net-mode] ap up failed twice, rebooting\n" );
            wiced_rtos_delay_milliseconds( 1000u );
            wiced_framework_reboot( );
        }
    }

    if ( wiced_dns_redirector_start( &dns_redirector, WICED_AP_INTERFACE ) == WICED_SUCCESS )
    {
        dns_started = 1u;
    }
    else
    {
        printf( "[net-mode] dns redirector failed; portal reachable by ip only\n" );
    }

    web_ui_set_captive( 1u );
    rewair_web_api_start( WICED_AP_INTERFACE );

    {
        char mac_buf[18] = "00:00:00:00:00:00";
        wiced_mac_t mac;
        if ( wiced_wifi_get_mac_address( &mac ) == WICED_SUCCESS )
        {
            mac_to_cstr( &mac, mac_buf );
        }
        rewair_state_set_wifi_ap( ssid, "192.168.0.1", mac_buf, wifi_dct_saved_count( ) );
    }

    current_mode = which;
    printf( "[net-mode] ap up ssid=%s ip=192.168.0.1 mode=%s\n", ssid,
            which == NET_MODE_AP_SETUP ? "setup" : "fallback" );
    return WICED_SUCCESS;
}

wiced_result_t rewair_net_mode_exit_ap_to_sta( void )
{
    if ( dns_started != 0u )
    {
        wiced_dns_redirector_stop( &dns_redirector );
        dns_started = 0u;
    }
    web_ui_set_captive( 0u );
    rewair_web_api_stop( );
    wiced_network_down( WICED_AP_INTERFACE );      /* SDK stops internal DHCP server */
    current_mode = NET_MODE_STA_JOINING;
    boot_join_failures = 0u;
    printf( "[net-mode] ap down, sta joining\n" );
    return WICED_SUCCESS;
}

void rewair_net_mode_tick( void )
{
    wiced_result_t result;

    if ( sta_requested != 0u )       /* set by Task 5's join path */
    {
        sta_requested = 0u;
        if ( current_mode == NET_MODE_AP_SETUP || current_mode == NET_MODE_AP_FALLBACK )
        {
            rewair_net_mode_exit_ap_to_sta( );
        }
    }

    switch ( current_mode )
    {
        case NET_MODE_STA_JOINING:
        case NET_MODE_STA_UP:
            if ( wifi_join_in_progress != 0u )
            {
                return;
            }
            if ( wiced_network_is_up( WICED_STA_INTERFACE ) == WICED_TRUE )
            {
                wifi_link_was_up = 1u;
                current_mode = NET_MODE_STA_UP;
                boot_join_failures = 0u;
                network_after_ip_ready( );
                return;
            }
            if ( wifi_dct_has_stored_ap( ) == 0 )
            {
                rewair_net_mode_enter_ap( NET_MODE_AP_SETUP );
                return;
            }
            /* stored AP present: (re)join from the DCT. Link-drop bookkeeping
             * moved verbatim from network_thread_main. */
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
                current_mode = NET_MODE_STA_UP;
                boot_join_failures = 0u;
                network_after_ip_ready( );
            }
            else
            {
                printf( "[wifi] autojoin failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
                wiced_network_down( WICED_STA_INTERFACE );
                wiced_leave_ap( WICED_STA_INTERFACE );
                boot_join_failures++;
                /* Fall back to AP only on the BOOT join path: mode is still
                 * STA_JOINING, i.e. the STA link never came up this boot. A
                 * steady-state drop (STA_UP -> down) keeps mode STA_UP and
                 * retries the autojoin arm forever without ever counting toward
                 * fallback (per the spec decision). */
                if ( current_mode == NET_MODE_STA_JOINING &&
                     boot_join_failures >= AP_JOIN_ATTEMPTS_BEFORE_FALLBACK )
                {
                    rewair_net_mode_enter_ap( NET_MODE_AP_FALLBACK );
                }
            }
            return;

        case NET_MODE_AP_SETUP:
        case NET_MODE_AP_FALLBACK:
            /* Task 6 adds self-heal here; nothing yet. */
            return;
    }
}
