#!/usr/bin/env zsh
set -euo pipefail

CHIP="${CHIP:-STM32F103VE}"
SPEED="${SPEED:-100}"
GDB_PORT="${GDB_PORT:-1337}"
LAUNCH_SLEEP="${LAUNCH_SLEEP:-1.0}"
RESET_HALT="${RESET_HALT:-0}"
CONNECT_UNDER_RESET="${CONNECT_UNDER_RESET:-0}"

script_dir="${0:A:h}"
log="$script_dir/probe-rs-gdb-f103-trace.log"
reset_args=()
connect_args=()

if [[ "$RESET_HALT" == 1 ]]; then
  reset_args=(--reset-halt)
fi

if [[ "$CONNECT_UNDER_RESET" == 1 ]]; then
  connect_args=(--connect-under-reset)
fi

probe-rs gdb \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive \
  --gdb-connection-string "127.0.0.1:$GDB_PORT" \
  "${connect_args[@]}" \
  "${reset_args[@]}" \
  > "$log" 2>&1 &
server_pid=$!

cleanup() {
  kill -INT "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT

sleep "$LAUNCH_SLEEP"

if ! arm-none-eabi-gdb -q -x "$script_dir/trace_f103_uart_handlers.gdb"; then
  print -u2 -r -- "Failed to trace F103 UART handlers. probe-rs gdb log:"
  sed -n '1,180p' "$log" >&2
  exit 1
fi
