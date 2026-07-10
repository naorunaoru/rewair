#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REWAIR_OTA_FORMAT_VERSION       1u
#define REWAIR_OTA_IMAGE_MAGIC          0x544f5752u /* "RWOT", little-endian */
#define REWAIR_OTA_RECORD_MAGIC         0x4d4f5752u /* "RWOM", little-endian */

#define REWAIR_OTA_SFLASH_CAPACITY      0x200000u
#define REWAIR_OTA_STAGE_ADDR           0x000000u
#define REWAIR_OTA_STAGE_SLOT_SIZE      0x080000u
#define REWAIR_OTA_BACKUP_ADDR          0x080000u
#define REWAIR_OTA_BACKUP_SLOT_SIZE     0x080000u
#define REWAIR_OTA_META_ADDR            0x100000u
#define REWAIR_OTA_META_SIZE            0x001000u
#define REWAIR_WIFI_FW_LUT_ADDR         0x101000u
#define REWAIR_WIFI_FW_BLOB_ADDR        0x102000u
#define REWAIR_WIFI_FW_SLOT_SIZE        0x034000u

#define REWAIR_OTA_SFLASH_SECTOR_SIZE   0x001000u
#define REWAIR_OTA_IMAGE_DATA_OFFSET    0x000100u

#define REWAIR_OTA_APP_ADDR             0x0800c000u
#define REWAIR_OTA_APP_SIZE             0x00074000u
#define REWAIR_OTA_SRAM_ADDR            0x20000000u
#define REWAIR_OTA_SRAM_SIZE            0x00020000u

#define REWAIR_OTA_UPLOAD_CHUNK_MAX     16384u
#define REWAIR_OTA_TRIAL_MAX_BOOTS      3u

typedef enum
{
    REWAIR_OTA_TARGET_APP = 1u,
    REWAIR_OTA_TARGET_BACKUP = 2u,
} rewair_ota_target_t;

typedef enum
{
    REWAIR_OTA_STATE_STAGED = 1u,
    REWAIR_OTA_STATE_BACKUP_READY = 2u,
    REWAIR_OTA_STATE_TRIAL = 3u,
    REWAIR_OTA_STATE_CONFIRMED = 4u,
    REWAIR_OTA_STATE_ROLLED_BACK = 5u,
} rewair_ota_state_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t target;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t initial_sp;
    uint32_t reset_vector;
    uint32_t header_crc32;
} rewair_ota_image_header_t;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t state;
    uint32_t boot_attempts;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t record_crc32;
} rewair_ota_record_t;

uint32_t rewair_ota_crc32_update( uint32_t crc, const void* data, uint32_t len );
uint32_t rewair_ota_crc32( const void* data, uint32_t len );
int rewair_ota_vector_valid( uint32_t initial_sp, uint32_t reset_vector, uint32_t image_size );
void rewair_ota_image_header_init( rewair_ota_image_header_t* header, uint32_t target,
                                   uint32_t image_size, uint32_t image_crc32,
                                   uint32_t initial_sp, uint32_t reset_vector );
int rewair_ota_image_header_valid( const rewair_ota_image_header_t* header, uint32_t target );
void rewair_ota_record_init( rewair_ota_record_t* record, uint32_t sequence, uint32_t state,
                             uint32_t boot_attempts, uint32_t image_size, uint32_t image_crc32 );
int rewair_ota_record_valid( const rewair_ota_record_t* record );

#ifdef __cplusplus
} /* extern "C" */
#endif
