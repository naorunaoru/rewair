#pragma once

#include <stdint.h>

#define REWAIR_MQTT_HOST_MAX              64u
#define REWAIR_MQTT_USERNAME_MAX          48u
#define REWAIR_MQTT_PASSWORD_MAX          64u
#define REWAIR_MQTT_TOPIC_PREFIX_MAX      80u
#define REWAIR_MQTT_DISCOVERY_PREFIX_MAX  32u

typedef struct
{
    uint32_t magic;
    uint8_t  enabled;
    uint8_t  discovery;
    uint16_t port;
    char     host[REWAIR_MQTT_HOST_MAX];
    char     username[REWAIR_MQTT_USERNAME_MAX];
    char     password[REWAIR_MQTT_PASSWORD_MAX];
    char     topic_prefix[REWAIR_MQTT_TOPIC_PREFIX_MAX];
    char     discovery_prefix[REWAIR_MQTT_DISCOVERY_PREFIX_MAX];
} rewair_mqtt_config_t;

typedef struct
{
    uint8_t  connected;
    uint32_t published;
    uint32_t reconnects;
    char     last_error[64];
} rewair_mqtt_status_t;

void rewair_mqtt_config_defaults( rewair_mqtt_config_t* config );
void rewair_mqtt_config_load( rewair_mqtt_config_t* config );
int  rewair_mqtt_config_save( const rewair_mqtt_config_t* config );
int  rewair_mqtt_config_reset( void );

/* Topic prefixes are intentionally conservative: letters, digits, '_', '-',
 * '.', and '/' only; no MQTT wildcards. Empty topic_prefix means use the
 * stable per-device default. discovery_prefix must be non-empty. */
int rewair_mqtt_topic_prefix_valid( const char* prefix, uint8_t allow_empty );

void rewair_mqtt_start( void );
void rewair_mqtt_reconfigure( void );
void rewair_mqtt_get_status( rewair_mqtt_status_t* status );
