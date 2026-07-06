#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rewair_json.h"

int main( void )
{
    char out[65];
    uint32_t n = 0u;
    char ssids[8][33];

    const char* join = "{\"ssid\":\"mikrotik-2g\",\"pass\":\"hunter\\\"2\"}";
    assert( rewair_req_get_string( join, (uint32_t)strlen( join ), "ssid", out, sizeof( out ) ) == 1 );
    assert( strcmp( out, "mikrotik-2g" ) == 0 );
    assert( rewair_req_get_string( join, (uint32_t)strlen( join ), "pass", out, sizeof( out ) ) == 1 );
    assert( strcmp( out, "hunter\"2" ) == 0 );
    assert( rewair_req_get_string( join, (uint32_t)strlen( join ), "nope", out, sizeof( out ) ) == 0 );

    const char* t = "{\"epoch\":1749300000}";
    assert( rewair_req_get_u32( t, (uint32_t)strlen( t ), "epoch", &n ) == 1 );
    assert( n == 1749300000u );

    const char* big = "{\"epoch\":99999999999}";
    assert( rewair_req_get_u32( big, (uint32_t)strlen( big ), "epoch", &n ) == 0 );

    const char* prio = "{\"order\":[\"a\",\"b\",\"c\"]}";
    assert( rewair_req_get_string_array( prio, (uint32_t)strlen( prio ), "order", ssids, 8u, &n ) == 1 );
    assert( n == 3u && strcmp( ssids[0], "a" ) == 0 && strcmp( ssids[2], "c" ) == 0 );

    assert( rewair_req_get_string( "{broken", 7u, "x", out, sizeof( out ) ) == -1 );

    printf( "test_req OK\n" );
    return 0;
}
