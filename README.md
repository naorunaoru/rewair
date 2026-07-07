# Rewair

Custom drop-in firmware work for reviving original Awair Element units by
replacing the EMW3165 radio-module firmware, plus a small self-contained web
UI served straight from the device.

What's inside of the Element?

- ST STM32F103VET6 — sensor board MCU
- ST STM32F103RBT6 — front panel daughterboard MCU
- Glead ZYM-BI201 — BLE 4.0 (TI CC2540, originally used for BLE GATT pairing)
- MXCHIP EMW3165 — WiFi module (has STM32F411 + Broadcom WiFi inside, the main interest of this repo)
- a bunch of sensors (Telaire T6703, Sharp GP2Y10, FIS QS-01, SHT30)
- Macronix MX25L1606E — 2 MiB external SPI flash on the EMW3165 board, used
  by Rewair to store the web UI (see [SPI Flash](#spi-flash) below)

## Repository Layout

- `wiced/apps/rewair/local_bridge`: WICED 3.3.1 app used for radio bring-up,
  the F103 UART protocol, and the web API/UI. Split into small single-purpose
  modules:
  - `local_bridge.c`: app entry point, sensor-UART ISR/reset handling, boot
    sequencing.
  - `rewair_state`: live state cache (score/sensors/wifi/time/settings) with
    a change-notification hook for SSE.
  - `rewair_settings`: user-facing settings (name, units, timezone, display
    mode) persisted in a DCT app section.
  - `rewair_tz`: POSIX-TZ parsing/formatting for the `TINF`/`TIME` frames.
  - `rewair_frames` / `rewair_frame_rx`: F103 UART frame encode/decode.
  - `rewair_wifi_dct`: WICED DCT-backed multi-network saved-AP list.
  - `rewair_wifi_scan` / `rewair_wifi_join`: Wi-Fi scan and join/connect flow.
  - `rewair_console`: serial console command parser.
  - `rewair_score`: sensor-index-to-score math.
  - `rewair_walltime`: epoch/wall-clock helpers.
  - `rewair_json`: minimal JSON emitter and request-body reader (uses the
    vendored `jsmn.c`/`jsmn.h` parser).
  - `rewair_drops`: dropped-frame counters.
  - `rewair_uifs`: reader for the packed web-UI image format (RWFS) on
    external SPI flash; host-testable, no WICED includes.
  - `web_api.c`/`.h`: HTTP page database, all `/api/...` JSON routes, and
    Server-Sent Events.
  - `web_ui.c`/`.h`: serves the web UI (`/`, `/app.js`, `/rewair.css`) out of
    the RWFS image in external flash, with a small built-in fallback page.
- `wiced/platforms/AWAIR`: Awair/EMW3165 WICED platform pin map, including the
  external SPI flash pin mapping.
- `webui/`: the web UI itself — Preact + htm, built with Vite, plain
  npm (no framework CLI). See [Web UI](#web-ui) below.
- `tests/host/`: 7 host-buildable test suites (`test_drops`, `test_tz`,
  `test_req`, `test_status`, `test_uifs`, `test_walltime`, `test_score`) that
  compile the relevant `rewair_*.c` files directly with the system `cc`, no
  WICED/ARM toolchain required.
- `scripts`:
  - `sync_to_wiced.zsh`: rsyncs the tracked app/platform into the external
    WICED SDK.
  - `build_local_bridge.zsh`: syncs, then builds the local_bridge firmware.
  - `flash_local_bridge_probe_rs.zsh`: flashes bootloader+app over SWD via
    probe-rs, preserving DCT by default.
  - `flash_sflash_openocd.zsh`: writes a raw image to the external SPI flash
    via OpenOCD + the WICED sflash-write stub, with readback verification.
  - `flash_webui.zsh`: builds the web UI and flashes it to the UI region of
    external flash (wraps the two scripts above).
  - `sflash_write_swd.tcl`: OpenOCD Tcl driver for the sflash stub protocol
    (SWD/CMSIS-DAP reimplementation of the WICED SDK's JTAG-only version).
  - `api_smoke.zsh`: curl+jq smoke test against a live device's web API and UI.
- `tools/f103_debug`: SWD/GDB helpers for F103-side protocol tracing.
- `tools/legacy/emw3165_sensor_console`: small bare-metal F411 fallback
  console used before WICED networking was working; still useful for
  isolating F103-vs-WICED protocol questions.
- `tools/recovery`: scripts to restore stock F411 firmware if an
  experimental flash leaves the module unable to boot.
- `docs`: status and compatibility notes.

Generated build output, local lab artifacts, the WICED SDK, and `webui/`
build products (`node_modules/`, `dist/`, `*.rwfs`) are intentionally not
tracked here.

## Prerequisites

- **A CMSIS-DAP debug probe** (e.g. a Raspberry Pi Pico running picoprobe)
  wired to the test points on the opposite side of the EMW3165 module:
  TP80/TP81 is SWD (needed for flashing firmware and for the SPI-flash write
  path), TP103/TP104 is the debug USART (only needed if you want a serial
  console).
- **The WICED SDK fork**, checked out as a sibling of this repo (see
  [External Dependencies](#external-dependencies) below). It vendors its own
  `arm-none-eabi` GCC toolchain under `tools/ARM_GNU/bin/OSX/` — the repo's
  build scripts call it via that path, so you do **not** need to separately
  install an ARM toolchain to build or flash firmware. (If you want to use
  `tools/f103_debug`'s GDB scripts, those invoke `arm-none-eabi-gdb` from
  your `PATH`; either add the SDK's `tools/ARM_GNU/bin/OSX/` to `PATH`, or
  `brew install arm-none-eabi-gdb` separately — not required for the core
  build/flash/UI/smoke workflow below.)
- **probe-rs** (`brew install probe-rs` or see
  <https://probe.rs/docs/getting-started/installation/>) — used for
  firmware flashing and to reset the target after an sflash write.
- **OpenOCD 0.12+ from brew** (`brew install openocd`) — used only for the
  external SPI flash write path (`flash_sflash_openocd.zsh` /
  `flash_webui.zsh`). The WICED SDK's own vendored OpenOCD is a 2015 JTAG-only
  build that does not speak CMSIS-DAP/SWD, so it cannot be used for this step.
- **Node.js >= 18 and npm** (`brew install node`, or nvm) — used to build the
  web UI.
- **jq** (`brew install jq`) — used by `scripts/api_smoke.zsh`.

## External Dependencies

Expected sibling checkout layout from the original workspace:

```text
awair/
  rewair/
  third_party/wiced-emw3165/
```

The WICED SDK fork is currently expected at:

```text
../third_party/wiced-emw3165
```

Override with `SDK_DIR=/path/to/wiced-emw3165` if needed (all `scripts/*.zsh`
that touch the SDK honor this).

## Build + Flash Firmware

```sh
scripts/build_local_bridge.zsh
```

This syncs `wiced/apps/rewair` and `wiced/platforms/AWAIR` into the external
SDK, then builds:

```text
rewair.local_bridge-AWAIR-FreeRTOS-LwIP-SDIO
```

Then flash it:

```sh
scripts/flash_local_bridge_probe_rs.zsh
```

By default this flashes the WICED bootloader and app, but preserves DCT/Wi-Fi
credentials. To intentionally wipe/reseed DCT:

```sh
FLASH_DCT=1 scripts/flash_local_bridge_probe_rs.zsh
```

## Web UI

The web UI (`webui/`) is a small Preact app, built with Vite, that talks to
the device over the [web API](#web-api-summary) below. It ships from this
repo unbuilt; you build it locally and flash the result to the device's
external SPI flash.

Local preview against a live device, no flashing required:

```sh
cd webui
npm ci
npm run dev
```

Then open the printed `localhost` URL with `?device=<device-ip>` appended,
e.g. `http://localhost:5173/?device=192.168.1.242`. The UI adapter
(`rw-api.js`) reads that query param and points all `fetch`/`EventSource`
calls at `http://<device-ip>` instead of same-origin. This cross-origin
fetch only works against a firmware build with CORS enabled
(`REWAIR_API_CORS_DEV`, see [Web API Summary](#web-api-summary)); the plain
`scripts/build_local_bridge.zsh` build does not currently define it, so add
`GLOBAL_DEFINES += REWAIR_API_CORS_DEV` to
`wiced/apps/rewair/local_bridge/local_bridge.mk` before building if you want
to use `npm run dev` against your device.

Build the production bundle (this is what actually ships to the device):

```sh
cd webui
npm run build
```

This runs `vite build` then packs `dist/` into `webui/webui.rwfs` (see
[SPI Flash](#spi-flash)).

Flash it to the device:

```sh
scripts/flash_webui.zsh
```

This builds the UI and writes `webui.rwfs` to the UI region of external SPI
flash via OpenOCD. **Stop any running `probe-rs` session first** — OpenOCD and
probe-rs cannot share the debug probe at the same time, and the script
refuses to proceed if it detects a `probe-rs` process running. It resets the
target back to the normal application (via probe-rs) once the OpenOCD session
ends, then polls the device's `/api/status` to confirm it rejoined Wi-Fi.

Once flashed, open `http://<device-ip>/` in a browser — the device serves the
UI itself, same-origin, no dev server needed.

## SPI Flash

The board has a Macronix MX25L1606E (2 MiB) external SPI flash. Pin mapping
(SPI1, empirically derived — differs from the reference WICED platform,
which is wired differently):

| Signal | Pin  |
| --- | --- |
| CS (software GPIO) | PA15 |
| MOSI | PA7 |
| MISO | PB4 |
| SCK  | PB3 |

Rewair owns the top 256 KiB (`0x1C0000`-`0x1FFFFF`) for the packed web UI
image (RWFS format — see `wiced/apps/rewair/local_bridge/rewair_uifs.h` and
the packer at `webui/scripts/pack-rwfs.mjs`). The address space below
`0x1C0000` holds the stock WLAN firmware blob and other factory-programmed
content; `flash_sflash_openocd.zsh` refuses to write there unless `FORCE=1`.

Console commands (over the serial debug console) and a dev-only HTTP route
both expose raw readback: `GET /api/debug/sflash?addr=<hex>&len=<n<=256>`
(compiled only under `REWAIR_API_CORS_DEV`) returns
`{"jedec":"...", "addr":"...", "data":"<hex>"}`.

## Smoke Test

```sh
REWAIR_IP=<device-ip> scripts/api_smoke.zsh
```

Runs 19 checks against a live device: web API status/scan/networks shape,
POST route validation (400/405/501), SSE, and the web UI being served from
sflash (`/`, `/app.js`, gzip headers, 404 on unknown paths).

Known flake: on the very first smoke run right after a boot, the Wi-Fi scan
check occasionally fails (roughly 1 run in 7) while the radio is still
settling. Rerun once if only that check fails; a fresh boot needs a moment
before scan results are reliable.

## Web API Summary

All routes below are in `wiced/apps/rewair/local_bridge/web_api.c`'s page
database. POST routes accept a `text/plain`-labeled JSON body (to keep
requests CORS-"simple") in addition to `application/json`.

| Route | Method | Purpose |
| --- | --- | --- |
| `/api/status` | GET | Full live status snapshot: score, sensors, wifi, time, settings. |
| `/api/scan` | GET | Trigger/read a Wi-Fi scan, returns an array of nearby APs. |
| `/api/networks` | GET | List saved Wi-Fi networks (DCT-backed). |
| `/api/events` | GET | Server-Sent Events stream of the same status snapshot, pushed on change or every 15s. |
| `/api/join` | POST | Join a Wi-Fi network (and save credentials). |
| `/api/forget` | POST | Remove a saved network. |
| `/api/priority` | POST | Reorder saved-network join priority. |
| `/api/settings` | POST | Update user settings (name, units, timezone, display mode). |
| `/api/time` | POST | Set/override the device's wall-clock time. |
| `/api/disp` | POST | Set the F103 display mode. |
| `/api/update` | POST | Firmware OTA — not implemented, returns 501. |
| `/api/reset` | POST | Clear saved Wi-Fi credentials and settings, then reboot. |
| `/api/debug/sflash` | GET | Dev-only (`REWAIR_API_CORS_DEV`) raw SPI-flash readback for debugging. |
| `/`, `/app.js`, `/rewair.css` | GET | The web UI itself, served from the RWFS image in external flash (or a small built-in fallback page for `/` if no image is flashed yet). |

## Current State

Both the web API and the self-contained web UI (served from the device's own
SPI flash) are working end to end. The F103 sensor board's `SENS` stream
still occasionally stalls after `REDY`/`TEST` on boot — intermittent, not
fully root-caused. See [docs/status.md](docs/status.md) for the current
detailed state and diagnostics.

If an experimental flash leaves the module unable to boot, see
`tools/recovery` for the restore path back to stock F411 firmware.
