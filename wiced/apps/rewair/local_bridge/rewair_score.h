#pragma once

/* Air-quality score computation, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 7, pure move). Pure C, stdint only -- host-testable.
 * Depends on rewair_fmt.h for cstr_eq/parse_fixed_centi, which is safe on
 * the host build (see rewair_fmt.h REWAIR_HOST_BUILD guards). */

#include <stdint.h>
#include "rewair_fmt.h"

#define SENS_TEMP  (1u << 0)
#define SENS_HUMID (1u << 1)
#define SENS_CO2   (1u << 2)
#define SENS_VOC   (1u << 3)
#define SENS_DUST  (1u << 4)
#define SENS_LIGHT (1u << 5)
#define SENS_ALL   (SENS_TEMP | SENS_HUMID | SENS_CO2 | SENS_VOC | SENS_DUST)

typedef struct
{
    uint32_t seen;
    int32_t temp;
    int32_t humid;
    int32_t co2;
    int32_t voc;
    int32_t dust;
    int32_t light;
} sens_values_t;

typedef struct
{
    uint32_t score;
    uint8_t  idx[5];   /* temp, humid, co2, voc, dust */
    char*    color;
} sens_score_t;

int32_t centi_to_int( int32_t centi );

uint32_t index_outside_range( int32_t value, int32_t low, int32_t high, int32_t step );
uint32_t index_above( int32_t value, int32_t good_max, int32_t step );
uint32_t penalty_outside_range( int32_t value, int32_t low, int32_t high,
                                 int32_t full_bad_delta, uint32_t max_penalty );
uint32_t penalty_above( int32_t value, int32_t good_max, int32_t full_bad_delta,
                          uint32_t max_penalty );

void sens_parse_pair( sens_values_t* sens, const char* key, const char* value );
char* score_color( uint32_t score );
void sens_compute_score( const sens_values_t* sens, sens_score_t* out );
