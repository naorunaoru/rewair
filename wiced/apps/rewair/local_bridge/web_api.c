#include "web_api.h"

#include <stdio.h>
#include <string.h>

#include "http_server.h"
#include "rewair_state.h"
#include "rewair_json.h"

#define API_BODY_MAX      1024u
#define API_STATUS_BUF    1024u
#define API_WORKER_STACK  6000u

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

/* ---- GET /api/networks — Task 8 adds DCT list walk; placeholder until then ---- */

/* ---- page database + start ---- */
static START_OF_HTTP_PAGE_DATABASE( api_pages )
    { "/api/status", "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_status_handler, NULL } },
    { "/api/scan",   "application/json", WICED_RAW_DYNAMIC_URL_CONTENT,
      .url_content.dynamic_data = { api_scan_handler, NULL } },
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
