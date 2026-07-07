/* Generic string/parse/format helpers, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 6, pure move). See rewair_fmt.h for the wiced-type note.
 * Phase 2 Task 7: the wiced-typed function bodies below are guarded with
 * `#ifndef REWAIR_HOST_BUILD` -- see rewair_fmt.h for why. */

#include <string.h>
#include <stdio.h>
#include "rewair_fmt.h"

uint32_t cstr_len( const char* s )
{
    uint32_t n = 0u;
    while ( s[n] != '\0' )
    {
        n++;
    }
    return n;
}

int cstr_eq( const char* a, const char* b )
{
    while ( *a == *b )
    {
        if ( *a == '\0' )
        {
            return 1;
        }
        a++;
        b++;
    }
    return 0;
}

int cmd4_eq( const char cmd[4], const char* text )
{
    return cmd[0] == text[0] && cmd[1] == text[1] && cmd[2] == text[2] &&
           cmd[3] == text[3] && text[4] == '\0';
}

int ascii_space( char c )
{
    return c == ' ' || c == '\t';
}

int parse_uint32( const char* text, uint32_t* out )
{
    uint32_t value = 0u;

    if ( *text == '\0' )
    {
        return 0;
    }

    while ( *text != '\0' )
    {
        if ( *text < '0' || *text > '9' )
        {
            return 0;
        }
        value = ( value * 10u ) + (uint32_t)( *text - '0' );
        text++;
    }

    *out = value;
    return 1;
}

#ifndef REWAIR_HOST_BUILD
void print_ipv4( const wiced_ip_address_t* address )
{
    uint32_t ipv4 = GET_IPV4_ADDRESS( *address );
    printf( "%lu.%lu.%lu.%lu",
            (unsigned long)( ( ipv4 >> 24 ) & 0xffu ),
            (unsigned long)( ( ipv4 >> 16 ) & 0xffu ),
            (unsigned long)( ( ipv4 >> 8 ) & 0xffu ),
            (unsigned long)( ipv4 & 0xffu ) );
}

void ipv4_to_cstr( const wiced_ip_address_t* address, char out[16] )
{
    uint32_t ipv4 = GET_IPV4_ADDRESS( *address );
    sprintf( out, "%lu.%lu.%lu.%lu",
             (unsigned long)( ( ipv4 >> 24 ) & 0xffu ),
             (unsigned long)( ( ipv4 >> 16 ) & 0xffu ),
             (unsigned long)( ( ipv4 >> 8 ) & 0xffu ),
             (unsigned long)( ipv4 & 0xffu ) );
}

void mac_to_cstr( const wiced_mac_t* mac, char out[18] )
{
    sprintf( out, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac->octet[0], mac->octet[1], mac->octet[2],
             mac->octet[3], mac->octet[4], mac->octet[5] );
}

void print_ssid( const wiced_ssid_t* ssid )
{
    uint32_t i;
    for ( i = 0u; i < ssid->length && i < SSID_NAME_SIZE; i++ )
    {
        uint8_t c = ssid->value[i];
        printf( "%c", ( c >= 0x20u && c <= 0x7eu ) ? c : '.' );
    }
}

int ssid_eq_text( const wiced_ssid_t* ssid, const char* text )
{
    uint32_t length = cstr_len( text );
    if ( length != ssid->length || length > SSID_NAME_SIZE )
    {
        return 0;
    }
    return memcmp( ssid->value, text, length ) == 0;
}
#endif

/* Compares two plain NUL-terminated SSID strings (e.g. rewair_status_t.ssid
 * against a request's ssid), unlike ssid_eq_text which compares against a
 * length-prefixed wiced_ssid_t. */
int ssid_eq_text_cstr( const char* a, const char* b )
{
    return cstr_eq( a, b );
}

int from_hex( char c, uint8_t* value )
{
    if ( c >= '0' && c <= '9' )
    {
        *value = (uint8_t)( c - '0' );
        return 1;
    }
    if ( c >= 'a' && c <= 'f' )
    {
        *value = (uint8_t)( c - 'a' + 10 );
        return 1;
    }
    if ( c >= 'A' && c <= 'F' )
    {
        *value = (uint8_t)( c - 'A' + 10 );
        return 1;
    }
    return 0;
}

char hex_nibble( uint8_t value )
{
    value &= 0xfu;
    return (char)( value < 10u ? ( '0' + value ) : ( 'A' + value - 10u ) );
}

uint32_t decode_hex_len( const uint8_t* text, int* ok )
{
    uint32_t value = 0u;
    uint32_t i;
    *ok = 1;
    for ( i = 0; i < 8u; i++ )
    {
        uint8_t nibble = 0u;
        if ( from_hex( (char)text[i], &nibble ) == 0 )
        {
            *ok = 0;
            return 0u;
        }
        value = ( value << 4 ) | nibble;
    }
    return value;
}

void frame_len_hex( uint32_t value, char out[9] )
{
    int shift;
    for ( shift = 28; shift >= 0; shift -= 4 )
    {
        *out++ = hex_nibble( (uint8_t)( value >> (uint32_t)shift ) );
    }
    *out = '\0';
}

int parse_fixed_centi( const char* text, int32_t* out )
{
    int negative = 0;
    uint32_t whole = 0u;
    uint32_t frac = 0u;
    uint32_t frac_digits = 0u;
    int saw_digit = 0;
    int32_t value;

    if ( *text == '-' )
    {
        negative = 1;
        text++;
    }
    else if ( *text == '+' )
    {
        text++;
    }

    while ( *text >= '0' && *text <= '9' )
    {
        saw_digit = 1;
        whole = ( whole * 10u ) + (uint32_t)( *text - '0' );
        text++;
    }

    if ( *text == '.' )
    {
        text++;
        while ( *text >= '0' && *text <= '9' )
        {
            if ( frac_digits < 2u )
            {
                frac = ( frac * 10u ) + (uint32_t)( *text - '0' );
                frac_digits++;
            }
            else if ( frac_digits == 2u && *text >= '5' )
            {
                frac++;
                frac_digits++;
            }
            saw_digit = 1;
            text++;
        }
    }

    if ( saw_digit == 0 || *text != '\0' )
    {
        return 0;
    }

    while ( frac_digits < 2u )
    {
        frac *= 10u;
        frac_digits++;
    }
    if ( frac >= 100u )
    {
        whole++;
        frac -= 100u;
    }

    value = (int32_t)( ( whole * 100u ) + frac );
    if ( negative != 0 )
    {
        value = -value;
    }
    *out = value;
    return 1;
}

void int32_to_cstr( int32_t value, char* out, uint32_t out_len )
{
    char tmp[11];
    uint32_t pos = 0u;
    uint32_t uvalue;

    if ( out_len == 0u )
    {
        return;
    }

    if ( value < 0 )
    {
        *out++ = '-';
        out_len--;
        uvalue = (uint32_t)( -( value + 1 ) ) + 1u;
    }
    else
    {
        uvalue = (uint32_t)value;
    }

    if ( out_len == 0u )
    {
        return;
    }

    if ( uvalue == 0u )
    {
        *out++ = '0';
        if ( out_len > 1u )
        {
            *out = '\0';
        }
        return;
    }

    while ( uvalue != 0u && pos < sizeof( tmp ) )
    {
        tmp[pos++] = (char)( '0' + ( uvalue % 10u ) );
        uvalue /= 10u;
    }

    while ( pos != 0u && out_len > 1u )
    {
        *out++ = tmp[--pos];
        out_len--;
    }
    *out = '\0';
}
