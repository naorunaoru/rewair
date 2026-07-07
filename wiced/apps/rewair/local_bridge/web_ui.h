#pragma once

#include "wiced.h"

#include "http_server.h"

/* Serves the web UI (index.html/app.js/rewair.css) out of the RWFS packed
 * image stored in external SPI flash at REWAIR_UI_IMAGE_BASE. Call
 * web_ui_init() once before wiced_http_server_start() so the page-database
 * entries below have a valid (or intentionally-fallback) rewair_uifs_t to
 * read from.
 *
 * Route registration order matters: the WICED HTTP daemon
 * (http_server_process_url_request) matches WICED_RAW_DYNAMIC_URL_CONTENT
 * entries with strncasecmp(page.url, request_url, strlen(page.url)) -- a
 * PREFIX match, not an exact one -- and takes the FIRST matching entry in
 * table order. "/" is a prefix of every URL on the server, so its entry
 * MUST be registered after every "/api/..." (and "/app.js", "/rewair.css")
 * entry in api_pages[], or it would swallow all of them. */
void web_ui_init( void );

int32_t web_ui_root_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                             wiced_http_message_body_t* http_data );
int32_t web_ui_appjs_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                              wiced_http_message_body_t* http_data );
int32_t web_ui_css_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                            wiced_http_message_body_t* http_data );
