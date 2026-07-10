#pragma once

#include <stdint.h>

/* BI201 transparent mode is a byte stream. Frames are COBS encoded and
 * terminated by zero, so either peer can recover after a partial notification,
 * UART noise, or an F411/module reset. */
#define REWAIR_BLE_PROTOCOL_VERSION   1u
#define REWAIR_BLE_FRAME_PAYLOAD_MAX 32u
#define REWAIR_BLE_FRAME_RAW_MAX     48u
#define REWAIR_BLE_FRAME_WIRE_MAX    50u
#define REWAIR_BLE_REQUEST_BODY_MAX 1024u

enum
{
    REWAIR_BLE_FRAME_REQUEST  = 1,
    REWAIR_BLE_FRAME_RESPONSE = 2,
    REWAIR_BLE_FRAME_EVENT    = 3,
    REWAIR_BLE_FRAME_ACK      = 4
};

enum
{
    REWAIR_BLE_FLAG_FIRST = 0x01u,
    REWAIR_BLE_FLAG_MORE  = 0x02u
};

typedef struct
{
    uint8_t  type;
    uint8_t  flags;
    uint8_t  operation;
    uint16_t request_id;
    uint16_t sequence;
    uint16_t status;
    uint16_t payload_length;
    uint8_t  payload[REWAIR_BLE_FRAME_PAYLOAD_MAX];
} rewair_ble_frame_t;

/* Returns wire bytes including the trailing zero delimiter, or zero on error. */
uint32_t rewair_ble_frame_encode( const rewair_ble_frame_t* frame,
                                  uint8_t* wire, uint32_t wire_size );

/* `wire` excludes the trailing zero delimiter. Returns zero on success. */
int rewair_ble_frame_decode( const uint8_t* wire, uint32_t wire_length,
                             rewair_ble_frame_t* frame );
