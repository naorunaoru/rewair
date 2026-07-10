#pragma once

#include <stdint.h>

int rewair_sflash_ensure_init( void );
int rewair_sflash_bounds_ok( uint32_t addr, uint32_t len );
int rewair_sflash_read_id( uint8_t out_id[3] );
int rewair_sflash_read_bytes( uint32_t addr, uint8_t* out, uint32_t size );

/* erase_range requires sector-aligned address and size. Callers own the flash
 * partition policy; these wrappers enforce device bounds and serialization. */
int rewair_sflash_write_bytes( uint32_t addr, const uint8_t* data, uint32_t size );
int rewair_sflash_erase_range( uint32_t addr, uint32_t size );
