#pragma once

/* Wi-Fi join commands, lifted verbatim out of local_bridge.c (Phase 2
 * Task 10, pure move). Firmware-side only (wiced types throughout), not
 * host-testable.
 *
 * Cross-boundary: wifi_join_in_progress is DEFINED in rewair_wifi_join.c
 * (its writers are all here) but read directly by local_bridge.c's
 * network_thread_main to suppress autojoin while a join is running --
 * extern-declared below for that use, volatile preserved, no accessor
 * functions introduced (same convention as Task 9's dct_wifi_mutex).
 *
 * wifi_join_command_ex was made extern in local_bridge.c by Task 9
 * (rewair_wifi_dct.c's wifi_list_add calls it) with a local extern
 * declaration there; it now lives here and rewair_wifi_dct.c includes this
 * header instead. See the Task 10 report for the full rationale.
 */

#include <stdint.h>
#include "wiced.h"

/* Set while a join/DHCP attempt is running; read by local_bridge.c's
 * network_thread_main to hold off DCT autojoin. */
extern volatile uint32_t wifi_join_in_progress;

wiced_result_t wifi_join_command_ex( const char* ssid_text, const char* pass_text,
                                     wiced_security_t security, const wiced_scan_result_t* scan,
                                     int force_security, int store_to_dct,
                                     wiced_ap_info_t* joined_ap_out, char joined_key_out[64],
                                     uint32_t* joined_key_len_out );
wiced_result_t wifi_join_command( const char* ssid_text, const char* pass_text,
                                  wiced_security_t security, const wiced_scan_result_t* scan,
                                  int force_security );
void wifi_join_test_index_command( const char* index_text, const char* pass_text,
                                   wiced_security_t security, int force_security );
void wifi_join_index_command( const char* index_text, const char* pass_text,
                              wiced_security_t security, int force_security );

/* implemented in local_bridge.c (both STAY there; the moved
 * wifi_join_command_ex calls them after a successful join). local_bridge.c
 * has no header of its own, so -- minimal move -- they are declared here,
 * matching how web_api.h has carried "provided by local_bridge.c"
 * declarations since Task 8. Linkage changed from static to external in
 * local_bridge.c, bodies untouched. */
void network_after_ip_ready( void );
void wifi_print_status( void );
