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
int rewair_mqtt_packet_subscribe( const char* topic_filter, uint16_t packet_id,
                                  uint8_t* out, uint32_t out_size );
int rewair_mqtt_packet_disconnect( uint8_t* out, uint32_t out_size );

typedef enum
{
    REWAIR_MQTT_EVENT_PUBLISH = 1,
    REWAIR_MQTT_EVENT_SUBACK  = 2
} rewair_mqtt_event_type_t;

typedef struct
{
    const uint8_t* topic;
    uint32_t       topic_length;
    const uint8_t* payload;
    uint32_t       payload_length;
    uint16_t       packet_id; /* Zero for QoS 0. */
    uint8_t        duplicate;
    uint8_t        qos;
    uint8_t        retained;
} rewair_mqtt_publish_event_t;

typedef struct
{
    uint16_t       packet_id;
    const uint8_t* return_codes;
    uint32_t       return_code_count;
} rewair_mqtt_suback_event_t;

typedef struct
{
    rewair_mqtt_event_type_t type;
    union
    {
        rewair_mqtt_publish_event_t publish;
        rewair_mqtt_suback_event_t  suback;
    } data;
} rewair_mqtt_event_t;

/* Event byte spans point into the decoder's caller-owned storage and remain
 * valid only for the duration of the handler call.  Frames which do not need
 * application handling are validated/framed and then ignored. */
typedef void ( *rewair_mqtt_event_handler_t )( void* context,
                                                const rewair_mqtt_event_t* event );

typedef enum
{
    REWAIR_MQTT_DECODE_OK               = 0,
    REWAIR_MQTT_DECODE_INVALID_ARGUMENT = -1,
    REWAIR_MQTT_DECODE_MALFORMED        = -2,
    REWAIR_MQTT_DECODE_OVERSIZE         = -3
} rewair_mqtt_decode_result_t;

/* Streaming MQTT 3.1.1 decoder state.  The storage and capacity members are
 * configured by init; the remaining members are private decoder state. */
typedef struct
{
    uint8_t* storage;
    uint32_t capacity;
    uint32_t used;
    uint32_t remaining_length;
    uint32_t remaining_multiplier;
    uint8_t  remaining_bytes;
    uint8_t  header_complete;
    int      error;
} rewair_mqtt_decoder_t;

/* The decoder owns no memory.  storage must remain valid until the decoder is
 * no longer used and must hold at least the five-byte maximum fixed header.
 * One feed call may deliver any number of events from coalesced TCP data.
 * MALFORMED and OVERSIZE errors latch until reset is called. */
int  rewair_mqtt_decoder_init( rewair_mqtt_decoder_t* decoder, uint8_t* storage,
                               uint32_t capacity );
void rewair_mqtt_decoder_reset( rewair_mqtt_decoder_t* decoder );
int  rewair_mqtt_decoder_feed( rewair_mqtt_decoder_t* decoder,
                               const uint8_t* bytes, uint32_t length,
                               rewair_mqtt_event_handler_t handler, void* context );

typedef enum
{
    REWAIR_MQTT_SUBACK_INVALID  = -1,
    REWAIR_MQTT_SUBACK_REJECTED = 0,
    REWAIR_MQTT_SUBACK_ACCEPTED = 1
} rewair_mqtt_suback_result_t;

/* Validate the response to the single QoS 0 filter emitted by the SUBSCRIBE
 * builder.  A matching 0x00 grant is accepted; 0x80 is a broker rejection. */
int rewair_mqtt_suback_validate( const rewair_mqtt_event_t* event,
                                 uint16_t expected_packet_id );
