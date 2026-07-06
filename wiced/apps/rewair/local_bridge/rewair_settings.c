#include "rewair_settings.h"

#include <string.h>

#include "wiced.h"
#include "wiced_framework.h"
#include "rewair_state.h"
#include "rewair_tz.h"

static void set_defaults( rewair_settings_t* s )
{
    memset( s, 0, sizeof( *s ) );
    s->magic = REWAIR_SETTINGS_MAGIC;
    strcpy( s->name, "Rewair" );
    strcpy( s->tz_posix, "WET0WEST,M3.5.0/1,M10.5.0" );
    strcpy( s->tz_zone, "Europe/Lisbon" );
}

void rewair_settings_load( rewair_settings_t* out )
{
    rewair_settings_t* stored = NULL;

    if ( wiced_dct_read_lock( (void**)&stored, WICED_FALSE, DCT_APP_SECTION,
                              0, sizeof( *stored ) ) != WICED_SUCCESS )
    {
        set_defaults( out );
        return;
    }
    if ( stored->magic != REWAIR_SETTINGS_MAGIC )
    {
        set_defaults( out );
    }
    else
    {
        *out = *stored;
        out->name[sizeof( out->name ) - 1u] = '\0';
        out->tz_posix[sizeof( out->tz_posix ) - 1u] = '\0';
        out->tz_zone[sizeof( out->tz_zone ) - 1u] = '\0';
    }
    wiced_dct_read_unlock( stored, WICED_FALSE );
}

int rewair_settings_save( const rewair_settings_t* s )
{
    wiced_result_t result = wiced_dct_write( s, DCT_APP_SECTION, 0, sizeof( *s ) );
    printf( "[settings] save %s\n", result == WICED_SUCCESS ? "ok" : "FAILED" );
    return result == WICED_SUCCESS ? 0 : -1;
}

int rewair_settings_reset_defaults( rewair_settings_t* out )
{
    rewair_settings_t defaults;

    set_defaults( &defaults );
    if ( out != NULL )
    {
        *out = defaults;
    }
    return rewair_settings_save( &defaults );
}

void rewair_settings_apply_to_state( const rewair_settings_t* s )
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
                               s->tz_zone, s->tz_posix, offset_min, dst );
}
