#include "rewair_mqtt_packet.h"

#include <string.h>

static int put_u8( uint8_t* out, uint32_t out_size, uint32_t* pos, uint8_t value )
{
    if ( *pos >= out_size )
    {
        return -1;
    }
    out[( *pos )++] = value;
    return 0;
}

static int put_bytes( uint8_t* out, uint32_t out_size, uint32_t* pos,
                      const void* data, uint32_t length )
{
    if ( length > out_size - *pos )
    {
        return -1;
    }
    memcpy( out + *pos, data, length );
    *pos += length;
    return 0;
}

static int put_utf8( uint8_t* out, uint32_t out_size, uint32_t* pos, const char* text )
{
    uint32_t length;

    if ( text == NULL )
    {
        return -1;
    }
    length = (uint32_t)strlen( text );
    if ( length == 0u || length > 65535u )
    {
        return -1;
    }
    if ( put_u8( out, out_size, pos, (uint8_t)( length >> 8 ) ) != 0 ||
         put_u8( out, out_size, pos, (uint8_t)length ) != 0 )
    {
        return -1;
    }
    return put_bytes( out, out_size, pos, text, length );
}

static int put_remaining_length( uint8_t* out, uint32_t out_size, uint32_t* pos,
                                 uint32_t length )
{
    uint32_t count = 0u;

    if ( length > 268435455u )
    {
        return -1;
    }
    do
    {
        uint8_t encoded = (uint8_t)( length % 128u );
        length /= 128u;
        if ( length != 0u )
        {
            encoded |= 0x80u;
        }
        if ( put_u8( out, out_size, pos, encoded ) != 0 )
        {
            return -1;
        }
        count++;
    } while ( length != 0u && count < 4u );

    return length == 0u ? 0 : -1;
}

int rewair_mqtt_packet_connect( const rewair_mqtt_connect_options_t* options,
                                uint8_t* out, uint32_t out_size )
{
    static const uint8_t variable_header_prefix[] =
    {
        0x00u, 0x04u, 'M', 'Q', 'T', 'T', 0x04u
    };
    uint32_t remaining;
    uint32_t pos = 0u;
    uint8_t flags = 0x02u; /* clean session */
    uint32_t client_len;
    uint32_t will_topic_len;
    uint32_t will_payload_len;
    uint32_t username_len = 0u;
    uint32_t password_len = 0u;

    if ( options == NULL || out == NULL || options->client_id == NULL ||
         options->will_topic == NULL || options->will_payload == NULL )
    {
        return -1;
    }
    client_len = (uint32_t)strlen( options->client_id );
    will_topic_len = (uint32_t)strlen( options->will_topic );
    will_payload_len = (uint32_t)strlen( options->will_payload );
    if ( client_len == 0u || client_len > 65535u || will_topic_len == 0u ||
         will_topic_len > 65535u || will_payload_len == 0u || will_payload_len > 65535u )
    {
        return -1;
    }

    /* Will flag + retained will, QoS 0. */
    flags |= 0x04u | 0x20u;
    if ( options->username != NULL && options->username[0] != '\0' )
    {
        username_len = (uint32_t)strlen( options->username );
        if ( username_len > 65535u )
        {
            return -1;
        }
        flags |= 0x80u;
        if ( options->password != NULL && options->password[0] != '\0' )
        {
            password_len = (uint32_t)strlen( options->password );
            if ( password_len > 65535u )
            {
                return -1;
            }
            flags |= 0x40u;
        }
    }

    remaining = (uint32_t)sizeof( variable_header_prefix ) + 3u +
                2u + client_len + 2u + will_topic_len + 2u + will_payload_len;
    if ( username_len != 0u )
    {
        remaining += 2u + username_len;
    }
    if ( password_len != 0u )
    {
        remaining += 2u + password_len;
    }

    if ( put_u8( out, out_size, &pos, 0x10u ) != 0 ||
         put_remaining_length( out, out_size, &pos, remaining ) != 0 ||
         put_bytes( out, out_size, &pos, variable_header_prefix,
                    (uint32_t)sizeof( variable_header_prefix ) ) != 0 ||
         put_u8( out, out_size, &pos, flags ) != 0 ||
         put_u8( out, out_size, &pos, (uint8_t)( options->keep_alive_s >> 8 ) ) != 0 ||
         put_u8( out, out_size, &pos, (uint8_t)options->keep_alive_s ) != 0 ||
         put_utf8( out, out_size, &pos, options->client_id ) != 0 ||
         put_utf8( out, out_size, &pos, options->will_topic ) != 0 ||
         put_utf8( out, out_size, &pos, options->will_payload ) != 0 )
    {
        return -1;
    }
    if ( username_len != 0u && put_utf8( out, out_size, &pos, options->username ) != 0 )
    {
        return -1;
    }
    if ( password_len != 0u && put_utf8( out, out_size, &pos, options->password ) != 0 )
    {
        return -1;
    }
    return (int)pos;
}

int rewair_mqtt_packet_publish( const char* topic, const char* payload, uint8_t retained,
                                uint8_t* out, uint32_t out_size )
{
    uint32_t topic_len;
    uint32_t payload_len;
    uint32_t pos = 0u;

    if ( topic == NULL || payload == NULL || out == NULL )
    {
        return -1;
    }
    topic_len = (uint32_t)strlen( topic );
    payload_len = (uint32_t)strlen( payload );
    if ( topic_len == 0u || topic_len > 65535u )
    {
        return -1;
    }
    if ( put_u8( out, out_size, &pos, (uint8_t)( 0x30u | ( retained != 0u ? 1u : 0u ) ) ) != 0 ||
         put_remaining_length( out, out_size, &pos, 2u + topic_len + payload_len ) != 0 ||
         put_utf8( out, out_size, &pos, topic ) != 0 ||
         put_bytes( out, out_size, &pos, payload, payload_len ) != 0 )
    {
        return -1;
    }
    return (int)pos;
}

int rewair_mqtt_packet_disconnect( uint8_t* out, uint32_t out_size )
{
    if ( out == NULL || out_size < 2u )
    {
        return -1;
    }
    out[0] = 0xe0u;
    out[1] = 0x00u;
    return 2;
}
