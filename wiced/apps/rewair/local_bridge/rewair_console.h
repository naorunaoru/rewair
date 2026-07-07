#pragma once

/* USART1 interactive console (line read/tokenize/dispatch + the console-only
 * sflash "id"/"read" command wrappers), lifted verbatim out of
 * local_bridge.c (Phase 2 Task 11, pure move). Firmware-side only (wiced
 * types throughout via the headers it pulls in), not host-testable.
 *
 * console_thread_main is the console thread's entry point, started by
 * application_start (local_bridge.c) via wiced_rtos_create_thread -- that
 * call site is unchanged except for now resolving the symbol through this
 * header instead of a same-file static declaration.
 *
 * See the Task 11 report for the full extern-variable placement rationale
 * (statics that stayed in local_bridge.c but are read/written here).
 */

#include <stdint.h>

void console_thread_main( uint32_t arg );
