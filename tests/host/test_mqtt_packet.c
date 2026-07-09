#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rewair_mqtt_packet.h"

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

int main( void )
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
    assert( packet[header_len + 7u] == 0xe6u ); /* user + pass + retained will + will + clean */
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
        len = rewair_mqtt_packet_publish( "long/topic", payload, 0u, packet, sizeof( packet ) );
        assert( len > 0 );
        assert( ( packet[1] & 0x80u ) != 0u );
        assert( mqtt_remaining( packet, &header_len ) == len - (int)header_len );
    }

    assert( rewair_mqtt_packet_publish( "", "x", 0u, packet, sizeof( packet ) ) == -1 );
    assert( rewair_mqtt_packet_connect( &options, packet, 8u ) == -1 );
    assert( rewair_mqtt_packet_disconnect( packet, sizeof( packet ) ) == 2 );
    assert( packet[0] == 0xe0u && packet[1] == 0u );

    printf( "test_mqtt_packet OK\n" );
    return 0;
}
