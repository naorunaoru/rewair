#pragma once

#include "wiced.h"

#include "http_server.h"

/* Serves the web UI (index.html/app.js/rewair.css) out of the RWFS packed
 * image linked into the F411 application as an in-memory resource. Call
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
uint32_t web_ui_is_valid( void );

/* Toggles captive-portal mode for the root handler. When on, every path that
 * does not exactly match "/" (or "/?...") is answered with a 302 redirect to
 * the AP setup gateway instead of a 404, so phone captive-portal probes
 * (/generate_204, /hotspot-detect.html, ...) open the setup UI. Set by
 * rewair_net_mode.c around the AP/STA transition (Task 3/4). */
void web_ui_set_captive( uint8_t on );

int32_t web_ui_root_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                             wiced_http_message_body_t* http_data );
int32_t web_ui_appjs_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                              wiced_http_message_body_t* http_data );
int32_t web_ui_css_handler( const char* url, wiced_http_response_stream_t* stream, void* arg,
                            wiced_http_message_body_t* http_data );
