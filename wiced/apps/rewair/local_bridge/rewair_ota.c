#include "rewair_ota.h"

#include <stdio.h>
#include <string.h>

#include "spi_flash.h"
#include "rewair_sflash.h"
#include "web_api.h"
#include "wiced_management.h"

#define OTA_VERIFY_CHUNK          512u
#define OTA_HEALTH_THREAD_STACK   2048u
#define OTA_HEALTH_POLL_MS        1000u
#define OTA_HEALTH_SETTLE_MS      15000u
#define OTA_TRIAL_TIMEOUT_MS      ( 6u * 60u * 1000u )
#define OTA_META_RECORD_COUNT     ( REWAIR_OTA_META_SIZE / sizeof( rewair_ota_record_t ) )

typedef struct
{
    uint32_t active;
    uint32_t expected_size;
    uint32_t expected_crc32;
    uint32_t received;
    uint32_t running_crc32;
    uint32_t initial_sp;
    uint32_t reset_vector;
} ota_upload_session_t;

static ota_upload_session_t upload;
static wiced_system_monitor_t trial_monitor;
static wiced_thread_t health_thread;

static int ota_bytes_erased( const void* data, uint32_t len )
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t i;

    for ( i = 0u; i < len; i++ )
    {
        if ( p[i] != 0xffu )
        {
            return 0;
        }
    }
    return 1;
}

static int ota_read_latest_direct( rewair_ota_record_t* latest )
{
    sflash_handle_t flash;
    uint32_t i;
    int found = 0;

    memset( latest, 0, sizeof( *latest ) );
    if ( init_sflash( &flash, NULL, SFLASH_WRITE_NOT_ALLOWED ) != 0 )
    {
        return -1;
    }
    for ( i = 0u; i < OTA_META_RECORD_COUNT; i++ )
    {
        rewair_ota_record_t record;

        if ( sflash_read( &flash, REWAIR_OTA_META_ADDR + i * (uint32_t)sizeof( record ),
                          &record, sizeof( record ) ) != 0 )
        {
            deinit_sflash( &flash );
            return -1;
        }
        if ( rewair_ota_record_valid( &record ) != 0 &&
             ( found == 0 || record.sequence > latest->sequence ) )
        {
            *latest = record;
            found = 1;
        }
    }
    deinit_sflash( &flash );
    return found;
}

static int ota_read_latest( rewair_ota_record_t* latest, uint32_t* append_address )
{
    uint32_t i;
    int found = 0;

    memset( latest, 0, sizeof( *latest ) );
    *append_address = 0u;
    for ( i = 0u; i < OTA_META_RECORD_COUNT; i++ )
    {
        rewair_ota_record_t record;
        uint32_t address = REWAIR_OTA_META_ADDR + i * (uint32_t)sizeof( record );

        if ( rewair_sflash_read_bytes( address, (uint8_t*)&record, sizeof( record ) ) != 0 )
        {
            return -1;
        }
        if ( ota_bytes_erased( &record, sizeof( record ) ) != 0 )
        {
            if ( *append_address == 0u )
            {
                *append_address = address;
            }
            continue;
        }
        if ( rewair_ota_record_valid( &record ) != 0 &&
             ( found == 0 || record.sequence > latest->sequence ) )
        {
            *latest = record;
            found = 1;
        }
    }
    return found;
}

static int ota_append_record( uint32_t state, uint32_t boot_attempts,
                              uint32_t image_size, uint32_t image_crc32 )
{
    rewair_ota_record_t latest;
    rewair_ota_record_t next;
    uint32_t append_address;
    int found = ota_read_latest( &latest, &append_address );

    if ( found < 0 || append_address == 0u )
    {
        return -1;
    }
    rewair_ota_record_init( &next, found > 0 ? latest.sequence + 1u : 1u, state,
                            boot_attempts, image_size, image_crc32 );
    return rewair_sflash_write_bytes( append_address, (const uint8_t*)&next,
                                      sizeof( next ) );
}

static int ota_readback_crc( uint32_t address, uint32_t size, uint32_t* out_crc )
{
    uint8_t buffer[OTA_VERIFY_CHUNK];
    uint32_t crc = 0xffffffffu;

    while ( size != 0u )
    {
        uint32_t take = size < sizeof( buffer ) ? size : (uint32_t)sizeof( buffer );

        if ( rewair_sflash_read_bytes( address, buffer, take ) != 0 )
        {
            return -1;
        }
        crc = rewair_ota_crc32_update( crc, buffer, take );
        address += take;
        size -= take;
    }
    *out_crc = crc ^ 0xffffffffu;
    return 0;
}

static void ota_health_thread_main( uint32_t arg )
{
    uint32_t elapsed = 0u;

    (void)arg;
    while ( elapsed < OTA_TRIAL_TIMEOUT_MS )
    {
        if ( rewair_web_api_is_started( ) != 0u )
        {
            rewair_ota_record_t latest;
            uint32_t append_address;

            wiced_rtos_delay_milliseconds( OTA_HEALTH_SETTLE_MS );
            if ( ota_read_latest( &latest, &append_address ) > 0 &&
                 latest.state == REWAIR_OTA_STATE_TRIAL &&
                 ota_append_record( REWAIR_OTA_STATE_CONFIRMED, latest.boot_attempts,
                                    latest.image_size, latest.image_crc32 ) == 0 )
            {
                printf( "[ota] trial confirmed\n" );
                wiced_update_system_monitor( &trial_monitor, 0xffffffffu );
            }
            WICED_END_OF_CURRENT_THREAD( );
        }
        wiced_rtos_delay_milliseconds( OTA_HEALTH_POLL_MS );
        elapsed += OTA_HEALTH_POLL_MS;
    }
    /* The registered monitor reboots the device if the web server never came
     * up. The bootloader increments the trial counter and eventually restores
     * the preserved image. */
    WICED_END_OF_CURRENT_THREAD( );
}

void rewair_ota_trial_watch_start( void )
{
    rewair_ota_record_t latest;

    if ( ota_read_latest_direct( &latest ) <= 0 || latest.state != REWAIR_OTA_STATE_TRIAL )
    {
        return;
    }
    if ( wiced_register_system_monitor( &trial_monitor, OTA_TRIAL_TIMEOUT_MS ) != WICED_SUCCESS )
    {
        printf( "[ota] trial monitor registration failed\n" );
        return;
    }
    if ( wiced_rtos_create_thread( &health_thread, WICED_DEFAULT_LIBRARY_PRIORITY, "ota health",
                                   ota_health_thread_main, OTA_HEALTH_THREAD_STACK, NULL ) != WICED_SUCCESS )
    {
        printf( "[ota] trial health thread failed\n" );
    }
    else
    {
        printf( "[ota] trial boot %lu/%u\n", (unsigned long)latest.boot_attempts,
                (unsigned int)REWAIR_OTA_TRIAL_MAX_BOOTS );
    }
}

wiced_result_t rewair_ota_upload_begin( uint32_t image_size, uint32_t image_crc32 )
{
    rewair_ota_record_t latest;
    uint32_t append_address;
    uint32_t erase_size;
    int found;

    if ( image_size < 8u || image_size > REWAIR_OTA_APP_SIZE )
    {
        return WICED_BADARG;
    }
    found = ota_read_latest( &latest, &append_address );
    if ( found < 0 )
    {
        return WICED_ERROR;
    }
    if ( found > 0 && ( latest.state == REWAIR_OTA_STATE_STAGED ||
                        latest.state == REWAIR_OTA_STATE_BACKUP_READY ||
                        latest.state == REWAIR_OTA_STATE_TRIAL ) )
    {
        return WICED_ALREADY_INITIALIZED;
    }

    memset( &upload, 0, sizeof( upload ) );
    if ( rewair_sflash_erase_range( REWAIR_OTA_META_ADDR, REWAIR_OTA_META_SIZE ) != 0 )
    {
        return WICED_ERROR;
    }
    erase_size = REWAIR_OTA_IMAGE_DATA_OFFSET + image_size;
    erase_size = ( erase_size + REWAIR_OTA_SFLASH_SECTOR_SIZE - 1u ) &
                 ~( REWAIR_OTA_SFLASH_SECTOR_SIZE - 1u );
    if ( erase_size > REWAIR_OTA_STAGE_SLOT_SIZE ||
         rewair_sflash_erase_range( REWAIR_OTA_STAGE_ADDR, erase_size ) != 0 )
    {
        return WICED_ERROR;
    }

    upload.active = 1u;
    upload.expected_size = image_size;
    upload.expected_crc32 = image_crc32;
    upload.running_crc32 = 0xffffffffu;
    printf( "[ota] upload begin size=%lu crc=%08lx\n", (unsigned long)image_size,
            (unsigned long)image_crc32 );
    return WICED_SUCCESS;
}

wiced_result_t rewair_ota_upload_chunk( uint32_t offset, const uint8_t* data, uint32_t size )
{
    uint32_t vector_bytes;

    if ( upload.active == 0u || data == NULL || size == 0u ||
         size > REWAIR_OTA_UPLOAD_CHUNK_MAX || offset != upload.received ||
         upload.received > upload.expected_size || size > upload.expected_size - upload.received )
    {
        return WICED_BADARG;
    }
    if ( rewair_sflash_write_bytes( REWAIR_OTA_STAGE_ADDR + REWAIR_OTA_IMAGE_DATA_OFFSET + offset,
                                    data, size ) != 0 )
    {
        upload.active = 0u;
        return WICED_ERROR;
    }

    vector_bytes = upload.received < 8u ? 8u - upload.received : 0u;
    if ( vector_bytes > size )
    {
        vector_bytes = size;
    }
    if ( vector_bytes != 0u )
    {
        memcpy( (uint8_t*)&upload.initial_sp + upload.received, data, vector_bytes );
    }
    upload.running_crc32 = rewair_ota_crc32_update( upload.running_crc32, data, size );
    upload.received += size;
    return WICED_SUCCESS;
}

wiced_result_t rewair_ota_upload_commit( void )
{
    rewair_ota_image_header_t header;
    uint32_t readback_crc;
    uint32_t uploaded_crc;

    if ( upload.active == 0u || upload.received != upload.expected_size )
    {
        return WICED_BADARG;
    }
    uploaded_crc = upload.running_crc32 ^ 0xffffffffu;
    if ( uploaded_crc != upload.expected_crc32 ||
         rewair_ota_vector_valid( upload.initial_sp, upload.reset_vector,
                                  upload.expected_size ) == 0 )
    {
        upload.active = 0u;
        return WICED_BADARG;
    }
    if ( ota_readback_crc( REWAIR_OTA_STAGE_ADDR + REWAIR_OTA_IMAGE_DATA_OFFSET,
                           upload.expected_size, &readback_crc ) != 0 ||
         readback_crc != upload.expected_crc32 )
    {
        upload.active = 0u;
        return WICED_ERROR;
    }

    rewair_ota_image_header_init( &header, REWAIR_OTA_TARGET_APP, upload.expected_size,
                                  upload.expected_crc32, upload.initial_sp, upload.reset_vector );
    if ( rewair_sflash_write_bytes( REWAIR_OTA_STAGE_ADDR, (const uint8_t*)&header,
                                    sizeof( header ) ) != 0 ||
         ota_append_record( REWAIR_OTA_STATE_STAGED, 0u, upload.expected_size,
                            upload.expected_crc32 ) != 0 )
    {
        upload.active = 0u;
        return WICED_ERROR;
    }
    printf( "[ota] upload verified and staged\n" );
    upload.active = 0u;
    return WICED_SUCCESS;
}

uint32_t rewair_ota_upload_received( void )
{
    return upload.received;
}
