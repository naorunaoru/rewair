#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rewair_ble_proto.h"

static void test_roundtrip( void )
{
    rewair_ble_frame_t in;
    rewair_ble_frame_t out;
    uint8_t wire[REWAIR_BLE_FRAME_WIRE_MAX];
    uint32_t wire_length;

    memset( &in, 0, sizeof( in ) );
    in.type = REWAIR_BLE_FRAME_RESPONSE;
    in.flags = REWAIR_BLE_FLAG_FIRST | REWAIR_BLE_FLAG_MORE;
    in.operation = 2u;
    in.request_id = 0x1234u;
    in.sequence = 7u;
    in.status = 200u;
    memcpy( in.payload, "a\0b", 3u );
    in.payload_length = 3u;

    wire_length = rewair_ble_frame_encode( &in, wire, sizeof( wire ) );
    assert( wire_length > 1u );
    assert( wire[wire_length - 1u] == 0u );
    assert( rewair_ble_frame_decode( wire, wire_length - 1u, &out ) == 0 );
    assert( out.type == in.type );
    assert( out.flags == in.flags );
    assert( out.operation == in.operation );
    assert( out.request_id == in.request_id );
    assert( out.sequence == in.sequence );
    assert( out.status == in.status );
    assert( out.payload_length == in.payload_length );
    assert( memcmp( out.payload, in.payload, in.payload_length ) == 0 );
}

static void test_max_payload_and_crc( void )
{
    rewair_ble_frame_t in;
    rewair_ble_frame_t out;
    uint8_t wire[REWAIR_BLE_FRAME_WIRE_MAX];
    uint32_t i;
    uint32_t wire_length;

    memset( &in, 0, sizeof( in ) );
    in.type = REWAIR_BLE_FRAME_REQUEST;
    in.flags = REWAIR_BLE_FLAG_FIRST;
    in.operation = 1u;
    in.request_id = 1u;
    in.payload_length = REWAIR_BLE_FRAME_PAYLOAD_MAX;
    for ( i = 0u; i < in.payload_length; i++ )
    {
        in.payload[i] = (uint8_t)i;
    }
    wire_length = rewair_ble_frame_encode( &in, wire, sizeof( wire ) );
    assert( wire_length != 0u );
    assert( rewair_ble_frame_decode( wire, wire_length - 1u, &out ) == 0 );
    assert( memcmp( out.payload, in.payload, in.payload_length ) == 0 );

    wire[wire_length / 2u] ^= 0x40u;
    assert( rewair_ble_frame_decode( wire, wire_length - 1u, &out ) != 0 );
}

static void test_rejects_invalid_input( void )
{
    rewair_ble_frame_t frame;
    uint8_t wire[REWAIR_BLE_FRAME_WIRE_MAX];

    memset( &frame, 0, sizeof( frame ) );
    frame.payload_length = REWAIR_BLE_FRAME_PAYLOAD_MAX + 1u;
    assert( rewair_ble_frame_encode( &frame, wire, sizeof( wire ) ) == 0u );
    assert( rewair_ble_frame_decode( wire, 0u, &frame ) != 0 );
}

int main( void )
{
    test_roundtrip( );
    test_max_payload_and_crc( );
    test_rejects_invalid_input( );
    printf( "test_ble_proto OK\n" );
    return 0;
}
