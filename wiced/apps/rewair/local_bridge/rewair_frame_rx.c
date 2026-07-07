/* F103 sensor-frame RX parsing, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 8, pure move). See rewair_frame_rx.h. */

#include "rewair_frame_rx.h"

#include <stdio.h>

void sensor_frame_reset( sensor_rx_t* rx )
{
    rx->state = 0u;
    rx->pos = 0u;
    rx->length = 0u;
}

const char* payload_next_field( const uint8_t* payload, uint32_t length, uint32_t* offset )
{
    uint32_t start = *offset;
    if ( start >= length )
    {
        return NULL;
    }

    while ( *offset < length && payload[*offset] != 0u )
    {
        ( *offset )++;
    }
    if ( *offset >= length )
    {
        return NULL;
    }
    ( *offset )++;
    return (const char*)&payload[start];
}

uint32_t payload_argc( const uint8_t* payload, uint32_t length )
{
    uint32_t argc = 0u;
    uint32_t start = 0u;
    while ( start < length )
    {
        argc++;
        while ( start < length && payload[start] != 0u )
        {
            start++;
        }
        start++;
    }
    return argc;
}

void print_sensor_frame( const sensor_rx_t* rx, uint8_t trailer )
{
    printf( "[rx] %.4s len=%lu argc=%lu trailer=0x%02x\n",
            rx->cmd, rx->length, payload_argc( rx->payload, rx->length ), trailer );
}
