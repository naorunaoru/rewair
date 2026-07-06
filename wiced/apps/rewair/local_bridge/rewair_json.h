#pragma once

#include <stdint.h>

/* ---- request body parsing (jsmn-backed) ---- */
int rewair_req_get_string( const char* body, uint32_t len, const char* key,
                           char* out, uint32_t out_size );
int rewair_req_get_u32( const char* body, uint32_t len, const char* key, uint32_t* out );
int rewair_req_get_string_array( const char* body, uint32_t len, const char* key,
                                 char out[][33], uint32_t max_items, uint32_t* count_out );

/* ---- status serialization (Task 4) ---- */
struct rewair_status;
int rewair_json_status( const struct rewair_status* st, char* buf, uint32_t buf_size );

/* Escapes src into dst as JSON string content ( " \\ -> escaped, ctrl -> space ).
 * Returns bytes written (excl. NUL). dst always NUL-terminated if dst_size > 0. */
uint32_t rewair_json_escape_string( const char* src, char* dst, uint32_t dst_size );
