#pragma once

#include "wiced.h"

#include "http_server.h"
#include "rewair_version.h"
#include "rewair_tz.h"

/* Compile-time dev flag: adds Access-Control-Allow-Origin: * */
/* #define REWAIR_API_CORS_DEV 1 */
#define REWAIR_API_CORS_DEV 1   /* ON during phase 1 bring-up; turn off for release */

wiced_result_t rewair_web_api_start( wiced_interface_t interface );

/* Force-closes the TCP connection backing `stream`. Used by web_ui.c when a
 * response's framing is already unrecoverable (e.g. Content-Length was sent
 * but a later chunk read failed) -- see web_ui.c's web_ui_serve. */
void rewair_web_api_disconnect_stream( wiced_http_response_stream_t* stream );

/* provided by local_bridge.c */
uint32_t                   sensor_scan_blocking( void );
const wiced_scan_result_t* console_scan_cache_get( uint32_t index );
const wiced_scan_result_t* find_best_scan_result_for_ssid( const char* ssid_text );

/* Wi-Fi DCT credential/list management (wifi_dct_saved_count, wifi_list_add,
 * wifi_list_remove, wifi_list_reorder, wifi_list_get, wifi_clear_stored_credentials)
 * now declared in rewair_wifi_dct.h (Phase 2 Task 9, pure move out of
 * local_bridge.c). web_api.c includes rewair_wifi_dct.h directly for these. */

/* sensor/display + manual time control, time-zone rule application, and
 * time-context push now declared in rewair_frames.h (Phase 2 Task 8,
 * pure move out of local_bridge.c): sensor_send_disp_mode,
 * sensor_apply_manual_time, sensor_set_tz_rule, send_time_context.
 * web_api.c includes rewair_frames.h directly for these. */

/* External SPI flash bring-up (Phase 2 Task 1). Both return 0 on success.
 * rewair_sflash_bounds_ok MUST be checked (addr, len) before read_bytes --
 * the driver's 24-bit address silently wraps past the 2 MiB device instead
 * of erroring. */
int rewair_sflash_read_id( uint8_t out_id[3] );
int rewair_sflash_read_bytes( uint32_t addr, uint8_t* out, uint32_t size );
int rewair_sflash_bounds_ok( uint32_t addr, uint32_t len );

/* Lazily initializes the sflash driver handle (idempotent, mutex-guarded).
 * Both rewair_sflash_read_id/read_bytes already call this internally, but
 * web_ui.c calls it explicitly up front so it can log a clear failure before
 * attempting rewair_uifs_init (Phase 2 Task 5). Returns 0 on success. */
int rewair_sflash_ensure_init( void );
