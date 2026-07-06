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

#include "rewair_state.h"

/* ---- status serialization ---- */

typedef struct
{
    char*    buf;
    uint32_t size;
    uint32_t pos;
    int      overflow;
} emit_t;

static void emit_raw( emit_t* e, const char* s )
{
    while ( *s != '\0' )
    {
        if ( e->pos + 1u >= e->size )
        {
            e->overflow = 1;
            return;
        }
        e->buf[e->pos++] = *s++;
    }
    e->buf[e->pos] = '\0';
}

uint32_t rewair_json_escape_string( const char* src, char* dst, uint32_t dst_size )
{
    uint32_t o = 0u;

    if ( dst_size == 0u )
    {
        return 0u;
    }
    while ( *src != '\0' && o + 1u < dst_size )
    {
        char c = *src++;
        if ( c == '"' || c == '\\' )
        {
            if ( o + 2u >= dst_size )
            {
                break;
            }
            dst[o++] = '\\';
        }
        else if ( (unsigned char)c < 0x20u )
        {
            c = ' ';
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return o;
}

static void emit_str( emit_t* e, const char* key, const char* value )
{
    char esc[160];

    emit_raw( e, "\"" );
    emit_raw( e, key );
    emit_raw( e, "\":\"" );
    (void)rewair_json_escape_string( value, esc, sizeof( esc ) );
    emit_raw( e, esc );
    emit_raw( e, "\"" );
}

static void emit_int( emit_t* e, const char* key, int32_t value )
{
    char num[16];

    emit_raw( e, "\"" );
    emit_raw( e, key );
    snprintf( num, sizeof( num ), "\":%ld", (long)value );
    emit_raw( e, num );
}

static void emit_u32( emit_t* e, const char* key, uint32_t value )
{
    char num[16];

    emit_raw( e, "\"" );
    emit_raw( e, key );
    snprintf( num, sizeof( num ), "\":%lu", (unsigned long)value );
    emit_raw( e, num );
}

static void emit_centi( emit_t* e, const char* key, int32_t centi )
{
    char num[24];
    int32_t whole = centi / 100;
    int32_t frac = centi % 100;

    if ( frac < 0 )
    {
        frac = -frac;
    }
    if ( centi < 0 && whole == 0 )
    {
        snprintf( num, sizeof( num ), "\"%s\":-0.%02ld", key, (long)frac );
    }
    else
    {
        snprintf( num, sizeof( num ), "\"%s\":%ld.%02ld", key, (long)whole, (long)frac );
    }
    emit_raw( e, num );
}

static void emit_bool( emit_t* e, const char* key, uint8_t value )
{
    emit_raw( e, "\"" );
    emit_raw( e, key );
    emit_raw( e, value != 0u ? "\":true" : "\":false" );
}

int rewair_json_status( const struct rewair_status* st, char* buf, uint32_t buf_size )
{
    emit_t e = { buf, buf_size, 0u, 0 };
    static const char* disp_names[3] = { "score", "clock", "sensors" };

    emit_raw( &e, "{" );
    emit_str( &e, "name", st->name );
    emit_raw( &e, "," );
    emit_str( &e, "fw", st->fw );

    emit_raw( &e, ",\"score\":{" );
    emit_int( &e, "value", (int32_t)st->score );
    emit_raw( &e, "," );
    emit_str( &e, "color", st->score_color );
    emit_raw( &e, ",\"indices\":{" );
    emit_int( &e, "temp", st->idx_temp );  emit_raw( &e, "," );
    emit_int( &e, "humid", st->idx_humid ); emit_raw( &e, "," );
    emit_int( &e, "co2", st->idx_co2 );    emit_raw( &e, "," );
    emit_int( &e, "voc", st->idx_voc );    emit_raw( &e, "," );
    emit_int( &e, "dust", st->idx_dust );
    emit_raw( &e, "}}" );

    emit_raw( &e, ",\"sens\":{" );
    emit_centi( &e, "temp", st->sens.temp );   emit_raw( &e, "," );
    emit_centi( &e, "humid", st->sens.humid ); emit_raw( &e, "," );
    emit_centi( &e, "co2", st->sens.co2 );     emit_raw( &e, "," );
    emit_centi( &e, "voc", st->sens.voc );     emit_raw( &e, "," );
    emit_centi( &e, "dust", st->sens.dust );   emit_raw( &e, "," );
    emit_int( &e, "light", st->sens.light );
    emit_raw( &e, "}" );

    if ( st->wifi_mode == 0u )
    {
        emit_raw( &e, ",\"wifi\":{\"mode\":\"sta\"," );
        emit_str( &e, "ssid", st->ssid );          emit_raw( &e, "," );
        emit_int( &e, "rssi", st->rssi );          emit_raw( &e, "," );
        emit_str( &e, "ip", st->ip );              emit_raw( &e, "," );
        emit_str( &e, "gw", st->gw );              emit_raw( &e, "," );
        emit_str( &e, "dns", st->dns );            emit_raw( &e, "," );
        emit_str( &e, "mac", st->mac );            emit_raw( &e, "," );
        emit_int( &e, "connected_s", (int32_t)st->connected_s ); emit_raw( &e, "," );
        emit_int( &e, "drops", (int32_t)st->drops );             emit_raw( &e, "," );
        emit_int( &e, "saved_count", (int32_t)st->saved_count );
        emit_raw( &e, "}" );
    }
    else
    {
        emit_raw( &e, ",\"wifi\":{\"mode\":\"ap\"," );
        emit_str( &e, "ap_ssid", st->ap_ssid );    emit_raw( &e, "," );
        emit_str( &e, "ap_ip", st->ap_ip );        emit_raw( &e, "," );
        emit_int( &e, "saved_count", (int32_t)st->saved_count );
        emit_raw( &e, "}" );
    }

    emit_raw( &e, ",\"time\":{" );
    emit_bool( &e, "valid", st->time_valid );  emit_raw( &e, "," );
    emit_u32( &e, "epoch", st->epoch );        emit_raw( &e, "," );
    emit_bool( &e, "synced", st->time_synced );
    emit_raw( &e, "}" );

    emit_raw( &e, ",\"settings\":{" );
    emit_str( &e, "name", st->name );                    emit_raw( &e, "," );
    emit_str( &e, "units", st->units == 0u ? "c" : "f" ); emit_raw( &e, "," );
    emit_int( &e, "tz_offset", st->tz_offset_min );      emit_raw( &e, "," );
    emit_bool( &e, "tz_dst", st->tz_dst );               emit_raw( &e, "," );
    emit_str( &e, "tz_zone", st->tz_zone );              emit_raw( &e, "," );
    emit_str( &e, "tz_posix", st->tz_posix );            emit_raw( &e, "," );
    emit_str( &e, "time_mode", st->time_mode == 0u ? "auto" : "manual" ); emit_raw( &e, "," );
    emit_str( &e, "disp_mode", disp_names[ st->disp_mode > 2u ? 0u : st->disp_mode ] );
    emit_raw( &e, "}}" );

    return e.overflow != 0 ? -1 : (int)e.pos;
}
