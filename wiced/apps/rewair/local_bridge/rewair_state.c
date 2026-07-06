#include "rewair_state.h"

#include <string.h>

#include "wiced.h"
#include "rewair_drops.h"
#include "rewair_version.h"

#define MAX_LISTENERS 4u

static rewair_status_t state;
static wiced_mutex_t   state_mutex;
static rewair_state_listener_t listeners[MAX_LISTENERS];
static uint32_t listener_count = 0u;
static rewair_drops_t drops;

static uint32_t uptime_s( void )
{
    wiced_time_t now_ms = 0;
    wiced_time_get_time( &now_ms );
    return (uint32_t)now_ms / 1000u;
}

static void copy_str( char* dst, uint32_t dst_size, const char* src )
{
    uint32_t i = 0u;
    if ( src != NULL )
    {
        while ( src[i] != '\0' && i + 1u < dst_size )
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void notify( void )
{
    uint32_t i;
    for ( i = 0u; i < listener_count; i++ )
    {
        listeners[i]( );
    }
}

void rewair_state_init( void )
{
    memset( &state, 0, sizeof( state ) );
    rewair_drops_init( &drops );
    copy_str( state.fw, sizeof( state.fw ), REWAIR_FW_VERSION );
    copy_str( state.score_color, sizeof( state.score_color ), "green" );
    wiced_rtos_init_mutex( &state_mutex );
}

int rewair_state_subscribe( rewair_state_listener_t cb )
{
    if ( listener_count >= MAX_LISTENERS )
    {
        return -1;
    }
    listeners[listener_count++] = cb;
    return 0;
}

void rewair_state_snapshot( rewair_status_t* out )
{
    wiced_rtos_lock_mutex( &state_mutex );
    state.drops = rewair_drops_total( &drops, uptime_s( ) );
    *out = state;
    wiced_rtos_unlock_mutex( &state_mutex );
}

void rewair_state_set_sens( const rewair_sens_t* sens, uint32_t score, const char* color,
                            const uint8_t idx[5] )
{
    wiced_rtos_lock_mutex( &state_mutex );
    state.sens = *sens;
    state.sens_valid = 1u;
    state.score = score;
    copy_str( state.score_color, sizeof( state.score_color ), color );
    state.idx_temp = idx[0];
    state.idx_humid = idx[1];
    state.idx_co2 = idx[2];
    state.idx_voc = idx[3];
    state.idx_dust = idx[4];
    state.seq++;
    wiced_rtos_unlock_mutex( &state_mutex );
    notify( );
}

void rewair_state_set_wifi_sta( const char* ssid, int32_t rssi, const char* ip,
                                const char* gw, const char* dns, const char* mac,
                                uint32_t saved_count )
{
    wiced_rtos_lock_mutex( &state_mutex );
    state.wifi_mode = 0u;
    copy_str( state.ssid, sizeof( state.ssid ), ssid );
    state.rssi = rssi;
    copy_str( state.ip, sizeof( state.ip ), ip );
    copy_str( state.gw, sizeof( state.gw ), gw );
    copy_str( state.dns, sizeof( state.dns ), dns );
    copy_str( state.mac, sizeof( state.mac ), mac );
    state.saved_count = saved_count;
    if ( state.connected_s == 0u )
    {
        state.connected_s = uptime_s( );
    }
    state.seq++;
    wiced_rtos_unlock_mutex( &state_mutex );
    notify( );
}

void rewair_state_wifi_drop( void )
{
    wiced_rtos_lock_mutex( &state_mutex );
    rewair_drops_record( &drops, uptime_s( ) );
    state.connected_s = 0u;
    state.seq++;
    wiced_rtos_unlock_mutex( &state_mutex );
    notify( );
}

void rewair_state_set_time( uint32_t epoch, uint8_t synced )
{
    wiced_rtos_lock_mutex( &state_mutex );
    state.time_valid = 1u;
    state.time_synced = synced;
    state.epoch = epoch;
    state.seq++;
    wiced_rtos_unlock_mutex( &state_mutex );
    notify( );
}

void rewair_state_set_settings( const char* name, uint8_t units, uint8_t time_mode,
                                uint8_t disp_mode, const char* tz_zone, const char* tz_posix,
                                int16_t tz_offset_min, uint8_t tz_dst )
{
    wiced_rtos_lock_mutex( &state_mutex );
    copy_str( state.name, sizeof( state.name ), name );
    state.units = units;
    state.time_mode = time_mode;
    state.disp_mode = disp_mode;
    copy_str( state.tz_zone, sizeof( state.tz_zone ), tz_zone );
    copy_str( state.tz_posix, sizeof( state.tz_posix ), tz_posix );
    state.tz_offset_min = tz_offset_min;
    state.tz_dst = tz_dst;
    state.seq++;
    wiced_rtos_unlock_mutex( &state_mutex );
    notify( );
}
