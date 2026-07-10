#pragma once

/*
 * RWFS: a minimal packed-image format for serving the web UI from a bounded
 * read-only byte source. On-disk layout (all fields little-endian):
 *
 *   offset 0: header
 *     char     magic[4]     "RWF1"
 *     uint32_t file_count
 *     uint32_t total_size    -- whole image size, in bytes
 *     uint32_t table_crc32   -- CRC32 of the raw table bytes (entries only,
 *                               not this header)
 *   offset 16: table[file_count], 40 bytes per entry
 *     char     path[24]      -- NUL-padded, e.g. "/app.js"
 *     uint32_t offset        -- of this file's blob, relative to image start
 *     uint32_t size          -- stored (possibly gzipped) byte length
 *     uint32_t crc32         -- CRC32 of the stored bytes
 *     uint8_t  gzip          -- 1 if the blob is gzip-compressed, else 0
 *     uint8_t  pad[3]
 *   data: concatenated file blobs follow the table.
 *
 * This module only touches <stdint.h>/<string.h> plus its own header -- no
 * wiced/platform includes -- so it is fully host-testable. All flash access
 * goes through the caller-supplied read callback.
 */

#include <stdint.h>

#define REWAIR_UIFS_MAX_FILES 16u
#define REWAIR_UIFS_MAX_PATH  24u
#define REWAIR_UIFS_MAGIC     "RWF1"

/* Reads `len` bytes starting at image-relative byte `addr` into `buf`.
 * Returns 0 on success, nonzero on failure. */
typedef int ( *rewair_uifs_read_t )( uint32_t addr, void* buf, uint32_t len, void* ctx );

typedef struct
{
    char     path[REWAIR_UIFS_MAX_PATH];
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint8_t  gzip;
    uint8_t  pad[3];
} rewair_uifs_file_t;

typedef struct
{
    rewair_uifs_read_t read;
    void*               read_ctx;
    uint32_t            file_count;
    uint32_t            total_size;
    rewair_uifs_file_t  files[REWAIR_UIFS_MAX_FILES];
} rewair_uifs_t;

/* Reads and validates the header + table via `read` (magic, file_count limit,
 * table_crc32, and every entry's offset+size <= total_size). Does not read
 * blob data. Returns 0 on success, nonzero if anything fails validation. */
int rewair_uifs_init( rewair_uifs_t* fs, rewair_uifs_read_t read, void* ctx );

/* Looks up `path` (exact match) in the already-initialized table.
 * Returns 0 and fills *out on success, nonzero if not found. */
int rewair_uifs_find( const rewair_uifs_t* fs, const char* path, rewair_uifs_file_t* out );

/* Reads `len` bytes starting at file-relative byte `off` from `f`'s blob.
 * Returns 0 on success, nonzero if the range falls outside f->size or the
 * underlying read callback fails. */
int rewair_uifs_read( const rewair_uifs_t* fs, const rewair_uifs_file_t* f, uint32_t off,
                      void* buf, uint32_t len );

/* Streams f's stored bytes through `fs->read` in bounded chunks and checks
 * them against f->crc32. Returns 0 if the blob matches, nonzero otherwise. */
int rewair_uifs_verify_file( const rewair_uifs_t* fs, const rewair_uifs_file_t* f );

/* Standard zlib/PKZIP CRC32 (polynomial 0xEDB88320, reflected), computed over
 * `len` bytes of `data`. Exposed for the packer's round-trip tests. */
uint32_t rewair_uifs_crc32( const void* data, uint32_t len );
