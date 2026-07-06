#pragma once

#include <stdint.h>

/* Pure struct — no WICED types, so host tests can build it.
 * Sensor values are centi-units (2240 = 22.40); light is lux. */
typedef struct
{
    int32_t temp;
    int32_t humid;
    int32_t co2;
    int32_t voc;
    int32_t dust;
    int32_t light;
} rewair_sens_t;

typedef struct rewair_status
{
    uint32_t      seq;
    uint32_t      sens_valid;
    rewair_sens_t sens;
    uint32_t      score;
    char          score_color[8];      /* "green" | "amber" | "purple" */
    uint8_t       idx_temp, idx_humid, idx_co2, idx_voc, idx_dust;

    uint8_t       wifi_mode;           /* 0 = sta, 1 = ap */
    char          ssid[33];
    int32_t       rssi;
    char          ip[16], gw[16], dns[16];
    char          mac[18];
    uint32_t      connected_s;
    uint32_t      drops;
    uint32_t      saved_count;
    char          ap_ssid[33];
    char          ap_ip[16];

    uint8_t       time_valid, time_synced;
    uint32_t      epoch;

    char          name[32];
    uint8_t       units;               /* 0 = c, 1 = f */
    uint8_t       time_mode;           /* 0 = auto, 1 = manual */
    uint8_t       disp_mode;           /* 0 = score, 1 = clock, 2 = sensors */
    int16_t       tz_offset_min;
    uint8_t       tz_dst;
    char          tz_zone[40];
    char          tz_posix[64];
    char          fw[24];
} rewair_status_t;

/* ---- firmware-only API, implemented in rewair_state.c (Task 5) ---- */
typedef void ( *rewair_state_listener_t )( void );

void rewair_state_init( void );
void rewair_state_snapshot( rewair_status_t* out );
int  rewair_state_subscribe( rewair_state_listener_t cb );

/* setters called from local_bridge.c; each bumps seq + notifies */
void rewair_state_set_sens( const rewair_sens_t* sens, uint32_t score, const char* color,
                            const uint8_t idx[5] );
void rewair_state_set_wifi_sta( const char* ssid, int32_t rssi, const char* ip,
                                const char* gw, const char* dns, const char* mac,
                                uint32_t saved_count );
void rewair_state_wifi_drop( void );
void rewair_state_set_time( uint32_t epoch, uint8_t synced );
void rewair_state_set_settings( const char* name, uint8_t units, uint8_t time_mode,
                                uint8_t disp_mode, const char* tz_zone, const char* tz_posix,
                                int16_t tz_offset_min, uint8_t tz_dst );
