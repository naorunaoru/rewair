#include "wiced.h"
#include "platform_peripheral.h"

#include <stdint.h>
#include <string.h>

#define BI201_UART                 WICED_UART_1
#define BI201_RX_BUFFER_SIZE       256u
#define BI201_RX_LOG_SIZE          1024u
#define BI201_MAILBOX_MAGIC        0x30324942u /* "BI20" in target memory */
#define BI201_MAILBOX_VERSION      1u

enum
{
    BI201_PHASE_BOOT = 0,
    BI201_PHASE_UART_READY,
    BI201_PHASE_MODULE_SETTLING,
    BI201_PHASE_QUERYING,
    BI201_PHASE_TRANSPARENT,
    BI201_PHASE_FAILED
};

/*
 * This symbol is intentionally global and stable in the ELF.  The host-side
 * read_bi201_probe.zsh script resolves its address with nm, then snapshots it
 * over SWD with probe-rs.  Do not make it static or remove volatile.
 */
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
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t rx_overwritten;
    uint32_t last_rx_ms;
    uint32_t rx_write;
    uint32_t beacon_seq;
    uint8_t  rx_log[BI201_RX_LOG_SIZE];
} bi201_probe_mailbox_t;

volatile bi201_probe_mailbox_t bi201_probe_mailbox;

static const platform_gpio_t bi201_control = { GPIOB, 13 };
static wiced_ring_buffer_t uart_rx_ring;
static uint8_t uart_rx_storage[BI201_RX_BUFFER_SIZE];

static const wiced_uart_config_t uart_config =
{
    .baud_rate    = 115200,
    .data_width   = DATA_WIDTH_8BIT,
    .parity       = NO_PARITY,
    .stop_bits    = STOP_BITS_1,
    .flow_control = FLOW_CONTROL_DISABLED,
};

static uint32_t now_ms( void )
{
    wiced_time_t now = 0;
    (void) wiced_time_get_time( &now );
    return (uint32_t) now;
}

static void record_rx_byte( uint8_t byte )
{
    uint32_t write = bi201_probe_mailbox.rx_write;

    bi201_probe_mailbox.rx_log[write % BI201_RX_LOG_SIZE] = byte;
    bi201_probe_mailbox.rx_write = write + 1u;
    bi201_probe_mailbox.rx_bytes++;
    if ( bi201_probe_mailbox.rx_bytes > BI201_RX_LOG_SIZE )
    {
        bi201_probe_mailbox.rx_overwritten =
            bi201_probe_mailbox.rx_bytes - BI201_RX_LOG_SIZE;
    }
    bi201_probe_mailbox.last_rx_ms = now_ms( );
}

static void receive_for( uint32_t duration_ms )
{
    uint32_t start = now_ms( );

    while ( (uint32_t) ( now_ms( ) - start ) < duration_ms )
    {
        uint8_t byte = 0;
        wiced_result_t result =
            wiced_uart_receive_bytes( BI201_UART, &byte, 1u, 25u );

        bi201_probe_mailbox.heartbeat++;
        bi201_probe_mailbox.last_uart_result = (int32_t) result;
        if ( result == WICED_SUCCESS )
        {
            record_rx_byte( byte );
        }
    }
}

static wiced_result_t transmit_text( const char* text )
{
    uint32_t length = (uint32_t) strlen( text );
    wiced_result_t result =
        wiced_uart_transmit_bytes( BI201_UART, text, length );

    bi201_probe_mailbox.last_uart_result = (int32_t) result;
    if ( result == WICED_SUCCESS )
    {
        bi201_probe_mailbox.tx_bytes += length;
    }
    return result;
}

static int query( const char* command, uint32_t response_ms )
{
    if ( transmit_text( command ) != WICED_SUCCESS )
    {
        bi201_probe_mailbox.phase = BI201_PHASE_FAILED;
        return -1;
    }
    receive_for( response_ms );
    return 0;
}

void application_start( void )
{
    wiced_result_t result;
    uint32_t next_beacon;

    memset( (void*) &bi201_probe_mailbox, 0, sizeof( bi201_probe_mailbox ) );
    bi201_probe_mailbox.magic = BI201_MAILBOX_MAGIC;
    bi201_probe_mailbox.version = BI201_MAILBOX_VERSION;
    bi201_probe_mailbox.size = sizeof( bi201_probe_mailbox );
    bi201_probe_mailbox.phase = BI201_PHASE_BOOT;

    /* USART1 is not initialized as stdio in this app.  Its PB6/PA10 pins are
     * dedicated to the BI201, with a passive host RX tap allowed on PB6. */
    ring_buffer_init( &uart_rx_ring, uart_rx_storage, sizeof( uart_rx_storage ) );
    result = wiced_uart_init( BI201_UART, &uart_config, &uart_rx_ring );
    bi201_probe_mailbox.uart_init_result = (int32_t) result;
    if ( result != WICED_SUCCESS )
    {
        bi201_probe_mailbox.phase = BI201_PHASE_FAILED;
        for ( ;; )
        {
            bi201_probe_mailbox.heartbeat++;
            wiced_rtos_delay_milliseconds( 100u );
        }
    }
    bi201_probe_mailbox.phase = BI201_PHASE_UART_READY;

    /* Matches the stock F411 sequence recovered from the firmware: PB13 low
     * for 50 ms, high to enable/release the module, then wait 1 second. */
    result = (wiced_result_t) platform_gpio_init( &bi201_control, OUTPUT_PUSH_PULL );
    bi201_probe_mailbox.gpio_result = (int32_t) result;
    if ( result != WICED_SUCCESS )
    {
        bi201_probe_mailbox.phase = BI201_PHASE_FAILED;
        return;
    }
    (void) platform_gpio_output_low( &bi201_control );
    wiced_rtos_delay_milliseconds( 50u );
    (void) platform_gpio_output_high( &bi201_control );
    bi201_probe_mailbox.phase = BI201_PHASE_MODULE_SETTLING;
    receive_for( 1000u );

    /* AT#RS is the first command used by the stock F411.  The remaining
     * commands are read-only queries documented by Glead.  AT#WM=0 finally
     * selects transparent mode, also matching stock behavior. */
    bi201_probe_mailbox.phase = BI201_PHASE_QUERYING;
    if ( query( "AT#RS\r\n", 1200u ) != 0 ||
         query( "AT#MN=?\r\n", 400u ) != 0 ||
         query( "AT#SW=?\r\n", 400u ) != 0 ||
         query( "AT#WM=?\r\n", 400u ) != 0 ||
         query( "AT#FL=?\r\n", 400u ) != 0 ||
         query( "AT#WM=0\r\n", 400u ) != 0 )
    {
        return;
    }

    bi201_probe_mailbox.phase = BI201_PHASE_TRANSPARENT;
    next_beacon = now_ms( );

    for ( ;; )
    {
        uint8_t byte = 0;
        uint32_t now;

        result = wiced_uart_receive_bytes( BI201_UART, &byte, 1u, 25u );
        bi201_probe_mailbox.heartbeat++;
        bi201_probe_mailbox.last_uart_result = (int32_t) result;
        if ( result == WICED_SUCCESS )
        {
            record_rx_byte( byte );
        }

        now = now_ms( );
        if ( (int32_t) ( now - next_beacon ) >= 0 )
        {
            char beacon[64];
            int length;

            length = snprintf( beacon, sizeof( beacon ),
                               "{\"cmd\":\"rewair_probe\",\"seq\":%lu}\r\n",
                               (unsigned long) bi201_probe_mailbox.beacon_seq );
            if ( length > 0 && (uint32_t) length < sizeof( beacon ) )
            {
                result = wiced_uart_transmit_bytes( BI201_UART, beacon, (uint32_t) length );
                bi201_probe_mailbox.last_uart_result = (int32_t) result;
                if ( result == WICED_SUCCESS )
                {
                    bi201_probe_mailbox.tx_bytes += (uint32_t) length;
                    bi201_probe_mailbox.beacon_seq++;
                }
            }
            next_beacon = now + 2000u;
        }
    }
}
