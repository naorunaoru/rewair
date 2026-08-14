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

int rewair_mqtt_packet_subscribe( const char* topic_filter, uint16_t packet_id,
                                  uint8_t* out, uint32_t out_size )
{
    uint32_t topic_length;
    uint32_t pos = 0u;

    if ( topic_filter == NULL || packet_id == 0u || out == NULL )
    {
        return -1;
    }
    topic_length = (uint32_t)strlen( topic_filter );
    if ( topic_length == 0u || topic_length > 65535u )
    {
        return -1;
    }

    /* A SUBSCRIBE packet always has fixed-header flags 0b0010.  Rewair asks
     * for a single QoS 0 topic filter. */
    if ( put_u8( out, out_size, &pos, 0x82u ) != 0 ||
         put_remaining_length( out, out_size, &pos, 5u + topic_length ) != 0 ||
         put_u8( out, out_size, &pos, (uint8_t)( packet_id >> 8 ) ) != 0 ||
         put_u8( out, out_size, &pos, (uint8_t)packet_id ) != 0 ||
         put_utf8( out, out_size, &pos, topic_filter ) != 0 ||
         put_u8( out, out_size, &pos, 0x00u ) != 0 )
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

static int mqtt_fixed_header_valid( uint8_t header )
{
    uint8_t type = (uint8_t)( header >> 4 );
    uint8_t flags = (uint8_t)( header & 0x0fu );

    if ( type == 0u || type == 15u )
    {
        return 0;
    }
    if ( type == 3u )
    {
        uint8_t qos = (uint8_t)( ( flags >> 1 ) & 0x03u );

        /* QoS bits 0b11 are reserved.  The remaining PUBLISH flags are
         * surfaced to the application rather than discarded. */
        if ( qos == 0x03u || ( qos == 0u && ( flags & 0x08u ) != 0u ) )
        {
            return 0;
        }
        return 1;
    }
    if ( type == 6u || type == 8u || type == 10u )
    {
        return flags == 0x02u ? 1 : 0;
    }
    return flags == 0u ? 1 : 0;
}

static int mqtt_packet_identifier_valid( const uint8_t* body, uint32_t length )
{
    return length == 2u && ( body[0] != 0u || body[1] != 0u );
}

static int mqtt_emit_publish( const uint8_t* body, uint32_t length, uint8_t header,
                              rewair_mqtt_event_handler_t handler, void* context )
{
    rewair_mqtt_event_t event;
    uint32_t topic_length;
    uint32_t payload_offset;
    uint8_t qos = (uint8_t)( ( header >> 1 ) & 0x03u );

    if ( length < 2u )
    {
        return REWAIR_MQTT_DECODE_MALFORMED;
    }
    topic_length = ( (uint32_t)body[0] << 8 ) | body[1];
    if ( topic_length == 0u || topic_length > length - 2u )
    {
        return REWAIR_MQTT_DECODE_MALFORMED;
    }
    payload_offset = 2u + topic_length;

    memset( &event, 0, sizeof( event ) );
    event.type = REWAIR_MQTT_EVENT_PUBLISH;
    event.data.publish.topic = body + 2u;
    event.data.publish.topic_length = topic_length;
    event.data.publish.qos = qos;
    event.data.publish.duplicate = ( header & 0x08u ) != 0u ? 1u : 0u;
    event.data.publish.retained = ( header & 0x01u ) != 0u ? 1u : 0u;

    if ( qos != 0u )
    {
        if ( length - payload_offset < 2u )
        {
            return REWAIR_MQTT_DECODE_MALFORMED;
        }
        event.data.publish.packet_id =
            (uint16_t)( ( (uint16_t)body[payload_offset] << 8 ) |
                        body[payload_offset + 1u] );
        if ( event.data.publish.packet_id == 0u )
        {
            return REWAIR_MQTT_DECODE_MALFORMED;
        }
        payload_offset += 2u;
    }

    event.data.publish.payload = body + payload_offset;
    event.data.publish.payload_length = length - payload_offset;
    if ( handler != NULL )
    {
        handler( context, &event );
    }
    return REWAIR_MQTT_DECODE_OK;
}

static int mqtt_emit_suback( const uint8_t* body, uint32_t length,
                             rewair_mqtt_event_handler_t handler, void* context )
{
    rewair_mqtt_event_t event;
    uint32_t i;

    if ( length < 3u || ( body[0] == 0u && body[1] == 0u ) )
    {
        return REWAIR_MQTT_DECODE_MALFORMED;
    }
    for ( i = 2u; i < length; i++ )
    {
        if ( body[i] != 0x00u && body[i] != 0x01u && body[i] != 0x02u &&
             body[i] != 0x80u )
        {
            return REWAIR_MQTT_DECODE_MALFORMED;
        }
    }

    memset( &event, 0, sizeof( event ) );
    event.type = REWAIR_MQTT_EVENT_SUBACK;
    event.data.suback.packet_id =
        (uint16_t)( ( (uint16_t)body[0] << 8 ) | body[1] );
    event.data.suback.return_codes = body + 2u;
    event.data.suback.return_code_count = length - 2u;
    if ( handler != NULL )
    {
        handler( context, &event );
    }
    return REWAIR_MQTT_DECODE_OK;
}

static int mqtt_decode_complete_frame( rewair_mqtt_decoder_t* decoder,
                                       rewair_mqtt_event_handler_t handler,
                                       void* context )
{
    uint8_t header = decoder->storage[0];
    uint8_t type = (uint8_t)( header >> 4 );
    const uint8_t* body = decoder->storage + 1u + decoder->remaining_bytes;
    uint32_t length = decoder->remaining_length;

    switch ( type )
    {
        case 2u: /* CONNACK */
            if ( length != 2u || ( body[0] & 0xfeu ) != 0u || body[1] > 5u ||
                 ( body[1] != 0u && body[0] != 0u ) )
            {
                return REWAIR_MQTT_DECODE_MALFORMED;
            }
            break;
        case 3u:
            return mqtt_emit_publish( body, length, header, handler, context );
        case 4u: /* PUBACK */
        case 5u: /* PUBREC */
        case 6u: /* PUBREL */
        case 7u: /* PUBCOMP */
        case 11u: /* UNSUBACK */
            if ( mqtt_packet_identifier_valid( body, length ) == 0 )
            {
                return REWAIR_MQTT_DECODE_MALFORMED;
            }
            break;
        case 9u:
            return mqtt_emit_suback( body, length, handler, context );
        case 12u: /* PINGREQ */
        case 13u: /* PINGRESP */
        case 14u: /* DISCONNECT */
            if ( length != 0u )
            {
                return REWAIR_MQTT_DECODE_MALFORMED;
            }
            break;
        default:
            /* CONNECT, SUBSCRIBE and UNSUBSCRIBE are not expected from a
             * broker, but framing them before ignoring keeps stream sync. */
            break;
    }
    return REWAIR_MQTT_DECODE_OK;
}

int rewair_mqtt_decoder_init( rewair_mqtt_decoder_t* decoder, uint8_t* storage,
                              uint32_t capacity )
{
    if ( decoder == NULL || storage == NULL || capacity < 5u )
    {
        return REWAIR_MQTT_DECODE_INVALID_ARGUMENT;
    }
    memset( decoder, 0, sizeof( *decoder ) );
    decoder->storage = storage;
    decoder->capacity = capacity;
    decoder->remaining_multiplier = 1u;
    return REWAIR_MQTT_DECODE_OK;
}

void rewair_mqtt_decoder_reset( rewair_mqtt_decoder_t* decoder )
{
    if ( decoder == NULL )
    {
        return;
    }
    decoder->used = 0u;
    decoder->remaining_length = 0u;
    decoder->remaining_multiplier = 1u;
    decoder->remaining_bytes = 0u;
    decoder->header_complete = 0u;
    decoder->error = REWAIR_MQTT_DECODE_OK;
}

static int mqtt_decoder_fail( rewair_mqtt_decoder_t* decoder, int error )
{
    decoder->error = error;
    return error;
}

int rewair_mqtt_decoder_feed( rewair_mqtt_decoder_t* decoder,
                              const uint8_t* bytes, uint32_t length,
                              rewair_mqtt_event_handler_t handler, void* context )
{
    uint32_t offset = 0u;

    if ( decoder == NULL || decoder->storage == NULL || decoder->capacity < 5u ||
         ( bytes == NULL && length != 0u ) )
    {
        return REWAIR_MQTT_DECODE_INVALID_ARGUMENT;
    }
    if ( decoder->error != REWAIR_MQTT_DECODE_OK )
    {
        return decoder->error;
    }
    if ( decoder->used > decoder->capacity )
    {
        return mqtt_decoder_fail( decoder, REWAIR_MQTT_DECODE_MALFORMED );
    }

    while ( offset < length )
    {
        if ( decoder->used == 0u )
        {
            uint8_t header = bytes[offset++];
            if ( mqtt_fixed_header_valid( header ) == 0 )
            {
                return mqtt_decoder_fail( decoder, REWAIR_MQTT_DECODE_MALFORMED );
            }
            decoder->storage[decoder->used++] = header;
        }

        while ( decoder->header_complete == 0u && offset < length )
        {
            uint8_t encoded = bytes[offset++];
            uint8_t digit = (uint8_t)( encoded & 0x7fu );

            if ( decoder->used >= decoder->capacity )
            {
                return mqtt_decoder_fail( decoder, REWAIR_MQTT_DECODE_OVERSIZE );
            }
            decoder->storage[decoder->used++] = encoded;
            decoder->remaining_length +=
                (uint32_t)digit * decoder->remaining_multiplier;
            decoder->remaining_bytes++;

            if ( ( encoded & 0x80u ) != 0u )
            {
                if ( decoder->remaining_bytes == 4u )
                {
                    return mqtt_decoder_fail( decoder, REWAIR_MQTT_DECODE_MALFORMED );
                }
                decoder->remaining_multiplier *= 128u;
                continue;
            }

            /* MQTT's variable-byte integer must use its shortest encoding. */
            if ( decoder->remaining_bytes > 1u && digit == 0u )
            {
                return mqtt_decoder_fail( decoder, REWAIR_MQTT_DECODE_MALFORMED );
            }
            decoder->header_complete = 1u;
            if ( decoder->remaining_length > decoder->capacity - decoder->used )
            {
                return mqtt_decoder_fail( decoder, REWAIR_MQTT_DECODE_OVERSIZE );
            }
        }

        if ( decoder->header_complete != 0u )
        {
            uint32_t frame_length = 1u + decoder->remaining_bytes +
                                    decoder->remaining_length;
            uint32_t needed = frame_length - decoder->used;
            uint32_t available = length - offset;
            uint32_t copy_length = needed < available ? needed : available;

            if ( copy_length != 0u )
            {
                memcpy( decoder->storage + decoder->used, bytes + offset, copy_length );
                decoder->used += copy_length;
                offset += copy_length;
            }
            if ( decoder->used == frame_length )
            {
                int result = mqtt_decode_complete_frame( decoder, handler, context );
                if ( result != REWAIR_MQTT_DECODE_OK )
                {
                    return mqtt_decoder_fail( decoder, result );
                }
                rewair_mqtt_decoder_reset( decoder );
            }
        }
    }
    return REWAIR_MQTT_DECODE_OK;
}

int rewair_mqtt_suback_validate( const rewair_mqtt_event_t* event,
                                 uint16_t expected_packet_id )
{
    uint8_t code;

    if ( event == NULL || event->type != REWAIR_MQTT_EVENT_SUBACK ||
         expected_packet_id == 0u ||
         event->data.suback.packet_id != expected_packet_id ||
         event->data.suback.return_codes == NULL ||
         event->data.suback.return_code_count != 1u )
    {
        return REWAIR_MQTT_SUBACK_INVALID;
    }
    code = event->data.suback.return_codes[0];
    if ( code == 0x00u )
    {
        return REWAIR_MQTT_SUBACK_ACCEPTED;
    }
    if ( code == 0x80u )
    {
        return REWAIR_MQTT_SUBACK_REJECTED;
    }
    return REWAIR_MQTT_SUBACK_INVALID;
}
