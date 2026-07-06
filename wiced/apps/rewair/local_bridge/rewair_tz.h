#pragma once

#include <stdint.h>

typedef struct
{
    int16_t  std_offset_min;   /* ISO convention: minutes east of UTC */
    int16_t  dst_offset_min;
    uint8_t  has_dst;
    uint8_t  start_month;      /* M-form rule: month 1-12 */
    uint8_t  start_week;       /* 1-5, 5 = last */
    uint8_t  start_dow;        /* 0 = Sunday */
    uint32_t start_time_s;     /* local seconds after midnight, default 2h */
    uint8_t  end_month;
    uint8_t  end_week;
    uint8_t  end_dow;
    uint32_t end_time_s;
} rewair_tz_rule_t;

int  rewair_tz_parse( const char* tz, rewair_tz_rule_t* out );
void rewair_tz_eval( const rewair_tz_rule_t* rule, uint32_t utc_epoch,
                     int16_t* offset_min_out, uint8_t* dst_out );

/* Day-of-month (1-31) for an M-form rule's week/dow in a given year/month,
 * e.g. week=5 (last), dow=0 (Sunday). */
uint32_t rewair_tz_rule_day( uint32_t year, uint8_t month, uint8_t week, uint8_t dow );
