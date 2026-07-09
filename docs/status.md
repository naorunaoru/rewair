# Rewair Status

Date: 2026-07-09.

## Hardware

- Radio module: EMW3165, STM32F411CE-class MCU plus Broadcom Wi-Fi over SDIO.
- Sensor/display MCU: STM32F103VET6.
- F411 debug console: USART1, `PB6` TX / `PA10` RX, 115200 8N1.
- F411 to F103 link: USART2, `PA2` TX / `PA3` RX, 115200 8N1.
- F103 reset from F411 appears to be on F411 `PB12`.
- External SPI flash: Macronix MX25L1606E, 2 MiB, JEDEC ID `c2 20 15`. SPI1,
  pins empirically derived (differ from the reference WICED platform, which
  is wired differently): CS `PA15` (software GPIO, active low), MOSI `PA7`,
  MISO `PB4`, SCK `PB3`. `PA15`/`PB3`/`PB4` double as JTAG TDI/TDO/NTRST, but
  repurposing them is safe here since on-board debug is SWD-only.

## What Works

- Baseline EMW3165 hardware characterization is complete.
- WICED 3.3.1 SDK fork builds for custom `AWAIR` platform.
- Rewair/local bridge boots WICED, initializes WLAN, and exposes a serial
  console.
- Wi-Fi scanning works; DCT-backed multi-network saved-AP list (join, forget,
  reorder priority) works, not just a single stored AP.
- Autojoin from DCT works on boot.
- DHCP works.
- SNTP sync works.
- POSIX-TZ-driven `TINF`/`TIME` frames are sent to F103 (derived from
  arbitrary timezone settings, not hardcoded to one zone).
- F103 responds to `REDY` and `TEST` after reset.
- Live state cache (`rewair_state`) tracks score/sensors/wifi/time/settings
  with a change-notification hook.
- Full web API (`wiced/apps/rewair/local_bridge/web_api.c`): status, scan,
  networks, join/forget/priority, settings, time, disp, reset, SSE stream at
  `/api/events`. See the README's [Web API
  Summary](../README.md#web-api-summary) for the route table.
- Web UI (`webui/`, Preact + Vite) builds to a packed RWFS image and is
  served by the device itself from external SPI flash at `/`, `/app.js`,
  `/rewair.css` — no separate web server needed once flashed.
- `rewair_net_mode` (`wiced/apps/rewair/local_bridge/rewair_net_mode.c`):
  pure-STA-or-pure-AP setup mode. No stored network -> open setup AP
  `rewair-setup-<xxxx>` (last 4 MAC hex) at `192.168.0.1` with internal
  DHCP, DNS redirect, and a captive `302` portal; boot autojoin failing 3
  times -> the same AP as a fallback, self-healing back to STA every ~5 min
  (skipped while a client is on the AP). `/api/join` in AP mode stores
  credentials (from the scan cache, no live probe) and switches to STA
  asynchronously (~1 s); `/api/reset` clears credentials and reboots back
  into the setup AP. See the README's [AP Setup
  Mode](../README.md#ap-setup-mode) section.
- F411 OTA is implemented through the portal: guarded firmware-side SPI
  writes, chunked upload/readback verification, boot-time known-good backup,
  idempotent internal-flash copy, and three-attempt trial rollback. It builds
  within both flash regions and has host tests; the bench power-loss gauntlet
  in `docs/ota.md` remains pending.
- 8 host-buildable test suites under `tests/host/` (`test_drops`, `test_tz`,
  `test_req`, `test_status`, `test_uifs`, `test_walltime`, `test_score`,
  `test_ota`), all
  passing against a clean `make -C tests/host`.
- `scripts/api_smoke.zsh`: 20-check smoke test against a live device,
  covering both the web API and the sflash-served UI.
- External SPI flash read/write tooling: `scripts/flash_sflash_openocd.zsh`
  (OpenOCD + WICED sflash-write RAM stub, SWD/CMSIS-DAP, with readback
  verification) and `scripts/flash_webui.zsh` (build + flash the UI image in
  one step). A dev-gated `GET /api/debug/sflash` HTTP route and console
  commands expose raw readback for debugging.

## Current Problem

The F103 sensor board's `SENS` stream is still intermittent. The
long-standing symptom: after reset the F103 emits

```text
REDY
TEST
```

and then sometimes never starts sending `SENS`, even though Rewair sends
`NETW`, `TINF`, `TIME`, and bounded retry nudges.

This has not settled into a fixed "broken" or "fixed" state — it comes and
goes across boots and reflashes. During Phase 2, `SENS` stalled again after
the external-sflash bring-up reflash (2026-07-06), then was observed flowing
normally again the next day (2026-07-07) during later Phase 2 work, and a
fresh live check made during this docs pass (also 2026-07-07, later reflash)
again showed a stalled `SENS` (`sens.*` fields pinned at 0 in `/api/status`
despite `wifi.connected_s` climbing normally). In short: **root cause is
still unknown**, and any single observation of "it's flowing" or "it's
stalled" only describes that particular boot, not the underlying bug.

The previous working theory (recorded pre-Phase-2, dated 2026-06-08) was that
this might be a WICED USART/DMA behavior issue on the F411 side rather than
an F103 sensor-board state-machine issue — based on USART2 TX reporting
success and `PA3` idling high without a confirmed wire-level trace. That
theory has not been confirmed or ruled out with an actual UART sniff; nobody
has captured the wire bytes yet.

Since then (Phase-1 Task 5, commit `ce91ca3`), the F103 reset strategy and
UART error handling changed substantially and were kept as a deliberate,
user-adjudicated bet that they might address the stall:

- `local_bridge.c` now tracks `sensor_uart_error_flags` /
  `sensor_uart_error_count` and friends, and logs a rate-limited `[uart-err]`
  line (parity/framing/noise/overrun bits, tick, counts) whenever the sensor
  USART reports an error, plus clears latched error state explicitly instead
  of leaving it to accumulate silently.
- The sensor reset sequencing (`sensor_reset_release`/`sensor_reset_cycle`,
  now living in `rewair_console.c`/`local_bridge.c` after the Phase 2
  decomposition) was reworked as part of the same change.

Whether this reset-strategy change actually fixes anything, versus the stall
simply being flaky on its own timescale, is still an open question — the
observations above (stalled, then flowing, then stalled again) are
consistent with either "fixed but still occasionally triggered by something
else" or "never fixed, just intermittent by nature." Treat any one boot's
behavior as a data point, not a verdict.

## Next Steps

- Get an actual wire-level capture of the F411-F103 UART link (115200 8N1) to
  settle the open question from 2026-06-08 that was never followed up:
  - F411 `PA2` / USART2 TX -> F103 RX.
  - F411 `PA3` / USART2 RX <- F103 TX.
  - Optional timing channels: F411 `PB12`/F103 reset, F411 boot marker.
  - Compare wire bytes against Rewair's own logs (frame header
    `*CMD%08X\0`, NUL-separated payload fields, trailing `#`) for
    missing/truncated/duplicated frames, especially bytes emitted during
    Wi-Fi/DHCP/NTP startup.
- Use `tools/f103_debug/snapshot_f103.zsh` and
  `tools/f103_debug/trace_f103_uart_handlers.zsh` to inspect F103-side state
  over SWD when a stall is caught live.
- Watch the `[uart-err]` console log lines (see `local_bridge.c`) across a
  run that stalls, to see whether a UART error precedes the stall or the
  link is simply silent.
- If the wire trace matches Rewair's logs exactly, the bug is very likely on
  the F103 side (inbound command handling / sensor-test state machine); if it
  differs, prioritize the WICED USART/DMA/ring-buffer path on the F411 side.

## Useful Commands

Build and flash Rewair firmware:

```sh
scripts/build_local_bridge.zsh
scripts/flash_local_bridge_probe_rs.zsh
```

Preserve DCT by default. Use `FLASH_DCT=1` only when intentionally resetting
stored Wi-Fi credentials.

Build and flash the web UI:

```sh
cd webui && npm ci && npm run build && cd ..
scripts/flash_webui.zsh
```

(`flash_webui.zsh` already runs `npm run build` itself; the manual `npm ci`
step above is only needed the first time, to install dependencies.)

After the one-time OTA bootloader bootstrap, subsequent F411 updates can be
installed from Settings → Firmware in that web UI. See `docs/ota.md`.

Run the host test suites:

```sh
make -C tests/host
```

Smoke-test a live device:

```sh
REWAIR_IP=192.168.1.242 scripts/api_smoke.zsh
```

F103 debug snapshot:

```sh
tools/f103_debug/snapshot_f103.zsh
```

F103 UART handler trace:

```sh
tools/f103_debug/trace_f103_uart_handlers.zsh
```

Fallback bare-metal sensor console (isolates F103 protocol behavior from the
WICED UART/DMA path; needs arm-none-eabi- toolchain on PATH — see README prerequisites):

```sh
make -C tools/legacy/emw3165_sensor_console
tools/legacy/emw3165_sensor_console/flash_sensor_console.zsh
```

Restore stock F411 firmware if an experimental flash leaves the module
unable to boot:

```sh
IMAGE=/path/to/known-good.bin tools/recovery/restore_stock_f411.zsh
```

See `tools/recovery/README.md` for the boot-sector-only variant and the
expected location of golden dump images.
