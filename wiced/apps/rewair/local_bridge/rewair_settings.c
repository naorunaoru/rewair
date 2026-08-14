#include "rewair_settings.h"

#include <stdio.h>
#include <string.h>

#include "wiced.h"
#include "wiced_framework.h"
#include "rewair_frames.h"
#include "rewair_state.h"
#include "rewair_tz.h"

static wiced_mutex_t settings_mutex;
static rewair_settings_t current_settings;
static uint8_t settings_ready = 0u;
static uint8_t current_settings_persisted = 0u;
static int16_t last_context_offset_min = 0;
static uint8_t context_offset_known = 0u;

enum
{
    REWAIR_SETTINGS_EFFECT_DISPLAY = 1u << 0,
    REWAIR_SETTINGS_EFFECT_SLEEP   = 1u << 1
};

/* A setting becomes authoritative once DCT and shared state are updated, even
 * if the subsequent F103 command fails. Remember that divergence so an
 * identical transport retry can repair hardware without rewriting flash. */
static uint8_t pending_hardware_effects = 0u;
static uint32_t pending_display_match_fields = 0u;

static void set_defaults( rewair_settings_t* s )
{
    memset( s, 0, sizeof( *s ) );
    s->magic = REWAIR_SETTINGS_MAGIC;
    strcpy( s->name, "Rewair" );
    s->sleep_mode = REWAIR_SLEEP_UNKNOWN;
    strcpy( s->tz_posix, "WET0WEST,M3.5.0/1,M10.5.0" );
    strcpy( s->tz_zone, "Europe/Lisbon" );
}

static int string_is_terminated( const char* value, uint32_t size )
{
    return memchr( value, '\0', size ) != NULL;
}

static uint32_t settings_diff( const rewair_settings_t* a, const rewair_settings_t* b )
{
    uint32_t fields = 0u;

    if ( strcmp( a->name, b->name ) != 0 )
    {
        fields |= REWAIR_SETTINGS_FIELD_NAME;
    }
    if ( a->units != b->units )
    {
        fields |= REWAIR_SETTINGS_FIELD_UNITS;
    }
    if ( a->time_mode != b->time_mode )
    {
        fields |= REWAIR_SETTINGS_FIELD_TIME_MODE;
    }
    if ( a->disp_mode != b->disp_mode )
    {
        fields |= REWAIR_SETTINGS_FIELD_DISP_MODE;
    }
    if ( a->sleep_mode != b->sleep_mode )
    {
        fields |= REWAIR_SETTINGS_FIELD_SLEEP_MODE;
    }
    if ( strcmp( a->tz_posix, b->tz_posix ) != 0 )
    {
        fields |= REWAIR_SETTINGS_FIELD_TZ_POSIX;
    }
    if ( strcmp( a->tz_zone, b->tz_zone ) != 0 )
    {
        fields |= REWAIR_SETTINGS_FIELD_TZ_ZONE;
    }
    return fields;
}

/* Loads and sanitizes the legacy DCT record. `persisted` is true only when
 * the returned object exactly matches a current-format record in flash. */
static void settings_load_dct( rewair_settings_t* out, uint8_t* persisted )
{
    rewair_settings_t* stored = NULL;
    rewair_settings_t original;
    rewair_settings_t defaults;

    *persisted = 0u;
    set_defaults( &defaults );

    if ( wiced_dct_read_lock( (void**)&stored, WICED_FALSE, DCT_APP_SECTION,
                              0, sizeof( *stored ) ) != WICED_SUCCESS )
    {
        set_defaults( out );
        return;
    }
    if ( stored->magic != REWAIR_SETTINGS_MAGIC &&
         stored->magic != REWAIR_SETTINGS_MAGIC_LEGACY )
    {
        set_defaults( out );
    }
    else
    {
        original = *stored;
        *out = *stored;
        out->name[sizeof( out->name ) - 1u] = '\0';
        out->tz_posix[sizeof( out->tz_posix ) - 1u] = '\0';
        out->tz_zone[sizeof( out->tz_zone ) - 1u] = '\0';

        /* RWR2 reserved this byte as a zero-filled pad, so it cannot tell us
         * which persistent SLEP mode the stock F103 currently holds. Keep all
         * other settings, but report the display policy as unknown until the
         * user explicitly selects one in the web UI. */
        if ( stored->magic == REWAIR_SETTINGS_MAGIC_LEGACY )
        {
            out->magic = REWAIR_SETTINGS_MAGIC;
            out->sleep_mode = REWAIR_SLEEP_UNKNOWN;
        }
        else if ( out->sleep_mode > REWAIR_SLEEP_UNKNOWN )
        {
            out->sleep_mode = REWAIR_SLEEP_UNKNOWN;
        }

        if ( out->units > REWAIR_UNITS_F )
        {
            out->units = REWAIR_UNITS_C;
        }
        if ( out->time_mode > REWAIR_TIME_MANUAL )
        {
            out->time_mode = REWAIR_TIME_AUTO;
        }
        if ( out->disp_mode > REWAIR_DISP_SENSORS )
        {
            out->disp_mode = REWAIR_DISP_SCORE;
        }
        {
            rewair_tz_rule_t rule;
            if ( rewair_tz_parse( out->tz_posix, &rule ) != 0 )
            {
                strcpy( out->tz_posix, defaults.tz_posix );
                strcpy( out->tz_zone, defaults.tz_zone );
            }
        }

        if ( original.magic == REWAIR_SETTINGS_MAGIC &&
             memcmp( &original, out, sizeof( original ) ) == 0 )
        {
            *persisted = 1u;
        }
    }
    wiced_dct_read_unlock( stored, WICED_FALSE );
}

static rewair_settings_result_t settings_save_dct( const rewair_settings_t* s )
{
    wiced_result_t result = wiced_dct_write( s, DCT_APP_SECTION, 0, sizeof( *s ) );
    printf( "[settings] save %s\n", result == WICED_SUCCESS ? "ok" : "FAILED" );
    return result == WICED_SUCCESS ? REWAIR_SETTINGS_OK : REWAIR_SETTINGS_ERR_STORAGE;
}

static void settings_apply_to_state( const rewair_settings_t* s )
{
    rewair_tz_rule_t rule;
    int16_t offset_min = 0;
    uint8_t dst = 0u;
    wiced_utc_time_t now = 0u;

    if ( rewair_tz_parse( s->tz_posix, &rule ) == 0 )
    {
        wiced_time_get_utc_time( &now );
        rewair_tz_eval( &rule, (uint32_t)now, &offset_min, &dst );
    }
    rewair_state_set_settings( s->name, s->units, s->time_mode, s->disp_mode,
                               s->sleep_mode,
                               s->tz_zone, s->tz_posix, offset_min, dst );
}

static int patch_is_valid( const rewair_settings_patch_t* patch,
                           rewair_tz_rule_t* parsed_rule )
{
    if ( patch == NULL || ( patch->fields & ~REWAIR_SETTINGS_FIELD_ALL ) != 0u )
    {
        return 0;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_NAME ) != 0u &&
         !string_is_terminated( patch->name, sizeof( patch->name ) ) )
    {
        return 0;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_UNITS ) != 0u &&
         patch->units > REWAIR_UNITS_F )
    {
        return 0;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_TIME_MODE ) != 0u &&
         patch->time_mode > REWAIR_TIME_MANUAL )
    {
        return 0;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_DISP_MODE ) != 0u &&
         patch->disp_mode > REWAIR_DISP_SENSORS )
    {
        return 0;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_SLEEP_MODE ) != 0u &&
         patch->sleep_mode > REWAIR_SLEEP_UNKNOWN )
    {
        return 0;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_TZ_POSIX ) != 0u )
    {
        if ( !string_is_terminated( patch->tz_posix, sizeof( patch->tz_posix ) ) ||
             patch->tz_posix[0] == '\0' ||
             rewair_tz_parse( patch->tz_posix, parsed_rule ) != 0 )
        {
            return 0;
        }
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_TZ_ZONE ) != 0u &&
         !string_is_terminated( patch->tz_zone, sizeof( patch->tz_zone ) ) )
    {
        return 0;
    }
    return 1;
}

static void patch_candidate( rewair_settings_t* candidate,
                             const rewair_settings_patch_t* patch )
{
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_NAME ) != 0u )
    {
        strcpy( candidate->name, patch->name );
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_UNITS ) != 0u )
    {
        candidate->units = patch->units;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_TIME_MODE ) != 0u )
    {
        candidate->time_mode = patch->time_mode;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_DISP_MODE ) != 0u )
    {
        candidate->disp_mode = patch->disp_mode;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_SLEEP_MODE ) != 0u )
    {
        candidate->sleep_mode = patch->sleep_mode;
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_TZ_POSIX ) != 0u )
    {
        strcpy( candidate->tz_posix, patch->tz_posix );
    }
    if ( ( patch->fields & REWAIR_SETTINGS_FIELD_TZ_ZONE ) != 0u )
    {
        strcpy( candidate->tz_zone, patch->tz_zone );
    }
    candidate->magic = REWAIR_SETTINGS_MAGIC;
}

static rewair_settings_result_t apply_hardware_effects( uint8_t effects,
                                                        uint32_t display_match_fields )
{
    rewair_settings_result_t result = REWAIR_SETTINGS_OK;

    if ( ( effects & REWAIR_SETTINGS_EFFECT_DISPLAY ) != 0u )
    {
        static const char* names[] = { "score", "clock", "sensors" };

        if ( sensor_send_disp_mode( names[current_settings.disp_mode] ) == WICED_SUCCESS )
        {
            pending_hardware_effects &= (uint8_t)~REWAIR_SETTINGS_EFFECT_DISPLAY;
            pending_display_match_fields = 0u;
        }
        else
        {
            pending_hardware_effects |= REWAIR_SETTINGS_EFFECT_DISPLAY;
            pending_display_match_fields = display_match_fields;
            result = REWAIR_SETTINGS_ERR_DISPLAY;
        }
    }
    if ( ( effects & REWAIR_SETTINGS_EFFECT_SLEEP ) != 0u )
    {
        if ( sensor_send_sleep_mode( current_settings.sleep_mode ) == WICED_SUCCESS )
        {
            pending_hardware_effects &= (uint8_t)~REWAIR_SETTINGS_EFFECT_SLEEP;
        }
        else
        {
            pending_hardware_effects |= REWAIR_SETTINGS_EFFECT_SLEEP;
            result = REWAIR_SETTINGS_ERR_DISPLAY;
        }
    }
    return result;
}

static rewair_settings_result_t apply_runtime_side_effects( uint32_t changed_fields,
                                                            const rewair_tz_rule_t* parsed_rule )
{
    rewair_settings_result_t result = REWAIR_SETTINGS_OK;
    wiced_utc_time_t timezone_now = 0u;
    uint8_t timezone_now_valid = 0u;
    uint8_t hardware_effects = 0u;
    uint32_t display_match_fields = 0u;

    if ( ( changed_fields & REWAIR_SETTINGS_FIELD_TZ_POSIX ) != 0u )
    {
        sensor_set_tz_rule( parsed_rule );
        if ( wiced_time_get_utc_time( &timezone_now ) == WICED_SUCCESS &&
             timezone_now != 0u )
        {
            timezone_now_valid = 1u;
        }
        else
        {
            context_offset_known = 0u;
        }
    }

    /* State precedes DISP because the aggregate sensors mode consults its
     * unit value while translating to the F103 protocol mode. */
    settings_apply_to_state( &current_settings );

    if ( timezone_now_valid != 0u )
    {
        uint8_t dst = 0u;

        send_time_context( (uint32_t)timezone_now );
        rewair_tz_eval( parsed_rule, (uint32_t)timezone_now,
                        &last_context_offset_min, &dst );
        context_offset_known = 1u;
    }

    if ( ( changed_fields & REWAIR_SETTINGS_FIELD_DISP_MODE ) != 0u )
    {
        display_match_fields |= REWAIR_SETTINGS_FIELD_DISP_MODE;
    }
    if ( ( changed_fields & REWAIR_SETTINGS_FIELD_UNITS ) != 0u &&
         current_settings.disp_mode == REWAIR_DISP_SENSORS )
    {
        display_match_fields |= REWAIR_SETTINGS_FIELD_UNITS;
    }
    if ( display_match_fields != 0u )
    {
        hardware_effects |= REWAIR_SETTINGS_EFFECT_DISPLAY;
    }
    if ( ( changed_fields & REWAIR_SETTINGS_FIELD_SLEEP_MODE ) != 0u &&
         current_settings.sleep_mode != REWAIR_SLEEP_UNKNOWN )
    {
        hardware_effects |= REWAIR_SETTINGS_EFFECT_SLEEP;
    }
    else if ( ( changed_fields & REWAIR_SETTINGS_FIELD_SLEEP_MODE ) != 0u )
    {
        /* UNKNOWN is an internal reset state with no corresponding F103
         * command, so an older failed sleep command is no longer retryable. */
        pending_hardware_effects &= (uint8_t)~REWAIR_SETTINGS_EFFECT_SLEEP;
    }

    result = apply_hardware_effects( hardware_effects, display_match_fields );
    return result;
}

rewair_settings_result_t rewair_settings_init( void )
{
    rewair_tz_rule_t rule;

    if ( wiced_rtos_init_mutex( &settings_mutex ) != WICED_SUCCESS )
    {
        printf( "[settings] mutex init failed\n" );
        return REWAIR_SETTINGS_ERR_NOT_READY;
    }
    settings_load_dct( &current_settings, &current_settings_persisted );
    if ( rewair_tz_parse( current_settings.tz_posix, &rule ) != 0 )
    {
        /* settings_load_dct already replaces invalid rules; this is defensive. */
        return REWAIR_SETTINGS_ERR_INVALID;
    }
    sensor_set_tz_rule( &rule );
    pending_hardware_effects = 0u;
    pending_display_match_fields = 0u;
    settings_ready = 1u;
    settings_apply_to_state( &current_settings );
    return REWAIR_SETTINGS_OK;
}

rewair_settings_result_t rewair_settings_get( rewair_settings_t* out )
{
    if ( out == NULL )
    {
        return REWAIR_SETTINGS_ERR_INVALID;
    }
    if ( settings_ready == 0u )
    {
        return REWAIR_SETTINGS_ERR_NOT_READY;
    }
    wiced_rtos_lock_mutex( &settings_mutex );
    *out = current_settings;
    wiced_rtos_unlock_mutex( &settings_mutex );
    return REWAIR_SETTINGS_OK;
}

rewair_settings_result_t rewair_settings_apply_patch( const rewair_settings_patch_t* patch,
                                                      rewair_settings_t* out,
                                                      uint32_t* changed_fields )
{
    rewair_settings_t candidate;
    rewair_tz_rule_t parsed_rule;
    uint32_t changed;
    uint8_t retry_effects = 0u;
    rewair_settings_result_t result;

    if ( changed_fields != NULL )
    {
        *changed_fields = 0u;
    }
    if ( !patch_is_valid( patch, &parsed_rule ) )
    {
        return REWAIR_SETTINGS_ERR_INVALID;
    }
    if ( settings_ready == 0u )
    {
        return REWAIR_SETTINGS_ERR_NOT_READY;
    }

    wiced_rtos_lock_mutex( &settings_mutex );
    candidate = current_settings;
    patch_candidate( &candidate, patch );
    changed = settings_diff( &current_settings, &candidate );

    if ( changed == 0u && current_settings_persisted != 0u )
    {
        if ( ( pending_hardware_effects & REWAIR_SETTINGS_EFFECT_DISPLAY ) != 0u &&
             ( patch->fields & pending_display_match_fields ) != 0u )
        {
            retry_effects |= REWAIR_SETTINGS_EFFECT_DISPLAY;
        }
        if ( ( pending_hardware_effects & REWAIR_SETTINGS_EFFECT_SLEEP ) != 0u &&
             ( patch->fields & REWAIR_SETTINGS_FIELD_SLEEP_MODE ) != 0u )
        {
            retry_effects |= REWAIR_SETTINGS_EFFECT_SLEEP;
        }
        if ( retry_effects != 0u )
        {
            result = apply_hardware_effects( retry_effects,
                                             pending_display_match_fields );
            if ( out != NULL )
            {
                *out = current_settings;
            }
            wiced_rtos_unlock_mutex( &settings_mutex );
            return result;
        }
        if ( out != NULL )
        {
            *out = current_settings;
        }
        wiced_rtos_unlock_mutex( &settings_mutex );
        return REWAIR_SETTINGS_UNCHANGED;
    }

    result = settings_save_dct( &candidate );
    if ( result != REWAIR_SETTINGS_OK )
    {
        if ( out != NULL )
        {
            *out = current_settings;
        }
        wiced_rtos_unlock_mutex( &settings_mutex );
        return result;
    }

    current_settings = candidate;
    current_settings_persisted = 1u;
    if ( changed_fields != NULL )
    {
        *changed_fields = changed;
    }
    result = changed == 0u ? REWAIR_SETTINGS_OK :
             apply_runtime_side_effects( changed, &parsed_rule );
    if ( out != NULL )
    {
        *out = current_settings;
    }
    wiced_rtos_unlock_mutex( &settings_mutex );
    return result;
}

rewair_settings_result_t rewair_settings_set_units( uint8_t units,
                                                    rewair_settings_t* out )
{
    rewair_settings_patch_t patch;

    memset( &patch, 0, sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_UNITS;
    patch.units = units;
    return rewair_settings_apply_patch( &patch, out, NULL );
}

rewair_settings_result_t rewair_settings_set_disp_mode( uint8_t disp_mode,
                                                        rewair_settings_t* out )
{
    rewair_settings_patch_t patch;

    memset( &patch, 0, sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_DISP_MODE;
    patch.disp_mode = disp_mode;
    return rewair_settings_apply_patch( &patch, out, NULL );
}

rewair_settings_result_t rewair_settings_set_sleep_mode( uint8_t sleep_mode,
                                                         rewair_settings_t* out )
{
    rewair_settings_patch_t patch;

    if ( sleep_mode > REWAIR_SLEEP_SLEEP )
    {
        return REWAIR_SETTINGS_ERR_INVALID;
    }
    memset( &patch, 0, sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_SLEEP_MODE;
    patch.sleep_mode = sleep_mode;
    return rewair_settings_apply_patch( &patch, out, NULL );
}

rewair_settings_result_t rewair_settings_refresh_state( void )
{
    if ( settings_ready == 0u )
    {
        return REWAIR_SETTINGS_ERR_NOT_READY;
    }
    wiced_rtos_lock_mutex( &settings_mutex );
    settings_apply_to_state( &current_settings );
    wiced_rtos_unlock_mutex( &settings_mutex );
    return REWAIR_SETTINGS_OK;
}

rewair_settings_result_t rewair_settings_time_tick( uint32_t utc_seconds )
{
    rewair_tz_rule_t rule;
    int16_t offset_min = 0;
    uint8_t dst = 0u;

    if ( settings_ready == 0u )
    {
        return REWAIR_SETTINGS_ERR_NOT_READY;
    }
    wiced_rtos_lock_mutex( &settings_mutex );
    if ( rewair_tz_parse( current_settings.tz_posix, &rule ) != 0 )
    {
        wiced_rtos_unlock_mutex( &settings_mutex );
        return REWAIR_SETTINGS_ERR_INVALID;
    }
    rewair_tz_eval( &rule, utc_seconds, &offset_min, &dst );
    if ( context_offset_known != 0u && offset_min == last_context_offset_min )
    {
        wiced_rtos_unlock_mutex( &settings_mutex );
        return REWAIR_SETTINGS_UNCHANGED;
    }

    sensor_set_tz_rule( &rule );
    settings_apply_to_state( &current_settings );
    send_time_context( utc_seconds );
    last_context_offset_min = offset_min;
    context_offset_known = 1u;
    wiced_rtos_unlock_mutex( &settings_mutex );
    return REWAIR_SETTINGS_OK;
}

rewair_settings_result_t rewair_settings_reset_defaults( rewair_settings_t* out )
{
    rewair_settings_t defaults;
    rewair_settings_patch_t patch;

    set_defaults( &defaults );
    memset( &patch, 0, sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_ALL;
    strcpy( patch.name, defaults.name );
    patch.units = defaults.units;
    patch.time_mode = defaults.time_mode;
    patch.disp_mode = defaults.disp_mode;
    patch.sleep_mode = defaults.sleep_mode;
    strcpy( patch.tz_posix, defaults.tz_posix );
    strcpy( patch.tz_zone, defaults.tz_zone );
    return rewair_settings_apply_patch( &patch, out, NULL );
}
