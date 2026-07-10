#include "web_ui.h"

#include <stdio.h>
#include <string.h>

#include "resources.h"
#include "web_api.h"
#include "rewair_uifs.h"

/* Streaming chunk size for stored (gzipped) blob reads. The RWFS resource is
 * memory-mapped inside the F411 application image, so each read is a bounded
 * memcpy from internal flash and never allocates the whole asset in RAM. */
#define WEB_UI_CHUNK_SIZE 1024u
#define WEB_UI_TX_PACE_MS 10u
#define REWAIR_WEBUI_RESOURCE resources___DIR_apps_DIR_rewair_DIR_local_bridge_DIR_webui_bin

static rewair_uifs_t ui_fs;
static uint32_t      ui_fs_valid = 0u;

static int ui_internal_read_cb( uint32_t addr, void* buf, uint32_t len, void* ctx )
{
    const resource_hnd_t* image = &REWAIR_WEBUI_RESOURCE;

    (void)ctx;
    if ( buf == NULL || image->location != RESOURCE_IN_MEMORY || addr > image->size ||
         len > image->size - addr )
    {
        return -1;
    }
    memcpy( buf, image->val.mem.data + addr, len );
    return 0;
}

void web_ui_init( void )
{
    const resource_hnd_t* image = &REWAIR_WEBUI_RESOURCE;

    if ( image->location != RESOURCE_IN_MEMORY || image->size == 0u )
    {
        printf( "[webui] embedded resource missing\n" );
        ui_fs_valid = 0u;
        return;
    }

    if ( rewair_uifs_init( &ui_fs, ui_internal_read_cb, NULL ) != 0 ||
         ui_fs.total_size != image->size )
    {
        ui_fs_valid = 0u;
        printf( "[webui] embedded image invalid\n" );
        return;
    }

    /* Header + table CRC are valid at this point, but rewair_uifs_init()
     * deliberately does not touch blob data (see rewair_uifs.h) -- a
     * truncated or partially-flashed image can still pass that check while
     * serving corrupt bytes. Walk every file in the table (fs.files[] is
     * fully populated by rewair_uifs_init) and verify its blob CRC via
     * rewair_uifs_verify_file before trusting the image for serving. */
    {
        uint32_t i;

        for ( i = 0u; i < ui_fs.file_count; i++ )
        {
            if ( rewair_uifs_verify_file( &ui_fs, &ui_fs.files[i] ) != 0 )
            {
                ui_fs_valid = 0u;
                printf( "[webui] image failed integrity check, file=\"%s\"\n", ui_fs.files[i].path );
                return;
            }
        }
    }

    ui_fs_valid = 1u;
    printf( "[webui] embedded image ok, %lu bytes, %lu files verified\n",
            (unsigned long)ui_fs.total_size, (unsigned long)ui_fs.file_count );
}

uint32_t web_ui_is_valid( void )
{
    return ui_fs_valid;
}

/* Writes a raw response header (status line + headers, no body) for a UI
 * blob response. content_length is the STORED (gzipped) size, since the
 * bytes are streamed as-is with Content-Encoding: gzip -- the client's own
 * HTTP stack inflates them, we never do. */
static int web_ui_write_header( wiced_http_response_stream_t* stream, const char* content_type,
                                uint32_t content_length )
{
    char header[256];
    int  n;

    n = snprintf( header, sizeof( header ),
                  HTTP_HEADER_200 "\r\n"
                  "Content-Type: %s\r\n"
                  "Content-Encoding: gzip\r\n"
                  "Content-Length: %lu\r\n"
#ifdef REWAIR_API_CORS_DEV
                  "Access-Control-Allow-Origin: *\r\n"
#endif
                  "Cache-Control: max-age=3600\r\n"
                  "\r\n",
                  content_type, (unsigned long)content_length );
    if ( n < 0 || (uint32_t)n >= sizeof( header ) )
    {
        return -1;
    }
    return ( wiced_http_response_stream_write( stream, header, (uint32_t)n ) == WICED_SUCCESS ) ? 0 : -1;
}

/* Streams f's stored (gzipped) bytes to the client in WEB_UI_CHUNK_SIZE
 * chunks. Returns 0 on success, nonzero if a chunk read fails partway
 * through (header is already sent by then -- nothing more we can do for
 * this response than stop early). */
static int web_ui_stream_file( const rewair_uifs_file_t* f, wiced_http_response_stream_t* stream )
{
    uint8_t  chunk[WEB_UI_CHUNK_SIZE];
    uint32_t off = 0u;

    while ( off < f->size )
    {
        uint32_t take = ( f->size - off > WEB_UI_CHUNK_SIZE ) ? WEB_UI_CHUNK_SIZE : ( f->size - off );

        if ( rewair_uifs_read( &ui_fs, f, off, chunk, take ) != 0 )
        {
            return -1;
        }
        /* LwIP's WICED send wrapper is non-blocking and has only a small TCP
         * send window. External-flash reads used to pace this loop naturally;
         * internal-flash memcpy can otherwise outrun the window and make
         * wiced_http_response_stream_write() drop a gzip chunk with ERR_MEM.
         * Flush each bounded chunk, check both results, and give the TCP/IP
         * thread time to acknowledge/free queued segments before continuing. */
        if ( wiced_http_response_stream_write( stream, chunk, take ) != WICED_SUCCESS ||
             wiced_http_response_stream_flush( stream ) != WICED_SUCCESS )
        {
            return -1;
        }
        off += take;
        if ( off < f->size )
        {
            wiced_rtos_delay_milliseconds( WEB_UI_TX_PACE_MS );
        }
    }
    return 0;
}

static void web_ui_send_404( wiced_http_response_stream_t* stream )
{
    const char* body = "Not Found";
    char header[128];
    int  n;

    n = snprintf( header, sizeof( header ),
                  HTTP_HEADER_404 "\r\n"
                  "Content-Type: text/plain\r\n"
                  "Content-Length: %lu\r\n"
                  "\r\n",
                  (unsigned long)strlen( body ) );
    if ( n < 0 || (uint32_t)n >= sizeof( header ) )
    {
        return;
    }
    wiced_http_response_stream_write( stream, header, (uint32_t)n );
    wiced_http_response_stream_write( stream, body, (uint32_t)strlen( body ) );
}

/* Set by rewair_net_mode.c: nonzero while the device is serving its AP setup
 * network (Task 3's rewair_net_mode_enter_ap / _exit_ap_to_sta are the sole
 * callers of web_ui_set_captive). volatile: written by the network thread,
 * read by HTTP worker threads servicing web_ui_root_handler. */
static volatile uint8_t ui_captive_mode = 0u;

void web_ui_set_captive( uint8_t on )
{
    ui_captive_mode = on;
}

static void web_ui_send_302_portal( wiced_http_response_stream_t* stream )
{
    static const char redirect[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: http://192.168.0.1/\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    wiced_http_response_stream_write( stream, redirect, (uint32_t)( sizeof( redirect ) - 1u ) );
    rewair_web_api_disconnect_stream( stream );
}

/* Built-in fallback for a corrupt firmware resource. Deliberately not gzipped;
 * a valid release image should never reach this path. */
static const char web_ui_fallback_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>Rewair</title></head><body>"
    "<h1>UI image invalid</h1>"
    "<p>The firmware's embedded web UI failed its integrity check.</p>"
    "<p>Reinstall a known-good Rewair firmware image.</p>"
    "</body></html>";

static void web_ui_send_fallback_root( wiced_http_response_stream_t* stream )
{
    char header[192];
    int  n;

    n = snprintf( header, sizeof( header ),
                  HTTP_HEADER_200 "\r\n"
                  "Content-Type: text/html\r\n"
                  "Content-Length: %lu\r\n"
                  "Cache-Control: no-store\r\n"
                  "\r\n",
                  (unsigned long)( sizeof( web_ui_fallback_html ) - 1u ) );
    if ( n < 0 || (uint32_t)n >= sizeof( header ) )
    {
        return;
    }
    wiced_http_response_stream_write( stream, header, (uint32_t)n );
    wiced_http_response_stream_write( stream, web_ui_fallback_html,
                                      (uint32_t)( sizeof( web_ui_fallback_html ) - 1u ) );
}

static int32_t web_ui_serve( const char* path, const char* content_type,
                             wiced_http_response_stream_t* stream )
{
    rewair_uifs_file_t f;

    if ( !ui_fs_valid || rewair_uifs_find( &ui_fs, path, &f ) != 0 )
    {
        return -1;
    }
    if ( web_ui_write_header( stream, content_type, f.size ) != 0 )
    {
        return -1;
    }
    if ( web_ui_stream_file( &f, stream ) != 0 )
    {
        /* The header already promised f.size bytes via Content-Length, but a
         * chunk read failed partway through the body -- there is no way to
         * retract or correct that promise on this response. Leaving the
         * connection open would make the client (and, on keep-alive, the
         * NEXT request on this same socket) hang waiting for bytes that will
         * never arrive. Forcing the connection closed is the only clean way
         * out: the client sees a reset/short read instead of a silent hang. */
        printf( "[webui] stream read failed mid-response, closing connection\n" );
        rewair_web_api_disconnect_stream( stream );
    }
    return 0;
}

int32_t web_ui_root_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                             wiced_http_message_body_t* http_data )
{
    (void)arg;

    /* "/" is registered as a WICED_RAW_DYNAMIC_URL_CONTENT entry, which the
     * daemon matches by PREFIX (see the comment on api_pages[] in
     * web_api.c) -- so this generator is invoked for every URL not claimed
     * by an earlier, more specific entry (e.g. "/nope.js", "/foo/bar"), not
     * just the literal root. Reject anything that isn't exactly "/" or
     * "/?query..." with 404 instead of serving the index page for it -- or,
     * in AP setup captive mode, with a 302 to the setup gateway so phone
     * captive-portal probes open the setup UI instead of reporting an
     * error. */
    if ( url == NULL || url[0] != '/' || ( url[1] != '\0' && url[1] != '?' ) )
    {
        if ( ui_captive_mode != 0u )
        {
            web_ui_send_302_portal( stream );
        }
        else
        {
            web_ui_send_404( stream );
        }
        return 0;
    }
    if ( http_data != NULL && http_data->request_type != WICED_HTTP_GET_REQUEST )
    {
        web_ui_send_404( stream );
        return 0;
    }
    if ( web_ui_serve( "/", "text/html", stream ) != 0 )
    {
        web_ui_send_fallback_root( stream );
    }
    return 0;
}

int32_t web_ui_appjs_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                              wiced_http_message_body_t* http_data )
{
    (void)url; (void)arg;
    if ( http_data != NULL && http_data->request_type != WICED_HTTP_GET_REQUEST )
    {
        web_ui_send_404( stream );
        return 0;
    }
    if ( web_ui_serve( "/app.js", "application/javascript", stream ) != 0 )
    {
        web_ui_send_404( stream );
    }
    return 0;
}

int32_t web_ui_css_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                            wiced_http_message_body_t* http_data )
{
    (void)url; (void)arg;
    if ( http_data != NULL && http_data->request_type != WICED_HTTP_GET_REQUEST )
    {
        web_ui_send_404( stream );
        return 0;
    }
    if ( web_ui_serve( "/rewair.css", "text/css", stream ) != 0 )
    {
        web_ui_send_404( stream );
    }
    return 0;
}
