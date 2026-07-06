#pragma once

#include "wiced.h"

#include "rewair_version.h"
#include "rewair_tz.h"

/* Compile-time dev flag: adds Access-Control-Allow-Origin: * */
/* #define REWAIR_API_CORS_DEV 1 */
#define REWAIR_API_CORS_DEV 1   /* ON during phase 1 bring-up; turn off for release */

wiced_result_t rewair_web_api_start( wiced_interface_t interface );

/* provided by local_bridge.c */
uint32_t                   sensor_scan_blocking( void );
const wiced_scan_result_t* console_scan_cache_get( uint32_t index );
const wiced_scan_result_t* find_best_scan_result_for_ssid( const char* ssid_text );
uint32_t                   wifi_dct_saved_count( void );

/* DCT multi-network list management (Task 8) */
wiced_result_t wifi_list_add( const char* ssid, const char* pass );
wiced_result_t wifi_list_remove( const char* ssid );
wiced_result_t wifi_list_reorder( char order[][33], uint32_t count );
int            wifi_list_get( uint32_t index, char ssid_out[33], wiced_security_t* sec_out,
                              int* connected_out );
wiced_result_t wifi_clear_stored_credentials( void );

/* sensor/display + manual time control (Task 8) */
wiced_result_t sensor_send_disp_mode( const char* mode );
void           sensor_apply_manual_time( uint32_t epoch );

/* time-zone rule application + time-context push (Task 6) */
void sensor_set_tz_rule( const rewair_tz_rule_t* rule );
void send_time_context( uint32_t utc_seconds );

/* External SPI flash bring-up (Phase 2 Task 1). Both return 0 on success.
 * rewair_sflash_bounds_ok MUST be checked (addr, len) before read_bytes --
 * the driver's 24-bit address silently wraps past the 2 MiB device instead
 * of erroring. */
int rewair_sflash_read_id( uint8_t out_id[3] );
int rewair_sflash_read_bytes( uint32_t addr, uint8_t* out, uint32_t size );
int rewair_sflash_bounds_ok( uint32_t addr, uint32_t len );
