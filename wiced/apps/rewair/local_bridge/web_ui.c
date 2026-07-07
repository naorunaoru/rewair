#include "web_ui.h"

#include <stdio.h>
#include <string.h>

#include "web_api.h"
#include "rewair_uifs.h"

/* Start of the 256 KB UI region (0x1C0000-0x1FFFFF) reserved on the 2 MiB
 * external SPI flash -- see Global Constraints / scripts/flash_webui.zsh. */
#define REWAIR_UI_IMAGE_BASE 0x1C0000u

/* Streaming chunk size for blob reads (brief: <=1 KB). Each chunk is one
 * rewair_uifs_read() call, which bottoms out in one rewair_sflash_read_bytes()
 * call -- so the sflash mutex is only ever held for a single ~1 KB SPI
 * transfer, never across the whole response. */
#define WEB_UI_CHUNK_SIZE 1024u

static rewair_uifs_t ui_fs;
static uint32_t      ui_fs_valid = 0u;

static int ui_sflash_read_cb( uint32_t addr, void* buf, uint32_t len, void* ctx )
{
    (void)ctx;
    if ( rewair_sflash_bounds_ok( REWAIR_UI_IMAGE_BASE + addr, len ) == 0 )
    {
        return -1;
    }
    return rewair_sflash_read_bytes( REWAIR_UI_IMAGE_BASE + addr, (uint8_t*)buf, len );
}

void web_ui_init( void )
{
    if ( rewair_sflash_ensure_init( ) != 0 )
    {
        printf( "[webui] sflash init failed; no valid image\n" );
        ui_fs_valid = 0u;
        return;
    }

    if ( rewair_uifs_init( &ui_fs, ui_sflash_read_cb, NULL ) != 0 )
    {
        ui_fs_valid = 0u;
        printf( "[webui] no valid image\n" );
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
    printf( "[webui] image ok, %lu files verified\n", (unsigned long)ui_fs.file_count );
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
    wiced_http_response_stream_write( stream, header, (uint32_t)n );
    return 0;
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
        wiced_http_response_stream_write( stream, chunk, take );
        off += take;
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

/* Built-in ~300-byte fallback page served for "/" when no valid UI image is
 * present in sflash (e.g. a fresh board before scripts/flash_webui.zsh has
 * ever run). Deliberately NOT gzipped -- Content-Encoding is omitted. */
static const char web_ui_fallback_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>Rewair</title></head><body>"
    "<h1>UI image missing</h1>"
    "<p>No valid web UI image was found in external flash.</p>"
    "<p>Run <code>scripts/flash_webui.zsh</code> from the repo to flash it, "
    "then reload this page.</p>"
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
