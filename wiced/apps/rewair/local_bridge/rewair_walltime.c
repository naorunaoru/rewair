/* Wall-clock/date helpers, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 6, pure move). Pure C, stdint only -- host-testable. */

#include "rewair_walltime.h"

int date_is_leap( uint32_t year )
{
    return ( ( year % 4u ) == 0u && ( year % 100u ) != 0u ) || ( year % 400u ) == 0u;
}

uint32_t date_days_in_month( uint32_t year, uint32_t month )
{
    static const uint8_t days[] = { 31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u };
    if ( month == 2u && date_is_leap( year ) != 0 )
    {
        return 29u;
    }
    return days[month - 1u];
}

void epoch_utc_to_wall( uint32_t epoch_seconds, wall_time_t* out )
{
    uint32_t days = epoch_seconds / 86400u;
    uint32_t seconds = epoch_seconds % 86400u;
    uint32_t year = 1970u;
    uint32_t month = 1u;

    while ( 1 )
    {
        uint32_t year_days = date_is_leap( year ) != 0 ? 366u : 365u;
        if ( days < year_days )
        {
            break;
        }
        days -= year_days;
        year++;
    }

    while ( 1 )
    {
        uint32_t month_days = date_days_in_month( year, month );
        if ( days < month_days )
        {
            break;
        }
        days -= month_days;
        month++;
    }

    out->year = year;
    out->month = month;
    out->day = days + 1u;
    out->hour = seconds / 3600u;
    seconds %= 3600u;
    out->minute = seconds / 60u;
    out->second = seconds % 60u;
}

void write_dec2( char* out, uint32_t value )
{
    out[0] = (char)( '0' + ( ( value / 10u ) % 10u ) );
    out[1] = (char)( '0' + ( value % 10u ) );
}

void write_dec4( char* out, uint32_t value )
{
    out[0] = (char)( '0' + ( ( value / 1000u ) % 10u ) );
    out[1] = (char)( '0' + ( ( value / 100u ) % 10u ) );
    out[2] = (char)( '0' + ( ( value / 10u ) % 10u ) );
    out[3] = (char)( '0' + ( value % 10u ) );
}

void wall_time_to_compact( const wall_time_t* wall, char out[15] )
{
    write_dec4( &out[0], wall->year );
    write_dec2( &out[4], wall->month );
    write_dec2( &out[6], wall->day );
    write_dec2( &out[8], wall->hour );
    write_dec2( &out[10], wall->minute );
    write_dec2( &out[12], wall->second );
    out[14] = '\0';
}

void compact_utc_time_with_offset( uint32_t utc_seconds, int32_t offset_seconds, char out[15] )
{
    wall_time_t wall;
    epoch_utc_to_wall( utc_seconds + (uint32_t)offset_seconds, &wall );
    wall_time_to_compact( &wall, out );
}
