/* Air-quality score computation, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 7, pure move). See rewair_score.h. */

#include "rewair_score.h"

int32_t centi_to_int( int32_t centi )
{
    if ( centi >= 0 )
    {
        return ( centi + 50 ) / 100;
    }
    return ( centi - 50 ) / 100;
}

uint32_t index_outside_range( int32_t value, int32_t low, int32_t high, int32_t step )
{
    int32_t delta = 0;
    uint32_t index = 0u;
    if ( value < low )
    {
        delta = low - value;
    }
    else if ( value > high )
    {
        delta = value - high;
    }

    if ( delta > 0 )
    {
        index = (uint32_t)( ( delta + step - 1 ) / step );
    }
    return index > 4u ? 4u : index;
}

uint32_t index_above( int32_t value, int32_t good_max, int32_t step )
{
    uint32_t index = 0u;
    if ( value > good_max )
    {
        index = (uint32_t)( ( value - good_max + step - 1 ) / step );
    }
    return index > 4u ? 4u : index;
}

uint32_t penalty_outside_range( int32_t value, int32_t low, int32_t high,
                                       int32_t full_bad_delta, uint32_t max_penalty )
{
    int32_t delta = 0;
    if ( value < low )
    {
        delta = low - value;
    }
    else if ( value > high )
    {
        delta = value - high;
    }

    if ( delta <= 0 )
    {
        return 0u;
    }
    if ( delta >= full_bad_delta )
    {
        return max_penalty;
    }
    return (uint32_t)( ( (uint32_t)delta * max_penalty + ( (uint32_t)full_bad_delta / 2u ) ) /
                       (uint32_t)full_bad_delta );
}

uint32_t penalty_above( int32_t value, int32_t good_max, int32_t full_bad_delta,
                               uint32_t max_penalty )
{
    int32_t delta;
    if ( value <= good_max )
    {
        return 0u;
    }

    delta = value - good_max;
    if ( delta >= full_bad_delta )
    {
        return max_penalty;
    }
    return (uint32_t)( ( (uint32_t)delta * max_penalty + ( (uint32_t)full_bad_delta / 2u ) ) /
                       (uint32_t)full_bad_delta );
}

void sens_parse_pair( sens_values_t* sens, const char* key, const char* value )
{
    int32_t parsed = 0;
    if ( parse_fixed_centi( value, &parsed ) == 0 )
    {
        return;
    }

    if ( cstr_eq( key, "temp" ) )
    {
        sens->temp = parsed;
        sens->seen |= SENS_TEMP;
    }
    else if ( cstr_eq( key, "humid" ) )
    {
        sens->humid = parsed;
        sens->seen |= SENS_HUMID;
    }
    else if ( cstr_eq( key, "co2" ) )
    {
        sens->co2 = parsed;
        sens->seen |= SENS_CO2;
    }
    else if ( cstr_eq( key, "voc" ) )
    {
        sens->voc = parsed;
        sens->seen |= SENS_VOC;
    }
    else if ( cstr_eq( key, "dust" ) )
    {
        sens->dust = parsed;
        sens->seen |= SENS_DUST;
    }
    else if ( cstr_eq( key, "light" ) )
    {
        sens->light = parsed;
        sens->seen |= SENS_LIGHT;
    }
}

char* score_color( uint32_t score )
{
    static char green[] = "green";
    static char amber[] = "amber";
    static char purple[] = "purple";

    if ( score >= 80u )
    {
        return green;
    }
    if ( score >= 60u )
    {
        return amber;
    }
    return purple;
}

void sens_compute_score( const sens_values_t* sens, sens_score_t* out )
{
    uint32_t penalty;

    out->idx[0] = (uint8_t)index_outside_range( sens->temp, 1800, 2600, 200 );
    out->idx[1] = (uint8_t)index_outside_range( sens->humid, 3000, 6000, 1000 );
    out->idx[2] = (uint8_t)index_above( sens->co2, 100000, 40000 );
    out->idx[3] = (uint8_t)index_above( sens->voc, 33300, 33300 );
    out->idx[4] = (uint8_t)index_above( sens->dust, 1200, 1200 );

    penalty  = penalty_outside_range( sens->temp, 1800, 2600, 1200, 8u );
    penalty += penalty_outside_range( sens->humid, 3000, 6000, 4000, 8u );
    penalty += penalty_above( sens->co2, 100000, 80000, 25u );
    penalty += penalty_above( sens->voc, 33300, 100000, 25u );
    penalty += penalty_above( sens->dust, 1200, 6000, 34u );

    out->score = penalty < 100u ? 100u - penalty : 0u;
    out->color = score_color( out->score );
}
