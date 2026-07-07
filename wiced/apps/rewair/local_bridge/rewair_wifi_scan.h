#pragma once

/* Wi-Fi scan cache + scan/security naming helpers, lifted verbatim out of
 * local_bridge.c (Phase 2 Task 10, pure move). Firmware-side only (wiced
 * types throughout), not host-testable.
 *
 * The scan cache itself (console_scan_cache, its count, the completion
 * semaphore and the scan_busy benign-race guard) stays private to
 * rewair_wifi_scan.c -- every outside reader already goes through the
 * accessor functions below (web_api.c via sensor_scan_blocking /
 * console_scan_cache_get / find_best_scan_result_for_ssid, previously
 * declared in web_api.h's "provided by local_bridge.c" block; the console
 * thread via wifi_scan_start / parse_wifi_security).
 *
 * wifi_security_name / wifi_result_name were made extern in local_bridge.c
 * by Task 9 (rewair_wifi_dct.c's DCT-failure log lines call them) with local
 * extern declarations in rewair_wifi_dct.c; they now live here as the
 * scan/naming module is their primary owner, and rewair_wifi_dct.c includes
 * this header instead. See the Task 10 report for the full rationale.
 */

#include <stdint.h>
#include "wiced.h"

const char* wifi_security_name( wiced_security_t security );
const char* wifi_result_name( wiced_result_t result );

/* Returns 1 and writes *security on a recognized name, 0 otherwise. Used by
 * the console "join"/"join-index"/"join-test-index" argument parsing. */
int parse_wifi_security( const char* text, wiced_security_t* security );

/* Fills a wiced_ap_info_t from a cached scan record. Used by the join path
 * (rewair_wifi_join.c) when joining against a cached scan result. */
void ap_from_scan_result( wiced_ap_info_t* ap, const wiced_scan_result_t* scan );

/* Console "scan" command: fire-and-forget scan into the cache (results are
 * printed by the completion handler as they arrive). */
void wifi_scan_start( void );

/* Blocking scan used by /api/scan (web_api.c): refreshes the cache and
 * returns the number of cached results (possibly stale results if a console
 * scan already owns the cache -- see the scan_busy comment in the .c). */
uint32_t sensor_scan_blocking( void );

const wiced_scan_result_t* console_scan_cache_get( uint32_t index );
const wiced_scan_result_t* find_best_scan_result_for_ssid( const char* ssid_text );
