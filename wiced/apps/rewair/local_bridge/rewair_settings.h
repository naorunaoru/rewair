#pragma once

#include <stdint.h>

#define REWAIR_SETTINGS_MAGIC        0x52575233u   /* 'RWR3' */
#define REWAIR_SETTINGS_MAGIC_LEGACY 0x52575232u   /* 'RWR2' */

enum
{
    REWAIR_UNITS_C = 0u,
    REWAIR_UNITS_F = 1u
};

enum
{
    REWAIR_TIME_AUTO   = 0u,
    REWAIR_TIME_MANUAL = 1u
};

enum
{
    REWAIR_DISP_SCORE   = 0u,
    REWAIR_DISP_CLOCK   = 1u,
    REWAIR_DISP_SENSORS = 2u
};

enum
{
    REWAIR_SLEEP_DIM     = 0u,
    REWAIR_SLEEP_ON      = 1u,
    REWAIR_SLEEP_SLEEP   = 2u,
    REWAIR_SLEEP_UNKNOWN = 3u
};

typedef struct
{
    uint32_t magic;
    char     name[32];
    uint8_t  units;          /* 0 = c, 1 = f */
    uint8_t  time_mode;      /* 0 = auto, 1 = manual */
    uint8_t  disp_mode;      /* 0 = score, 1 = clock, 2 = sensors */
    uint8_t  sleep_mode;     /* dim, on, sleep; unknown until first selection */
    char     tz_posix[64];
    char     tz_zone[40];
} rewair_settings_t;

enum
{
    REWAIR_SETTINGS_FIELD_NAME       = 1u << 0,
    REWAIR_SETTINGS_FIELD_UNITS      = 1u << 1,
    REWAIR_SETTINGS_FIELD_TIME_MODE  = 1u << 2,
    REWAIR_SETTINGS_FIELD_DISP_MODE  = 1u << 3,
    REWAIR_SETTINGS_FIELD_SLEEP_MODE = 1u << 4,
    REWAIR_SETTINGS_FIELD_TZ_POSIX   = 1u << 5,
    REWAIR_SETTINGS_FIELD_TZ_ZONE    = 1u << 6,
    REWAIR_SETTINGS_FIELD_ALL        = ( 1u << 7 ) - 1u
};

/* A fixed-size, transport-neutral settings patch. Only fields selected by
 * `fields` are read. Keeping values owned by the patch (rather than borrowed
 * string pointers) makes it safe to hand between HTTP/MQTT worker contexts. */
typedef struct
{
    uint32_t fields;
    char     name[32];
    uint8_t  units;
    uint8_t  time_mode;
    uint8_t  disp_mode;
    uint8_t  sleep_mode;
    char     tz_posix[64];
    char     tz_zone[40];
} rewair_settings_patch_t;

typedef enum
{
    REWAIR_SETTINGS_OK             = 0,
    REWAIR_SETTINGS_UNCHANGED      = 1,
    REWAIR_SETTINGS_ERR_INVALID    = -1,
    REWAIR_SETTINGS_ERR_STORAGE    = -2,
    REWAIR_SETTINGS_ERR_DISPLAY    = -3,
    REWAIR_SETTINGS_ERR_NOT_READY  = -4
} rewair_settings_result_t;

/* Initializes the synchronized settings owner from DCT and applies the loaded
 * values to runtime state. Call once, after rewair_state_init(), before worker
 * threads are started. */
rewair_settings_result_t rewair_settings_init( void );

/* Returns a serialized snapshot of the current authoritative settings. */
rewair_settings_result_t rewair_settings_get( rewair_settings_t* out );

/* Atomically validates and applies a whole-struct read/patch/save transition.
 * On success, `out` receives the authoritative values and `changed_fields`
 * receives the fields whose values actually changed. Display/UART side-effect
 * failure is reported after the successfully persisted values become current.
 * An identical retry resends only a pending failed UART effect, without a DCT
 * write or state notification; once repaired, later repeats return UNCHANGED. */
rewair_settings_result_t rewair_settings_apply_patch( const rewair_settings_patch_t* patch,
                                                      rewair_settings_t* out,
                                                      uint32_t* changed_fields );

/* Typed control entry points intended for MQTT and other non-HTTP writers. */
rewair_settings_result_t rewair_settings_set_units( uint8_t units,
                                                    rewair_settings_t* out );
rewair_settings_result_t rewair_settings_set_disp_mode( uint8_t disp_mode,
                                                        rewair_settings_t* out );
rewair_settings_result_t rewair_settings_set_sleep_mode( uint8_t sleep_mode,
                                                         rewair_settings_t* out );

/* Re-evaluates the current timezone offset/DST and refreshes runtime state
 * without reading DCT. Used by the periodic DST transition check. */
rewair_settings_result_t rewair_settings_refresh_state( void );

/* Sends timezone/display-clock context only when the current UTC offset has
 * changed since the last settings-owned refresh (including a timezone edit). */
rewair_settings_result_t rewair_settings_time_tick( uint32_t utc_seconds );

/* Resets settings to firmware defaults through the same serialized owner. */
rewair_settings_result_t rewair_settings_reset_defaults( rewair_settings_t* out );
