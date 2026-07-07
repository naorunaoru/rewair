#pragma once

/* Wall-clock/date helpers, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 6, pure move). Pure C, stdint only -- host-testable. */

#include <stdint.h>

typedef struct
{
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} wall_time_t;

int      date_is_leap( uint32_t year );
uint32_t date_days_in_month( uint32_t year, uint32_t month );
void     epoch_utc_to_wall( uint32_t epoch_seconds, wall_time_t* out );

void write_dec2( char* out, uint32_t value );
void write_dec4( char* out, uint32_t value );

void wall_time_to_compact( const wall_time_t* wall, char out[15] );
void compact_utc_time_with_offset( uint32_t utc_seconds, int32_t offset_seconds, char out[15] );
