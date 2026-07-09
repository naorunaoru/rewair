#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rewair_ota_layout.h"

static void test_crc32_known_vector( void )
{
    assert( rewair_ota_crc32( "123456789", 9u ) == 0xcbf43926u );
    assert( rewair_ota_crc32( "", 0u ) == 0u );
    printf( "test_crc32_known_vector OK\n" );
}

static void test_vector_validation( void )
{
    assert( rewair_ota_vector_valid( 0x2001fffcu, REWAIR_OTA_APP_ADDR + 0x101u,
                                     1024u ) == 1 );
    assert( rewair_ota_vector_valid( 0x10000000u, REWAIR_OTA_APP_ADDR + 0x101u,
                                     1024u ) == 0 );
    assert( rewair_ota_vector_valid( 0x2001fffcu, REWAIR_OTA_APP_ADDR + 0x100u,
                                     1024u ) == 0 );
    assert( rewair_ota_vector_valid( 0x2001fffcu, REWAIR_OTA_APP_ADDR + 0x1001u,
                                     1024u ) == 0 );
    printf( "test_vector_validation OK\n" );
}

static void test_image_header_integrity( void )
{
    rewair_ota_image_header_t header;

    assert( sizeof( header ) == 32u );
    rewair_ota_image_header_init( &header, REWAIR_OTA_TARGET_APP, 4096u, 0x12345678u,
                                  0x2001fffcu, REWAIR_OTA_APP_ADDR + 0x101u );
    assert( rewair_ota_image_header_valid( &header, REWAIR_OTA_TARGET_APP ) == 1 );
    assert( rewair_ota_image_header_valid( &header, REWAIR_OTA_TARGET_BACKUP ) == 0 );
    header.image_crc32 ^= 1u;
    assert( rewair_ota_image_header_valid( &header, REWAIR_OTA_TARGET_APP ) == 0 );
    printf( "test_image_header_integrity OK\n" );
}

static void test_record_integrity( void )
{
    rewair_ota_record_t record;

    assert( sizeof( record ) == 32u );
    rewair_ota_record_init( &record, 7u, REWAIR_OTA_STATE_TRIAL, 2u, 4096u,
                            0x89abcdefu );
    assert( rewair_ota_record_valid( &record ) == 1 );
    record.boot_attempts++;
    assert( rewair_ota_record_valid( &record ) == 0 );
    printf( "test_record_integrity OK\n" );
}

int main( void )
{
    test_crc32_known_vector( );
    test_vector_validation( );
    test_image_header_integrity( );
    test_record_integrity( );
    return 0;
}
