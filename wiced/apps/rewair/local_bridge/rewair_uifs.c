#include "rewair_uifs.h"

#include <string.h>

#define REWAIR_UIFS_HEADER_SIZE 16u
#define REWAIR_UIFS_ENTRY_SIZE  40u
#define REWAIR_UIFS_VERIFY_CHUNK 256u

static uint32_t get_u32le( const uint8_t* p )
{
    return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) |
           ( (uint32_t)p[3] << 24 );
}

uint32_t rewair_uifs_crc32( const void* data, uint32_t len )
{
    static const uint32_t table[16] =
    {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
    };
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xffffffffu;
    uint32_t i;

    /* Table-free-ish 4-bit-per-step CRC32 (standard zlib/PKZIP polynomial
     * 0xEDB88320, reflected). Only a 16-entry table, small enough for flash
     * .rodata without needing the full 256-entry table. */
    for ( i = 0u; i < len; i++ )
    {
        crc ^= p[i];
        crc = table[ crc & 0x0fu ] ^ ( crc >> 4 );
        crc = table[ crc & 0x0fu ] ^ ( crc >> 4 );
    }
    return crc ^ 0xffffffffu;
}

static int read_bytes( rewair_uifs_read_t read, void* ctx, uint32_t addr, void* buf, uint32_t len )
{
    if ( read == NULL )
    {
        return -1;
    }
    return read( addr, buf, len, ctx );
}

int rewair_uifs_init( rewair_uifs_t* fs, rewair_uifs_read_t read, void* ctx )
{
    uint8_t  header[REWAIR_UIFS_HEADER_SIZE];
    uint8_t  entry[REWAIR_UIFS_ENTRY_SIZE];
    uint32_t file_count;
    uint32_t total_size;
    uint32_t table_crc32;
    uint32_t computed_crc;
    uint32_t i;

    if ( fs == NULL )
    {
        return -1;
    }
    memset( fs, 0, sizeof( *fs ) );

    if ( read_bytes( read, ctx, 0u, header, REWAIR_UIFS_HEADER_SIZE ) != 0 )
    {
        return -1;
    }
    if ( memcmp( header, REWAIR_UIFS_MAGIC, 4u ) != 0 )
    {
        return -1;
    }

    file_count  = get_u32le( header + 4 );
    total_size  = get_u32le( header + 8 );
    table_crc32 = get_u32le( header + 12 );

    if ( file_count == 0u || file_count > REWAIR_UIFS_MAX_FILES )
    {
        return -1;
    }
    /* The table itself must fit inside the claimed image size. */
    if ( total_size < REWAIR_UIFS_HEADER_SIZE + file_count * REWAIR_UIFS_ENTRY_SIZE )
    {
        return -1;
    }

    /* Validate the table CRC by re-reading each entry and hashing it
     * incrementally -- avoids needing a full-table scratch buffer. */
    {
        /* CRC32 computed incrementally across entries using the same
         * algorithm as rewair_uifs_crc32, so we inline the loop here to feed
         * it entry-by-entry without buffering the whole table. */
        static const uint32_t table[16] =
        {
            0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
            0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
            0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
            0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
        };
        uint32_t crc = 0xffffffffu;

        for ( i = 0u; i < file_count; i++ )
        {
            uint32_t addr = REWAIR_UIFS_HEADER_SIZE + i * REWAIR_UIFS_ENTRY_SIZE;
            uint32_t b;

            if ( read_bytes( read, ctx, addr, entry, REWAIR_UIFS_ENTRY_SIZE ) != 0 )
            {
                return -1;
            }
            for ( b = 0u; b < REWAIR_UIFS_ENTRY_SIZE; b++ )
            {
                crc ^= entry[b];
                crc = table[ crc & 0x0fu ] ^ ( crc >> 4 );
                crc = table[ crc & 0x0fu ] ^ ( crc >> 4 );
            }

            /* Stash the parsed entry now -- we already have its bytes. */
            memcpy( fs->files[i].path, entry, REWAIR_UIFS_MAX_PATH );
            fs->files[i].path[REWAIR_UIFS_MAX_PATH - 1u] = '\0';
            fs->files[i].offset = get_u32le( entry + 24 );
            fs->files[i].size   = get_u32le( entry + 28 );
            fs->files[i].crc32  = get_u32le( entry + 32 );
            fs->files[i].gzip   = entry[36];
        }
        computed_crc = crc ^ 0xffffffffu;
    }

    if ( computed_crc != table_crc32 )
    {
        return -1;
    }

    /* Bounds-check every entry against the claimed total_size. Use 64-bit
     * math to sidestep u32 overflow on offset+size. */
    for ( i = 0u; i < file_count; i++ )
    {
        uint64_t end = (uint64_t)fs->files[i].offset + (uint64_t)fs->files[i].size;
        if ( end > (uint64_t)total_size )
        {
            return -1;
        }
    }

    fs->read       = read;
    fs->read_ctx   = ctx;
    fs->file_count = file_count;
    fs->total_size = total_size;
    return 0;
}

int rewair_uifs_find( const rewair_uifs_t* fs, const char* path, rewair_uifs_file_t* out )
{
    uint32_t i;

    if ( fs == NULL || path == NULL || out == NULL )
    {
        return -1;
    }
    for ( i = 0u; i < fs->file_count; i++ )
    {
        if ( strncmp( fs->files[i].path, path, REWAIR_UIFS_MAX_PATH ) == 0 )
        {
            *out = fs->files[i];
            return 0;
        }
    }
    return -1;
}

int rewair_uifs_read( const rewair_uifs_t* fs, const rewair_uifs_file_t* f, uint32_t off,
                      void* buf, uint32_t len )
{
    uint64_t end;

    if ( fs == NULL || f == NULL )
    {
        return -1;
    }
    if ( len == 0u )
    {
        return 0;
    }
    end = (uint64_t)off + (uint64_t)len;
    if ( end > (uint64_t)f->size )
    {
        return -1;
    }
    return read_bytes( fs->read, fs->read_ctx, f->offset + off, buf, len );
}

int rewair_uifs_verify_file( const rewair_uifs_t* fs, const rewair_uifs_file_t* f )
{
    uint8_t  chunk[REWAIR_UIFS_VERIFY_CHUNK];
    uint32_t remaining;
    uint32_t off;
    uint32_t crc;

    if ( fs == NULL || f == NULL )
    {
        return -1;
    }

    crc = 0xffffffffu;
    off = 0u;
    remaining = f->size;
    while ( remaining > 0u )
    {
        static const uint32_t table[16] =
        {
            0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
            0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
            0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
            0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
        };
        uint32_t take = ( remaining > REWAIR_UIFS_VERIFY_CHUNK ) ? REWAIR_UIFS_VERIFY_CHUNK : remaining;
        uint32_t i;

        if ( rewair_uifs_read( fs, f, off, chunk, take ) != 0 )
        {
            return -1;
        }
        for ( i = 0u; i < take; i++ )
        {
            crc ^= chunk[i];
            crc = table[ crc & 0x0fu ] ^ ( crc >> 4 );
            crc = table[ crc & 0x0fu ] ^ ( crc >> 4 );
        }

        off += take;
        remaining -= take;
    }
    crc ^= 0xffffffffu;

    return ( crc == f->crc32 ) ? 0 : -1;
}
