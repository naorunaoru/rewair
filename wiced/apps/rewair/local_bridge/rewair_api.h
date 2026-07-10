#pragma once

#include <stdint.h>

/* Stable operation identifiers shared by HTTP route adapters and BLE frames. */
enum
{
    REWAIR_API_OP_CAPABILITIES = 1,
    REWAIR_API_OP_STATUS       = 2,
    REWAIR_API_OP_SCAN         = 3,
    REWAIR_API_OP_NETWORKS     = 4,
    REWAIR_API_OP_JOIN         = 5,
    REWAIR_API_OP_FORGET       = 6,
    REWAIR_API_OP_PRIORITY     = 7,
    REWAIR_API_OP_SETTINGS     = 8,
    REWAIR_API_OP_TIME         = 9,
    REWAIR_API_OP_DISPLAY      = 10,
    REWAIR_API_OP_RESET        = 11,
    REWAIR_API_OP_UPDATE       = 12
};

/* Executes one transport-neutral API operation. The response is the same JSON
 * representation used by HTTP. Returns its byte length or -1 if no response
 * can be produced. `status_out` uses HTTP-compatible numeric status codes so
 * every transport exposes identical success/error semantics. */
int rewair_api_execute( uint8_t operation, const uint8_t* body, uint32_t body_length,
                        char* response, uint32_t response_size, uint16_t* status_out );
