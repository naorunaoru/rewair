#include <assert.h>
#include <stdio.h>
#include "rewair_tz.h"

/* Epoch helpers: values computed with `date -u -j -f "%Y-%m-%d %H:%M:%S" ... +%s` */
#define T_2026_01_15_NOON 1768478400u   /* 2026-01-15 12:00:00 UTC */
#define T_2026_07_15_NOON 1784116800u   /* 2026-07-15 12:00:00 UTC */
#define T_2026_03_29_0059 1774745940u   /* 2026-03-29 00:59:00 UTC — 1 min before Lisbon DST on */
#define T_2026_03_29_0101 1774746060u   /* 2026-03-29 01:01:00 UTC — 1 min after */
#define T_2026_10_25_0059 1792889940u   /* 2026-10-25 00:59:00 UTC — 1 min before Lisbon DST off */
#define T_2026_10_25_0101 1792890060u   /* 2026-10-25 01:01:00 UTC — 1 min after */

int main( void )
{
    rewair_tz_rule_t r;
    int16_t off;
    uint8_t dst;

    /* Lisbon: last Sun of Mar 01:00 local -> last Sun of Oct 02:00 local (=01:00 UTC both) */
    assert( rewair_tz_parse( "WET0WEST,M3.5.0/1,M10.5.0", &r ) == 0 );
    assert( r.std_offset_min == 0 && r.dst_offset_min == 60 && r.has_dst == 1u );

    rewair_tz_eval( &r, T_2026_01_15_NOON, &off, &dst );
    assert( off == 0 && dst == 0u );
    rewair_tz_eval( &r, T_2026_07_15_NOON, &off, &dst );
    assert( off == 60 && dst == 1u );
    rewair_tz_eval( &r, T_2026_03_29_0059, &off, &dst );
    assert( off == 0 && dst == 0u );
    rewair_tz_eval( &r, T_2026_03_29_0101, &off, &dst );
    assert( off == 60 && dst == 1u );
    rewair_tz_eval( &r, T_2026_10_25_0059, &off, &dst );
    assert( off == 60 && dst == 1u );
    rewair_tz_eval( &r, T_2026_10_25_0101, &off, &dst );
    assert( off == 0 && dst == 0u );

    /* CET: POSIX sign inversion (CET-1 == UTC+1), default 02:00 transition time */
    assert( rewair_tz_parse( "CET-1CEST,M3.5.0,M10.5.0/3", &r ) == 0 );
    assert( r.std_offset_min == 60 && r.dst_offset_min == 120 );
    rewair_tz_eval( &r, T_2026_01_15_NOON, &off, &dst );
    assert( off == 60 && dst == 0u );
    rewair_tz_eval( &r, T_2026_07_15_NOON, &off, &dst );
    assert( off == 120 && dst == 1u );

    /* Sydney: southern hemisphere, DST wraps the new year */
    assert( rewair_tz_parse( "AEST-10AEDT,M10.1.0,M4.1.0/3", &r ) == 0 );
    rewair_tz_eval( &r, T_2026_01_15_NOON, &off, &dst );
    assert( off == 660 && dst == 1u );   /* January is DST in Sydney */
    rewair_tz_eval( &r, T_2026_07_15_NOON, &off, &dst );
    assert( off == 600 && dst == 0u );

    /* Fixed offset, no DST */
    assert( rewair_tz_parse( "UTC0", &r ) == 0 );
    assert( r.has_dst == 0u );
    rewair_tz_eval( &r, T_2026_07_15_NOON, &off, &dst );
    assert( off == 0 && dst == 0u );

    /* Fixed negative-east (ISO UTC-4) */
    assert( rewair_tz_parse( "AST4", &r ) == 0 );
    rewair_tz_eval( &r, T_2026_07_15_NOON, &off, &dst );
    assert( off == -240 && dst == 0u );

    /* Garbage rejected */
    assert( rewair_tz_parse( "", &r ) == -1 );
    assert( rewair_tz_parse( "M3.5.0", &r ) == -1 );

    /* Day-of-month for M-form rule (last Sunday of the month) */
    assert( rewair_tz_rule_day( 2026u, 3u, 5u, 0u ) == 29u );   /* last Sun of Mar 2026 */
    assert( rewair_tz_rule_day( 2026u, 10u, 5u, 0u ) == 25u );  /* last Sun of Oct 2026 */

    printf( "test_tz OK\n" );
    return 0;
}
