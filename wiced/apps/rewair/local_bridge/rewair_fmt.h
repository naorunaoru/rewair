#pragma once

/* Generic string/parse/format helpers, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 6, pure move). Some of these take WICED types
 * (wiced_ssid_t, wiced_ip_address_t, wiced_mac_t) so this header pulls in
 * wiced.h/wiced_tcpip.h -- it is firmware-side only, NOT host-testable.
 *
 * Phase 2 Task 7 sanctioned deviation: the wiced-typed declarations/includes
 * below are guarded with `#ifndef REWAIR_HOST_BUILD` so this header (and
 * rewair_fmt.c) can also be compiled for the host test suite, which needs
 * the pure helpers (cstr_eq, parse_fixed_centi, ...) for the new
 * rewair_score host test. Guards only -- no declarations were changed. */

#include <stdint.h>
#ifndef REWAIR_HOST_BUILD
#include "wiced.h"
#include "wiced_tcpip.h"
#endif

uint32_t cstr_len( const char* s );
int      cstr_eq( const char* a, const char* b );
int      cmd4_eq( const char cmd[4], const char* text );
int      ascii_space( char c );

int  parse_uint32( const char* text, uint32_t* out );
int  parse_fixed_centi( const char* text, int32_t* out );
void int32_to_cstr( int32_t value, char* out, uint32_t out_len );

#ifndef REWAIR_HOST_BUILD
void print_ipv4( const wiced_ip_address_t* address );
void ipv4_to_cstr( const wiced_ip_address_t* address, char out[16] );
void mac_to_cstr( const wiced_mac_t* mac, char out[18] );

void print_ssid( const wiced_ssid_t* ssid );
int  ssid_eq_text( const wiced_ssid_t* ssid, const char* text );
#endif
int  ssid_eq_text_cstr( const char* a, const char* b );

int      from_hex( char c, uint8_t* value );
char     hex_nibble( uint8_t value );
uint32_t decode_hex_len( const uint8_t* text, int* ok );
void     frame_len_hex( uint32_t value, char out[9] );
