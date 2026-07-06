#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rewair_uifs.h"

/* ---- in-memory image + read callback plumbing ---- */

typedef struct
{
    const uint8_t* data;
    uint32_t       size;
} mem_image_t;

static int mem_read( uint32_t addr, void* buf, uint32_t len, void* ctx )
{
    const mem_image_t* img = (const mem_image_t*)ctx;

    if ( (uint64_t)addr + (uint64_t)len > (uint64_t)img->size )
    {
        return -1;
    }
    memcpy( buf, img->data + addr, len );
    return 0;
}

/* Reads the whole fixture .rwfs file produced by the packer into memory.
 * Returns a malloc'd buffer (caller frees) and sets *size_out. */
static uint8_t* read_file( const char* path, uint32_t* size_out )
{
    FILE* f = fopen( path, "rb" );
    long  sz;
    uint8_t* buf;

    assert( f != NULL );
    assert( fseek( f, 0, SEEK_END ) == 0 );
    sz = ftell( f );
    assert( sz > 0 );
    assert( fseek( f, 0, SEEK_SET ) == 0 );

    buf = (uint8_t*)malloc( (size_t)sz );
    assert( buf != NULL );
    assert( fread( buf, 1, (size_t)sz, f ) == (size_t)sz );
    fclose( f );

    *size_out = (uint32_t)sz;
    return buf;
}

/* ---- hand-crafted image builder (little-endian, no packer involved) ---- */

#define HDR_SIZE 16u
#define ENTRY_SIZE 40u

static void put_u32le( uint8_t* p, uint32_t v )
{
    p[0] = (uint8_t)( v & 0xffu );
    p[1] = (uint8_t)( ( v >> 8 ) & 0xffu );
    p[2] = (uint8_t)( ( v >> 16 ) & 0xffu );
    p[3] = (uint8_t)( ( v >> 24 ) & 0xffu );
}

static void write_entry( uint8_t* table, uint32_t index, const char* path, uint32_t offset,
                          uint32_t size, uint32_t crc, uint8_t gzip )
{
    uint8_t* e = table + index * ENTRY_SIZE;

    memset( e, 0, ENTRY_SIZE );
    strncpy( (char*)e, path, 23 );
    put_u32le( e + 24, offset );
    put_u32le( e + 28, size );
    put_u32le( e + 32, crc );
    e[36] = gzip;
}

/* Builds a minimal valid one-file image "/a" with payload `data`(len bytes,
 * stored uncompressed/gzip=0 for simplicity). Returns malloc'd buffer. */
static uint8_t* build_simple_image( const char* path, const uint8_t* data, uint32_t len,
                                     uint32_t* total_out )
{
    uint32_t total = HDR_SIZE + ENTRY_SIZE + len;
    uint8_t* img = (uint8_t*)malloc( total );
    uint32_t crc = rewair_uifs_crc32( data, len );
    uint32_t table_crc;

    assert( img != NULL );
    memset( img, 0, total );
    write_entry( img + HDR_SIZE, 0, path, HDR_SIZE + ENTRY_SIZE, len, crc, 0 );
    table_crc = rewair_uifs_crc32( img + HDR_SIZE, ENTRY_SIZE );

    memcpy( img, "RWF1", 4 );
    put_u32le( img + 4, 1u );
    put_u32le( img + 8, total );
    put_u32le( img + 12, table_crc );
    memcpy( img + HDR_SIZE + ENTRY_SIZE, data, len );

    *total_out = total;
    return img;
}

static void test_crc32_known_vector( void )
{
    /* CRC32("123456789") == 0xCBF43926 -- the standard check value. */
    assert( rewair_uifs_crc32( "123456789", 9u ) == 0xCBF43926u );
    assert( rewair_uifs_crc32( "", 0u ) == 0x00000000u );
    printf( "test_crc32_known_vector OK\n" );
}

static void test_hand_crafted_roundtrip( void )
{
    const uint8_t payload[] = "hello rwfs";
    uint32_t total;
    uint8_t* img = build_simple_image( "/a", payload, (uint32_t)( sizeof( payload ) - 1u ), &total );
    mem_image_t mi = { img, total };
    rewair_uifs_t fs;
    rewair_uifs_file_t f;
    char buf[32];

    assert( rewair_uifs_init( &fs, mem_read, &mi ) == 0 );
    assert( fs.file_count == 1u );
    assert( fs.total_size == total );

    assert( rewair_uifs_find( &fs, "/a", &f ) == 0 );
    assert( f.size == sizeof( payload ) - 1u );
    assert( f.gzip == 0u );

    memset( buf, 0, sizeof( buf ) );
    assert( rewair_uifs_read( &fs, &f, 0u, buf, f.size ) == 0 );
    assert( memcmp( buf, payload, f.size ) == 0 );

    /* partial range read */
    memset( buf, 0, sizeof( buf ) );
    assert( rewair_uifs_read( &fs, &f, 6u, buf, 4u ) == 0 );
    assert( memcmp( buf, "rwfs", 4u ) == 0 );

    /* verify_file must pass on the untouched image */
    assert( rewair_uifs_verify_file( &fs, &f ) == 0 );

    /* unknown path */
    assert( rewair_uifs_find( &fs, "/nope", &f ) != 0 );

    /* out-of-range read (off+len beyond file size) */
    {
        rewair_uifs_file_t f2;
        assert( rewair_uifs_find( &fs, "/a", &f2 ) == 0 );
        assert( rewair_uifs_read( &fs, &f2, 0u, buf, f2.size + 1u ) != 0 );
        assert( rewair_uifs_read( &fs, &f2, f2.size, buf, 1u ) != 0 );
    }

    free( img );
    printf( "test_hand_crafted_roundtrip OK\n" );
}

static void test_bad_magic( void )
{
    uint32_t total;
    uint8_t* img = build_simple_image( "/a", (const uint8_t*)"x", 1u, &total );
    mem_image_t mi = { img, total };
    rewair_uifs_t fs;

    img[0] = 'X'; /* corrupt magic */
    assert( rewair_uifs_init( &fs, mem_read, &mi ) != 0 );

    free( img );
    printf( "test_bad_magic OK\n" );
}

static void test_bad_table_crc( void )
{
    uint32_t total;
    uint8_t* img = build_simple_image( "/a", (const uint8_t*)"x", 1u, &total );
    mem_image_t mi = { img, total };
    rewair_uifs_t fs;

    img[12] ^= 0xffu; /* flip a byte of table_crc32 */
    assert( rewair_uifs_init( &fs, mem_read, &mi ) != 0 );

    free( img );
    printf( "test_bad_table_crc OK\n" );
}

static void test_truncated_image( void )
{
    uint32_t total;
    uint8_t* img = build_simple_image( "/a", (const uint8_t*)"hello", 5u, &total );
    mem_image_t mi;
    rewair_uifs_t fs;

    /* Truncate the underlying "device" so even the header+table can't be
     * fully read (shorter than HDR_SIZE + ENTRY_SIZE). init only reads
     * header+table -- it must fail via the read callback rather than read
     * past the end of the backing store. */
    mi.data = img;
    mi.size = HDR_SIZE + ENTRY_SIZE - 4u;

    assert( rewair_uifs_init( &fs, mem_read, &mi ) != 0 );

    free( img );
    printf( "test_truncated_image OK\n" );
}

static void test_blob_truncated_caught_by_verify( void )
{
    /* Truncation of the *blob* region (past what init touches) is instead
     * caught when Task 5's boot check calls rewair_uifs_verify_file, since
     * the read callback will fail once asked for bytes past the real device
     * size even though total_size in the header claims otherwise. */
    uint32_t total;
    uint8_t* img = build_simple_image( "/a", (const uint8_t*)"hello", 5u, &total );
    mem_image_t mi;
    rewair_uifs_t fs;
    rewair_uifs_file_t f;

    mi.data = img;
    mi.size = total; /* init succeeds: header+table are intact */
    assert( rewair_uifs_init( &fs, mem_read, &mi ) == 0 );
    assert( rewair_uifs_find( &fs, "/a", &f ) == 0 );

    /* Now simulate the backing store actually being shorter than total_size
     * claims (e.g. a flash region that was never fully written). */
    mi.size = HDR_SIZE + ENTRY_SIZE + 2u; /* only 2 of 5 blob bytes readable */
    assert( rewair_uifs_verify_file( &fs, &f ) != 0 );

    free( img );
    printf( "test_blob_truncated_caught_by_verify OK\n" );
}

static void test_entry_past_total_size( void )
{
    uint32_t total;
    uint8_t* img = build_simple_image( "/a", (const uint8_t*)"hello", 5u, &total );
    mem_image_t mi;
    rewair_uifs_t fs;
    uint32_t table_crc;

    /* Corrupt the entry to claim a size that runs past total_size, then fix
     * up table_crc32 so the corruption is only caught by the offset+size
     * bounds check, not incidentally by the crc check. */
    write_entry( img + HDR_SIZE, 0, "/a", HDR_SIZE + ENTRY_SIZE, total, /* size == total: way past end */
                 rewair_uifs_crc32( img + HDR_SIZE + ENTRY_SIZE, 5u ), 0 );
    table_crc = rewair_uifs_crc32( img + HDR_SIZE, ENTRY_SIZE );
    put_u32le( img + 12, table_crc );

    mi.data = img;
    mi.size = total;
    assert( rewair_uifs_init( &fs, mem_read, &mi ) != 0 );

    free( img );
    printf( "test_entry_past_total_size OK\n" );
}

static void test_16_file_limit( void )
{
    /* REWAIR_UIFS_MAX_FILES + 1 entries must be rejected by init. */
    uint32_t count = REWAIR_UIFS_MAX_FILES + 1u;
    uint32_t table_size = count * ENTRY_SIZE;
    uint32_t total = HDR_SIZE + table_size;
    uint8_t* img = (uint8_t*)malloc( total );
    mem_image_t mi = { img, total };
    rewair_uifs_t fs;
    uint32_t i;
    uint32_t table_crc;

    assert( img != NULL );
    memset( img, 0, total );
    for ( i = 0u; i < count; i++ )
    {
        char path[8];
        snprintf( path, sizeof( path ), "/%u", (unsigned)i );
        write_entry( img + HDR_SIZE, i, path, total, 0u, 0u, 0u );
    }
    table_crc = rewair_uifs_crc32( img + HDR_SIZE, table_size );

    memcpy( img, "RWF1", 4 );
    put_u32le( img + 4, count );
    put_u32le( img + 8, total );
    put_u32le( img + 12, table_crc );

    assert( rewair_uifs_init( &fs, mem_read, &mi ) != 0 );

    free( img );
    printf( "test_16_file_limit OK\n" );
}

static void test_exactly_16_files_ok( void )
{
    uint32_t count = REWAIR_UIFS_MAX_FILES;
    uint32_t table_size = count * ENTRY_SIZE;
    uint32_t total = HDR_SIZE + table_size;
    uint8_t* img = (uint8_t*)malloc( total );
    mem_image_t mi = { img, total };
    rewair_uifs_t fs;
    uint32_t i;
    uint32_t table_crc;

    assert( img != NULL );
    memset( img, 0, total );
    for ( i = 0u; i < count; i++ )
    {
        char path[8];
        snprintf( path, sizeof( path ), "/%u", (unsigned)i );
        write_entry( img + HDR_SIZE, i, path, total, 0u, 0u, 0u );
    }
    table_crc = rewair_uifs_crc32( img + HDR_SIZE, table_size );

    memcpy( img, "RWF1", 4 );
    put_u32le( img + 4, count );
    put_u32le( img + 8, total );
    put_u32le( img + 12, table_crc );

    assert( rewair_uifs_init( &fs, mem_read, &mi ) == 0 );
    assert( fs.file_count == REWAIR_UIFS_MAX_FILES );

    free( img );
    printf( "test_exactly_16_files_ok OK\n" );
}

/* ---- through-the-packer integration test ---- */

static void test_packer_fixture( void )
{
    uint32_t size;
    uint8_t* data = read_file( "/tmp/test_uifs_fixture.rwfs", &size );
    mem_image_t mi = { data, size };
    rewair_uifs_t fs;
    rewair_uifs_file_t f;
    uint8_t buf[4096];

    assert( rewair_uifs_init( &fs, mem_read, &mi ) == 0 );
    assert( fs.file_count == 3u );
    assert( fs.total_size == size );

    assert( rewair_uifs_find( &fs, "/", &f ) == 0 );
    assert( f.gzip == 1u );
    assert( rewair_uifs_verify_file( &fs, &f ) == 0 );
    assert( f.size <= sizeof( buf ) );
    assert( rewair_uifs_read( &fs, &f, 0u, buf, f.size ) == 0 );
    /* gzip magic bytes */
    assert( buf[0] == 0x1f && buf[1] == 0x8b );

    assert( rewair_uifs_find( &fs, "/app.js", &f ) == 0 );
    assert( f.gzip == 1u );
    assert( rewair_uifs_verify_file( &fs, &f ) == 0 );

    assert( rewair_uifs_find( &fs, "/rewair.css", &f ) == 0 );
    assert( f.gzip == 1u );
    assert( rewair_uifs_verify_file( &fs, &f ) == 0 );

    /* corrupting stored bytes must make verify_file fail */
    {
        rewair_uifs_file_t fc;
        uint8_t save;
        assert( rewair_uifs_find( &fs, "/app.js", &fc ) == 0 );
        save = data[fc.offset];
        data[fc.offset] ^= 0xffu;
        assert( rewair_uifs_verify_file( &fs, &fc ) != 0 );
        data[fc.offset] = save; /* restore */
    }

    assert( rewair_uifs_find( &fs, "/missing.txt", &f ) != 0 );

    free( data );
    printf( "test_packer_fixture OK\n" );
}

int main( void )
{
    test_crc32_known_vector();
    test_hand_crafted_roundtrip();
    test_bad_magic();
    test_bad_table_crc();
    test_truncated_image();
    test_blob_truncated_caught_by_verify();
    test_entry_past_total_size();
    test_16_file_limit();
    test_exactly_16_files_ok();
    test_packer_fixture();

    printf( "test_uifs OK\n" );
    return 0;
}
