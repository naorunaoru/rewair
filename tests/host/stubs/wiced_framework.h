#pragma once

#include <stdint.h>

#include "wiced.h"

#define DCT_APP_SECTION 0

wiced_result_t wiced_dct_read_lock( void** info_ptr, int ptr_is_writable,
                                    int section, uint32_t offset, uint32_t size );
wiced_result_t wiced_dct_read_unlock( void* info_ptr, int ptr_is_writable );
wiced_result_t wiced_dct_write( const void* info_ptr, int section,
                                uint32_t offset, uint32_t size );
