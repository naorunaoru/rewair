#include "rewair_wifi_dct.h"

#include <stdio.h>
#include <string.h>
#include "wiced_management.h"
#include "internal/wiced_internal_api.h"
#include "web_api.h"
#include "rewair_state.h"
#include "rewair_fmt.h"
/* wifi_join_command_ex now lives in rewair_wifi_join.c and
 * wifi_security_name / wifi_result_name in rewair_wifi_scan.c (Phase 2
 * Task 10, pure move) -- Task 9's local extern declarations for them are
 * replaced by the proper module headers. */
#include "rewair_wifi_scan.h"
#include "rewair_wifi_join.h"

/* Guards DCT wifi-section read/write critical sections only; never held
 * across joins, frame sends, or state-cache calls. */
wiced_mutex_t dct_wifi_mutex;

int wifi_dct_has_stored_ap( void )
{
    wiced_config_ap_entry_t* ap = NULL;
    int present = 0;

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    if ( wiced_dct_read_lock( (void**)&ap, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                              OFFSETOF( platform_dct_wifi_config_t, stored_ap_list ),
                              sizeof( *ap ) ) != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        return 0;
    }

    present = ap->details.SSID.length != 0u && ap->details.SSID.length <= SSID_NAME_SIZE;
    wiced_dct_read_unlock( ap, WICED_FALSE );
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );
    return present;
}

uint32_t wifi_dct_saved_count( void )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    uint32_t count = 0u;
    uint32_t i;

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    if ( wiced_dct_read_lock( (void**)&wifi_config, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                              0, sizeof( *wifi_config ) ) != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
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
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );
    return count;
}

void wifi_print_saved_credentials( void )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    uint32_t i;

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    if ( wiced_dct_read_lock( (void**)&wifi_config, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                              0, sizeof( *wifi_config ) ) != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
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
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );
}

wiced_result_t wifi_clear_stored_credentials( void )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    wiced_result_t result;

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        printf( "[wifi] DCT read failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        return result;
    }

    memset( wifi_config->stored_ap_list, 0, sizeof( wifi_config->stored_ap_list ) );
    wifi_config->device_configured = WICED_FALSE;
    result = wiced_dct_write( wifi_config, DCT_WIFI_CONFIG_SECTION, 0, sizeof( *wifi_config ) );
    wiced_dct_read_unlock( wifi_config, WICED_TRUE );
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );

    printf( "[wifi] stored credentials %s\n", result == WICED_SUCCESS ? "cleared" : "clear failed" );
    return result;
}

wiced_result_t wifi_store_ap_credentials( const wiced_ap_info_t* ap,
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

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
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
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );

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

/* ---- multi-network DCT list management (Task 8, web API) ----
 * These preserve existing stored_ap_list entries -- unlike wifi_store_ap_credentials
 * (still used by the console "join" path), which wipes the whole list and writes
 * slot 0 only. */

/* Shared slot writer for wifi_list_add / wifi_list_store: find the entry whose
 * SSID matches (update it IN PLACE -- details and security key both rewritten,
 * so a retry with a corrected password replaces the bad entry) or the first
 * free slot, then write details+key and mark device_configured. Factored
 * verbatim out of wifi_list_add (Phase 3 Task 5 fix); wifi_list_add's effect
 * is unchanged. */
static wiced_result_t list_write_slot( const char* ssid, const wiced_ap_info_t* details,
                                       const char* key, uint32_t key_len )
{
    wiced_result_t result;
    platform_dct_wifi_config_t* wifi_config = NULL;
    int free_slot = -1;
    int match_slot = -1;
    int i;

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        printf( "[wifi] DCT read failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        return result;
    }

    for ( i = 0; i < (int)CONFIG_AP_LIST_SIZE; i++ )
    {
        const wiced_config_ap_entry_t* entry = &wifi_config->stored_ap_list[i];

        if ( entry->details.SSID.length == 0u || entry->details.SSID.length > SSID_NAME_SIZE )
        {
            if ( free_slot < 0 )
            {
                free_slot = i;
            }
            continue;
        }
        if ( ssid_eq_text( &entry->details.SSID, ssid ) != 0 )
        {
            match_slot = i;
            break;
        }
    }

    if ( match_slot < 0 && free_slot < 0 )
    {
        wiced_dct_read_unlock( wifi_config, WICED_TRUE );
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        printf( "[wifi] stored_ap_list full; cannot add \"%s\"\n", ssid );
        return WICED_ERROR;
    }

    {
        int slot = match_slot >= 0 ? match_slot : free_slot;
        wiced_config_ap_entry_t* stored = &wifi_config->stored_ap_list[slot];

        stored->details = *details;
        stored->security_key_length = (uint8_t)key_len;
        memset( stored->security_key, 0, sizeof( stored->security_key ) );
        if ( key_len != 0u )
        {
            memcpy( stored->security_key, key, key_len );
        }
        wifi_config->device_configured = WICED_TRUE;

        result = wiced_dct_write( wifi_config, DCT_WIFI_CONFIG_SECTION, 0, sizeof( *wifi_config ) );
        wiced_dct_read_unlock( wifi_config, WICED_TRUE );
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );

        if ( result == WICED_SUCCESS )
        {
            printf( "[wifi] saved credentials to DCT slot %d (%s)\n", slot,
                    match_slot >= 0 ? "updated" : "new" );
        }
        else
        {
            printf( "[wifi] DCT write failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        }
    }

    return result;
}

wiced_result_t wifi_list_add( const char* ssid, const char* pass )
{
    const wiced_scan_result_t* scan;
    wiced_security_t security = WICED_SECURITY_WPA2_MIXED_PSK;
    wiced_ap_info_t joined_ap;
    char joined_key[64];
    uint32_t joined_key_len = 0u;
    wiced_result_t result;

    if ( ssid == NULL || cstr_len( ssid ) == 0u || cstr_len( ssid ) > SSID_NAME_SIZE )
    {
        return WICED_BADARG;
    }

    scan = find_best_scan_result_for_ssid( ssid );
    if ( scan == NULL && ( pass == NULL || cstr_len( pass ) == 0u ) )
    {
        security = WICED_SECURITY_OPEN;
    }

    /* Join first; only persist on success (mirrors wifi_join_command, but this
     * variant does NOT let the join path wipe-and-write slot 0 -- we do our own
     * find-existing-or-first-free-slot write below with the actual joined AP
     * details). */
    result = wifi_join_command_ex( ssid, pass != NULL ? pass : "", security, scan,
                                   scan != NULL ? 0 : 1 /* force_security only when we guessed */,
                                   0 /* store_to_dct */,
                                   &joined_ap, joined_key, &joined_key_len );
    if ( result != WICED_SUCCESS )
    {
        return result;
    }

    if ( joined_ap.SSID.length == 0u || joined_ap.SSID.length > SSID_NAME_SIZE ||
         joined_key_len > SECURITY_KEY_SIZE )
    {
        return WICED_BADARG;
    }

    return list_write_slot( ssid, &joined_ap, joined_key, joined_key_len );
}

wiced_result_t wifi_list_store( const char* ssid, const char* pass )
{
    const wiced_scan_result_t* scan;
    wiced_ap_info_t details;
    uint32_t ssid_len;
    uint32_t pass_len = pass != NULL ? cstr_len( pass ) : 0u;

    if ( ssid == NULL || cstr_len( ssid ) == 0u || cstr_len( ssid ) > SSID_NAME_SIZE )
    {
        return WICED_BADARG;
    }
    if ( pass_len > SECURITY_KEY_SIZE )
    {
        return WICED_BADARG;
    }
    ssid_len = cstr_len( ssid );

    /* Resolve AP details from the scan cache WITHOUT joining. The AP portal
     * flow guarantees a recent cache: the UI lists networks via /api/scan
     * before the user can pick one. */
    memset( &details, 0, sizeof( details ) );
    scan = find_best_scan_result_for_ssid( ssid );
    if ( scan != NULL )
    {
        details.SSID            = scan->SSID;
        details.BSSID           = scan->BSSID;
        details.signal_strength = scan->signal_strength;
        details.max_data_rate   = scan->max_data_rate;
        details.bss_type        = scan->bss_type;
        details.security        = scan->security;
        details.channel         = scan->channel;
        details.band            = scan->band;
    }
    else
    {
        /* Not in the cache (hidden network / stale cache): guess security from
         * the password and leave BSSID/channel zero. Safe because the SDK's
         * autojoin (wiced_join_ap_specific, WICED/internal/wifi.c) only does a
         * BSSID/channel-specific join when BSSID is non-null AND channel != 0;
         * otherwise -- and whenever the specific join fails -- it falls back to
         * wwd_wifi_join by SSID with the stored security. So a zeroed entry
         * joins by SSID; only a wrong security guess keeps it from joining, in
         * which case AP_FALLBACK returns the user to the portal to retry (the
         * retry updates this same slot). */
        details.SSID.length = (uint8_t)ssid_len;
        memcpy( details.SSID.value, ssid, ssid_len );
        details.bss_type = WICED_BSS_TYPE_INFRASTRUCTURE;
        details.security = pass_len == 0u ? WICED_SECURITY_OPEN : WICED_SECURITY_WPA2_MIXED_PSK;
    }
    details.next = NULL;

    return list_write_slot( ssid, &details, pass != NULL ? pass : "", pass_len );
}

wiced_result_t wifi_list_remove( const char* ssid )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    wiced_result_t result;
    int slot = -1;
    int i;
    rewair_status_t st;
    int was_active;

    if ( ssid == NULL || cstr_len( ssid ) == 0u )
    {
        return WICED_BADARG;
    }

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        printf( "[wifi] DCT read failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        return result;
    }

    for ( i = 0; i < (int)CONFIG_AP_LIST_SIZE; i++ )
    {
        const wiced_config_ap_entry_t* entry = &wifi_config->stored_ap_list[i];
        if ( entry->details.SSID.length != 0u && entry->details.SSID.length <= SSID_NAME_SIZE &&
             ssid_eq_text( &entry->details.SSID, ssid ) != 0 )
        {
            slot = i;
            break;
        }
    }

    if ( slot < 0 )
    {
        wiced_dct_read_unlock( wifi_config, WICED_TRUE );
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        return WICED_NOT_FOUND;
    }

    memset( &wifi_config->stored_ap_list[slot], 0, sizeof( wifi_config->stored_ap_list[slot] ) );
    if ( slot + 1 < (int)CONFIG_AP_LIST_SIZE )
    {
        memmove( &wifi_config->stored_ap_list[slot],
                 &wifi_config->stored_ap_list[slot + 1],
                 sizeof( wifi_config->stored_ap_list[0] ) * (uint32_t)( (int)CONFIG_AP_LIST_SIZE - slot - 1 ) );
        memset( &wifi_config->stored_ap_list[CONFIG_AP_LIST_SIZE - 1], 0,
                sizeof( wifi_config->stored_ap_list[0] ) );
    }

    {
        int any_left = 0;
        for ( i = 0; i < (int)CONFIG_AP_LIST_SIZE; i++ )
        {
            if ( wifi_config->stored_ap_list[i].details.SSID.length != 0u )
            {
                any_left = 1;
                break;
            }
        }
        wifi_config->device_configured = any_left != 0 ? WICED_TRUE : WICED_FALSE;
    }

    result = wiced_dct_write( wifi_config, DCT_WIFI_CONFIG_SECTION, 0, sizeof( *wifi_config ) );
    wiced_dct_read_unlock( wifi_config, WICED_TRUE );
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );

    if ( result != WICED_SUCCESS )
    {
        printf( "[wifi] DCT write failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        return result;
    }

    rewair_state_snapshot( &st );
    was_active = st.wifi_mode == 0u && ssid_eq_text_cstr( st.ssid, ssid );
    if ( was_active != 0 )
    {
        wiced_network_down( WICED_STA_INTERFACE );
        rewair_state_wifi_drop( );
    }

    printf( "[wifi] removed \"%s\" from stored_ap_list\n", ssid );
    return WICED_SUCCESS;
}

wiced_result_t wifi_list_reorder( char order[][33], uint32_t count )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    wiced_config_ap_entry_t saved[CONFIG_AP_LIST_SIZE];
    wiced_result_t result;
    uint32_t saved_count = 0u;
    uint32_t i;
    uint32_t j;

    if ( order == NULL || count == 0u || count > CONFIG_AP_LIST_SIZE )
    {
        return WICED_BADARG;
    }

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    result = wiced_dct_read_lock( (void**)&wifi_config, WICED_TRUE, DCT_WIFI_CONFIG_SECTION,
                                  0, sizeof( *wifi_config ) );
    if ( result != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        printf( "[wifi] DCT read failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
        return result;
    }

    memset( saved, 0, sizeof( saved ) );
    for ( i = 0u; i < CONFIG_AP_LIST_SIZE; i++ )
    {
        const wiced_config_ap_entry_t* entry = &wifi_config->stored_ap_list[i];
        if ( entry->details.SSID.length != 0u && entry->details.SSID.length <= SSID_NAME_SIZE )
        {
            saved[saved_count++] = *entry;
        }
    }

    if ( count != saved_count )
    {
        wiced_dct_read_unlock( wifi_config, WICED_TRUE );
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        return WICED_BADARG;
    }

    /* Verify order[] is a permutation of the saved SSID set: each entry in
     * `saved` must be matched by exactly one entry in `order`. */
    {
        uint8_t matched[CONFIG_AP_LIST_SIZE];
        memset( matched, 0, sizeof( matched ) );

        for ( i = 0u; i < count; i++ )
        {
            int found = 0;
            for ( j = 0u; j < saved_count; j++ )
            {
                if ( matched[j] != 0u )
                {
                    continue;
                }
                if ( ssid_eq_text( &saved[j].details.SSID, order[i] ) != 0 )
                {
                    matched[j] = 1u;
                    found = 1;
                    break;
                }
            }
            if ( found == 0 )
            {
                wiced_dct_read_unlock( wifi_config, WICED_TRUE );
                wiced_rtos_unlock_mutex( &dct_wifi_mutex );
                return WICED_BADARG;
            }
        }
    }

    /* Rebuild stored_ap_list in the requested order. */
    memset( wifi_config->stored_ap_list, 0, sizeof( wifi_config->stored_ap_list ) );
    for ( i = 0u; i < count; i++ )
    {
        for ( j = 0u; j < saved_count; j++ )
        {
            if ( ssid_eq_text( &saved[j].details.SSID, order[i] ) != 0 )
            {
                wifi_config->stored_ap_list[i] = saved[j];
                break;
            }
        }
    }

    result = wiced_dct_write( wifi_config, DCT_WIFI_CONFIG_SECTION, 0, sizeof( *wifi_config ) );
    wiced_dct_read_unlock( wifi_config, WICED_TRUE );
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );

    if ( result != WICED_SUCCESS )
    {
        printf( "[wifi] DCT write failed result=%d (%s)\n", (int)result, wifi_result_name( result ) );
    }
    return result;
}

int wifi_list_get( uint32_t index, char ssid_out[33], wiced_security_t* sec_out, int* connected_out )
{
    platform_dct_wifi_config_t* wifi_config = NULL;
    rewair_status_t st;
    uint32_t seen = 0u;
    uint32_t i;
    int found = 0;

    if ( ssid_out == NULL )
    {
        return 0;
    }

    wiced_rtos_lock_mutex( &dct_wifi_mutex );

    if ( wiced_dct_read_lock( (void**)&wifi_config, WICED_FALSE, DCT_WIFI_CONFIG_SECTION,
                              0, sizeof( *wifi_config ) ) != WICED_SUCCESS )
    {
        wiced_rtos_unlock_mutex( &dct_wifi_mutex );
        return 0;
    }

    for ( i = 0u; i < CONFIG_AP_LIST_SIZE; i++ )
    {
        const wiced_config_ap_entry_t* entry = &wifi_config->stored_ap_list[i];
        uint32_t n;

        if ( entry->details.SSID.length == 0u || entry->details.SSID.length > SSID_NAME_SIZE )
        {
            continue;
        }
        if ( seen != index )
        {
            seen++;
            continue;
        }

        n = entry->details.SSID.length;
        memcpy( ssid_out, entry->details.SSID.value, n );
        ssid_out[n] = '\0';
        if ( sec_out != NULL )
        {
            *sec_out = entry->details.security;
        }
        found = 1;
        break;
    }

    wiced_dct_read_unlock( wifi_config, WICED_FALSE );
    wiced_rtos_unlock_mutex( &dct_wifi_mutex );

    if ( found == 0 )
    {
        return 0;
    }

    if ( connected_out != NULL )
    {
        rewair_state_snapshot( &st );
        *connected_out = ( st.wifi_mode == 0u && ssid_eq_text_cstr( st.ssid, ssid_out ) ) ? 1 : 0;
    }

    return 1;
}
