#include "rewair_ble_proto.h"

#include <string.h>

#define FRAME_HEADER_SIZE 12u
#define FRAME_CRC_SIZE     4u

static void put_u16le( uint8_t* p, uint16_t value )
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)( value >> 8 );
}

static uint16_t get_u16le( const uint8_t* p )
{
    return (uint16_t)( (uint16_t)p[0] | (uint16_t)( (uint16_t)p[1] << 8 ) );
}

static void put_u32le( uint8_t* p, uint32_t value )
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)( value >> 8 );
    p[2] = (uint8_t)( value >> 16 );
    p[3] = (uint8_t)( value >> 24 );
}

static uint32_t get_u32le( const uint8_t* p )
{
    return (uint32_t)p[0] |
           ( (uint32_t)p[1] << 8 ) |
           ( (uint32_t)p[2] << 16 ) |
           ( (uint32_t)p[3] << 24 );
}

static uint32_t frame_crc32( const uint8_t* data, uint32_t length )
{
    uint32_t crc = 0xffffffffu;
    uint32_t i;

    while ( length-- != 0u )
    {
        crc ^= *data++;
        for ( i = 0u; i < 8u; i++ )
        {
            crc = ( crc >> 1 ) ^ ( 0xedb88320u & ( 0u - ( crc & 1u ) ) );
        }
    }
    return crc ^ 0xffffffffu;
}

static uint32_t cobs_encode( const uint8_t* input, uint32_t length,
                             uint8_t* output, uint32_t output_size )
{
    uint32_t read = 0u;
    uint32_t write = 1u;
    uint32_t code_index = 0u;
    uint8_t code = 1u;

    if ( output_size == 0u )
    {
        return 0u;
    }
    while ( read < length )
    {
        if ( input[read] == 0u )
        {
            if ( code_index >= output_size )
            {
                return 0u;
            }
            output[code_index] = code;
            code = 1u;
            code_index = write++;
            if ( write > output_size )
            {
                return 0u;
            }
            read++;
        }
        else
        {
            if ( write >= output_size )
            {
                return 0u;
            }
            output[write++] = input[read++];
            code++;
            if ( code == 0xffu )
            {
                if ( code_index >= output_size )
                {
                    return 0u;
                }
                output[code_index] = code;
                code = 1u;
                code_index = write++;
                if ( write > output_size )
                {
                    return 0u;
                }
            }
        }
    }
    if ( code_index >= output_size )
    {
        return 0u;
    }
    output[code_index] = code;
    return write;
}

static uint32_t cobs_decode( const uint8_t* input, uint32_t length,
                             uint8_t* output, uint32_t output_size )
{
    uint32_t read = 0u;
    uint32_t write = 0u;

    while ( read < length )
    {
        uint8_t code = input[read++];
        uint32_t copy;

        if ( code == 0u )
        {
            return 0u;
        }
        copy = (uint32_t)code - 1u;
        if ( copy > length - read || copy > output_size - write )
        {
            return 0u;
        }
        if ( copy != 0u )
        {
            memcpy( output + write, input + read, copy );
            read += copy;
            write += copy;
        }
        if ( code != 0xffu && read < length )
        {
            if ( write >= output_size )
            {
                return 0u;
            }
            output[write++] = 0u;
        }
    }
    return write;
}

uint32_t rewair_ble_frame_encode( const rewair_ble_frame_t* frame,
                                  uint8_t* wire, uint32_t wire_size )
{
    uint8_t raw[REWAIR_BLE_FRAME_RAW_MAX];
    uint32_t raw_length;
    uint32_t encoded_length;

    if ( frame == NULL || wire == NULL ||
         frame->payload_length > REWAIR_BLE_FRAME_PAYLOAD_MAX )
    {
        return 0u;
    }
    raw_length = FRAME_HEADER_SIZE + frame->payload_length + FRAME_CRC_SIZE;
    if ( raw_length > sizeof( raw ) || wire_size < 2u )
    {
        return 0u;
    }

    raw[0] = REWAIR_BLE_PROTOCOL_VERSION;
    raw[1] = frame->type;
    raw[2] = frame->flags;
    raw[3] = frame->operation;
    put_u16le( raw + 4u, frame->request_id );
    put_u16le( raw + 6u, frame->sequence );
    put_u16le( raw + 8u, frame->status );
    put_u16le( raw + 10u, frame->payload_length );
    if ( frame->payload_length != 0u )
    {
        memcpy( raw + FRAME_HEADER_SIZE, frame->payload, frame->payload_length );
    }
    put_u32le( raw + FRAME_HEADER_SIZE + frame->payload_length,
               frame_crc32( raw, FRAME_HEADER_SIZE + frame->payload_length ) );

    encoded_length = cobs_encode( raw, raw_length, wire, wire_size - 1u );
    if ( encoded_length == 0u || encoded_length >= wire_size )
    {
        return 0u;
    }
    wire[encoded_length++] = 0u;
    return encoded_length;
}

int rewair_ble_frame_decode( const uint8_t* wire, uint32_t wire_length,
                             rewair_ble_frame_t* frame )
{
    uint8_t raw[REWAIR_BLE_FRAME_RAW_MAX];
    uint32_t raw_length;
    uint32_t payload_length;
    uint32_t expected_length;
    uint32_t expected_crc;

    if ( wire == NULL || frame == NULL || wire_length == 0u )
    {
        return -1;
    }
    raw_length = cobs_decode( wire, wire_length, raw, sizeof( raw ) );
    if ( raw_length < FRAME_HEADER_SIZE + FRAME_CRC_SIZE ||
         raw[0] != REWAIR_BLE_PROTOCOL_VERSION )
    {
        return -1;
    }
    payload_length = get_u16le( raw + 10u );
    expected_length = FRAME_HEADER_SIZE + payload_length + FRAME_CRC_SIZE;
    if ( payload_length > REWAIR_BLE_FRAME_PAYLOAD_MAX || raw_length != expected_length )
    {
        return -1;
    }
    expected_crc = get_u32le( raw + FRAME_HEADER_SIZE + payload_length );
    if ( frame_crc32( raw, FRAME_HEADER_SIZE + payload_length ) != expected_crc )
    {
        return -1;
    }

    memset( frame, 0, sizeof( *frame ) );
    frame->type = raw[1];
    frame->flags = raw[2];
    frame->operation = raw[3];
    frame->request_id = get_u16le( raw + 4u );
    frame->sequence = get_u16le( raw + 6u );
    frame->status = get_u16le( raw + 8u );
    frame->payload_length = (uint16_t)payload_length;
    if ( payload_length != 0u )
    {
        memcpy( frame->payload, raw + FRAME_HEADER_SIZE, payload_length );
    }
    return 0;
}
