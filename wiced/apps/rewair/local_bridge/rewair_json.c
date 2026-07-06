#include "rewair_json.h"

#include <stdio.h>
#include <string.h>

#define JSMN_HEADER
#include "jsmn.h"

#define REQ_MAX_TOKENS 40u

static int req_tokenize( const char* body, uint32_t len, jsmntok_t* tokens, uint32_t max_tokens )
{
    jsmn_parser parser;
    int count;

    jsmn_init( &parser );
    count = jsmn_parse( &parser, body, len, tokens, max_tokens );
    if ( count < 1 || tokens[0].type != JSMN_OBJECT )
    {
        return -1;
    }
    return count;
}

static int tok_eq( const char* body, const jsmntok_t* tok, const char* text )
{
    uint32_t tok_len = (uint32_t)( tok->end - tok->start );
    return tok->type == JSMN_STRING && tok_len == (uint32_t)strlen( text ) &&
           strncmp( body + tok->start, text, tok_len ) == 0;
}

/* Copies a JSMN string token, resolving \" \\ \/ \n \t escapes. */
static void tok_copy_string( const char* body, const jsmntok_t* tok, char* out, uint32_t out_size )
{
    uint32_t i = (uint32_t)tok->start;
    uint32_t o = 0u;

    while ( i < (uint32_t)tok->end && o + 1u < out_size )
    {
        char c = body[i];
        if ( c == '\\' && i + 1u < (uint32_t)tok->end )
        {
            char e = body[i + 1u];
            i += 2u;
            switch ( e )
            {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                default:   c = e;    break;   /* covers \" \\ \/ */
            }
        }
        else
        {
            i++;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

/* Returns index of the value token for `key`, or -1. */
static int find_value( const char* body, const jsmntok_t* tokens, int count, const char* key )
{
    int i = 1;

    while ( i < count - 1 )
    {
        if ( tokens[i].parent == 0 && tok_eq( body, &tokens[i], key ) )
        {
            return i + 1;
        }
        i++;
    }
    return -1;
}

int rewair_req_get_string( const char* body, uint32_t len, const char* key,
                           char* out, uint32_t out_size )
{
    jsmntok_t tokens[REQ_MAX_TOKENS];
    int count = req_tokenize( body, len, tokens, REQ_MAX_TOKENS );
    int vi;

    if ( count < 0 )
    {
        return -1;
    }
    vi = find_value( body, tokens, count, key );
    if ( vi < 0 || tokens[vi].type != JSMN_STRING )
    {
        return 0;
    }
    tok_copy_string( body, &tokens[vi], out, out_size );
    return 1;
}

int rewair_req_get_u32( const char* body, uint32_t len, const char* key, uint32_t* out )
{
    jsmntok_t tokens[REQ_MAX_TOKENS];
    int count = req_tokenize( body, len, tokens, REQ_MAX_TOKENS );
    int vi;
    uint32_t value = 0u;
    int i;

    if ( count < 0 )
    {
        return -1;
    }
    vi = find_value( body, tokens, count, key );
    if ( vi < 0 || tokens[vi].type != JSMN_PRIMITIVE )
    {
        return 0;
    }
    for ( i = tokens[vi].start; i < tokens[vi].end; i++ )
    {
        uint32_t d;
        if ( body[i] < '0' || body[i] > '9' )
        {
            return 0;
        }
        d = (uint32_t)( body[i] - '0' );
        if ( value > 429496729u || ( value == 429496729u && d > 5u ) )
        {
            return 0;
        }
        value = value * 10u + d;
    }
    *out = value;
    return 1;
}

int rewair_req_get_string_array( const char* body, uint32_t len, const char* key,
                                 char out[][33], uint32_t max_items, uint32_t* count_out )
{
    jsmntok_t tokens[REQ_MAX_TOKENS];
    int count = req_tokenize( body, len, tokens, REQ_MAX_TOKENS );
    int vi;
    uint32_t stored = 0u;
    int i;

    if ( count < 0 )
    {
        return -1;
    }
    vi = find_value( body, tokens, count, key );
    if ( vi < 0 || tokens[vi].type != JSMN_ARRAY )
    {
        return 0;
    }
    for ( i = vi + 1; i < count && stored < max_items; i++ )
    {
        if ( tokens[i].parent != vi )
        {
            continue;
        }
        if ( tokens[i].type != JSMN_STRING )
        {
            return 0;
        }
        tok_copy_string( body, &tokens[i], out[stored], 33u );
        stored++;
    }
    *count_out = stored;
    return 1;
}
