#pragma once

#include <stdint.h>

#define REWAIR_SETTINGS_MAGIC 0x52575232u   /* 'RWR2' */

typedef struct
{
    uint32_t magic;
    char     name[32];
    uint8_t  units;          /* 0 = c, 1 = f */
    uint8_t  time_mode;      /* 0 = auto, 1 = manual */
    uint8_t  disp_mode;      /* 0 = score, 1 = clock, 2 = sensors */
    uint8_t  pad;
    char     tz_posix[64];
    char     tz_zone[40];
} rewair_settings_t;

void rewair_settings_load( rewair_settings_t* out );
int  rewair_settings_save( const rewair_settings_t* s );
void rewair_settings_apply_to_state( const rewair_settings_t* s );
