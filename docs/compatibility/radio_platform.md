# EMW3165 Radio Platform Notes

Goal: put the EMW3165 back on the air with a drop-in Rewair firmware while
keeping the stock Awair F103/display firmware and UART protocol reusable.

## Rewair Repo Map

- `wiced/apps/rewair/local_bridge`: current WICED app. It brings up Wi-Fi,
  persists credentials (including a multi-network saved-AP list) in the
  standard WICED DCT, syncs time, speaks the F103 UART protocol, and serves
  the web API and web UI over HTTP. Split into small single-purpose modules:
  `local_bridge.c` (entry point, sensor-UART ISR/reset handling),
  `rewair_state` (live status cache + SSE notification hook),
  `rewair_settings`, `rewair_tz` (POSIX-TZ), `rewair_frames`/`rewair_frame_rx`
  (F103 frame codec), `rewair_wifi_dct`/`rewair_wifi_scan`/`rewair_wifi_join`,
  `rewair_console`, `rewair_score`, `rewair_walltime`, `rewair_fmt`
  (string/IP/MAC/SSID formatting), `rewair_json` (with vendored `jsmn.c`/`jsmn.h`),
  `rewair_drops`, `rewair_uifs` (storage-agnostic RWFS reader),
  `rewair_net_mode` (pure-STA-or-pure-AP mode state machine: setup-AP bring-up
  via the WICED internal DHCP server and `DNS_redirect` daemon, boot-autojoin
  fallback, self-heal — see the README's [AP Setup
  Mode](../../README.md#ap-setup-mode)), and `web_api.c`/`web_ui.c` (HTTP
  routes, captive-portal redirect, and UI serving).
- `wiced/platforms/AWAIR`: Awair/EMW3165 platform port and pin map used by the
  WICED SDK build, including the external SPI flash pin mapping (see
  [External SPI Flash](#external-spi-flash) below).
- `webui/`: the web UI (Preact + htm, built with Vite/npm), packed into an
  RWFS image (`webui/scripts/pack-rwfs.mjs`) and linked into the F411 app.
- `tests/host/`: 7 host-buildable test suites covering the WICED-independent
  modules above.
- `scripts/sync_to_wiced.zsh`: copies the tracked app and platform files into
  the external WICED SDK tree.
- `scripts/build_local_bridge.zsh`: syncs and builds
  `rewair.local_bridge-AWAIR-FreeRTOS-LwIP-SDIO`.
- `scripts/flash_local_bridge_probe_rs.zsh`: flashes the built image over SWD
  while preserving DCT by default.
- `scripts/flash_sflash_openocd.zsh`: writes raw images to unallocated external
  SPI flash via OpenOCD; see [External SPI Flash](#external-spi-flash).
- `scripts/api_smoke.zsh`: live-device smoke test covering the web API and UI.
- `tools/legacy/emw3165_sensor_console`: bare-metal fallback console for
  checking the F103 protocol without the WICED stack.
- `tools/f103_debug`: SWD/GDB helpers for inspecting the F103 when the UART link
  stops responding.
- `tools/recovery`: restore path back to stock F411 firmware.
- `docs/status.md`: current project state, known-good commands, and next steps.

## Module Summary

- Radio module: EMW3165 with STM32F411-class MCU and Broadcom Wi-Fi over SDIO.
- Console: USART1, `PB6` TX / `PA10` RX, 115200 8N1.
- Sensor link: USART2, `PA2` TX / `PA3` RX, 115200 8N1.
- Sensor reset control appears to be on F411 `PB12`.

## External SPI Flash

The board carries a Macronix MX25L1606E (2 MiB, JEDEC `c2 20 15`) external
SPI flash on SPI1. Pins (empirically derived, not from a reference
schematic — the stock BCM943362WCD4 reference platform this WICED port is
based on wires its own SPI flash differently):

- CS (SSN): `PA15` (`WICED_GPIO_15`), driven as a plain software GPIO,
  active low.
- SCK: `PB3` (`WICED_GPIO_16`), AF5.
- MISO: `PB4` (`WICED_GPIO_17`), AF5.
- MOSI: `PA7` (`WICED_GPIO_8`), AF5.

`PA15`/`PB3`/`PB4` double as the module's JTAG TDI/TDO/NTRST pins;
repurposing them for SPI1 is safe on this board because on-target debug is
SWD-only (`PA13`/`PA14`), so full JTAG is never used. See
`wiced/platforms/AWAIR/platform.c` and `platform.h` for the driver-level
detail, and `wiced/platforms/AWAIR/README.txt` for the reverse-engineering
notes.

Region layout on the 2 MiB device:

- `0x000000`-`0x100FFF`: OTA staging, known-good application backup, and journal.
- `0x101000`-`0x135FFF`: WICED apps lookup table and BCM43362A2 WLAN firmware.
- `0x136000`-`0x1FFFFF`: reserved/free. The web UI is linked into internal F411
  flash and no longer consumes an external-flash partition.

Write tooling: `scripts/flash_sflash_openocd.zsh` drives the WICED SDK's
`waf_sflash_write` RAM stub over OpenOCD/CMSIS-DAP via
`scripts/sflash_write_swd.tcl`, then verifies a readback sample against the
device's `/api/debug/sflash` route. Reads are also available from the serial
console and via that same dev-gated HTTP route.

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

The bare-metal sensor console in `tools/legacy/emw3165_sensor_console` is a
small diagnostic firmware. It does not start WICED or Wi-Fi; it only talks to
the F103 over the sensor UART and helps verify protocol behavior.

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
