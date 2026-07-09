#pragma once

#include <stdint.h>

#include "wiced.h"
#include "rewair_ota_layout.h"

/* Registers a trial-boot liveness monitor when the bootloader has just applied
 * an update. Call once, immediately after wiced_core_init(). */
void rewair_ota_trial_watch_start( void );

wiced_result_t rewair_ota_upload_begin( uint32_t image_size, uint32_t image_crc32 );
wiced_result_t rewair_ota_upload_chunk( uint32_t offset, const uint8_t* data, uint32_t size );
wiced_result_t rewair_ota_upload_commit( void );
uint32_t rewair_ota_upload_received( void );
