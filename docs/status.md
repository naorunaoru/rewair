# Rewair Status

Date paused: 2026-06-08.

## Hardware

- Radio module: EMW3165, STM32F411CE-class MCU plus Broadcom Wi-Fi over SDIO.
- Sensor/display MCU: STM32F103VET6.
- F411 debug console: USART1, `PB6` TX / `PA10` RX, 115200 8N1.
- F411 to F103 link: USART2, `PA2` TX / `PA3` RX, 115200 8N1.
- F103 reset from F411 appears to be on F411 `PB12`.

## What Works

- Baseline EMW3165 hardware characterization is complete.
- WICED 3.3.1 SDK fork builds for custom `AWAIR` platform.
- Rewair/local bridge boots WICED, initializes WLAN, and exposes a serial
  console.
- Wi-Fi scanning works.
- `join` and `join-index` can join a 2.4 GHz WPA2 AP.
- Credentials persist through standard WICED DCT `stored_ap_list[0]`.
- Autojoin from DCT works on boot.
- DHCP works.
- SNTP sync works.
- Lisbon timezone/DST `TINF` and `TIME` frames are sent to F103.
- F103 responds to `REDY` and `TEST` after reset.
- Bare-metal F411 sensor console can receive `SENS` and reply with `SCOR`.
- Display/control frames such as `DISP mode clock`, `LEDS color white blink 0`,
  and `SCOR ...` were proven on the bare-metal path.

## Current Problem

Under WICED/local bridge, the F103 commonly emits:

```text
REDY
TEST
```

and then never starts `SENS`, even though Rewair sends `NETW`, `TINF`, `TIME`,
and bounded retry nudges. USART2 TX reports success and PA3 idles high, but we
have not yet confirmed the actual bytes on the F411/F103 wires.

Gut hypothesis saved for next session: this may be a WICED USART/DMA behavior
issue on the F411 side, not a F103 sensor-board state-machine issue.

## Next Experiment

Passive sniff the F411/F103 UART link at 115200 8N1:

- F411 `PA2` / USART2 TX -> F103 RX.
- F411 `PA3` / USART2 RX <- F103 TX.
- Optional timing channels: F411 `PB12`/F103 reset, F411 boot marker.

Compare wire bytes against Rewair logs for:

- frame header `*CMD%08X\0`;
- payload NUL separators;
- final `#`;
- missing/truncated/duplicated frames;
- bytes emitted during Wi-Fi/DHCP/NTP startup.

If the wire trace differs from logs, prioritize WICED USART/DMA/ring-buffer
behavior. If the wire trace matches logs exactly, return to F103-side tracing of
inbound command handling and sensor/test state.

## Useful Commands

Build and flash Rewair:

```sh
scripts/build_local_bridge.zsh
scripts/flash_local_bridge_probe_rs.zsh
```

Preserve DCT by default. Use `FLASH_DCT=1` only when intentionally resetting
stored Wi-Fi credentials.

F103 debug snapshot:

```sh
tools/f103_debug/snapshot_f103.zsh
```

F103 UART handler trace:

```sh
tools/f103_debug/trace_f103_uart_handlers.zsh
```

Fallback bare-metal sensor console:

```sh
make -C tools/emw3165_sensor_console
tools/emw3165_sensor_console/flash_sensor_console.zsh
```
