#include "web_api.h"

#include <stdio.h>
#include <string.h>

#include "http_server.h"
#include "rewair_state.h"
#include "rewair_json.h"
#include "rewair_settings.h"
#include "rewair_tz.h"
#include "rewair_frames.h"
#include "rewair_wifi_dct.h"
#include "rewair_wifi_scan.h"
#include "rewair_net_mode.h"
#include "rewair_ota.h"
#include "rewair_sflash.h"
#include "web_ui.h"

#define API_BODY_MAX      1024u
#define API_STATUS_BUF    1024u
#define API_WORKER_STACK  6000u
#define API_MAX_SAVED_NETWORKS CONFIG_AP_LIST_SIZE

static wiced_http_server_t http_server;
static uint32_t            server_started = 0u;
static uint8_t             server_init_done = 0u;

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

    if ( body_len == 0u )
    {
        /* Empty response (e.g. 204 No Content): emit NO Content-Type. Advertising
         * "application/json" with a zero-length body makes strict clients try to
         * parse nothing (fetch's r.json() throws on empty input). */
        n = snprintf( header, sizeof( header ),
                      "%s\r\n"
                      "Content-Length: 0\r\n"
                      API_CORS_HEADER
                      "Cache-Control: no-store\r\n"
                      "\r\n",
                      status_line );
    }
    else
    {
        n = snprintf( header, sizeof( header ),
                      "%s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %lu\r\n"
                      API_CORS_HEADER
                      "Cache-Control: no-store\r\n"
                      "\r\n",
                      status_line, content_type, (unsigned long)body_len );
    }
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

/* ---- request body extraction ---- */
/* Timeout (ms) to wait for continuation body segments after the header packet.
 * Browsers send the body immediately after the headers, so the wait is short in
 * practice; the cap only bounds a broken/malicious client that sends headers and
 * then stalls (it would block the daemon's single worker thread until then). */
#define API_BODY_RX_TIMEOUT_MS 1500u

/* Reads the full POST body into `out` (NUL-terminated), returning its length or
 * -1 (with an error response already sent).
 *
 * The WICED daemon only parses the packet containing the request headers, and
 * exposes just the body bytes that happened to arrive in that same TCP segment
 * (message_data_length), plus how many are still promised by Content-Length
 * (total_message_data_remaining). Browsers routinely put the body in a SEPARATE
 * segment from the headers, so the header packet often carries zero body bytes.
 * We therefore pull the remainder straight off the socket here.
 *
 * Concurrency: url generators run on the daemon's single worker thread inside
 * http_server_deferred_receive_callback, which is the only place that receives
 * on this socket. So calling wiced_tcp_receive() here cannot race the daemon;
 * any deferred-receive events it already queued for these packets will later
 * fire, receive nothing (WICED_NO_WAIT), and no-op. */
static int api_read_body( wiced_http_message_body_t* http_data, char* out, uint32_t out_size,
                          wiced_http_response_stream_t* stream )
{
    uint32_t have;
    uint32_t total;
    uint32_t got;

    if ( http_data == NULL )
    {
        api_send_error( stream, HTTP_HEADER_400, "missing body" );
        return -1;
    }

    have  = (uint32_t)http_data->message_data_length;
    total = have + (uint32_t)http_data->total_message_data_remaining;

    if ( total == 0u )
    {
        api_send_error( stream, HTTP_HEADER_400, "missing body" );
        return -1;
    }
    if ( total >= out_size )
    {
        api_send_error( stream, "HTTP/1.1 413 Payload Too Large", "body too large" );
        return -1;
    }

    if ( have > 0u && http_data->data != NULL )
    {
        memcpy( out, http_data->data, have );
    }
    got = have;

    while ( got < total )
    {
        wiced_packet_t* packet = NULL;
        uint8_t*        packet_data;
        uint16_t        fragment_length;
        uint16_t        available_length;
        uint32_t        copy_length;

        if ( wiced_tcp_receive( stream->tcp_stream.socket, &packet, API_BODY_RX_TIMEOUT_MS ) != WICED_SUCCESS ||
             packet == NULL )
        {
            api_send_error( stream, HTTP_HEADER_400, "incomplete body" );
            return -1;
        }
        if ( wiced_packet_get_data( packet, 0, &packet_data, &fragment_length, &available_length ) != WICED_SUCCESS )
        {
            wiced_packet_delete( packet );
            api_send_error( stream, HTTP_HEADER_400, "incomplete body" );
            return -1;
        }

        copy_length = (uint32_t)fragment_length;
        if ( got + copy_length > total )
        {
            copy_length = total - got;   /* never copy past the promised length */
        }
        memcpy( out + got, packet_data, copy_length );
        got += copy_length;
        wiced_packet_delete( packet );
    }

    out[total] = '\0';
    return (int)total;
}

/* ---- connected_s: association-start-uptime -> duration ----
 * rewair_state_snapshot() reports st.connected_s as the uptime (ms/1000) at
 * which the station associated. Every serializer that hands a snapshot to a
 * client must convert that into an elapsed-seconds duration before calling
 * rewair_json_status(), or the client sees a constant "moment of connection"
 * value instead of a growing duration. This helper is the single place that
 * conversion happens -- api_status_handler, the SSE broadcaster, and
 * api_events_handler's initial frame all call it so the logic can't drift
 * between call sites. */
static void rewair_apply_connected_duration( rewair_status_t* st )
{
    wiced_time_t now_ms = 0;

    wiced_time_get_time( &now_ms );
    if ( st->wifi_mode == 0u && st->connected_s != 0u )
    {
        st->connected_s = (uint32_t)now_ms / 1000u - st->connected_s;
    }
}

/* ---- GET /api/status ---- */
static int32_t api_status_handler( const char* url, wiced_http_response_stream_t* stream,
                                   void* arg, wiced_http_message_body_t* http_data )
{
    static char buf[API_STATUS_BUF];
    rewair_status_t st;
    int len;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_GET_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "GET only" );
        return 0;
    }

    rewair_state_snapshot( &st );
    rewair_apply_connected_duration( &st );
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

    if ( rewair_net_mode_current( ) == NET_MODE_AP_SETUP ||
         rewair_net_mode_current( ) == NET_MODE_AP_FALLBACK )
    {
        /* AP-mode join: store credentials (wifi_list_store -- no join probe, so
         * the STA interface never comes up while the AP is running and the HTTP
         * worker never blocks on a join), respond SUCCESS immediately, then flag
         * the network thread to tear down the AP and autojoin from DCT on its next
         * tick. We never touch radio/web-server state from this HTTP worker context
         * (web_api single-control-thread contract): the tick owns all of that.
         * The explicit flush below pushes the 204 onto the wire BEFORE the switch
         * is armed (api_send only buffers; the daemon's deferred flush could lose
         * a scheduling race against the tick's teardown) -- same response-then-act
         * pattern as the reset handler. Same success shape as the STA arm below. */
        if ( wifi_list_store( ssid, pass ) != WICED_SUCCESS )
        {
            api_send_error( stream, HTTP_HEADER_400, "store failed" );
            return 0;
        }
        api_send( stream, HTTP_HEADER_204, "application/json", "", 0u );
        wiced_http_response_stream_flush( stream );
        rewair_net_mode_request_sta( );
        printf( "[net-mode] join stored from ap; switching to sta\n" );
        return 0;
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

static int api_query_value( const char* url, const char* name, const char** value,
                            uint32_t* value_len )
{
    const char* p = strchr( url, '?' );
    uint32_t name_len = (uint32_t)strlen( name );

    if ( p == NULL )
    {
        return 0;
    }
    p++;
    while ( *p != '\0' )
    {
        const char* end = strchr( p, '&' );
        const char* eq = strchr( p, '=' );

        if ( end == NULL )
        {
            end = p + strlen( p );
        }
        if ( eq != NULL && eq < end && (uint32_t)( eq - p ) == name_len &&
             strncmp( p, name, name_len ) == 0 )
        {
            *value = eq + 1;
            *value_len = (uint32_t)( end - ( eq + 1 ) );
            return 1;
        }
        if ( *end == '\0' )
        {
            break;
        }
        p = end + 1;
    }
    return 0;
}

static int api_parse_u32( const char* text, uint32_t len, uint32_t base, uint32_t* out )
{
    uint32_t value = 0u;
    uint32_t i;

    if ( len == 0u || ( base != 10u && base != 16u ) )
    {
        return 0;
    }
    for ( i = 0u; i < len; i++ )
    {
        uint32_t digit;

        if ( text[i] >= '0' && text[i] <= '9' )
        {
            digit = (uint32_t)( text[i] - '0' );
        }
        else if ( base == 16u && text[i] >= 'a' && text[i] <= 'f' )
        {
            digit = (uint32_t)( text[i] - 'a' + 10 );
        }
        else if ( base == 16u && text[i] >= 'A' && text[i] <= 'F' )
        {
            digit = (uint32_t)( text[i] - 'A' + 10 );
        }
        else
        {
            return 0;
        }
        if ( digit >= base || value > ( 0xffffffffu - digit ) / base )
        {
            return 0;
        }
        value = value * base + digit;
    }
    *out = value;
    return 1;
}

static int api_update_receive_chunk( wiced_http_message_body_t* http_data,
                                     wiced_http_response_stream_t* stream, uint32_t offset )
{
    uint32_t have;
    uint32_t total;
    uint32_t written = 0u;

    if ( http_data == NULL )
    {
        api_send_error( stream, HTTP_HEADER_400, "missing update chunk" );
        return -1;
    }
    have = (uint32_t)http_data->message_data_length;
    total = have + (uint32_t)http_data->total_message_data_remaining;
    if ( total == 0u || total > REWAIR_OTA_UPLOAD_CHUNK_MAX )
    {
        api_send_error( stream, "HTTP/1.1 413 Payload Too Large", "invalid update chunk size" );
        return -1;
    }
    if ( have != 0u )
    {
        if ( http_data->data == NULL ||
             rewair_ota_upload_chunk( offset, http_data->data, have ) != WICED_SUCCESS )
        {
            api_send_error( stream, HTTP_HEADER_400, "unexpected update chunk" );
            return -1;
        }
        written = have;
    }

    while ( written < total )
    {
        wiced_packet_t* packet = NULL;
        uint8_t* packet_data;
        uint16_t fragment_length;
        uint16_t available_length;
        uint32_t take;

        if ( wiced_tcp_receive( stream->tcp_stream.socket, &packet, 5000u ) != WICED_SUCCESS ||
             packet == NULL ||
             wiced_packet_get_data( packet, 0, &packet_data, &fragment_length,
                                    &available_length ) != WICED_SUCCESS )
        {
            if ( packet != NULL )
            {
                wiced_packet_delete( packet );
            }
            api_send_error( stream, HTTP_HEADER_400, "incomplete update chunk" );
            return -1;
        }
        take = (uint32_t)fragment_length;
        if ( take > total - written )
        {
            take = total - written;
        }
        if ( take == 0u )
        {
            wiced_packet_delete( packet );
            api_send_error( stream, HTTP_HEADER_400, "empty update fragment" );
            return -1;
        }
        if ( rewair_ota_upload_chunk( offset + written, packet_data, take ) != WICED_SUCCESS )
        {
            wiced_packet_delete( packet );
            api_send_error( stream, HTTP_HEADER_500, "failed to stage update chunk" );
            return -1;
        }
        written += take;
        wiced_packet_delete( packet );
    }
    return (int)written;
}

/* ---- POST /api/update ----
 * The stock daemon stores Content-Length in uint16_t, so a firmware image is
 * transferred as sequential <=16 KiB requests. The portal still exposes one
 * update operation: op=begin invalidates old staging, offset=N appends bytes,
 * and op=commit verifies flash and arms the bootloader. */
static int32_t api_update_handler( const char* url, wiced_http_response_stream_t* stream,
                                   void* arg, wiced_http_message_body_t* http_data )
{
    const char* value;
    uint32_t value_len;

    (void)arg;
    if ( strncmp( url, "/api/update", 11u ) != 0 ||
         ( url[11] != '\0' && url[11] != '?' ) )
    {
        api_send_error( stream, "HTTP/1.1 404 Not Found", "not found" );
        return 0;
    }
    if ( !method_is( http_data, WICED_HTTP_POST_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "POST only" );
        return 0;
    }

    if ( api_query_value( url, "op", &value, &value_len ) != 0 )
    {
        if ( value_len == 5u && strncmp( value, "begin", 5u ) == 0 )
        {
            const char* size_text;
            const char* crc_text;
            uint32_t size_len;
            uint32_t crc_len;
            uint32_t size;
            uint32_t crc;
            wiced_result_t result;

            if ( api_query_value( url, "size", &size_text, &size_len ) == 0 ||
                 api_query_value( url, "crc", &crc_text, &crc_len ) == 0 ||
                 api_parse_u32( size_text, size_len, 10u, &size ) == 0 ||
                 api_parse_u32( crc_text, crc_len, 16u, &crc ) == 0 )
            {
                api_send_error( stream, HTTP_HEADER_400, "size and crc required" );
                return 0;
            }
            result = rewair_ota_upload_begin( size, crc );
            if ( result == WICED_ALREADY_INITIALIZED )
            {
                api_send_error( stream, "HTTP/1.1 409 Conflict", "update already pending" );
            }
            else if ( result == WICED_BADARG )
            {
                api_send_error( stream, HTTP_HEADER_400, "invalid firmware size" );
            }
            else if ( result != WICED_SUCCESS )
            {
                api_send_error( stream, HTTP_HEADER_500, "failed to prepare staging flash" );
            }
            else
            {
                const char* body = "{\"status\":\"ready\"}";
                api_send( stream, HTTP_HEADER_200, "application/json", body,
                          (uint32_t)strlen( body ) );
            }
            return 0;
        }
        if ( value_len == 6u && strncmp( value, "commit", 6u ) == 0 )
        {
            const char* body = "{\"status\":\"staged\",\"reboot\":true}";

            if ( rewair_ota_upload_commit( ) != WICED_SUCCESS )
            {
                api_send_error( stream, HTTP_HEADER_400, "firmware verification failed" );
                return 0;
            }
            api_send( stream, HTTP_HEADER_200, "application/json", body,
                      (uint32_t)strlen( body ) );
            wiced_http_response_stream_flush( stream );
            wiced_rtos_delay_milliseconds( 500u );
            wiced_framework_reboot( );
            return 0;
        }
        api_send_error( stream, HTTP_HEADER_400, "unknown update operation" );
        return 0;
    }

    if ( api_query_value( url, "offset", &value, &value_len ) != 0 )
    {
        uint32_t offset;
        int received;
        char body[64];
        int n;

        if ( api_parse_u32( value, value_len, 10u, &offset ) == 0 )
        {
            api_send_error( stream, HTTP_HEADER_400, "invalid update offset" );
            return 0;
        }
        received = api_update_receive_chunk( http_data, stream, offset );
        if ( received < 0 )
        {
            return 0;
        }
        n = snprintf( body, sizeof( body ), "{\"received\":%lu}",
                      (unsigned long)rewair_ota_upload_received( ) );
        api_send( stream, HTTP_HEADER_200, "application/json", body,
                  n > 0 ? (uint32_t)n : 0u );
        return 0;
    }

    api_send_error( stream, HTTP_HEADER_400, "update operation required" );
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

#ifdef REWAIR_API_CORS_DEV
/* ---- GET /api/debug/sflash?addr=<hex>&len=<n<=256> ----
 * Dev-only external-SPI-flash readback route (Phase 2 Task 1 gate + a
 * building block for Task 2's write/readback checks). Compiled only when
 * REWAIR_API_CORS_DEV is defined (dev builds, on for the whole of phase 2).
 *
 * For WICED_RAW_DYNAMIC_URL_CONTENT routes, http_server_process_url_request()
 * passes the *full* original url string (path + "?query") to the generator
 * unmodified -- unlike WICED_DYNAMIC_URL_CONTENT, which splits query params
 * off before calling. So the query string has to be located and parsed here. */
#define API_DEBUG_SFLASH_MAX_LEN 256u

static int debug_hex_nibble( char c, uint8_t* value )
{
    if ( c >= '0' && c <= '9' ) { *value = (uint8_t)( c - '0' ); return 1; }
    if ( c >= 'a' && c <= 'f' ) { *value = (uint8_t)( c - 'a' + 10 ); return 1; }
    if ( c >= 'A' && c <= 'F' ) { *value = (uint8_t)( c - 'A' + 10 ); return 1; }
    return 0;
}

static int debug_parse_hex_u32( const char* text, uint32_t len, uint32_t* out )
{
    uint32_t value = 0u;
    uint32_t i;

    if ( len == 0u || len > 8u )
    {
        return 0;
    }
    for ( i = 0u; i < len; i++ )
    {
        uint8_t nibble = 0u;
        if ( debug_hex_nibble( text[i], &nibble ) == 0 )
        {
            return 0;
        }
        value = ( value << 4 ) | nibble;
    }
    *out = value;
    return 1;
}

static int debug_parse_dec_u32( const char* text, uint32_t len, uint32_t* out )
{
    uint32_t value = 0u;
    uint32_t i;

    if ( len == 0u )
    {
        return 0;
    }
    for ( i = 0u; i < len; i++ )
    {
        if ( text[i] < '0' || text[i] > '9' )
        {
            return 0;
        }
        value = ( value * 10u ) + (uint32_t)( text[i] - '0' );
    }
    *out = value;
    return 1;
}

/* Finds "key=" in the query string (the part of url after '?') and copies
 * its value (up to the next '&' or end of string) into out. Returns the
 * value length, or -1 if the key is absent. Truncates silently if the value
 * doesn't fit in out_size (callers bounds-check the numeric result anyway). */
static int debug_query_get( const char* query, const char* key, char* out, uint32_t out_size )
{
    uint32_t key_len = (uint32_t)strlen( key );
    const char* p = query;

    while ( *p != '\0' )
    {
        if ( strncmp( p, key, key_len ) == 0 && p[key_len] == '=' )
        {
            const char* v = p + key_len + 1u;
            uint32_t n = 0u;
            while ( v[n] != '\0' && v[n] != '&' )
            {
                n++;
            }
            if ( n >= out_size )
            {
                n = out_size - 1u;
            }
            memcpy( out, v, n );
            out[n] = '\0';
            return (int)n;
        }
        /* skip to next "key=value" pair */
        while ( *p != '\0' && *p != '&' )
        {
            p++;
        }
        if ( *p == '&' )
        {
            p++;
        }
    }
    return -1;
}

static int32_t api_debug_sflash_handler( const char* url, wiced_http_response_stream_t* stream,
                                         void* arg, wiced_http_message_body_t* http_data )
{
    const char* query;
    char addr_text[16];
    char len_text[16];
    uint32_t addr = 0u;
    uint32_t len = 0u;
    uint8_t id[3];
    uint8_t data[API_DEBUG_SFLASH_MAX_LEN];
    static char body[ 32u + 2u * API_DEBUG_SFLASH_MAX_LEN + 64u ];
    int n;
    uint32_t i;

    (void)arg;
    if ( !method_is( http_data, WICED_HTTP_GET_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "GET only" );
        return 0;
    }

    query = strchr( url, '?' );
    query = ( query != NULL ) ? query + 1 : "";

    if ( debug_query_get( query, "addr", addr_text, sizeof( addr_text ) ) < 0 ||
         debug_parse_hex_u32( addr_text, (uint32_t)strlen( addr_text ), &addr ) == 0 )
    {
        api_send_error( stream, HTTP_HEADER_400, "addr required (hex)" );
        return 0;
    }
    if ( debug_query_get( query, "len", len_text, sizeof( len_text ) ) < 0 ||
         debug_parse_dec_u32( len_text, (uint32_t)strlen( len_text ), &len ) == 0 ||
         len == 0u || len > API_DEBUG_SFLASH_MAX_LEN )
    {
        api_send_error( stream, HTTP_HEADER_400, "len required, 1..256" );
        return 0;
    }
    if ( rewair_sflash_bounds_ok( addr, len ) == 0 )
    {
        api_send_error( stream, HTTP_HEADER_400, "beyond device" );
        return 0;
    }

    if ( rewair_sflash_read_id( id ) != 0 )
    {
        api_send_error( stream, HTTP_HEADER_500, "sflash id read failed" );
        return 0;
    }
    if ( rewair_sflash_read_bytes( addr, data, len ) != 0 )
    {
        api_send_error( stream, HTTP_HEADER_500, "sflash read failed" );
        return 0;
    }

    n = snprintf( body, sizeof( body ), "{\"jedec\":\"%02x%02x%02x\",\"addr\":\"%lx\",\"data\":\"",
                  id[0], id[1], id[2], (unsigned long)addr );
    if ( n < 0 || (uint32_t)n >= sizeof( body ) )
    {
        api_send_error( stream, HTTP_HEADER_500, "buffer overflow" );
        return 0;
    }
    for ( i = 0u; i < len && (uint32_t)n + 2u < sizeof( body ); i++ )
    {
        n += snprintf( body + n, sizeof( body ) - (uint32_t)n, "%02x", data[i] );
    }
    if ( (uint32_t)n + 2u >= sizeof( body ) )
    {
        api_send_error( stream, HTTP_HEADER_500, "buffer overflow" );
        return 0;
    }
    body[n++] = '"';
    body[n++] = '}';

    api_send( stream, HTTP_HEADER_200, "application/json", body, (uint32_t)n );
    return 0;
}
#endif /* REWAIR_API_CORS_DEV */

/* ---- GET /api/events (SSE) ----
 *
 * The stock WICED HTTP daemon exposes no application disconnect callback and
 * recycles its fixed socket objects after a peer closes. Retaining a socket
 * pointer for a conventional long-lived SSE broadcaster can therefore turn
 * it into a pointer to an unrelated, newer request. A later heartbeat may
 * write to or disconnect that new request and destabilise the networking
 * worker.
 *
 * Serve one complete event and close instead. EventSource reconnects after
 * the advertised retry interval, preserving live updates without keeping a
 * socket pointer beyond this handler's lifetime. */
static int32_t api_events_handler( const char* url, wiced_http_response_stream_t* stream,
                                   void* arg, wiced_http_message_body_t* http_data )
{
    static char json[API_STATUS_BUF];
    static char frame[API_STATUS_BUF + 32u];
    rewair_status_t st;
    int len;
    int n;

    (void)url; (void)arg;
    if ( !method_is( http_data, WICED_HTTP_GET_REQUEST ) )
    {
        api_send_error( stream, HTTP_HEADER_405, "GET only" );
        return 0;
    }

    rewair_state_snapshot( &st );
    rewair_apply_connected_duration( &st );
    len = rewair_json_status( &st, json, sizeof( json ) );
    if ( len < 0 )
    {
        api_send_error( stream, HTTP_HEADER_500, "status unavailable" );
        return 0;
    }

    n = snprintf( frame, sizeof( frame ), "data: %s\nretry: 2500\n\n", json );
    if ( n < 0 || n >= (int)sizeof( frame ) )
    {
        api_send_error( stream, HTTP_HEADER_500, "status unavailable" );
        return 0;
    }

    api_send( stream, HTTP_HEADER_200, "text/event-stream", frame, (uint32_t)n );
    wiced_http_response_stream_flush( stream );
    wiced_http_server_queue_disconnect_request( &http_server, stream->tcp_stream.socket );
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
    /* GET-only route: single entry is enough since GET requests carry no
     * Content-Type, so mime matching passes via MIME_TYPE_ALL regardless of
     * this entry's declared "text/event-stream" mime. */
    { "/api/events",    "text/event-stream", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_events_handler, NULL } },
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
#ifdef REWAIR_API_CORS_DEV
    { "/api/debug/sflash", "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_debug_sflash_handler, NULL } },
#endif
    /* Duplicate POST entries for "text/plain": the web UI POSTs JSON bodies
     * with Content-Type: text/plain to avoid a CORS preflight, and the WICED
     * daemon only matches a request to a page entry whose mime equals the
     * request's Content-Type (or the request has none), scanning subsequent
     * entries with the same URL on a mime mismatch. */
    { "/api/join",      "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_join_handler, NULL } },
    { "/api/forget",    "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_forget_handler, NULL } },
    { "/api/priority",  "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_priority_handler, NULL } },
    { "/api/settings",  "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_settings_handler, NULL } },
    { "/api/time",      "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_time_handler, NULL } },
    { "/api/disp",      "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_disp_handler, NULL } },
    { "/api/update",    "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_update_handler, NULL } },
    { "/api/update",    "application/octet-stream", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_update_handler, NULL } },
    { "/api/reset",     "text/plain", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_reset_handler, NULL } },
    /* ---- Web UI (Phase 2 Task 5), served from the RWFS image in external
     * sflash by web_ui.c. IMPORTANT: the WICED daemon matches
     * WICED_RAW_DYNAMIC_URL_CONTENT entries by strncasecmp() against the
     * request URL truncated to strlen(entry.url) -- i.e. a PREFIX match --
     * and stops at the first match in table order. "/" is a prefix of every
     * URL (including every "/api/..." route above), so its entry MUST be the
     * LAST one in this table or it would swallow all the API routes.
     * "/app.js" and "/rewair.css" are not prefixes of anything else here, so
     * their position relative to "/api/..." doesn't matter, but they must
     * still come before "/". */
    { "/app.js",        "application/javascript", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { web_ui_appjs_handler, NULL } },
    { "/rewair.css",    "text/css", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { web_ui_css_handler, NULL } },
    { "/",              "text/html", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { web_ui_root_handler, NULL } },
END_OF_HTTP_PAGE_DATABASE( );

/* Lets web_ui.c force-close a stream's socket without needing direct access
 * to the file-scope http_server instance above (mirrors the explicit
 * disconnect pattern already used by api_scan_handler/api_networks_handler
 * in this file for their own unrecoverable-framing cases). */
void rewair_web_api_disconnect_stream( wiced_http_response_stream_t* stream )
{
    wiced_http_server_queue_disconnect_request( &http_server, stream->tcp_stream.socket );
}

wiced_result_t rewair_web_api_start( wiced_interface_t interface )
{
    wiced_result_t result;

    if ( server_started != 0u )
    {
        return WICED_SUCCESS;
    }

    if ( server_init_done == 0u )
    {
        web_ui_init( );
        server_init_done = 1u;
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

/* Contract: rewair_web_api_start()/rewair_web_api_stop() must only ever be
 * called from the network-mode control thread; the SDK server lifecycle is
 * not safe to start and stop concurrently. */
wiced_result_t rewair_web_api_stop( void )
{
    if ( server_started == 0u )
    {
        return WICED_SUCCESS;
    }

    server_started = 0u;
    printf( "[web] http server stopping\n" );
    return wiced_http_server_stop( &http_server );
}

uint32_t rewair_web_api_is_started( void )
{
    return server_started;
}
