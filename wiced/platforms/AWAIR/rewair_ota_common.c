#include "rewair_ota_layout.h"

#include <stddef.h>
#include <string.h>

uint32_t rewair_ota_crc32_update( uint32_t crc, const void* data, uint32_t len )
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t i;

    for ( i = 0u; i < len; i++ )
    {
        uint32_t bit;

        crc ^= p[i];
        for ( bit = 0u; bit < 8u; bit++ )
        {
            crc = ( crc >> 1 ) ^ ( 0xedb88320u & ( 0u - ( crc & 1u ) ) );
        }
    }
    return crc;
}

uint32_t rewair_ota_crc32( const void* data, uint32_t len )
{
    return rewair_ota_crc32_update( 0xffffffffu, data, len ) ^ 0xffffffffu;
}

int rewair_ota_vector_valid( uint32_t initial_sp, uint32_t reset_vector, uint32_t image_size )
{
    uint32_t reset_address = reset_vector & ~1u;

    if ( image_size < 8u || image_size > REWAIR_OTA_APP_SIZE )
    {
        return 0;
    }
    if ( initial_sp < REWAIR_OTA_SRAM_ADDR ||
         initial_sp > REWAIR_OTA_SRAM_ADDR + REWAIR_OTA_SRAM_SIZE ||
         ( initial_sp & 3u ) != 0u )
    {
        return 0;
    }
    if ( ( reset_vector & 1u ) == 0u || reset_address < REWAIR_OTA_APP_ADDR ||
         reset_address >= REWAIR_OTA_APP_ADDR + image_size )
    {
        return 0;
    }
    return 1;
}

void rewair_ota_image_header_init( rewair_ota_image_header_t* header, uint32_t target,
                                   uint32_t image_size, uint32_t image_crc32,
                                   uint32_t initial_sp, uint32_t reset_vector )
{
    memset( header, 0, sizeof( *header ) );
    header->magic = REWAIR_OTA_IMAGE_MAGIC;
    header->version = REWAIR_OTA_FORMAT_VERSION;
    header->target = target;
    header->image_size = image_size;
    header->image_crc32 = image_crc32;
    header->initial_sp = initial_sp;
    header->reset_vector = reset_vector;
    header->header_crc32 = rewair_ota_crc32( header, (uint32_t)offsetof( rewair_ota_image_header_t,
                                                                        header_crc32 ) );
}

int rewair_ota_image_header_valid( const rewair_ota_image_header_t* header, uint32_t target )
{
    uint32_t crc;

    if ( header->magic != REWAIR_OTA_IMAGE_MAGIC ||
         header->version != REWAIR_OTA_FORMAT_VERSION ||
         header->target != target )
    {
        return 0;
    }
    crc = rewair_ota_crc32( header, (uint32_t)offsetof( rewair_ota_image_header_t, header_crc32 ) );
    if ( crc != header->header_crc32 )
    {
        return 0;
    }
    return rewair_ota_vector_valid( header->initial_sp, header->reset_vector, header->image_size );
}

void rewair_ota_record_init( rewair_ota_record_t* record, uint32_t sequence, uint32_t state,
                             uint32_t boot_attempts, uint32_t image_size, uint32_t image_crc32 )
{
    memset( record, 0, sizeof( *record ) );
    record->magic = REWAIR_OTA_RECORD_MAGIC;
    record->version = REWAIR_OTA_FORMAT_VERSION;
    record->sequence = sequence;
    record->state = state;
    record->boot_attempts = boot_attempts;
    record->image_size = image_size;
    record->image_crc32 = image_crc32;
    record->record_crc32 = rewair_ota_crc32( record, (uint32_t)offsetof( rewair_ota_record_t,
                                                                        record_crc32 ) );
}

int rewair_ota_record_valid( const rewair_ota_record_t* record )
{
    uint32_t crc;

    if ( record->magic != REWAIR_OTA_RECORD_MAGIC ||
         record->version != REWAIR_OTA_FORMAT_VERSION ||
         record->state < REWAIR_OTA_STATE_STAGED ||
         record->state > REWAIR_OTA_STATE_ROLLED_BACK ||
         record->image_size == 0u || record->image_size > REWAIR_OTA_APP_SIZE )
    {
        return 0;
    }
    crc = rewair_ota_crc32( record, (uint32_t)offsetof( rewair_ota_record_t, record_crc32 ) );
    return crc == record->record_crc32;
}
