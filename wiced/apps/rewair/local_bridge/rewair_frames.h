#pragma once

/* F103 sensor-frame TX (frame building + senders), lifted verbatim out of
 * local_bridge.c (Phase 2 Task 8, pure move). Firmware-side only (wiced
 * types throughout), not host-testable.
 *
 * Cross-boundary statics: several file-globals below are shared with
 * local_bridge.c (sensor-thread / reset / console clusters, which stay
 * there). Per the pure-move rule, each was left in its PRIMARY-writer
 * module and extern-declared here for the other side -- no accessor
 * functions were introduced. See the Task 8 report for the placement
 * rationale for each variable.
 */

#include <stdint.h>
#include "wiced.h"
#include "rewair_tz.h"
#include "rewair_score.h"

/* ---- TX mutex + diagnostic counters (primary writer: this module's
 * sensor_uart_send_frame_bytes). Read by local_bridge.c's sensor_uart_stat
 * (sensor-thread diagnostics, stays); sensor_uart_tx_mutex is also
 * initialized by local_bridge.c's sensor_uart_start (UART bring-up, stays). */
extern wiced_mutex_t sensor_uart_tx_mutex;
extern volatile uint32_t sensor_uart_tx_count;
extern volatile uint32_t sensor_uart_tx_drop_count;
extern volatile uint32_t sensor_uart_wiced_tx_fail_count;
extern volatile uint32_t sensor_uart_wiced_tx_result;
extern volatile uint32_t sensor_uart_tx_sr_before;
extern volatile uint32_t sensor_uart_tx_sr_after;

/* ---- Boot-tracking statics (primary writer: this module's senders --
 * send_sensor_boot_context guards itself with sensor_boot_context_sent;
 * send_netw_up increments sensor_netw_boot_pulses). Reset to 0 by
 * local_bridge.c's sensor_reset_release/sensor_reset_cycle (reset cluster,
 * stays) and by the "context" console command (console cluster, stays);
 * sensor_netw_boot_pulses is also read by sensor_uart_stat (stays). */
extern volatile uint32_t sensor_boot_context_sent;
extern volatile uint32_t sensor_netw_boot_pulses;

/* ---- Timezone rule (primary writer: sensor_set_tz_rule, this module).
 * Read directly by local_bridge.c's application_start DST-recheck loop
 * (stays), which is why both statics are extern-exposed rather than kept
 * file-local. */
extern rewair_tz_rule_t current_tz_rule;
extern uint32_t current_tz_rule_valid;

/* ---- Frame building ---- */
uint32_t fields_payload_len( char** fields, uint32_t count );
void frame_append( uint8_t* frame, uint32_t* frame_len, const void* data, uint32_t length );
void sensor_uart_send_frame_bytes( const uint8_t* frame, uint32_t frame_len );
void sensor_send_frame( const char cmd[4], char** fields, uint32_t field_count );

/* ---- Senders ---- */
void sensor_set_tz_rule( const rewair_tz_rule_t* rule );
void send_netw_up( void );
void send_tinf_from_rule( const rewair_tz_rule_t* rule, uint32_t year );
void send_time_from_rule( const rewair_tz_rule_t* rule, uint32_t utc_seconds );
void send_time_context( uint32_t utc_seconds );
void send_disp_clock_canary( void );
wiced_result_t sensor_send_disp_mode( const char* mode );
void sensor_apply_manual_time( uint32_t epoch );
void send_sensor_boot_context( void );
void send_scor_from_sens( const sens_values_t* sens );
