#include "rewair_api.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "wiced.h"
#include "wiced_wifi.h"
#include "rewair_json.h"
#include "rewair_state.h"
#include "rewair_wifi_dct.h"
#include "rewair_wifi_scan.h"

static int api_error( char* response, uint32_t response_size, uint16_t status,
                      const char* message, uint16_t* status_out )
{
    int length;

    *status_out = status;
    length = snprintf( response, response_size, "{\"error\":\"%s\"}", message );
    return length >= 0 && (uint32_t)length < response_size ? length : -1;
}

static void apply_connected_duration( rewair_status_t* status )
{
    wiced_time_t now_ms = 0;

    (void)wiced_time_get_time( &now_ms );
    if ( status->wifi_mode == 0u && status->connected_s != 0u )
    {
        status->connected_s = (uint32_t)now_ms / 1000u - status->connected_s;
    }
}

static int appendf( char* response, uint32_t response_size, uint32_t* position,
                    const char* format, ... )
{
    va_list args;
    int length;

    if ( *position >= response_size )
    {
        return -1;
    }
    va_start( args, format );
    length = vsnprintf( response + *position, response_size - *position, format, args );
    va_end( args );
    if ( length < 0 || (uint32_t)length >= response_size - *position )
    {
        return -1;
    }
    *position += (uint32_t)length;
    return 0;
}

static const char* security_name( wiced_security_t security )
{
    if ( security == WICED_SECURITY_OPEN )
    {
        return "open";
    }
    if ( ( security & WEP_ENABLED ) != 0 )
    {
        return "WEP";
    }
    if ( ( security & WPA2_SECURITY ) != 0 )
    {
        return "WPA2";
    }
    if ( ( security & WPA_SECURITY ) != 0 )
    {
        return "WPA";
    }
    return "WPA2";
}

static int serialize_scan( char* response, uint32_t response_size, uint16_t* status_out )
{
    uint32_t count = sensor_scan_blocking( );
    uint32_t position = 0u;
    uint32_t i;

    if ( appendf( response, response_size, &position, "[" ) != 0 )
    {
        return api_error( response, response_size, 500u, "scan too large", status_out );
    }
    for ( i = 0u; i < count; i++ )
    {
        const wiced_scan_result_t* result = console_scan_cache_get( i );
        char ssid[33];
        char escaped[66];
        uint32_t length;

        if ( result == NULL )
        {
            break;
        }
        length = result->SSID.length <= 32u ? result->SSID.length : 32u;
        memcpy( ssid, result->SSID.value, length );
        ssid[length] = '\0';
        (void)rewair_json_escape_string( ssid, escaped, sizeof( escaped ) );
        if ( appendf( response, response_size, &position,
                      "%s{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":\"%s\"}",
                      i == 0u ? "" : ",", escaped, (int)result->signal_strength,
                      security_name( result->security ) ) != 0 )
        {
            return api_error( response, response_size, 500u, "scan too large", status_out );
        }
    }
    if ( appendf( response, response_size, &position, "]" ) != 0 )
    {
        return api_error( response, response_size, 500u, "scan too large", status_out );
    }
    *status_out = 200u;
    return (int)position;
}

static int serialize_networks( char* response, uint32_t response_size, uint16_t* status_out )
{
    uint32_t position = 0u;
    uint32_t index;

    if ( appendf( response, response_size, &position, "[" ) != 0 )
    {
        return api_error( response, response_size, 500u, "networks too large", status_out );
    }
    for ( index = 0u; ; index++ )
    {
        char ssid[33];
        char escaped[66];
        wiced_security_t security = WICED_SECURITY_OPEN;
        int connected = 0;
        int in_range = 0;
        int32_t rssi = 0;
        uint32_t i;

        if ( wifi_list_get( index, ssid, &security, &connected ) == 0 )
        {
            break;
        }
        for ( i = 0u; ; i++ )
        {
            const wiced_scan_result_t* result = console_scan_cache_get( i );
            char scanned_ssid[33];
            uint32_t length;

            if ( result == NULL )
            {
                break;
            }
            length = result->SSID.length <= 32u ? result->SSID.length : 32u;
            memcpy( scanned_ssid, result->SSID.value, length );
            scanned_ssid[length] = '\0';
            if ( strcmp( scanned_ssid, ssid ) == 0 )
            {
                in_range = 1;
                rssi = result->signal_strength;
                break;
            }
        }
        (void)rewair_json_escape_string( ssid, escaped, sizeof( escaped ) );
        if ( appendf( response, response_size, &position,
                      "%s{\"ssid\":\"%s\",\"sec\":\"%s\",\"saved\":true,"
                      "\"in_range\":%s,\"rssi\":%d,\"connected\":%s}",
                      index == 0u ? "" : ",", escaped, security_name( security ),
                      in_range != 0 ? "true" : "false", (int)rssi,
                      connected != 0 ? "true" : "false" ) != 0 )
        {
            return api_error( response, response_size, 500u, "networks too large", status_out );
        }
    }
    if ( appendf( response, response_size, &position, "]" ) != 0 )
    {
        return api_error( response, response_size, 500u, "networks too large", status_out );
    }
    *status_out = 200u;
    return (int)position;
}

int rewair_api_execute( uint8_t operation, const uint8_t* body, uint32_t body_length,
                        char* response, uint32_t response_size, uint16_t* status_out )
{
    (void)body;
    (void)body_length;

    if ( response == NULL || response_size == 0u || status_out == NULL )
    {
        return -1;
    }

    if ( operation == REWAIR_API_OP_CAPABILITIES )
    {
        static const char capabilities[] =
            "{\"api\":1,\"ble_protocol\":1,"
            "\"operations\":[\"capabilities\",\"status\",\"scan\",\"networks\"],"
            "\"mutations\":false}";

        if ( sizeof( capabilities ) > response_size )
        {
            return -1;
        }
        memcpy( response, capabilities, sizeof( capabilities ) );
        *status_out = 200u;
        return (int)( sizeof( capabilities ) - 1u );
    }

    if ( operation == REWAIR_API_OP_STATUS )
    {
        rewair_status_t status;
        int length;

        rewair_state_snapshot( &status );
        apply_connected_duration( &status );
        length = rewair_json_status( &status, response, response_size );
        if ( length < 0 )
        {
            return api_error( response, response_size, 500u, "status too large", status_out );
        }
        *status_out = 200u;
        return length;
    }

    if ( operation == REWAIR_API_OP_SCAN )
    {
        return serialize_scan( response, response_size, status_out );
    }

    if ( operation == REWAIR_API_OP_NETWORKS )
    {
        return serialize_networks( response, response_size, status_out );
    }

    return api_error( response, response_size, 501u, "operation not available", status_out );
}
