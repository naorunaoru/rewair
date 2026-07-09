#pragma once

#include "wiced.h"

#include "http_server.h"
#include "rewair_version.h"
#include "rewair_tz.h"

/* Compile-time dev flag: adds Access-Control-Allow-Origin: * */
/* #define REWAIR_API_CORS_DEV 1 */
#define REWAIR_API_CORS_DEV 1   /* ON during phase 1 bring-up; turn off for release */

wiced_result_t rewair_web_api_start( wiced_interface_t interface );
wiced_result_t rewair_web_api_stop( void );
uint32_t rewair_web_api_is_started( void );

/* Force-closes the TCP connection backing `stream`. Used by web_ui.c when a
 * response's framing is already unrecoverable (e.g. Content-Length was sent
 * but a later chunk read failed) -- see web_ui.c's web_ui_serve. */
void rewair_web_api_disconnect_stream( wiced_http_response_stream_t* stream );

/* Wi-Fi scan cache access (sensor_scan_blocking, console_scan_cache_get,
 * find_best_scan_result_for_ssid) now declared in rewair_wifi_scan.h
 * (Phase 2 Task 10, pure move out of local_bridge.c). web_api.c includes
 * rewair_wifi_scan.h directly for these. */

/* Wi-Fi DCT credential/list management (wifi_dct_saved_count, wifi_list_add,
 * wifi_list_remove, wifi_list_reorder, wifi_list_get, wifi_clear_stored_credentials)
 * now declared in rewair_wifi_dct.h (Phase 2 Task 9, pure move out of
 * local_bridge.c). web_api.c includes rewair_wifi_dct.h directly for these. */

/* sensor/display + manual time control, time-zone rule application, and
 * time-context push now declared in rewair_frames.h (Phase 2 Task 8,
 * pure move out of local_bridge.c): sensor_send_disp_mode,
 * sensor_apply_manual_time, sensor_set_tz_rule, send_time_context.
 * web_api.c includes rewair_frames.h directly for these. */
