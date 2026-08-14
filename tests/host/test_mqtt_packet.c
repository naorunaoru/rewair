#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rewair_mqtt_packet.h"

#define CAPTURED_EVENTS_MAX 8u
#define CAPTURED_TOPIC_MAX  128u
#define CAPTURED_PAYLOAD_MAX 320u
#define CAPTURED_CODES_MAX  8u

typedef struct
{
    rewair_mqtt_event_type_t type;
    uint8_t topic[CAPTURED_TOPIC_MAX];
    uint32_t topic_length;
    uint8_t payload[CAPTURED_PAYLOAD_MAX];
    uint32_t payload_length;
    uint16_t packet_id;
    uint8_t duplicate;
    uint8_t qos;
    uint8_t retained;
    uint8_t return_codes[CAPTURED_CODES_MAX];
    uint32_t return_code_count;
} captured_event_t;

typedef struct
{
    captured_event_t events[CAPTURED_EVENTS_MAX];
    uint32_t count;
    uint16_t expected_suback_id;
    int last_suback_result;
} capture_t;

static int mqtt_remaining( const uint8_t* packet, uint32_t* header_len )
{
    uint32_t multiplier = 1u;
    uint32_t value = 0u;
    uint32_t pos = 1u;
    uint8_t byte;

    do
    {
        byte = packet[pos++];
        value += (uint32_t)( byte & 0x7fu ) * multiplier;
        multiplier *= 128u;
    } while ( ( byte & 0x80u ) != 0u );
    *header_len = pos;
    return (int)value;
}

static void capture_event( void* context, const rewair_mqtt_event_t* event )
{
    capture_t* capture = (capture_t*)context;
    captured_event_t* captured;

    assert( capture != NULL );
    assert( event != NULL );
    assert( capture->count < CAPTURED_EVENTS_MAX );
    captured = &capture->events[capture->count++];
    memset( captured, 0, sizeof( *captured ) );
    captured->type = event->type;

    if ( event->type == REWAIR_MQTT_EVENT_PUBLISH )
    {
        assert( event->data.publish.topic_length <= sizeof( captured->topic ) );
        assert( event->data.publish.payload_length <= sizeof( captured->payload ) );
        memcpy( captured->topic, event->data.publish.topic,
                event->data.publish.topic_length );
        memcpy( captured->payload, event->data.publish.payload,
                event->data.publish.payload_length );
        captured->topic_length = event->data.publish.topic_length;
        captured->payload_length = event->data.publish.payload_length;
        captured->packet_id = event->data.publish.packet_id;
        captured->duplicate = event->data.publish.duplicate;
        captured->qos = event->data.publish.qos;
        captured->retained = event->data.publish.retained;
    }
    else
    {
        assert( event->type == REWAIR_MQTT_EVENT_SUBACK );
        assert( event->data.suback.return_code_count <=
                sizeof( captured->return_codes ) );
        memcpy( captured->return_codes, event->data.suback.return_codes,
                event->data.suback.return_code_count );
        captured->packet_id = event->data.suback.packet_id;
        captured->return_code_count = event->data.suback.return_code_count;
        capture->last_suback_result =
            rewair_mqtt_suback_validate( event, capture->expected_suback_id );
    }
}

static void test_existing_builders( void )
{
    uint8_t packet[512];
    rewair_mqtt_connect_options_t options;
    uint32_t header_len;
    int len;

    memset( &options, 0, sizeof( options ) );
    options.client_id = "rewair-aabbccddeeff";
    options.username = "ha";
    options.password = "secret";
    options.will_topic = "rewair/rewair_aabbccddeeff/availability";
    options.will_payload = "offline";
    options.keep_alive_s = 45u;

    len = rewair_mqtt_packet_connect( &options, packet, sizeof( packet ) );
    assert( len > 0 );
    assert( packet[0] == 0x10u );
    assert( mqtt_remaining( packet, &header_len ) == len - (int)header_len );
    assert( packet[header_len + 0u] == 0x00u && packet[header_len + 1u] == 0x04u );
    assert( memcmp( packet + header_len + 2u, "MQTT", 4u ) == 0 );
    assert( packet[header_len + 6u] == 0x04u );
    assert( packet[header_len + 7u] == 0xe6u );
    assert( packet[header_len + 8u] == 0x00u && packet[header_len + 9u] == 45u );

    len = rewair_mqtt_packet_publish( "rewair/test/state", "{\"temperature\":22.40}", 1u,
                                      packet, sizeof( packet ) );
    assert( len > 0 );
    assert( packet[0] == 0x31u );
    assert( mqtt_remaining( packet, &header_len ) == len - (int)header_len );
    assert( packet[header_len] == 0u && packet[header_len + 1u] == 17u );
    assert( memcmp( packet + header_len + 2u, "rewair/test/state", 17u ) == 0 );

    /* Multi-byte remaining-length encoding. */
    {
        char payload[220];
        memset( payload, 'x', sizeof( payload ) - 1u );
        payload[sizeof( payload ) - 1u] = '\0';
        len = rewair_mqtt_packet_publish( "long/topic", payload, 0u,
                                          packet, sizeof( packet ) );
        assert( len > 0 );
        assert( ( packet[1] & 0x80u ) != 0u );
        assert( mqtt_remaining( packet, &header_len ) == len - (int)header_len );
    }

    assert( rewair_mqtt_packet_publish( "", "x", 0u,
                                        packet, sizeof( packet ) ) == -1 );
    assert( rewair_mqtt_packet_connect( &options, packet, 8u ) == -1 );
    assert( rewair_mqtt_packet_disconnect( packet, sizeof( packet ) ) == 2 );
    assert( packet[0] == 0xe0u && packet[1] == 0u );
}

static void test_subscribe_builder( void )
{
    static const uint8_t expected[] =
    {
        0x82u, 0x08u, 0x12u, 0x34u, 0x00u, 0x03u, 'a', '/', 'b', 0x00u
    };
    uint8_t packet[256];
    char long_topic[124];
    int len;

    len = rewair_mqtt_packet_subscribe( "a/b", 0x1234u,
                                        packet, sizeof( packet ) );
    assert( len == (int)sizeof( expected ) );
    assert( memcmp( packet, expected, sizeof( expected ) ) == 0 );

    memset( long_topic, 'a', sizeof( long_topic ) - 1u );
    long_topic[sizeof( long_topic ) - 1u] = '\0';
    len = rewair_mqtt_packet_subscribe( long_topic, 1u,
                                        packet, sizeof( packet ) );
    assert( len > 0 );
    assert( packet[0] == 0x82u );
    assert( packet[1] == 0x80u && packet[2] == 0x01u );

    assert( rewair_mqtt_packet_subscribe( "a/b", 0u,
                                          packet, sizeof( packet ) ) == -1 );
    assert( rewair_mqtt_packet_subscribe( "", 1u,
                                          packet, sizeof( packet ) ) == -1 );
    assert( rewair_mqtt_packet_subscribe( "a/b", 1u, packet, 4u ) == -1 );
}

static void test_decoder_byte_at_a_time( void )
{
    uint8_t packet[128];
    uint8_t storage[128];
    rewair_mqtt_decoder_t decoder;
    capture_t capture;
    uint32_t i;
    int len;

    memset( &capture, 0, sizeof( capture ) );
    len = rewair_mqtt_packet_publish( "rewair/settings/display_mode/set", "Score", 1u,
                                      packet, sizeof( packet ) );
    assert( len > 0 );
    assert( rewair_mqtt_decoder_init( &decoder, storage,
                                      sizeof( storage ) ) == REWAIR_MQTT_DECODE_OK );
    for ( i = 0u; i < (uint32_t)len; i++ )
    {
        assert( rewair_mqtt_decoder_feed( &decoder, packet + i, 1u,
                                          capture_event, &capture ) ==
                REWAIR_MQTT_DECODE_OK );
        assert( capture.count == ( i + 1u == (uint32_t)len ? 1u : 0u ) );
    }
    assert( capture.events[0].type == REWAIR_MQTT_EVENT_PUBLISH );
    assert( capture.events[0].topic_length == 32u );
    assert( memcmp( capture.events[0].topic,
                    "rewair/settings/display_mode/set", 32u ) == 0 );
    assert( capture.events[0].payload_length == 5u );
    assert( memcmp( capture.events[0].payload, "Score", 5u ) == 0 );
    assert( capture.events[0].qos == 0u );
    assert( capture.events[0].packet_id == 0u );
    assert( capture.events[0].retained == 1u );
}

static void test_decoder_split_header_and_body( void )
{
    uint8_t packet[512];
    uint8_t storage[512];
    rewair_mqtt_decoder_t decoder;
    capture_t capture;
    char payload[220];
    uint32_t header_len;
    int len;

    memset( &capture, 0, sizeof( capture ) );
    memset( payload, 'p', sizeof( payload ) - 1u );
    payload[sizeof( payload ) - 1u] = '\0';
    len = rewair_mqtt_packet_publish( "split/topic", payload, 0u,
                                      packet, sizeof( packet ) );
    assert( len > 0 );
    assert( mqtt_remaining( packet, &header_len ) == len - (int)header_len );
    assert( header_len == 3u );
    assert( rewair_mqtt_decoder_init( &decoder, storage,
                                      sizeof( storage ) ) == REWAIR_MQTT_DECODE_OK );

    assert( rewair_mqtt_decoder_feed( &decoder, packet, 1u,
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( rewair_mqtt_decoder_feed( &decoder, packet + 1u, 1u,
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( rewair_mqtt_decoder_feed( &decoder, packet + 2u, 1u,
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( rewair_mqtt_decoder_feed( &decoder, packet + header_len, 7u,
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( capture.count == 0u );
    assert( rewair_mqtt_decoder_feed( &decoder, packet + header_len + 7u,
                                      (uint32_t)len - header_len - 7u,
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( capture.count == 1u );
    assert( capture.events[0].payload_length == sizeof( payload ) - 1u );
    assert( memcmp( capture.events[0].payload, payload,
                    sizeof( payload ) - 1u ) == 0 );
}

static void test_decoder_coalesced_frames_and_flags( void )
{
    static const uint8_t pingresp[] = { 0xd0u, 0x00u };
    static const uint8_t suback[] = { 0x90u, 0x03u, 0xbeu, 0xefu, 0x00u };
    static const uint8_t first[] =
        { 0x3bu, 0x08u, 0x00u, 0x01u, 'a', 0x12u, 0x34u, 'o', 'n', 'e' };
    uint8_t second[64];
    uint8_t joined[160];
    uint8_t storage[160];
    rewair_mqtt_decoder_t decoder;
    capture_t capture;
    uint32_t joined_length = 0u;
    int second_length;

    memset( &capture, 0, sizeof( capture ) );
    capture.expected_suback_id = 0xbeefu;
    capture.last_suback_result = REWAIR_MQTT_SUBACK_INVALID;
    second_length = rewair_mqtt_packet_publish( "b", "two", 0u,
                                                second, sizeof( second ) );
    assert( second_length > 0 );

    memcpy( joined + joined_length, first, sizeof( first ) );
    joined_length += sizeof( first );
    memcpy( joined + joined_length, pingresp, sizeof( pingresp ) );
    joined_length += sizeof( pingresp );
    memcpy( joined + joined_length, suback, sizeof( suback ) );
    joined_length += sizeof( suback );
    memcpy( joined + joined_length, second, (uint32_t)second_length );
    joined_length += (uint32_t)second_length;

    assert( rewair_mqtt_decoder_init( &decoder, storage,
                                      sizeof( storage ) ) == REWAIR_MQTT_DECODE_OK );
    assert( rewair_mqtt_decoder_feed( &decoder, joined, joined_length,
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( capture.count == 3u ); /* PINGRESP was safely ignored. */
    assert( capture.events[0].type == REWAIR_MQTT_EVENT_PUBLISH );
    assert( capture.events[0].retained == 1u );
    assert( capture.events[0].duplicate == 1u );
    assert( capture.events[0].qos == 1u );
    assert( capture.events[0].packet_id == 0x1234u );
    assert( capture.events[1].type == REWAIR_MQTT_EVENT_SUBACK );
    assert( capture.events[1].packet_id == 0xbeefu );
    assert( capture.last_suback_result == REWAIR_MQTT_SUBACK_ACCEPTED );
    assert( capture.events[2].type == REWAIR_MQTT_EVENT_PUBLISH );
    assert( capture.events[2].retained == 0u );
    assert( capture.events[2].duplicate == 0u );
}

static void test_suback_validation( void )
{
    static const uint8_t accepted[] = { 0x90u, 0x03u, 0x12u, 0x34u, 0x00u };
    static const uint8_t rejected[] = { 0x90u, 0x03u, 0x12u, 0x34u, 0x80u };
    static const uint8_t multiple[] = { 0x90u, 0x04u, 0x12u, 0x34u, 0x00u, 0x00u };
    uint8_t storage[32];
    rewair_mqtt_decoder_t decoder;
    capture_t capture;

    memset( &capture, 0, sizeof( capture ) );
    capture.expected_suback_id = 0x1234u;
    assert( rewair_mqtt_decoder_init( &decoder, storage,
                                      sizeof( storage ) ) == REWAIR_MQTT_DECODE_OK );

    assert( rewair_mqtt_decoder_feed( &decoder, accepted, sizeof( accepted ),
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( capture.last_suback_result == REWAIR_MQTT_SUBACK_ACCEPTED );
    assert( rewair_mqtt_decoder_feed( &decoder, rejected, sizeof( rejected ),
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( capture.last_suback_result == REWAIR_MQTT_SUBACK_REJECTED );

    capture.expected_suback_id = 0x9999u;
    assert( rewair_mqtt_decoder_feed( &decoder, accepted, sizeof( accepted ),
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( capture.last_suback_result == REWAIR_MQTT_SUBACK_INVALID );

    capture.expected_suback_id = 0x1234u;
    assert( rewair_mqtt_decoder_feed( &decoder, multiple, sizeof( multiple ),
                                      capture_event, &capture ) == REWAIR_MQTT_DECODE_OK );
    assert( capture.last_suback_result == REWAIR_MQTT_SUBACK_INVALID );
}

static void test_decoder_rejects_malformed_and_oversize( void )
{
    static const uint8_t bad_flags[] = { 0xd1u };
    static const uint8_t bad_qos[] = { 0x36u };
    static const uint8_t qos0_duplicate[] = { 0x38u };
    static const uint8_t non_minimal[] = { 0x30u, 0x80u, 0x00u };
    static const uint8_t too_many_remaining_bytes[] =
        { 0x30u, 0x80u, 0x80u, 0x80u, 0x80u };
    static const uint8_t bad_publish[] = { 0x30u, 0x02u, 0x00u, 0x01u };
    static const uint8_t bad_suback_code[] =
        { 0x90u, 0x03u, 0x12u, 0x34u, 0x03u };
    static const uint8_t zero_suback_id[] =
        { 0x90u, 0x03u, 0x00u, 0x00u, 0x00u };
    static const uint8_t oversized[] = { 0x30u, 0x07u };
    static const uint8_t pingresp[] = { 0xd0u, 0x00u };
    uint8_t storage[32];
    uint8_t small_storage[8];
    rewair_mqtt_decoder_t decoder;

    assert( rewair_mqtt_decoder_init( NULL, storage,
                                      sizeof( storage ) ) ==
            REWAIR_MQTT_DECODE_INVALID_ARGUMENT );
    assert( rewair_mqtt_decoder_init( &decoder, storage, 4u ) ==
            REWAIR_MQTT_DECODE_INVALID_ARGUMENT );

    assert( rewair_mqtt_decoder_init( &decoder, storage,
                                      sizeof( storage ) ) == REWAIR_MQTT_DECODE_OK );
    assert( rewair_mqtt_decoder_feed( &decoder, bad_flags, sizeof( bad_flags ),
                                      NULL, NULL ) == REWAIR_MQTT_DECODE_MALFORMED );
    /* Protocol errors latch so callers cannot accidentally resume mid-frame. */
    assert( rewair_mqtt_decoder_feed( &decoder, pingresp, sizeof( pingresp ),
                                      NULL, NULL ) == REWAIR_MQTT_DECODE_MALFORMED );
    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, pingresp, sizeof( pingresp ),
                                      NULL, NULL ) == REWAIR_MQTT_DECODE_OK );

    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, bad_qos, sizeof( bad_qos ),
                                      NULL, NULL ) == REWAIR_MQTT_DECODE_MALFORMED );
    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, qos0_duplicate,
                                      sizeof( qos0_duplicate ), NULL, NULL ) ==
            REWAIR_MQTT_DECODE_MALFORMED );
    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, non_minimal,
                                      sizeof( non_minimal ), NULL, NULL ) ==
            REWAIR_MQTT_DECODE_MALFORMED );
    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, too_many_remaining_bytes,
                                      sizeof( too_many_remaining_bytes ), NULL, NULL ) ==
            REWAIR_MQTT_DECODE_MALFORMED );
    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, bad_publish, sizeof( bad_publish ),
                                      NULL, NULL ) == REWAIR_MQTT_DECODE_MALFORMED );
    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, bad_suback_code,
                                      sizeof( bad_suback_code ), NULL, NULL ) ==
            REWAIR_MQTT_DECODE_MALFORMED );
    rewair_mqtt_decoder_reset( &decoder );
    assert( rewair_mqtt_decoder_feed( &decoder, zero_suback_id,
                                      sizeof( zero_suback_id ), NULL, NULL ) ==
            REWAIR_MQTT_DECODE_MALFORMED );

    assert( rewair_mqtt_decoder_init( &decoder, small_storage,
                                      sizeof( small_storage ) ) == REWAIR_MQTT_DECODE_OK );
    assert( rewair_mqtt_decoder_feed( &decoder, oversized, sizeof( oversized ),
                                      NULL, NULL ) == REWAIR_MQTT_DECODE_OVERSIZE );
}

int main( void )
{
    test_existing_builders( );
    test_subscribe_builder( );
    test_decoder_byte_at_a_time( );
    test_decoder_split_header_and_body( );
    test_decoder_coalesced_frames_and_flags( );
    test_suback_validation( );
    test_decoder_rejects_malformed_and_oversize( );

    printf( "test_mqtt_packet OK\n" );
    return 0;
}
