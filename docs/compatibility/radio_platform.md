# EMW3165 Radio Platform Notes

Goal: put the EMW3165 back on the air with a drop-in Rewair firmware while
keeping the stock Awair F103/display firmware and UART protocol reusable.

## Rewair Repo Map

- `wiced/apps/rewair/local_bridge`: current WICED app. It brings up Wi-Fi,
  persists credentials in the standard WICED DCT, syncs time, and speaks the
  F103 UART protocol.
- `wiced/platforms/AWAIR`: Awair/EMW3165 platform port and pin map used by the
  WICED SDK build.
- `scripts/sync_to_wiced.zsh`: copies the tracked app and platform files into
  the external WICED SDK tree.
- `scripts/build_local_bridge.zsh`: syncs and builds
  `rewair.local_bridge-AWAIR-FreeRTOS-LwIP-SDIO`.
- `scripts/flash_local_bridge_probe_rs.zsh`: flashes the built image over SWD
  while preserving DCT by default.
- `tools/emw3165_sensor_console`: bare-metal fallback console for checking the
  F103 protocol without the WICED stack.
- `tools/f103_debug`: SWD/GDB helpers for inspecting the F103 when the UART link
  stops responding.
- `docs/status.md`: current project state, known-good commands, and next steps.

## Module Summary

- Radio module: EMW3165 with STM32F411-class MCU and Broadcom Wi-Fi over SDIO.
- Console: USART1, `PB6` TX / `PA10` RX, 115200 8N1.
- Sensor link: USART2, `PA2` TX / `PA3` RX, 115200 8N1.
- Sensor reset control appears to be on F411 `PB12`.

## Active WICED Route

Rewair tracks the app and platform sources. The patched WICED SDK fork lives as
an external sibling checkout at `../third_party/wiced-emw3165`. The repo scripts
sync into that SDK before building.

Current build target:

```sh
scripts/build_local_bridge.zsh
```

Current flash path:

```sh
scripts/flash_local_bridge_probe_rs.zsh
```

By default, flashing preserves the WICED DCT so saved Wi-Fi credentials survive
development iterations. Set `FLASH_DCT=1` only when deliberately resetting the
DCT.

## Fallback Console

The bare-metal sensor console in `tools/emw3165_sensor_console` is a small
diagnostic firmware. It does not start WICED or Wi-Fi; it only talks to the F103
over the sensor UART and helps verify protocol behavior.

Use it when you need to separate F103 protocol behavior from the WICED UART/DMA
path.

## Pause Note, 2026-06-08

Current working hypothesis: the repeated "F103 stops responding after
REDY/TEST" failure may be caused by USART behavior under WICED on the F411 side,
not by the sensor board entering a strange half-working state.

When resuming, try to confirm or disprove this before deeper F103 debugging:

- Passively sniff the UART exchange between the EMW3165/F411 and F103 sensor MCU
  at 115200 8N1.
- Capture both directions if possible:
  - F411 USART2 TX / PA2 -> F103 RX.
  - F411 USART2 RX / PA3 <- F103 TX.
- If spare analyzer channels are available, also capture F103 reset/PB12 and
  maybe F411 boot timing.
- Compare actual wire bytes against local bridge logs for `NETW`, `TINF`,
  `TIME`, `DISP`, and any missing/extra bytes around the moment `SENS` never
  starts.
- Specifically check for WICED UART TX oddities: truncation, duplicated frames,
  missing final `#`, stray NUL/header bytes, DMA/ring-buffer timing artifacts,
  or line contention during Wi-Fi/DHCP/NTP startup.

If the wire trace matches the logs exactly and the F103 still remains silent,
then return to F103-side SWD tracing of inbound handlers and sensor/test state.
If the wire trace differs from the logs, prioritize the WICED USART/DMA path.
