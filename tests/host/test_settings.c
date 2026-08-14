#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rewair_settings.h"
#include "rewair_state.h"
#include "rewair_tz.h"
#include "wiced.h"
#include "wiced_framework.h"

#define CHECK( expression ) do { \
    if ( !( expression ) ) { \
        fprintf( stderr, "CHECK failed at %s:%d: %s\n", \
                 __FILE__, __LINE__, #expression ); \
        exit( 1 ); \
    } \
} while ( 0 )

static rewair_settings_t dct_settings;
static wiced_utc_time_t mock_now = 1704067200u; /* 2024-01-01 */
static int write_result = WICED_SUCCESS;
static uint32_t write_count;
static uint32_t state_count;
static uint32_t disp_count;
static uint32_t sleep_count;
static uint32_t time_context_count;
static uint32_t tz_rule_count;
static int disp_result = WICED_SUCCESS;
static int sleep_result = WICED_SUCCESS;
static rewair_settings_t runtime_settings;
static char last_disp_mode[16];

wiced_result_t wiced_rtos_init_mutex( wiced_mutex_t* mutex )
{
    return pthread_mutex_init( mutex, NULL ) == 0 ? WICED_SUCCESS : WICED_ERROR;
}

wiced_result_t wiced_rtos_lock_mutex( wiced_mutex_t* mutex )
{
    return pthread_mutex_lock( mutex ) == 0 ? WICED_SUCCESS : WICED_ERROR;
}

wiced_result_t wiced_rtos_unlock_mutex( wiced_mutex_t* mutex )
{
    return pthread_mutex_unlock( mutex ) == 0 ? WICED_SUCCESS : WICED_ERROR;
}

wiced_result_t wiced_time_get_utc_time( wiced_utc_time_t* utc_time )
{
    *utc_time = mock_now;
    return WICED_SUCCESS;
}

wiced_result_t wiced_dct_read_lock( void** info_ptr, int ptr_is_writable,
                                    int section, uint32_t offset, uint32_t size )
{
    (void)ptr_is_writable;
    (void)section;
    (void)offset;
    CHECK( size == sizeof( dct_settings ) );
    *info_ptr = &dct_settings;
    return WICED_SUCCESS;
}

wiced_result_t wiced_dct_read_unlock( void* info_ptr, int ptr_is_writable )
{
    (void)info_ptr;
    (void)ptr_is_writable;
    return WICED_SUCCESS;
}

wiced_result_t wiced_dct_write( const void* info_ptr, int section,
                                uint32_t offset, uint32_t size )
{
    (void)section;
    (void)offset;
    CHECK( size == sizeof( dct_settings ) );
    write_count++;
    if ( write_result == WICED_SUCCESS )
    {
        memcpy( &dct_settings, info_ptr, size );
    }
    return write_result;
}

void rewair_state_set_settings( const char* name, uint8_t units, uint8_t time_mode,
                                uint8_t disp_mode, uint8_t sleep_mode, const char* tz_zone,
                                const char* tz_posix, int16_t tz_offset_min, uint8_t tz_dst )
{
    memset( &runtime_settings, 0, sizeof( runtime_settings ) );
    strcpy( runtime_settings.name, name );
    runtime_settings.units = units;
    runtime_settings.time_mode = time_mode;
    runtime_settings.disp_mode = disp_mode;
    runtime_settings.sleep_mode = sleep_mode;
    strcpy( runtime_settings.tz_zone, tz_zone );
    strcpy( runtime_settings.tz_posix, tz_posix );
    (void)tz_offset_min;
    (void)tz_dst;
    state_count++;
}

void sensor_set_tz_rule( const rewair_tz_rule_t* rule )
{
    (void)rule;
    tz_rule_count++;
}

void send_time_context( uint32_t utc_seconds )
{
    (void)utc_seconds;
    time_context_count++;
}

wiced_result_t sensor_send_disp_mode( const char* mode )
{
    strcpy( last_disp_mode, mode );
    disp_count++;
    return disp_result;
}

wiced_result_t sensor_send_sleep_mode( uint8_t mode )
{
    (void)mode;
    sleep_count++;
    return sleep_result;
}

static void seed_current_settings( void )
{
    memset( &dct_settings, 0, sizeof( dct_settings ) );
    dct_settings.magic = REWAIR_SETTINGS_MAGIC;
    strcpy( dct_settings.name, "Rewair" );
    dct_settings.units = REWAIR_UNITS_C;
    dct_settings.time_mode = REWAIR_TIME_AUTO;
    dct_settings.disp_mode = REWAIR_DISP_SCORE;
    dct_settings.sleep_mode = REWAIR_SLEEP_UNKNOWN;
    strcpy( dct_settings.tz_posix, "WET0WEST,M3.5.0/1,M10.5.0" );
    strcpy( dct_settings.tz_zone, "Europe/Lisbon" );
}

static void test_typed_updates_and_noops( void )
{
    rewair_settings_t current;

    CHECK( rewair_settings_set_units( REWAIR_UNITS_C, NULL ) ==
           REWAIR_SETTINGS_UNCHANGED );
    CHECK( write_count == 0u );
    CHECK( disp_count == 0u );

    CHECK( rewair_settings_set_disp_mode( REWAIR_DISP_SENSORS, NULL ) ==
           REWAIR_SETTINGS_OK );
    CHECK( write_count == 1u );
    CHECK( disp_count == 1u );
    CHECK( strcmp( last_disp_mode, "sensors" ) == 0 );

    CHECK( rewair_settings_set_units( REWAIR_UNITS_F, &current ) ==
           REWAIR_SETTINGS_OK );
    CHECK( current.units == REWAIR_UNITS_F );
    CHECK( write_count == 2u );
    CHECK( disp_count == 2u );
    CHECK( strcmp( last_disp_mode, "sensors" ) == 0 );

    CHECK( rewair_settings_set_units( REWAIR_UNITS_F, NULL ) ==
           REWAIR_SETTINGS_UNCHANGED );
    CHECK( write_count == 2u );
    CHECK( disp_count == 2u );

    CHECK( rewair_settings_set_sleep_mode( REWAIR_SLEEP_UNKNOWN, NULL ) ==
           REWAIR_SETTINGS_ERR_INVALID );
    CHECK( rewair_settings_set_sleep_mode( REWAIR_SLEEP_ON, NULL ) ==
           REWAIR_SETTINGS_OK );
    CHECK( sleep_count == 1u );
    CHECK( runtime_settings.sleep_mode == REWAIR_SLEEP_ON );
}

static void test_atomic_patch_and_validation( void )
{
    rewair_settings_patch_t patch;
    rewair_settings_t current;
    uint32_t changed = 0u;
    uint32_t writes_before = write_count;

    memset( &patch, 0, sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_NAME |
                   REWAIR_SETTINGS_FIELD_TIME_MODE |
                   REWAIR_SETTINGS_FIELD_DISP_MODE;
    strcpy( patch.name, "Bedroom" );
    patch.time_mode = REWAIR_TIME_MANUAL;
    patch.disp_mode = REWAIR_DISP_CLOCK;
    CHECK( rewair_settings_apply_patch( &patch, &current, &changed ) ==
           REWAIR_SETTINGS_OK );
    CHECK( write_count == writes_before + 1u );
    CHECK( strcmp( current.name, "Bedroom" ) == 0 );
    CHECK( current.time_mode == REWAIR_TIME_MANUAL );
    CHECK( current.disp_mode == REWAIR_DISP_CLOCK );
    CHECK( changed == patch.fields );

    memset( &patch, 0, sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_DISP_MODE;
    patch.disp_mode = 99u;
    CHECK( rewair_settings_apply_patch( &patch, NULL, NULL ) ==
           REWAIR_SETTINGS_ERR_INVALID );
    CHECK( write_count == writes_before + 1u );

    memset( &patch, 'x', sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_NAME;
    CHECK( rewair_settings_apply_patch( &patch, NULL, NULL ) ==
           REWAIR_SETTINGS_ERR_INVALID );
    CHECK( write_count == writes_before + 1u );
}

static void test_failure_semantics( void )
{
    rewair_settings_t current;
    uint32_t state_before = state_count;
    uint32_t disp_before = disp_count;
    uint32_t writes_before;

    write_result = WICED_ERROR;
    CHECK( rewair_settings_set_units( REWAIR_UNITS_C, &current ) ==
           REWAIR_SETTINGS_ERR_STORAGE );
    CHECK( current.units == REWAIR_UNITS_F );
    CHECK( state_count == state_before );
    CHECK( disp_count == disp_before );

    write_result = WICED_SUCCESS;
    disp_result = WICED_ERROR;
    writes_before = write_count;
    state_before = state_count;
    disp_before = disp_count;
    CHECK( rewair_settings_set_disp_mode( REWAIR_DISP_SCORE, &current ) ==
           REWAIR_SETTINGS_ERR_DISPLAY );
    CHECK( current.disp_mode == REWAIR_DISP_SCORE );
    CHECK( dct_settings.disp_mode == REWAIR_DISP_SCORE );
    CHECK( runtime_settings.disp_mode == REWAIR_DISP_SCORE );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 1u );

    /* The identical retry repairs only the failed UART effect. */
    disp_result = WICED_SUCCESS;
    CHECK( rewair_settings_set_disp_mode( REWAIR_DISP_SCORE, &current ) ==
           REWAIR_SETTINGS_OK );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 2u );

    /* Once repaired, another identical command is a true no-op. */
    CHECK( rewair_settings_set_disp_mode( REWAIR_DISP_SCORE, NULL ) ==
           REWAIR_SETTINGS_UNCHANGED );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 2u );
}

static void test_sensors_unit_display_retry( void )
{
    rewair_settings_t current;
    uint32_t writes_before;
    uint32_t state_before;
    uint32_t disp_before;

    CHECK( rewair_settings_set_disp_mode( REWAIR_DISP_SENSORS, NULL ) ==
           REWAIR_SETTINGS_OK );
    writes_before = write_count;
    state_before = state_count;
    disp_before = disp_count;

    disp_result = WICED_ERROR;
    CHECK( rewair_settings_set_units( REWAIR_UNITS_C, &current ) ==
           REWAIR_SETTINGS_ERR_DISPLAY );
    CHECK( current.units == REWAIR_UNITS_C );
    CHECK( dct_settings.units == REWAIR_UNITS_C );
    CHECK( runtime_settings.units == REWAIR_UNITS_C );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 1u );

    disp_result = WICED_SUCCESS;
    CHECK( rewair_settings_set_units( REWAIR_UNITS_C, NULL ) ==
           REWAIR_SETTINGS_OK );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 2u );

    CHECK( rewair_settings_set_units( REWAIR_UNITS_C, NULL ) ==
           REWAIR_SETTINGS_UNCHANGED );
    CHECK( disp_count == disp_before + 2u );
}

static void test_sleep_retry( void )
{
    rewair_settings_t current;
    uint32_t writes_before = write_count;
    uint32_t state_before = state_count;
    uint32_t sleep_before = sleep_count;

    sleep_result = WICED_ERROR;
    CHECK( rewair_settings_set_sleep_mode( REWAIR_SLEEP_SLEEP, &current ) ==
           REWAIR_SETTINGS_ERR_DISPLAY );
    CHECK( current.sleep_mode == REWAIR_SLEEP_SLEEP );
    CHECK( dct_settings.sleep_mode == REWAIR_SLEEP_SLEEP );
    CHECK( runtime_settings.sleep_mode == REWAIR_SLEEP_SLEEP );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( sleep_count == sleep_before + 1u );

    sleep_result = WICED_SUCCESS;
    CHECK( rewair_settings_set_sleep_mode( REWAIR_SLEEP_SLEEP, NULL ) ==
           REWAIR_SETTINGS_OK );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( sleep_count == sleep_before + 2u );

    CHECK( rewair_settings_set_sleep_mode( REWAIR_SLEEP_SLEEP, NULL ) ==
           REWAIR_SETTINGS_UNCHANGED );
    CHECK( sleep_count == sleep_before + 2u );
}

static void test_display_and_sleep_retry_independently( void )
{
    rewair_settings_patch_t patch;
    rewair_settings_t current;
    uint32_t changed = 0u;
    uint32_t writes_before = write_count;
    uint32_t state_before = state_count;
    uint32_t disp_before = disp_count;
    uint32_t sleep_before = sleep_count;

    memset( &patch, 0, sizeof( patch ) );
    patch.fields = REWAIR_SETTINGS_FIELD_DISP_MODE |
                   REWAIR_SETTINGS_FIELD_SLEEP_MODE;
    patch.disp_mode = REWAIR_DISP_CLOCK;
    patch.sleep_mode = REWAIR_SLEEP_DIM;
    disp_result = WICED_ERROR;
    sleep_result = WICED_ERROR;
    CHECK( rewair_settings_apply_patch( &patch, &current, &changed ) ==
           REWAIR_SETTINGS_ERR_DISPLAY );
    CHECK( changed == patch.fields );
    CHECK( current.disp_mode == REWAIR_DISP_CLOCK );
    CHECK( current.sleep_mode == REWAIR_SLEEP_DIM );
    CHECK( dct_settings.disp_mode == REWAIR_DISP_CLOCK );
    CHECK( dct_settings.sleep_mode == REWAIR_SLEEP_DIM );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 1u );
    CHECK( sleep_count == sleep_before + 1u );

    /* Both effects retry; display succeeds while sleep remains pending. */
    disp_result = WICED_SUCCESS;
    changed = 99u;
    CHECK( rewair_settings_apply_patch( &patch, NULL, &changed ) ==
           REWAIR_SETTINGS_ERR_DISPLAY );
    CHECK( changed == 0u );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 2u );
    CHECK( sleep_count == sleep_before + 2u );

    /* Only the still-pending sleep effect is sent on the next retry. */
    sleep_result = WICED_SUCCESS;
    CHECK( rewair_settings_apply_patch( &patch, NULL, NULL ) ==
           REWAIR_SETTINGS_OK );
    CHECK( write_count == writes_before + 1u );
    CHECK( state_count == state_before + 1u );
    CHECK( disp_count == disp_before + 2u );
    CHECK( sleep_count == sleep_before + 3u );

    CHECK( rewair_settings_apply_patch( &patch, NULL, NULL ) ==
           REWAIR_SETTINGS_UNCHANGED );
    CHECK( disp_count == disp_before + 2u );
    CHECK( sleep_count == sleep_before + 3u );
}

static void test_time_tick_deduplicates( void )
{
    uint32_t before = time_context_count;

    CHECK( rewair_settings_time_tick( 1704067200u ) == REWAIR_SETTINGS_OK );
    CHECK( time_context_count == before + 1u );
    CHECK( rewair_settings_time_tick( 1704067200u ) == REWAIR_SETTINGS_UNCHANGED );
    CHECK( time_context_count == before + 1u );
    CHECK( rewair_settings_time_tick( 1719792000u ) == REWAIR_SETTINGS_OK );
    CHECK( time_context_count == before + 2u );
}

int main( void )
{
    rewair_settings_t current;

    seed_current_settings( );
    CHECK( rewair_settings_init( ) == REWAIR_SETTINGS_OK );
    CHECK( rewair_settings_get( &current ) == REWAIR_SETTINGS_OK );
    CHECK( current.magic == REWAIR_SETTINGS_MAGIC );
    CHECK( state_count == 1u );
    CHECK( tz_rule_count == 1u );
    CHECK( write_count == 0u );

    test_typed_updates_and_noops( );
    test_atomic_patch_and_validation( );
    test_failure_semantics( );
    test_sensors_unit_display_retry( );
    test_sleep_retry( );
    test_display_and_sleep_retry_independently( );
    test_time_tick_deduplicates( );

    printf( "test_settings OK\n" );
    return 0;
}
