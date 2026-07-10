#pragma once

#include "wiced.h"

#include <stdint.h>

#define REWAIR_BLE_DIAG_MAGIC 0x314c4252u /* "RBL1" in target memory */

enum
{
    REWAIR_BLE_PHASE_OFF = 0,
    REWAIR_BLE_PHASE_UART_READY,
    REWAIR_BLE_PHASE_MODULE_SETTLING,
    REWAIR_BLE_PHASE_COMMAND_MODE,
    REWAIR_BLE_PHASE_TRANSPARENT,
    REWAIR_BLE_PHASE_FAILED
};

/* Stable SWD-readable diagnostics now that USART1 belongs to the BI201. */
typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t heartbeat;
    uint32_t phase;
    int32_t  gpio_result;
    int32_t  uart_init_result;
    int32_t  last_uart_result;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t valid_frames;
    uint32_t bad_frames;
    uint32_t oversized_frames;
    uint32_t requests;
    uint32_t responses;
    uint32_t acknowledgements;
    uint32_t ack_timeouts;
    uint32_t busy_rejections;
    uint32_t last_request_id;
    uint32_t last_operation;
    uint32_t last_status;
} rewair_ble_diag_t;

extern volatile rewair_ble_diag_t rewair_ble_diag;

wiced_result_t rewair_ble_start( void );
