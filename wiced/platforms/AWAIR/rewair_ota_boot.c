#include "rewair_ota_boot.h"

#include <string.h>

#include "platform_peripheral.h"
#include "rewair_ota_layout.h"
#include "spi_flash.h"

platform_result_t platform_write_flash_chunk( uint32_t address, const void* data, uint32_t size );

#define OTA_COPY_CHUNK 512u
#define OTA_META_RECORD_COUNT ( REWAIR_OTA_META_SIZE / sizeof( rewair_ota_record_t ) )

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

static int ota_read_latest( const sflash_handle_t* flash, rewair_ota_record_t* latest,
                            uint32_t* append_address )
{
    uint32_t i;
    int found = 0;

    *append_address = 0u;
    memset( latest, 0, sizeof( *latest ) );
    for ( i = 0u; i < OTA_META_RECORD_COUNT; i++ )
    {
        rewair_ota_record_t record;
        uint32_t address = REWAIR_OTA_META_ADDR + i * (uint32_t)sizeof( record );

        if ( sflash_read( flash, address, &record, sizeof( record ) ) != 0 )
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

static int ota_append_record( const sflash_handle_t* flash, const rewair_ota_record_t* previous,
                              uint32_t append_address, uint32_t state, uint32_t boot_attempts )
{
    rewair_ota_record_t record;

    if ( append_address < REWAIR_OTA_META_ADDR ||
         append_address + sizeof( record ) > REWAIR_OTA_META_ADDR + REWAIR_OTA_META_SIZE )
    {
        return -1;
    }
    rewair_ota_record_init( &record, previous->sequence + 1u, state, boot_attempts,
                            previous->image_size, previous->image_crc32 );
    return sflash_write( flash, append_address, &record, sizeof( record ) ) == 0 ? 0 : -1;
}

static int ota_flash_crc( const sflash_handle_t* flash, uint32_t address, uint32_t size,
                          uint32_t* out_crc )
{
    uint8_t buffer[OTA_COPY_CHUNK];
    uint32_t crc = 0xffffffffu;

    while ( size != 0u )
    {
        uint32_t take = size < sizeof( buffer ) ? size : (uint32_t)sizeof( buffer );

        if ( sflash_read( flash, address, buffer, take ) != 0 )
        {
            return -1;
        }
        crc = rewair_ota_crc32_update( crc, buffer, take );
        address += take;
        size -= take;
        platform_watchdog_kick( );
    }
    *out_crc = crc ^ 0xffffffffu;
    return 0;
}

static uint32_t ota_internal_crc( uint32_t size )
{
    return rewair_ota_crc32( (const void*)REWAIR_OTA_APP_ADDR, size );
}

static int ota_erase_internal_app( void )
{
    static const uint16_t sectors[] =
    {
        FLASH_Sector_3,
        FLASH_Sector_4,
        FLASH_Sector_5,
        FLASH_Sector_6,
        FLASH_Sector_7
    };
    uint32_t i;

    FLASH_Unlock( );
    FLASH_ClearFlag( FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                     FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR );
    for ( i = 0u; i < sizeof( sectors ) / sizeof( sectors[0] ); i++ )
    {
        if ( FLASH_EraseSector( sectors[i], VoltageRange_3 ) != FLASH_COMPLETE )
        {
            FLASH_Lock( );
            return -1;
        }
        platform_watchdog_kick( );
    }
    FLASH_Lock( );
    return 0;
}

static int ota_erase_external( const sflash_handle_t* flash, uint32_t address, uint32_t size )
{
    uint32_t end = address + size;

    while ( address < end )
    {
        if ( sflash_sector_erase( flash, address ) != 0 )
        {
            return -1;
        }
        address += REWAIR_OTA_SFLASH_SECTOR_SIZE;
        platform_watchdog_kick( );
    }
    return 0;
}

static int ota_backup_running_app( const sflash_handle_t* flash )
{
    rewair_ota_image_header_t header;
    uint8_t buffer[OTA_COPY_CHUNK];
    uint32_t offset = 0u;
    uint32_t crc = 0xffffffffu;
    const uint32_t* vectors = (const uint32_t*)REWAIR_OTA_APP_ADDR;

    if ( rewair_ota_vector_valid( vectors[0], vectors[1], REWAIR_OTA_APP_SIZE ) == 0 )
    {
        return -1;
    }
    if ( ota_erase_external( flash, REWAIR_OTA_BACKUP_ADDR, REWAIR_OTA_BACKUP_SLOT_SIZE ) != 0 )
    {
        return -1;
    }
    while ( offset < REWAIR_OTA_APP_SIZE )
    {
        uint32_t take = REWAIR_OTA_APP_SIZE - offset;

        if ( take > sizeof( buffer ) )
        {
            take = sizeof( buffer );
        }
        memcpy( buffer, (const void*)( REWAIR_OTA_APP_ADDR + offset ), take );
        if ( sflash_write( flash, REWAIR_OTA_BACKUP_ADDR + REWAIR_OTA_IMAGE_DATA_OFFSET + offset,
                           buffer, take ) != 0 )
        {
            return -1;
        }
        crc = rewair_ota_crc32_update( crc, buffer, take );
        offset += take;
        platform_watchdog_kick( );
    }
    crc ^= 0xffffffffu;
    rewair_ota_image_header_init( &header, REWAIR_OTA_TARGET_BACKUP, REWAIR_OTA_APP_SIZE,
                                  crc, vectors[0], vectors[1] );
    if ( sflash_write( flash, REWAIR_OTA_BACKUP_ADDR, &header, sizeof( header ) ) != 0 )
    {
        return -1;
    }
    return 0;
}

static int ota_image_valid( const sflash_handle_t* flash, uint32_t address, uint32_t target,
                            rewair_ota_image_header_t* header )
{
    uint32_t crc;
    uint32_t vectors[2];

    if ( sflash_read( flash, address, header, sizeof( *header ) ) != 0 ||
         rewair_ota_image_header_valid( header, target ) == 0 )
    {
        return -1;
    }
    if ( sflash_read( flash, address + REWAIR_OTA_IMAGE_DATA_OFFSET, vectors,
                      sizeof( vectors ) ) != 0 ||
         vectors[0] != header->initial_sp || vectors[1] != header->reset_vector )
    {
        return -1;
    }
    if ( ota_flash_crc( flash, address + REWAIR_OTA_IMAGE_DATA_OFFSET,
                        header->image_size, &crc ) != 0 || crc != header->image_crc32 )
    {
        return -1;
    }
    return 0;
}

static int ota_copy_to_internal( const sflash_handle_t* flash, uint32_t source_address,
                                 uint32_t size, uint32_t expected_crc )
{
    uint8_t buffer[OTA_COPY_CHUNK];
    uint32_t offset = 0u;

    if ( ota_erase_internal_app( ) != 0 )
    {
        return -1;
    }
    while ( offset < size )
    {
        uint32_t take = size - offset;

        if ( take > sizeof( buffer ) )
        {
            take = sizeof( buffer );
        }
        if ( sflash_read( flash, source_address + offset, buffer, take ) != 0 ||
             platform_write_flash_chunk( REWAIR_OTA_APP_ADDR + offset, buffer, take ) != PLATFORM_SUCCESS )
        {
            return -1;
        }
        offset += take;
        platform_watchdog_kick( );
    }
    return ota_internal_crc( size ) == expected_crc ? 0 : -1;
}

static int ota_restore_backup( const sflash_handle_t* flash )
{
    rewair_ota_image_header_t backup;

    if ( ota_image_valid( flash, REWAIR_OTA_BACKUP_ADDR, REWAIR_OTA_TARGET_BACKUP,
                          &backup ) != 0 )
    {
        return -1;
    }
    return ota_copy_to_internal( flash, REWAIR_OTA_BACKUP_ADDR + REWAIR_OTA_IMAGE_DATA_OFFSET,
                                 backup.image_size, backup.image_crc32 );
}

int rewair_ota_boot_check( void )
{
    sflash_handle_t flash;
    rewair_ota_record_t record;
    rewair_ota_image_header_t staged;
    rewair_ota_image_header_t backup;
    uint32_t append_address;
    int found;

    if ( init_sflash( &flash, NULL, SFLASH_WRITE_ALLOWED ) != 0 )
    {
        return 0;
    }
    found = ota_read_latest( &flash, &record, &append_address );
    if ( found <= 0 || record.state == REWAIR_OTA_STATE_CONFIRMED ||
         record.state == REWAIR_OTA_STATE_ROLLED_BACK )
    {
        deinit_sflash( &flash );
        return 0;
    }

    if ( record.state == REWAIR_OTA_STATE_STAGED )
    {
        if ( ota_image_valid( &flash, REWAIR_OTA_STAGE_ADDR, REWAIR_OTA_TARGET_APP,
                              &staged ) != 0 || staged.image_size != record.image_size ||
             staged.image_crc32 != record.image_crc32 || ota_backup_running_app( &flash ) != 0 ||
             ota_image_valid( &flash, REWAIR_OTA_BACKUP_ADDR, REWAIR_OTA_TARGET_BACKUP,
                              &backup ) != 0 )
        {
            ota_append_record( &flash, &record, append_address, REWAIR_OTA_STATE_ROLLED_BACK, 0u );
            deinit_sflash( &flash );
            return 0;
        }
        if ( ota_append_record( &flash, &record, append_address,
                                REWAIR_OTA_STATE_BACKUP_READY, 0u ) != 0 ||
             ota_read_latest( &flash, &record, &append_address ) <= 0 )
        {
            deinit_sflash( &flash );
            return 0;
        }
    }

    if ( record.state == REWAIR_OTA_STATE_BACKUP_READY )
    {
        if ( ota_image_valid( &flash, REWAIR_OTA_STAGE_ADDR, REWAIR_OTA_TARGET_APP,
                              &staged ) == 0 &&
             ota_image_valid( &flash, REWAIR_OTA_BACKUP_ADDR, REWAIR_OTA_TARGET_BACKUP,
                              &backup ) == 0 &&
             staged.image_size == record.image_size && staged.image_crc32 == record.image_crc32 &&
             ota_copy_to_internal( &flash, REWAIR_OTA_STAGE_ADDR + REWAIR_OTA_IMAGE_DATA_OFFSET,
                                   staged.image_size, staged.image_crc32 ) == 0 )
        {
            ota_append_record( &flash, &record, append_address, REWAIR_OTA_STATE_TRIAL, 1u );
        }
        else if ( ota_restore_backup( &flash ) == 0 )
        {
            ota_append_record( &flash, &record, append_address,
                               REWAIR_OTA_STATE_ROLLED_BACK, 0u );
        }
        deinit_sflash( &flash );
        return 0;
    }

    if ( record.state == REWAIR_OTA_STATE_TRIAL )
    {
        if ( record.boot_attempts < REWAIR_OTA_TRIAL_MAX_BOOTS )
        {
            ota_append_record( &flash, &record, append_address, REWAIR_OTA_STATE_TRIAL,
                               record.boot_attempts + 1u );
        }
        else if ( ota_restore_backup( &flash ) == 0 )
        {
            ota_append_record( &flash, &record, append_address,
                               REWAIR_OTA_STATE_ROLLED_BACK, 0u );
        }
    }

    deinit_sflash( &flash );
    return 0;
}
