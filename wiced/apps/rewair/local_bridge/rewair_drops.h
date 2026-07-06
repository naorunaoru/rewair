#pragma once

#include <stdint.h>

#define REWAIR_DROPS_BUCKETS        24u
#define REWAIR_DROPS_BUCKET_SECONDS 3600u

typedef struct
{
    uint16_t buckets[REWAIR_DROPS_BUCKETS];
    uint32_t last_abs_bucket;
} rewair_drops_t;

void     rewair_drops_init( rewair_drops_t* d );
void     rewair_drops_record( rewair_drops_t* d, uint32_t uptime_s );
uint32_t rewair_drops_total( rewair_drops_t* d, uint32_t uptime_s );
