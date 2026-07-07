#pragma once

/* rewair_net_mode -- pure-STA-or-pure-AP network mode state machine.
 *
 * Design: 2026-07-07-phase3-ap-portal-design.md.
 *
 * Policy: the device is always in exactly ONE radio role -- either joined to a
 * home AP as a station (STA) or hosting its own setup access point (AP). It is
 * never both at once (no concurrent apsta). AP is entered only when STA cannot
 * be brought up: no stored credentials (AP_SETUP) or boot-time autojoin
 * exhausted (AP_FALLBACK). While AP is up the STA autojoin arm is unreachable by
 * construction (see the switch in rewair_net_mode_tick).
 *
 * Threading: rewair_net_mode_tick() is the single writer of all mode state. It
 * runs on local_bridge.c's network thread, called once every NETWORK_RETRY_MS
 * (60 s). Every mode transition -- enter_ap / exit_ap_to_sta and the STA
 * autojoin bookkeeping -- happens on that thread. The ONLY cross-thread input is
 * rewair_net_mode_request_sta(), which sets an atomic request flag consumed at
 * the top of the next tick; the web join handler (Task 5) uses it so it never
 * touches radio state from HTTP worker context. Likewise rewair_web_api_start/
 * stop are only ever called from inside the tick (via enter_ap/exit_ap_to_sta),
 * honouring web_api's single-control-thread contract.
 */

#include <stdint.h>
#include "wiced.h"

typedef enum
{
    NET_MODE_STA_JOINING = 0,
    NET_MODE_STA_UP,
    NET_MODE_AP_SETUP,      /* no stored credentials */
    NET_MODE_AP_FALLBACK,   /* credentials exist, boot autojoin exhausted */
} rewair_net_mode_t;

rewair_net_mode_t rewair_net_mode_current( void );
void           rewair_net_mode_tick( void );          /* called from the 60s network loop */
wiced_result_t rewair_net_mode_enter_ap( rewair_net_mode_t which );  /* AP_SETUP or AP_FALLBACK */
wiced_result_t rewair_net_mode_exit_ap_to_sta( void );/* teardown AP; next tick autojoins */
void           rewair_net_mode_request_sta( void );   /* async: web join handler (Task 5) */
int            rewair_net_mode_sta_requested( void ); /* pending-request poll for the network loop's fast wakeup (Task 5) */
