#pragma once

#include "wiced.h"

#include "rewair_version.h"

/* Compile-time dev flag: adds Access-Control-Allow-Origin: * */
/* #define REWAIR_API_CORS_DEV 1 */
#define REWAIR_API_CORS_DEV 1   /* ON during phase 1 bring-up; turn off for release */

wiced_result_t rewair_web_api_start( wiced_interface_t interface );

/* provided by local_bridge.c */
uint32_t                   sensor_scan_blocking( void );
const wiced_scan_result_t* console_scan_cache_get( uint32_t index );
const wiced_scan_result_t* find_best_scan_result_for_ssid( const char* ssid_text );
uint32_t                   wifi_dct_saved_count( void );
