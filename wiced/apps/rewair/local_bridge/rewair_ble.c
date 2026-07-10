#include "rewair_ble.h"

#include <stdio.h>
#include <string.h>

#include "platform_peripheral.h"
#include "rewair_api.h"
#include "rewair_ble_proto.h"

#define BI201_UART                 WICED_UART_1
#define BI201_RX_RING_SIZE         512u
#define BI201_RX_THREAD_STACK      2304u
#define BI201_API_THREAD_STACK     3072u
#define BI201_API_RESPONSE_MAX     2048u

typedef struct
{
    uint16_t request_id;
    uint8_t  operation;
    uint16_t body_length;
    uint8_t  body[REWAIR_BLE_REQUEST_BODY_MAX + 1u];
} ble_request_t;

typedef struct
{
    uint8_t  active;
    uint16_t request_id;
    uint8_t  operation;
    uint16_t next_sequence;
    uint16_t body_length;
    uint8_t  body[REWAIR_BLE_REQUEST_BODY_MAX + 1u];
} ble_assembler_t;

volatile rewair_ble_diag_t rewair_ble_diag;

static const platform_gpio_t bi201_control = { GPIOB, 13 };
static const wiced_uart_config_t bi201_uart_config =
{
    .baud_rate    = 115200,
    .data_width   = DATA_WIDTH_8BIT,
    .parity       = NO_PARITY,
    .stop_bits    = STOP_BITS_1,
    .flow_control = FLOW_CONTROL_DISABLED,
};

static wiced_ring_buffer_t bi201_rx_ring;
static uint8_t bi201_rx_storage[BI201_RX_RING_SIZE];
static wiced_thread_t bi201_rx_thread;
static wiced_thread_t bi201_api_thread;
static wiced_semaphore_t request_ready;
static wiced_semaphore_t ack_ready;
static wiced_mutex_t tx_mutex;
static volatile uint32_t request_pending;
static volatile uint32_t ack_waiting;
static volatile uint16_t ack_request_id;
static volatile uint16_t ack_sequence;
static volatile uint8_t ack_operation;
static ble_request_t pending_request;
static char response_buffer[BI201_API_RESPONSE_MAX];

static uint32_t now_ms( void )
{
    wiced_time_t now = 0;
    (void)wiced_time_get_time( &now );
    return (uint32_t)now;
}

static void receive_for( uint32_t duration_ms )
{
    uint32_t start = now_ms( );

    while ( (uint32_t)( now_ms( ) - start ) < duration_ms )
    {
        uint8_t byte;
        wiced_result_t result = wiced_uart_receive_bytes( BI201_UART, &byte, 1u, 25u );

        rewair_ble_diag.last_uart_result = (int32_t)result;
        if ( result == WICED_SUCCESS )
        {
            rewair_ble_diag.rx_bytes++;
        }
    }
}

static wiced_result_t transmit_bytes( const void* data, uint32_t length )
{
    wiced_result_t result;

    wiced_rtos_lock_mutex( &tx_mutex );
    result = wiced_uart_transmit_bytes( BI201_UART, data, length );
    wiced_rtos_unlock_mutex( &tx_mutex );
    rewair_ble_diag.last_uart_result = (int32_t)result;
    if ( result == WICED_SUCCESS )
    {
        rewair_ble_diag.tx_bytes += length;
    }
    return result;
}

static wiced_result_t transmit_command( const char* command, uint32_t response_ms )
{
    wiced_result_t result = transmit_bytes( command, (uint32_t)strlen( command ) );

    if ( result == WICED_SUCCESS )
    {
        receive_for( response_ms );
    }
    return result;
}

static wiced_result_t send_response( uint8_t operation, uint16_t request_id,
                                     uint16_t status, const uint8_t* payload,
                                     uint32_t payload_length )
{
    uint32_t offset = 0u;
    uint16_t sequence = 0u;

    do
    {
        rewair_ble_frame_t frame;
        uint8_t wire[REWAIR_BLE_FRAME_WIRE_MAX];
        uint32_t take = payload_length - offset;
        uint32_t wire_length;

        if ( take > REWAIR_BLE_FRAME_PAYLOAD_MAX )
        {
            take = REWAIR_BLE_FRAME_PAYLOAD_MAX;
        }
        memset( &frame, 0, sizeof( frame ) );
        frame.type = REWAIR_BLE_FRAME_RESPONSE;
        frame.flags = sequence == 0u ? REWAIR_BLE_FLAG_FIRST : 0u;
        if ( offset + take < payload_length )
        {
            frame.flags |= REWAIR_BLE_FLAG_MORE;
        }
        frame.operation = operation;
        frame.request_id = request_id;
        frame.sequence = sequence;
        frame.status = status;
        frame.payload_length = (uint16_t)take;
        if ( take != 0u )
        {
            memcpy( frame.payload, payload + offset, take );
        }
        wire_length = rewair_ble_frame_encode( &frame, wire, sizeof( wire ) );
        if ( ( frame.flags & REWAIR_BLE_FLAG_MORE ) != 0u )
        {
            while ( wiced_rtos_get_semaphore( &ack_ready, 0u ) == WICED_SUCCESS )
            {
            }
            ack_request_id = request_id;
            ack_operation = operation;
            ack_sequence = sequence;
            ack_waiting = 1u;
        }
        if ( wire_length == 0u || transmit_bytes( wire, wire_length ) != WICED_SUCCESS )
        {
            ack_waiting = 0u;
            return WICED_ERROR;
        }
        offset += take;
        sequence++;
        /* The BI201 has no UART flow control. Wait until the central proves it
         * received this fragment before filling the module with the next one. */
        if ( ack_waiting != 0u &&
             wiced_rtos_get_semaphore( &ack_ready, 5000u ) != WICED_SUCCESS )
        {
            ack_waiting = 0u;
            rewair_ble_diag.ack_timeouts++;
            return WICED_TIMEOUT;
        }
        if ( offset < payload_length )
        {
            /* ACK arrival proves the fragment drained, but the CC2540 bridge
             * still needs a brief half-duplex turnaround before accepting the
             * next UART-to-notification burst. */
            wiced_rtos_delay_milliseconds( 150u );
        }
    } while ( offset < payload_length );

    rewair_ble_diag.responses++;
    rewair_ble_diag.last_status = status;
    return WICED_SUCCESS;
}

static void send_error( const rewair_ble_frame_t* request, uint16_t status,
                        const char* message )
{
    char body[128];
    int length = snprintf( body, sizeof( body ), "{\"error\":\"%s\"}", message );

    if ( length > 0 && (uint32_t)length < sizeof( body ) )
    {
        (void)send_response( request->operation, request->request_id, status,
                             (const uint8_t*)body, (uint32_t)length );
    }
}

static void assembler_reset( ble_assembler_t* assembler )
{
    memset( assembler, 0, sizeof( *assembler ) );
}

static void accept_request_frame( ble_assembler_t* assembler,
                                  const rewair_ble_frame_t* frame )
{
    uint32_t new_length;

    if ( frame->type == REWAIR_BLE_FRAME_ACK )
    {
        if ( ack_waiting != 0u && frame->request_id == ack_request_id &&
             frame->operation == ack_operation && frame->sequence == ack_sequence )
        {
            ack_waiting = 0u;
            rewair_ble_diag.acknowledgements++;
            wiced_rtos_set_semaphore( &ack_ready );
        }
        return;
    }
    if ( frame->type != REWAIR_BLE_FRAME_REQUEST )
    {
        return;
    }
    if ( ( frame->flags & REWAIR_BLE_FLAG_FIRST ) != 0u )
    {
        assembler_reset( assembler );
        if ( frame->sequence != 0u )
        {
            send_error( frame, 400u, "bad first sequence" );
            return;
        }
        assembler->active = 1u;
        assembler->request_id = frame->request_id;
        assembler->operation = frame->operation;
    }
    if ( assembler->active == 0u || frame->request_id != assembler->request_id ||
         frame->operation != assembler->operation ||
         frame->sequence != assembler->next_sequence )
    {
        assembler_reset( assembler );
        send_error( frame, 400u, "fragment sequence" );
        return;
    }

    new_length = (uint32_t)assembler->body_length + frame->payload_length;
    if ( new_length > REWAIR_BLE_REQUEST_BODY_MAX )
    {
        assembler_reset( assembler );
        send_error( frame, 413u, "request too large" );
        return;
    }
    if ( frame->payload_length != 0u )
    {
        memcpy( assembler->body + assembler->body_length,
                frame->payload, frame->payload_length );
    }
    assembler->body_length = (uint16_t)new_length;
    assembler->next_sequence++;
    if ( ( frame->flags & REWAIR_BLE_FLAG_MORE ) != 0u )
    {
        return;
    }
    assembler->body[assembler->body_length] = '\0';

    if ( request_pending != 0u )
    {
        rewair_ble_diag.busy_rejections++;
        send_error( frame, 409u, "request busy" );
        assembler_reset( assembler );
        return;
    }

    pending_request.request_id = assembler->request_id;
    pending_request.operation = assembler->operation;
    pending_request.body_length = assembler->body_length;
    memcpy( pending_request.body, assembler->body, assembler->body_length + 1u );
    request_pending = 1u;
    rewair_ble_diag.requests++;
    rewair_ble_diag.last_request_id = pending_request.request_id;
    rewair_ble_diag.last_operation = pending_request.operation;
    assembler_reset( assembler );
    wiced_rtos_set_semaphore( &request_ready );
}

static void bi201_rx_thread_main( uint32_t arg )
{
    uint8_t encoded[REWAIR_BLE_FRAME_WIRE_MAX];
    uint32_t encoded_length = 0u;
    ble_assembler_t assembler;

    (void)arg;
    assembler_reset( &assembler );
    for ( ;; )
    {
        uint8_t byte = 0u;
        wiced_result_t result = wiced_uart_receive_bytes( BI201_UART, &byte, 1u, 25u );

        rewair_ble_diag.heartbeat++;
        rewair_ble_diag.last_uart_result = (int32_t)result;
        if ( result != WICED_SUCCESS )
        {
            continue;
        }
        rewair_ble_diag.rx_bytes++;
        if ( byte == 0u )
        {
            if ( encoded_length != 0u )
            {
                rewair_ble_frame_t frame;

                if ( rewair_ble_frame_decode( encoded, encoded_length, &frame ) == 0 )
                {
                    rewair_ble_diag.valid_frames++;
                    accept_request_frame( &assembler, &frame );
                }
                else
                {
                    rewair_ble_diag.bad_frames++;
                    assembler_reset( &assembler );
                }
            }
            encoded_length = 0u;
            continue;
        }
        if ( encoded_length >= sizeof( encoded ) )
        {
            rewair_ble_diag.oversized_frames++;
            encoded_length = 0u;
            assembler_reset( &assembler );
            continue;
        }
        encoded[encoded_length++] = byte;
    }
}

static void bi201_api_thread_main( uint32_t arg )
{
    (void)arg;
    for ( ;; )
    {
        uint16_t status = 500u;
        int length;

        if ( wiced_rtos_get_semaphore( &request_ready, WICED_NEVER_TIMEOUT ) != WICED_SUCCESS )
        {
            continue;
        }
        length = rewair_api_execute( pending_request.operation,
                                     pending_request.body,
                                     pending_request.body_length,
                                     response_buffer, sizeof( response_buffer ), &status );
        if ( length < 0 )
        {
            static const char fallback[] = "{\"error\":\"response failed\"}";
            status = 500u;
            memcpy( response_buffer, fallback, sizeof( fallback ) );
            length = (int)( sizeof( fallback ) - 1u );
        }
        /* The completed request arrived BLE-to-UART. Let the transparent
         * bridge turn around before starting the UART-to-notification reply. */
        wiced_rtos_delay_milliseconds( 200u );
        (void)send_response( pending_request.operation, pending_request.request_id,
                             status, (const uint8_t*)response_buffer, (uint32_t)length );
        request_pending = 0u;
    }
}

wiced_result_t rewair_ble_start( void )
{
    wiced_result_t result;

    memset( (void*)&rewair_ble_diag, 0, sizeof( rewair_ble_diag ) );
    rewair_ble_diag.magic = REWAIR_BLE_DIAG_MAGIC;
    rewair_ble_diag.version = 1u;
    rewair_ble_diag.size = sizeof( rewair_ble_diag );

    if ( wiced_rtos_init_mutex( &tx_mutex ) != WICED_SUCCESS ||
         wiced_rtos_init_semaphore( &request_ready ) != WICED_SUCCESS ||
         wiced_rtos_init_semaphore( &ack_ready ) != WICED_SUCCESS )
    {
        rewair_ble_diag.phase = REWAIR_BLE_PHASE_FAILED;
        return WICED_ERROR;
    }

    ring_buffer_init( &bi201_rx_ring, bi201_rx_storage, sizeof( bi201_rx_storage ) );
    result = wiced_uart_init( BI201_UART, &bi201_uart_config, &bi201_rx_ring );
    rewair_ble_diag.uart_init_result = (int32_t)result;
    if ( result != WICED_SUCCESS )
    {
        rewair_ble_diag.phase = REWAIR_BLE_PHASE_FAILED;
        return result;
    }
    rewair_ble_diag.phase = REWAIR_BLE_PHASE_UART_READY;

    result = (wiced_result_t)platform_gpio_init( &bi201_control, OUTPUT_PUSH_PULL );
    rewair_ble_diag.gpio_result = (int32_t)result;
    if ( result != WICED_SUCCESS )
    {
        rewair_ble_diag.phase = REWAIR_BLE_PHASE_FAILED;
        return result;
    }
    (void)platform_gpio_output_low( &bi201_control );
    /* Stock used 50 ms, which wakes a healthy module but did not clear stale
     * GATT/CCCD state after repeated abrupt development disconnects. Holding
     * reset/enable low for 500 ms produces a deterministic cold module boot. */
    wiced_rtos_delay_milliseconds( 500u );
    (void)platform_gpio_output_high( &bi201_control );
    rewair_ble_diag.phase = REWAIR_BLE_PHASE_MODULE_SETTLING;
    receive_for( 1000u );

    rewair_ble_diag.phase = REWAIR_BLE_PHASE_COMMAND_MODE;
    if ( transmit_command( "AT#RS\r\n", 1200u ) != WICED_SUCCESS ||
         transmit_command( "AT#WM=0\r\n", 500u ) != WICED_SUCCESS )
    {
        rewair_ble_diag.phase = REWAIR_BLE_PHASE_FAILED;
        return WICED_ERROR;
    }
    rewair_ble_diag.phase = REWAIR_BLE_PHASE_TRANSPARENT;

    if ( wiced_rtos_create_thread( &bi201_api_thread, WICED_DEFAULT_LIBRARY_PRIORITY,
                                   "ble api", bi201_api_thread_main,
                                   BI201_API_THREAD_STACK, NULL ) != WICED_SUCCESS ||
         wiced_rtos_create_thread( &bi201_rx_thread, WICED_DEFAULT_LIBRARY_PRIORITY,
                                   "ble rx", bi201_rx_thread_main,
                                   BI201_RX_THREAD_STACK, NULL ) != WICED_SUCCESS )
    {
        rewair_ble_diag.phase = REWAIR_BLE_PHASE_FAILED;
        return WICED_ERROR;
    }
    return WICED_SUCCESS;
}
