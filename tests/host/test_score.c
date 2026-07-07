#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rewair_score.h"

static void assert_color( const char* color, const char* expect )
{
    assert( strcmp( color, expect ) == 0 );
}

int main( void )
{
    /* --- (a) all-nominal: every sensor comfortably inside its "good" band ---
     * temp=2200 (22.00C) in [1800,2600]; humid=4500 (45.00%) in [3000,6000];
     * co2=50000 (500.00ppm-equiv centi) <= good_max 100000; voc=10000 <= 33300;
     * dust=500 <= 1200. All deltas are <= 0, so every index/penalty is 0. */
    {
        sens_values_t sens = { 0u, 2200, 4500, 50000, 10000, 500, 0 };
        sens_score_t out;
        sens_compute_score( &sens, &out );
        assert( out.idx[0] == 0u && out.idx[1] == 0u && out.idx[2] == 0u &&
                out.idx[3] == 0u && out.idx[4] == 0u );
        assert( out.score == 100u );
        assert_color( out.color, "green" );
    }

    /* --- (b) a known-mixed case: hand-derived from the code's constants ---
     * temp=3000: outside [1800,2600] high side, delta=3000-2600=400.
     *   index = ceil(400/200) = 2.
     *   penalty (full_bad_delta=1200, max=8) = (400*8 + 1200/2) / 1200
     *     = (3200+600)/1200 = 3800/1200 = 3 (integer division).
     * humid=6500: outside [3000,6000] high side, delta=6500-6000=500.
     *   index = ceil(500/1000) = 1.
     *   penalty (full_bad_delta=4000, max=8) = (500*8 + 2000)/4000
     *     = (4000+2000)/4000 = 6000/4000 = 1.
     * co2=150000: above good_max 100000, delta=50000.
     *   index = ceil(50000/40000) = 2.
     *   penalty (full_bad_delta=80000, max=25) = (50000*25 + 40000)/80000
     *     = (1250000+40000)/80000 = 1290000/80000 = 16 (16.125 truncated).
     * voc=50000: above good_max 33300, delta=16700.
     *   index = ceil(16700/33300) = 1.
     *   penalty (full_bad_delta=100000, max=25) = (16700*25 + 50000)/100000
     *     = (417500+50000)/100000 = 467500/100000 = 4 (4.675 truncated).
     * dust=2000: above good_max 1200, delta=800.
     *   index = ceil(800/1200) = 1.
     *   penalty (full_bad_delta=6000, max=34) = (800*34 + 3000)/6000
     *     = (27200+3000)/6000 = 30200/6000 = 5 (5.033 truncated).
     * total penalty = 3+1+16+4+5 = 29 -> score = 100-29 = 71 -> "amber" (>=60, <80).
     * (Cross-checked against a standalone re-implementation of the same
     * formulas compiled and run during development; results matched exactly.) */
    {
        sens_values_t sens = { 0u, 3000, 6500, 150000, 50000, 2000, 0 };
        sens_score_t out;
        sens_compute_score( &sens, &out );
        assert( out.idx[0] == 2u );
        assert( out.idx[1] == 1u );
        assert( out.idx[2] == 2u );
        assert( out.idx[3] == 1u );
        assert( out.idx[4] == 1u );
        assert( out.score == 71u );
        assert_color( out.color, "amber" );
    }

    /* --- (c) an extreme case: every sensor far past its full-bad delta ---
     * temp=10000, humid=10000, co2=500000, voc=500000, dust=50000 all blow
     * past each metric's full_bad_delta, so every index/penalty saturates at
     * its cap (index caps at 4, penalty caps at its max_penalty per metric:
     * 8+8+25+25+34 = 100). score = 100-100 = 0 -> "purple" (< 60). */
    {
        sens_values_t sens = { 0u, 10000, 10000, 500000, 500000, 50000, 0 };
        sens_score_t out;
        sens_compute_score( &sens, &out );
        assert( out.idx[0] == 4u && out.idx[1] == 4u && out.idx[2] == 4u &&
                out.idx[3] == 4u && out.idx[4] == 4u );
        assert( out.score == 0u );
        assert_color( out.color, "purple" );
    }

    /* --- score_color boundaries, verified directly from the code's thresholds --- */
    assert_color( score_color( 100u ), "green" );
    assert_color( score_color( 80u ), "green" );   /* >= 80 -> green */
    assert_color( score_color( 79u ), "amber" );   /* just under the green cut */
    assert_color( score_color( 60u ), "amber" );   /* >= 60 -> amber */
    assert_color( score_color( 59u ), "purple" );  /* just under the amber cut */
    assert_color( score_color( 0u ), "purple" );

    /* --- sens_parse_pair: keys incl. "light", seen-bits and centi values --- */
    {
        sens_values_t sens = { 0u, 0, 0, 0, 0, 0, 0 };
        sens_parse_pair( &sens, "temp", "22.00" );
        sens_parse_pair( &sens, "humid", "45.50" );
        sens_parse_pair( &sens, "co2", "500.00" );
        sens_parse_pair( &sens, "voc", "100.00" );
        sens_parse_pair( &sens, "dust", "5.00" );
        sens_parse_pair( &sens, "light", "12.34" );

        assert( ( sens.seen & SENS_TEMP ) != 0u );
        assert( ( sens.seen & SENS_HUMID ) != 0u );
        assert( ( sens.seen & SENS_CO2 ) != 0u );
        assert( ( sens.seen & SENS_VOC ) != 0u );
        assert( ( sens.seen & SENS_DUST ) != 0u );
        assert( ( sens.seen & SENS_LIGHT ) != 0u );
        assert( ( sens.seen & SENS_ALL ) == SENS_ALL );

        assert( sens.temp == 2200 );
        assert( sens.humid == 4550 );
        assert( sens.co2 == 50000 );
        assert( sens.voc == 10000 );
        assert( sens.dust == 500 );
        assert( sens.light == 1234 );

        /* an unknown key is ignored: no seen-bit change, no field touched */
        {
            sens_values_t before = sens;
            sens_parse_pair( &sens, "bogus", "1.00" );
            assert( sens.seen == before.seen );
            assert( sens.temp == before.temp );
        }

        /* a malformed value (parse_fixed_centi fails) leaves the pair untouched */
        {
            sens_values_t before = sens;
            sens_parse_pair( &sens, "temp", "not-a-number" );
            assert( sens.seen == before.seen );
            assert( sens.temp == before.temp );
        }
    }

    /* --- centi_to_int: rounding to nearest integer, half-away-from-zero --- */
    assert( centi_to_int( 2200 ) == 22 );
    assert( centi_to_int( 2250 ) == 23 );  /* +0.5 rounds up */
    assert( centi_to_int( 2249 ) == 22 );  /* rounds down (just under .5) */
    assert( centi_to_int( -2200 ) == -22 );
    assert( centi_to_int( -2250 ) == -23 ); /* -0.5 rounds away from zero */
    assert( centi_to_int( 0 ) == 0 );

    printf( "test_score OK\n" );
    return 0;
}
