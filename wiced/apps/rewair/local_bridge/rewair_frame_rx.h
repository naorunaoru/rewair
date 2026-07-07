#pragma once

/* F103 sensor-frame RX parsing, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 8, pure move). Pure C, stdint only -- host-testable.
 *
 * NOTE: `sensor_rx_byte` (the byte-at-a-time state machine driver) stays in
 * local_bridge.c, NOT here -- it dispatches to `handle_sensor_frame`, which
 * is sensor-thread cluster glue that stays in local_bridge.c too. Moving
 * `sensor_rx_byte` here would require calling back into local_bridge.c,
 * creating a header cycle; it is thread-glue, not pure framing, so the
 * pure-move discipline keeps it where its callee lives. Only the genuinely
 * standalone parsing helpers below moved. */

#include <stdint.h>

#define SENSOR_PAYLOAD_MAX 512u

typedef struct
{
    uint8_t state;
    uint32_t pos;
    uint32_t length;
    char cmd[4];
    uint8_t header[13];
    uint8_t payload[SENSOR_PAYLOAD_MAX];
} sensor_rx_t;

void sensor_frame_reset( sensor_rx_t* rx );
const char* payload_next_field( const uint8_t* payload, uint32_t length, uint32_t* offset );
uint32_t payload_argc( const uint8_t* payload, uint32_t length );
void print_sensor_frame( const sensor_rx_t* rx, uint8_t trailer );
