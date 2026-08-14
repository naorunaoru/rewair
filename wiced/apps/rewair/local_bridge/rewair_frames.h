#pragma once

/* F103 sensor-frame TX (frame building + senders), lifted out of
 * local_bridge.c. Firmware-side only (WICED types throughout). */

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

/* ---- Frame building ---- */
uint32_t fields_payload_len( char** fields, uint32_t count );
void frame_append( uint8_t* frame, uint32_t* frame_len, const void* data, uint32_t length );
wiced_result_t sensor_uart_send_frame_bytes( const uint8_t* frame, uint32_t frame_len );
wiced_result_t sensor_send_frame( const char cmd[4], char** fields, uint32_t field_count );

/* ---- Senders ---- */
/* Initializes the lock protecting the active timezone rule and serialized
 * TINF/TIME context transactions. Must precede rewair_settings_init(). */
wiced_result_t rewair_time_context_init( void );
void sensor_set_tz_rule( const rewair_tz_rule_t* rule );
void send_netw_up( void );
void send_tinf_context( uint32_t year );
void send_time_context( uint32_t utc_seconds );
void send_disp_clock_canary( void );
wiced_result_t sensor_send_disp_mode( const char* mode );
wiced_result_t sensor_send_sleep_mode( uint8_t mode );
void sensor_apply_manual_time( uint32_t epoch );
void send_sensor_boot_context( void );
void send_scor_from_sens( const sens_values_t* sens );
