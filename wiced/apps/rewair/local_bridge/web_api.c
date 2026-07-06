#include "web_api.h"

#include <stdio.h>
#include <string.h>

#include "http_server.h"
#include "rewair_state.h"
#include "rewair_json.h"
#include "rewair_settings.h"
#include "rewair_tz.h"

#define API_BODY_MAX      1024u
#define API_STATUS_BUF    1024u
#define API_WORKER_STACK  6000u
#define API_MAX_SAVED_NETWORKS CONFIG_AP_LIST_SIZE

static wiced_http_server_t http_server;
static uint32_t            server_started = 0u;

#ifdef REWAIR_API_CORS_DEV
#define API_CORS_HEADER "Access-Control-Allow-Origin: *\r\n"
#else
#define API_CORS_HEADER ""
#endif

static void api_send( wiced_http_response_stream_t* stream, const char* status_line,
                      const char* content_type, const char* body, uint32_t body_len )
{
    char header[224];
    int n;

    n = snprintf( header, sizeof( header ),
                  "%s\r\n"
                  "Content-Type: %s\r\n"
                  "Content-Length: %lu\r\n"
                  API_CORS_HEADER
                  "Cache-Control: no-store\r\n"
                  "\r\n",
                  status_line, content_type, (unsigned long)body_len );
    if ( n < 0 || n >= (int)sizeof( header ) )
    {
        return;
    }
    wiced_http_response_stream_write( stream, header, (uint32_t)n );
    if ( body_len != 0u )
    {
        wiced_http_response_stream_write( stream, body, body_len );
    }
}

static void api_send_error( wiced_http_response_stream_t* stream, const char* status_line,
                            const char* message )
{
    char body[160];
    int n = snprintf( body, sizeof( body ), "{\"error\":\"%s\"}", message );

    api_send( stream, status_line, "application/json", body, n > 0 ? (uint32_t)n : 0u );
}

static int method_is( const wiced_http_message_body_t* http_data, wiced_http_request_type_t t )
{
    return http_data != NULL && http_data->request_type == t;
}

/* ---- request body extraction ----
 * Bodies must arrive complete in the first packet (Global Constraints); this
 * rejects split bodies with 400 rather than trying to reassemble them. */
static int api_read_body( wiced_http_message_body_t* http_data, char* out, uint32_t out_size,
                          wiced_http_response_stream_t* stream )
{
    if ( http_data == NULL || http_data->data == NULL || http_data->message_data_length == 0u )
    {
        api_send_error( stream, HTTP_HEADER_400, "missing body" );
        return -1;
    }
    if ( http_data->total_message_data_remaining != 0u )
    {
        api_send_error( stream, HTTP_HEADER_400, "body split across packets" );
        return -1;
    }
    if ( http_data->message_data_length >= out_size )
    {
        api_send_error( stream, "HTTP/1.1 413 Payload Too Large", "body too large" );
        return -1;
    }
    memcpy( out, http_data->data, http_data->message_data_length );
    out[http_data->message_data_length] = '\0';
    return (int)http_data->message_data_length;
}

/* ---- GET /api/status ---- */
static int32_t api_status_handler( const char* url, wiced_http_response_stream_t* stream,
                                   void* arg, wiced_http_message_body_t* http_data )
{
    static char buf[API_STATUS_BUF];
    rewair_status_t st;
    wiced_time_t now_ms = 0;
    int len;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_GET_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "GET only" );
        return 0;
    }

    rewair_state_snapshot( &st );
    wiced_time_get_time( &now_ms );
    if ( st.wifi_mode == 0u && st.connected_s != 0u )
    {
        st.connected_s = (uint32_t)now_ms / 1000u - st.connected_s;
    }
    len = rewair_json_status( &st, buf, sizeof( buf ) );
    if ( len < 0 )
    {
        api_send_error( stream, HTTP_HEADER_500, "status too large" );
        return 0;
    }
    api_send( stream, HTTP_HEADER_200, "application/json", buf, (uint32_t)len );
    return 0;
}

/* ---- security display string ---- */
static const char* sec_name( wiced_security_t s )
{
    if ( s == WICED_SECURITY_OPEN )
    {
        return "open";
    }
    if ( ( s & WEP_ENABLED ) != 0 )
    {
        return "WEP";
    }
    if ( ( s & WPA2_SECURITY ) != 0 )
    {
        return "WPA2";
    }
    if ( ( s & WPA_SECURITY ) != 0 )
    {
        return "WPA";
    }
    return "WPA2";
}

/* ---- GET /api/scan ---- */
static int32_t api_scan_handler( const char* url, wiced_http_response_stream_t* stream,
                                 void* arg, wiced_http_message_body_t* http_data )
{
    static char buf[192];
    uint32_t count;
    uint32_t i;
    int n;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_GET_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "GET only" );
        return 0;
    }

    count = sensor_scan_blocking( );

    /* stream the array without a big buffer: raw header first (no Content-Length,
       so use Connection: close for this route) */
    {
        const char* hdr = HTTP_HEADER_200 "\r\n"
                          "Content-Type: application/json\r\n"
                          API_CORS_HEADER
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        wiced_http_response_stream_write( stream, hdr, (uint32_t)strlen( hdr ) );
    }
    wiced_http_response_stream_write( stream, "[", 1u );
    for ( i = 0u; i < count; i++ )
    {
        const wiced_scan_result_t* r = console_scan_cache_get( i );
        char ssid[33];
        char ssid_esc[66];
        uint32_t sl;

        if ( r == NULL )
        {
            break;
        }
        sl = r->SSID.length <= 32u ? r->SSID.length : 32u;
        memcpy( ssid, r->SSID.value, sl );
        ssid[sl] = '\0';
        (void)rewair_json_escape_string( ssid, ssid_esc, sizeof( ssid_esc ) );
        n = snprintf( buf, sizeof( buf ), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":\"%s\"}",
                      i == 0u ? "" : ",", ssid_esc, (int)r->signal_strength, sec_name( r->security ) );
        if ( n > 0 )
        {
            wiced_http_response_stream_write( stream, buf, (uint32_t)n );
        }
    }
    wiced_http_response_stream_write( stream, "]", 1u );

    /* The "Connection: close" header above is advisory only -- the WICED HTTP
     * server only auto-closes when the *request* carried that header. Since
     * this route has no Content-Length (streamed body), we must explicitly
     * queue the disconnect so the client sees EOF instead of hanging in
     * keep-alive until its own timeout. */
    wiced_http_response_stream_flush( stream );
    wiced_http_server_queue_disconnect_request( &http_server, stream->tcp_stream.socket );
    return 0;
}

/* ---- GET /api/networks ---- */
static int32_t api_networks_handler( const char* url, wiced_http_response_stream_t* stream,
                                     void* arg, wiced_http_message_body_t* http_data )
{
    static char buf[224];
    uint32_t index;
    int n;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_GET_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "GET only" );
        return 0;
    }

    /* Streamed, no Content-Length -- same explicit-disconnect pattern as
     * /api/scan (the daemon does not honor a response Connection: close). */
    {
        const char* hdr = HTTP_HEADER_200 "\r\n"
                          "Content-Type: application/json\r\n"
                          API_CORS_HEADER
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        wiced_http_response_stream_write( stream, hdr, (uint32_t)strlen( hdr ) );
    }
    wiced_http_response_stream_write( stream, "[", 1u );

    for ( index = 0u; ; index++ )
    {
        char ssid[33];
        char ssid_esc[66];
        wiced_security_t sec = WICED_SECURITY_OPEN;
        int connected = 0;
        int in_range = 0;
        int32_t rssi = 0;
        uint32_t i;

        if ( wifi_list_get( index, ssid, &sec, &connected ) == 0 )
        {
            break;
        }

        for ( i = 0u; ; i++ )
        {
            const wiced_scan_result_t* r = console_scan_cache_get( i );
            char scan_ssid[33];
            uint32_t sl;

            if ( r == NULL )
            {
                break;
            }
            sl = r->SSID.length <= 32u ? r->SSID.length : 32u;
            memcpy( scan_ssid, r->SSID.value, sl );
            scan_ssid[sl] = '\0';
            if ( strcmp( scan_ssid, ssid ) == 0 )
            {
                in_range = 1;
                rssi = r->signal_strength;
                break;
            }
        }

        (void)rewair_json_escape_string( ssid, ssid_esc, sizeof( ssid_esc ) );
        n = snprintf( buf, sizeof( buf ),
                      "%s{\"ssid\":\"%s\",\"sec\":\"%s\",\"saved\":true,\"in_range\":%s,"
                      "\"rssi\":%d,\"connected\":%s}",
                      index == 0u ? "" : ",", ssid_esc, sec_name( sec ),
                      in_range != 0 ? "true" : "false", (int)rssi,
                      connected != 0 ? "true" : "false" );
        if ( n > 0 )
        {
            wiced_http_response_stream_write( stream, buf, (uint32_t)n );
        }
    }

    wiced_http_response_stream_write( stream, "]", 1u );
    wiced_http_response_stream_flush( stream );
    wiced_http_server_queue_disconnect_request( &http_server, stream->tcp_stream.socket );
    return 0;
}

/* ---- POST /api/join ---- */
static int32_t api_join_handler( const char* url, wiced_http_response_stream_t* stream,
                                 void* arg, wiced_http_message_body_t* http_data )
{
    char body[API_BODY_MAX];
    char ssid[33];
    char pass[65];
    int len;
    wiced_result_t result;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }
    len = api_read_body( http_data, body, sizeof( body ), stream );
    if ( len < 0 )
    {
        return 0;
    }
    if ( rewair_req_get_string( body, (uint32_t)len, "ssid", ssid, sizeof( ssid ) ) != 1 )
    {
        api_send_error( stream, HTTP_HEADER_400, "ssid required" );
        return 0;
    }
    if ( rewair_req_get_string( body, (uint32_t)len, "pass", pass, sizeof( pass ) ) != 1 )
    {
        pass[0] = '\0';
    }

    result = wifi_list_add( ssid, pass );
    if ( result == WICED_SUCCESS )
    {
        api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
    }
    else if ( result == WICED_WWD_NOT_AUTHENTICATED || result == WICED_WWD_INVALID_JOIN_STATUS )
    {
        api_send_error( stream, "HTTP/1.1 401 Unauthorized", "Wrong password" );
    }
    else if ( result == WICED_WWD_NETWORK_NOT_FOUND || result == WICED_NOT_FOUND )
    {
        api_send_error( stream, HTTP_HEADER_404, "Network not found" );
    }
    else
    {
        api_send_error( stream, HTTP_HEADER_504, "Join failed" );
    }
    return 0;
}

/* ---- POST /api/forget ---- */
static int32_t api_forget_handler( const char* url, wiced_http_response_stream_t* stream,
                                   void* arg, wiced_http_message_body_t* http_data )
{
    char body[API_BODY_MAX];
    char ssid[33];
    int len;
    wiced_result_t result;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }
    len = api_read_body( http_data, body, sizeof( body ), stream );
    if ( len < 0 )
    {
        return 0;
    }
    if ( rewair_req_get_string( body, (uint32_t)len, "ssid", ssid, sizeof( ssid ) ) != 1 )
    {
        api_send_error( stream, HTTP_HEADER_400, "ssid required" );
        return 0;
    }

    result = wifi_list_remove( ssid );
    if ( result == WICED_SUCCESS )
    {
        api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
    }
    else if ( result == WICED_NOT_FOUND )
    {
        api_send_error( stream, HTTP_HEADER_404, "Not saved" );
    }
    else
    {
        api_send_error( stream, HTTP_HEADER_500, "forget failed" );
    }
    return 0;
}

/* ---- POST /api/priority ---- */
static int32_t api_priority_handler( const char* url, wiced_http_response_stream_t* stream,
                                     void* arg, wiced_http_message_body_t* http_data )
{
    char body[API_BODY_MAX];
    char order[API_MAX_SAVED_NETWORKS][33];
    uint32_t count = 0u;
    int len;
    wiced_result_t result;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }
    len = api_read_body( http_data, body, sizeof( body ), stream );
    if ( len < 0 )
    {
        return 0;
    }
    if ( rewair_req_get_string_array( body, (uint32_t)len, "order", order,
                                      API_MAX_SAVED_NETWORKS, &count ) != 1 )
    {
        api_send_error( stream, HTTP_HEADER_400, "order array required" );
        return 0;
    }

    result = wifi_list_reorder( order, count );
    if ( result == WICED_SUCCESS )
    {
        api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
    }
    else
    {
        api_send_error( stream, HTTP_HEADER_400, "order must match saved set" );
    }
    return 0;
}

/* ---- POST /api/settings ---- */
static int32_t api_settings_handler( const char* url, wiced_http_response_stream_t* stream,
                                     void* arg, wiced_http_message_body_t* http_data )
{
    char body[API_BODY_MAX];
    rewair_settings_t settings;
    char text[65];
    int len;
    int got;
    wiced_utc_time_t now = 0u;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }
    len = api_read_body( http_data, body, sizeof( body ), stream );
    if ( len < 0 )
    {
        return 0;
    }

    rewair_settings_load( &settings );

    got = rewair_req_get_string( body, (uint32_t)len, "name", text, sizeof( settings.name ) );
    if ( got < 0 )
    {
        /* rewair_req_get_string returns -1 for a body that fails to tokenize as
         * a JSON object at all (vs. 0 for "valid object, key absent") -- since
         * every field in this handler is optional, that distinction is the only
         * thing standing between a malformed body and a silent 204 no-op. */
        api_send_error( stream, HTTP_HEADER_400, "malformed body" );
        return 0;
    }
    if ( got == 1 )
    {
        strncpy( settings.name, text, sizeof( settings.name ) - 1u );
        settings.name[sizeof( settings.name ) - 1u] = '\0';
    }

    got = rewair_req_get_string( body, (uint32_t)len, "units", text, sizeof( text ) );
    if ( got == 1 )
    {
        if ( strcmp( text, "c" ) == 0 )
        {
            settings.units = 0u;
        }
        else if ( strcmp( text, "f" ) == 0 )
        {
            settings.units = 1u;
        }
        else
        {
            api_send_error( stream, HTTP_HEADER_400, "units must be c or f" );
            return 0;
        }
    }

    got = rewair_req_get_string( body, (uint32_t)len, "time_mode", text, sizeof( text ) );
    if ( got == 1 )
    {
        if ( strcmp( text, "auto" ) == 0 )
        {
            settings.time_mode = 0u;
        }
        else if ( strcmp( text, "manual" ) == 0 )
        {
            settings.time_mode = 1u;
        }
        else
        {
            api_send_error( stream, HTTP_HEADER_400, "time_mode must be auto or manual" );
            return 0;
        }
    }

    got = rewair_req_get_string( body, (uint32_t)len, "disp_mode", text, sizeof( text ) );
    if ( got == 1 )
    {
        if ( strcmp( text, "score" ) == 0 )
        {
            settings.disp_mode = 0u;
        }
        else if ( strcmp( text, "clock" ) == 0 )
        {
            settings.disp_mode = 1u;
        }
        else if ( strcmp( text, "sensors" ) == 0 )
        {
            settings.disp_mode = 2u;
        }
        else
        {
            api_send_error( stream, HTTP_HEADER_400, "disp_mode must be score, clock, or sensors" );
            return 0;
        }
    }

    /* Validate tz_posix (if present) before any hardware side effects below,
     * so a 400 here never leaves the disp/tz state half-applied. */
    {
        char tz_posix[sizeof( settings.tz_posix )];
        char tz_zone[sizeof( settings.tz_zone )];
        rewair_tz_rule_t rule;
        int got_posix = rewair_req_get_string( body, (uint32_t)len, "tz_posix", tz_posix, sizeof( tz_posix ) );
        int got_zone = rewair_req_get_string( body, (uint32_t)len, "tz_zone", tz_zone, sizeof( tz_zone ) );

        if ( got_posix == 1 && rewair_tz_parse( tz_posix, &rule ) != 0 )
        {
            api_send_error( stream, HTTP_HEADER_400, "bad tz rule" );
            return 0;
        }

        /* All fields validated -- now apply side effects and persist. */
        if ( got == 1 )
        {
            (void)sensor_send_disp_mode( text );
        }

        if ( got_posix == 1 )
        {
            strncpy( settings.tz_posix, tz_posix, sizeof( settings.tz_posix ) - 1u );
            settings.tz_posix[sizeof( settings.tz_posix ) - 1u] = '\0';
            if ( got_zone == 1 )
            {
                strncpy( settings.tz_zone, tz_zone, sizeof( settings.tz_zone ) - 1u );
                settings.tz_zone[sizeof( settings.tz_zone ) - 1u] = '\0';
            }
            sensor_set_tz_rule( &rule );
            if ( wiced_time_get_utc_time( &now ) == WICED_SUCCESS && now != 0u )
            {
                send_time_context( (uint32_t)now );
            }
        }
        else if ( got_zone == 1 )
        {
            strncpy( settings.tz_zone, tz_zone, sizeof( settings.tz_zone ) - 1u );
            settings.tz_zone[sizeof( settings.tz_zone ) - 1u] = '\0';
        }
    }

    rewair_settings_save( &settings );
    rewair_settings_apply_to_state( &settings );
    api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
    return 0;
}

/* ---- POST /api/time ---- */
static int32_t api_time_handler( const char* url, wiced_http_response_stream_t* stream,
                                 void* arg, wiced_http_message_body_t* http_data )
{
    char body[API_BODY_MAX];
    uint32_t epoch = 0u;
    int len;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }
    len = api_read_body( http_data, body, sizeof( body ), stream );
    if ( len < 0 )
    {
        return 0;
    }
    if ( rewair_req_get_u32( body, (uint32_t)len, "epoch", &epoch ) != 1 )
    {
        api_send_error( stream, HTTP_HEADER_400, "epoch required" );
        return 0;
    }
    if ( epoch < 1600000000u )
    {
        api_send_error( stream, HTTP_HEADER_400, "implausible epoch" );
        return 0;
    }

    sensor_apply_manual_time( epoch );
    api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
    return 0;
}

/* ---- POST /api/disp ---- */
static int32_t api_disp_handler( const char* url, wiced_http_response_stream_t* stream,
                                 void* arg, wiced_http_message_body_t* http_data )
{
    char body[API_BODY_MAX];
    char mode[16];
    int len;
    rewair_settings_t settings;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }
    len = api_read_body( http_data, body, sizeof( body ), stream );
    if ( len < 0 )
    {
        return 0;
    }
    if ( rewair_req_get_string( body, (uint32_t)len, "mode", mode, sizeof( mode ) ) != 1 )
    {
        api_send_error( stream, HTTP_HEADER_400, "mode required" );
        return 0;
    }
    if ( sensor_send_disp_mode( mode ) != WICED_SUCCESS )
    {
        api_send_error( stream, HTTP_HEADER_400, "mode must be score, clock, or sensors" );
        return 0;
    }

    rewair_settings_load( &settings );
    if ( strcmp( mode, "score" ) == 0 )
    {
        settings.disp_mode = 0u;
    }
    else if ( strcmp( mode, "clock" ) == 0 )
    {
        settings.disp_mode = 1u;
    }
    else
    {
        settings.disp_mode = 2u;
    }
    rewair_settings_save( &settings );
    rewair_settings_apply_to_state( &settings );

    api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
    return 0;
}

/* ---- POST /api/update ---- */
static int32_t api_update_handler( const char* url, wiced_http_response_stream_t* stream,
                                   void* arg, wiced_http_message_body_t* http_data )
{
    (void)url; (void)arg; (void)http_data;
    api_send_error( stream, "HTTP/1.1 501 Not Implemented", "OTA not supported in this firmware" );
    return 0;
}

/* ---- POST /api/reset ---- */
static int32_t api_reset_handler( const char* url, wiced_http_response_stream_t* stream,
                                  void* arg, wiced_http_message_body_t* http_data )
{
    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }

    wifi_clear_stored_credentials( );
    rewair_settings_reset_defaults( NULL );

    api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
    wiced_http_response_stream_flush( stream );
    wiced_rtos_delay_milliseconds( 300u );   /* let the 204 flush */
    wiced_framework_reboot( );
    return 0;
}

/* ---- page database + start ---- */
static START_OF_HTTP_PAGE_DATABASE( api_pages )
    { "/api/status",    "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_status_handler, NULL } },
    { "/api/scan",      "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_scan_handler, NULL } },
    { "/api/networks",  "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_networks_handler, NULL } },
    { "/api/join",      "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_join_handler, NULL } },
    { "/api/forget",    "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_forget_handler, NULL } },
    { "/api/priority",  "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_priority_handler, NULL } },
    { "/api/settings",  "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_settings_handler, NULL } },
    { "/api/time",      "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_time_handler, NULL } },
    { "/api/disp",      "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_disp_handler, NULL } },
    { "/api/update",    "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_update_handler, NULL } },
    { "/api/reset",     "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_reset_handler, NULL } },
END_OF_HTTP_PAGE_DATABASE( );

wiced_result_t rewair_web_api_start( wiced_interface_t interface )
{
    wiced_result_t result;

    if ( server_started != 0u )
    {
        return WICED_SUCCESS;
    }
    result = wiced_http_server_start( &http_server, 80u, 4u, api_pages, interface,
                                      API_WORKER_STACK );
    if ( result == WICED_SUCCESS )
    {
        server_started = 1u;
        printf( "[web] http server up on port 80\n" );
    }
    else
    {
        printf( "[web] http server start FAILED result=%d\n", (int)result );
    }
    return result;
}
