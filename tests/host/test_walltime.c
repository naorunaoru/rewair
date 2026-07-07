#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rewair_walltime.h"

/* Epoch helpers: values computed with `date -u -j -f "%Y-%m-%d %H:%M:%S" ... +%s` */
#define T_1970_01_01_0000   0u            /* 1970-01-01 00:00:00 UTC — epoch zero */
#define T_2024_02_29_0000   1709164800u   /* 2024-02-29 00:00:00 UTC — leap day */
#define T_2024_03_01_0000   1709251200u   /* 2024-03-01 00:00:00 UTC — day after leap day */
#define T_2026_02_28_1200   1772280000u   /* 2026-02-28 12:00:00 UTC — non-leap year, no Feb 29 */
#define T_2026_03_01_0000   1772323200u   /* 2026-03-01 00:00:00 UTC — day after non-leap Feb */
#define T_2000_02_29_060708 951804428u    /* 2000-02-29 06:07:08 UTC — century leap (div 400) */
#define T_2026_07_06_1200   1783339200u   /* 2026-07-06 12:00:00 UTC */
#define T_2026_07_06_2200   1783375200u   /* 2026-07-06 22:00:00 UTC */
#define T_2026_07_07_0200   1783389600u   /* 2026-07-07 02:00:00 UTC */
#define T_2026_07_06_0200   1783303200u   /* 2026-07-06 02:00:00 UTC */
#define T_2026_07_05_2200   1783288800u   /* 2026-07-05 22:00:00 UTC */
#define T_UINT32_MAX         4294967295u  /* 2106-02-07 06:28:15 UTC */

static void assert_wall( uint32_t epoch, uint32_t year, uint32_t month, uint32_t day,
                          uint32_t hour, uint32_t minute, uint32_t second )
{
    wall_time_t wall;
    epoch_utc_to_wall( epoch, &wall );
    assert( wall.year == year );
    assert( wall.month == month );
    assert( wall.day == day );
    assert( wall.hour == hour );
    assert( wall.minute == minute );
    assert( wall.second == second );
}

int main( void )
{
    char buf[15];

    /* --- date_is_leap: century-boundary Gregorian rule --- */
    assert( date_is_leap( 2024u ) == 1 );  /* div by 4, not by 100 */
    assert( date_is_leap( 2026u ) == 0 );  /* not div by 4 */
    assert( date_is_leap( 1900u ) == 0 );  /* div by 100, not by 400 */
    assert( date_is_leap( 2000u ) == 1 );  /* div by 400 */
    assert( date_is_leap( 2100u ) == 0 );  /* div by 100, not by 400 */
    assert( date_is_leap( 1970u ) == 0 );

    /* --- date_days_in_month --- */
    assert( date_days_in_month( 2024u, 2u ) == 29u );  /* leap Feb */
    assert( date_days_in_month( 2026u, 2u ) == 28u );  /* non-leap Feb */
    assert( date_days_in_month( 1900u, 2u ) == 28u );  /* century non-leap */
    assert( date_days_in_month( 2000u, 2u ) == 29u );  /* century leap */
    assert( date_days_in_month( 2026u, 1u ) == 31u );
    assert( date_days_in_month( 2026u, 4u ) == 30u );
    assert( date_days_in_month( 2026u, 12u ) == 31u );

    /* --- epoch_utc_to_wall round-trips --- */
    assert_wall( T_1970_01_01_0000, 1970u, 1u, 1u, 0u, 0u, 0u );
    assert_wall( T_2024_02_29_0000, 2024u, 2u, 29u, 0u, 0u, 0u );      /* leap-year Feb 29 */
    assert_wall( T_2024_03_01_0000, 2024u, 3u, 1u, 0u, 0u, 0u );       /* rolls over correctly */
    assert_wall( T_2026_02_28_1200, 2026u, 2u, 28u, 12u, 0u, 0u );     /* non-leap: Feb stops at 28 */
    assert_wall( T_2026_03_01_0000, 2026u, 3u, 1u, 0u, 0u, 0u );
    assert_wall( T_2000_02_29_060708, 2000u, 2u, 29u, 6u, 7u, 8u );    /* century leap (div 400) */
    assert_wall( T_UINT32_MAX, 2106u, 2u, 7u, 6u, 28u, 15u );          /* uint32_t epoch ceiling */

    /* --- wall_time_to_compact formatting --- */
    {
        wall_time_t wall;
        epoch_utc_to_wall( T_2026_07_06_1200, &wall );
        wall_time_to_compact( &wall, buf );
        assert( memcmp( buf, "20260706120000", 14 ) == 0 );
        assert( buf[14] == '\0' );
    }
    {
        /* single-digit month/day/hour/minute/second all zero-pad correctly */
        wall_time_t wall;
        epoch_utc_to_wall( T_2000_02_29_060708, &wall );
        wall_time_to_compact( &wall, buf );
        assert( memcmp( buf, "20000229060708", 14 ) == 0 );
    }

    /* --- compact_utc_time_with_offset: positive offset --- */
    /* 2026-07-06 22:00:00 UTC + 4h -> 2026-07-07 02:00:00 local */
    compact_utc_time_with_offset( T_2026_07_06_2200, 4 * 3600, buf );
    assert( memcmp( buf, "20260707020000", 14 ) == 0 );
    {
        /* cross-check against the plain epoch for the same instant */
        wall_time_t wall;
        epoch_utc_to_wall( T_2026_07_07_0200, &wall );
        char buf2[15];
        wall_time_to_compact( &wall, buf2 );
        assert( memcmp( buf, buf2, 14 ) == 0 );
    }

    /* --- compact_utc_time_with_offset: negative offset --- */
    /* 2026-07-06 02:00:00 UTC - 4h -> 2026-07-05 22:00:00 local */
    compact_utc_time_with_offset( T_2026_07_06_0200, -4 * 3600, buf );
    assert( memcmp( buf, "20260705220000", 14 ) == 0 );
    {
        wall_time_t wall;
        epoch_utc_to_wall( T_2026_07_05_2200, &wall );
        char buf2[15];
        wall_time_to_compact( &wall, buf2 );
        assert( memcmp( buf, buf2, 14 ) == 0 );
    }

    /* --- compact_utc_time_with_offset: zero offset matches plain UTC --- */
    compact_utc_time_with_offset( T_2026_07_06_1200, 0, buf );
    assert( memcmp( buf, "20260706120000", 14 ) == 0 );

    printf( "test_walltime OK\n" );
    return 0;
}
