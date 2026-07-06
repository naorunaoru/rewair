#include "rewair_drops.h"

#include <string.h>

static void advance( rewair_drops_t* d, uint32_t uptime_s )
{
    uint32_t abs_bucket = uptime_s / REWAIR_DROPS_BUCKET_SECONDS;
    uint32_t skipped;
    uint32_t i;

    if ( abs_bucket <= d->last_abs_bucket )
    {
        return;
    }

    skipped = abs_bucket - d->last_abs_bucket;
    if ( skipped >= REWAIR_DROPS_BUCKETS )
    {
        memset( d->buckets, 0, sizeof( d->buckets ) );
    }
    else
    {
        for ( i = 1u; i <= skipped; i++ )
        {
            d->buckets[ ( d->last_abs_bucket + i ) % REWAIR_DROPS_BUCKETS ] = 0u;
        }
    }
    d->last_abs_bucket = abs_bucket;
}

void rewair_drops_init( rewair_drops_t* d )
{
    memset( d, 0, sizeof( *d ) );
}

void rewair_drops_record( rewair_drops_t* d, uint32_t uptime_s )
{
    advance( d, uptime_s );
    d->buckets[ d->last_abs_bucket % REWAIR_DROPS_BUCKETS ]++;
}

uint32_t rewair_drops_total( rewair_drops_t* d, uint32_t uptime_s )
{
    uint32_t sum = 0u;
    uint32_t i;

    advance( d, uptime_s );
    for ( i = 0u; i < REWAIR_DROPS_BUCKETS; i++ )
    {
        sum += d->buckets[i];
    }
    return sum;
}
