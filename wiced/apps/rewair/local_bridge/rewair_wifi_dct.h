#pragma once

/* Wi-Fi DCT credential/list management, lifted verbatim out of local_bridge.c
 * (Phase 2 Task 9, pure move). Firmware-side only (wiced types throughout,
 * DCT/platform headers), not host-testable.
 *
 * Cross-boundary: dct_wifi_mutex is DEFINED here (its only lock/unlock sites
 * are the functions below), but it is also locked/unlocked directly by
 * local_bridge.c's network_after_ip_ready (stays -- reads the stored-AP SSID
 * for its status snapshot) and initialized by local_bridge.c's
 * application_start (mutex storage moves, the one-time init call does not --
 * same convention as Task 8's sensor_uart_tx_mutex). extern-declared below
 * for that use, no accessor functions introduced. See the Task 9 report for
 * the full placement rationale.
 */

#include <stdint.h>
#include "wiced.h"

/* Guards DCT wifi-section read/write critical sections only; never held
 * across joins, frame sends, or state-cache calls. */
extern wiced_mutex_t dct_wifi_mutex;

int            wifi_dct_has_stored_ap( void );
uint32_t       wifi_dct_saved_count( void );
void           wifi_print_saved_credentials( void );
wiced_result_t wifi_clear_stored_credentials( void );
wiced_result_t wifi_store_ap_credentials( const wiced_ap_info_t* ap,
                                           const char* security_key,
                                           uint32_t security_key_length );

/* ---- multi-network DCT list management (Task 8, web API) ----
 * These preserve existing stored_ap_list entries -- unlike wifi_store_ap_credentials
 * (still used by the console "join" path), which wipes the whole list and writes
 * slot 0 only. */
wiced_result_t wifi_list_add( const char* ssid, const char* pass );
/* AP-mode no-probe variant of wifi_list_add: persists to the same
 * find-existing-or-first-free slot WITHOUT joining first. Security/BSSID/channel
 * come from the scan cache when the SSID is present there (the portal UI's
 * /api/scan guarantees a recent cache), else security is guessed from the
 * password with zeroed BSSID/channel (SDK autojoin falls back to join-by-SSID,
 * so a zeroed entry still joins). Used by the AP arm of /api/join, which must
 * not bring the STA interface up while the setup AP is running. */
wiced_result_t wifi_list_store( const char* ssid, const char* pass );
wiced_result_t wifi_list_remove( const char* ssid );
wiced_result_t wifi_list_reorder( char order[][33], uint32_t count );
int            wifi_list_get( uint32_t index, char ssid_out[33], wiced_security_t* sec_out,
                              int* connected_out );
