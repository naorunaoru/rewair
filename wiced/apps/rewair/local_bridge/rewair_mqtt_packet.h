#pragma once

#include <stdint.h>

typedef struct
{
    const char* client_id;
    const char* username;
    const char* password;
    const char* will_topic;
    const char* will_payload;
    uint16_t    keep_alive_s;
} rewair_mqtt_connect_options_t;

/* MQTT 3.1.1 packet builders. Return encoded length, or -1 on invalid input /
 * insufficient output space. Kept WICED-free so the wire format is host tested. */
int rewair_mqtt_packet_connect( const rewair_mqtt_connect_options_t* options,
                                uint8_t* out, uint32_t out_size );
int rewair_mqtt_packet_publish( const char* topic, const char* payload, uint8_t retained,
                                uint8_t* out, uint32_t out_size );
int rewair_mqtt_packet_disconnect( uint8_t* out, uint32_t out_size );
