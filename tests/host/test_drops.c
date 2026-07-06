#include <assert.h>
#include <stdio.h>
#include "rewair_drops.h"

int main( void )
{
    rewair_drops_t d;

    rewair_drops_init( &d );
    assert( rewair_drops_total( &d, 0u ) == 0u );

    /* three drops in the first hour */
    rewair_drops_record( &d, 100u );
    rewair_drops_record( &d, 200u );
    rewair_drops_record( &d, 3599u );
    assert( rewair_drops_total( &d, 3599u ) == 3u );

    /* still visible 23h later */
    assert( rewair_drops_total( &d, 3599u + 23u * 3600u ) == 3u );

    /* expired after the window passes (bucket 0 recycled) */
    assert( rewair_drops_total( &d, 3599u + 24u * 3600u + 1u ) == 0u );

    /* long idle gap must zero skipped buckets, not resurrect stale counts */
    rewair_drops_init( &d );
    rewair_drops_record( &d, 10u );
    rewair_drops_record( &d, 100u * 3600u );      /* ~4 days later */
    assert( rewair_drops_total( &d, 100u * 3600u ) == 1u );

    printf( "test_drops OK\n" );
    return 0;
}
