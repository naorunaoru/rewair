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
  - `rewair_net_mode`: pure-STA-or-pure-AP network mode state machine (setup
    AP bring-up/teardown, boot-autojoin fallback, self-heal). See [AP Setup
    Mode](#ap-setup-mode) below.
  - `rewair_mqtt` / `rewair_mqtt_packet`: persisted MQTT broker settings,
    MQTT 3.1.1 transport, reconnect/last-will handling, retained telemetry,
    and Home Assistant discovery.
  - `rewair_console`: legacy console source retained for reference but no
    longer built, because its USART1 is now dedicated to the BI201.
  - `rewair_score`: sensor-index-to-score math.
  - `rewair_walltime`: epoch/wall-clock helpers.
  - `rewair_fmt`: string/IP/MAC/SSID formatting helpers; host-testable core
    with firmware-only parts guarded by `REWAIR_HOST_BUILD`.
  - `rewair_json`: minimal JSON emitter and request-body reader (uses the
    vendored `jsmn.c`/`jsmn.h` parser).
  - `rewair_api`: transport-neutral operation dispatch and JSON serializers
    shared by HTTP and BLE.
  - `rewair_ble` / `rewair_ble_proto`: production BI201 UART driver plus the
    framed, CRC-protected BLE API transport. USART1 is dedicated to the BI201.
  - `rewair_drops`: dropped-frame counters.
  - `rewair_uifs`: reader for the packed web-UI image format (RWFS) on
    external SPI flash; host-testable, no WICED includes.
  - `rewair_ota`: chunked upload, staged-image verification, and trial-boot
    confirmation for F411 OTA. See [F411 firmware OTA](docs/ota.md).
  - `web_api.c`/`.h`: HTTP page database, all `/api/...` JSON routes, and
    Server-Sent Events.
  - `web_ui.c`/`.h`: serves the web UI (`/`, `/app.js`, `/rewair.css`) out of
    the RWFS image in external flash, with a small built-in fallback page.
- `wiced/platforms/AWAIR`: Awair/EMW3165 WICED platform pin map, including the
  external SPI flash pin mapping and the power-loss-safe OTA boot copier.
- `webui/`: the web UI itself — Preact + htm, built with Vite, plain
  npm (no framework CLI). See [Web UI](#web-ui) below.
- `tests/host/`: 10 host-buildable test suites (`test_drops`, `test_tz`,
  `test_req`, `test_status`, `test_uifs`, `test_walltime`, `test_score`,
  `test_ota`, `test_mqtt_packet`, `test_ble_proto`) that
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
- `wiced/apps/rewair/bi201_probe` and `tools/bi201`: standalone F411-to-BI201
  UART probe, SWD mailbox reader, and macOS BLE/GATT/API clients. See
  [`docs/bi201.md`](docs/bi201.md).
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
  `tools/f103_debug`'s GDB scripts or build the optional legacy console
  (`tools/legacy/emw3165_sensor_console`), those need a standalone
  `arm-none-eabi` toolchain on `PATH` — e.g. `brew install --cask gcc-arm-embedded`.
  Not required for the core build/flash/UI/smoke workflow below.)
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
the device over HTTP or Web Bluetooth. It ships from this repo unbuilt; you
can flash the result to the device's external SPI flash or publish `dist/` on
an HTTPS static host for BLE access before the device has Wi-Fi.

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
fetch only works against a firmware build with CORS enabled — dev CORS and
the `/api/debug/sflash` route are enabled out of the box because
`wiced/apps/rewair/local_bridge/web_api.h` defines `REWAIR_API_CORS_DEV` at
line 11 (comment: "ON during phase 1 bring-up; turn off for release"). To
build a release without them, comment out that define in `web_api.h`.

For BLE, open the same localhost page without a `device` query parameter and
use **Connect over Bluetooth**. A normal no-Wi-Fi deployment must be hosted
from a secure HTTPS origin; the Vite bundle uses relative asset URLs and can
be published directly to GitHub Pages or another static host. After the Pages
workflow lands on `main` and GitHub Pages is configured to use GitHub Actions,
this repository publishes it at <https://naorunaoru.github.io/rewair/>. BLE is
currently read-only while an authenticated session design is added; see
[docs/bi201.md](docs/bi201.md).

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
the packer at `webui/scripts/pack-rwfs.mjs`). F411 OTA uses `0x000000`–
`0x100FFF` for staging, a known-good backup, and its state journal. The
firmware write guard rejects every write touching the UI region. See
[docs/ota.md](docs/ota.md) for the exact map and rollback behavior.

Console commands (over the serial debug console) and a debug HTTP route
both expose raw readback: `GET /api/debug/sflash?addr=<hex>&len=<n<=256>`
(compiled under `REWAIR_API_CORS_DEV`, currently enabled by default) returns
`{"jedec":"...", "addr":"...", "data":"<hex>"}`.

## Smoke Test

```sh
REWAIR_IP=<device-ip> scripts/api_smoke.zsh
```

Runs 21 checks against a live device: web API status/scan/networks/MQTT shape,
POST route validation (400/405), SSE, split-packet POST body handling,
and the web UI being served from sflash (`/`, `/app.js`, gzip headers, 404
on unknown paths).

Known flake: the split-packet POST-body check (`scripts/check_post_body.py`)
intermittently times out connecting during full-suite runs (more often when
suites run back-to-back) but passes reliably standalone. Working theory: dead
SSE subscriber sockets hold HTTP-daemon connection slots until the next state
broadcast reaps them. If only that check fails, rerun it standalone to confirm.

## Web API Summary

All routes below are in `wiced/apps/rewair/local_bridge/web_api.c`'s page
database. POST routes accept a `text/plain`-labeled JSON body (to keep
requests CORS-"simple") in addition to `application/json`.

Capabilities, status, scan, and networks are implemented by the shared
`rewair_api.c` core and are also available over BLE with the same JSON and
status semantics.

| Route | Method | Purpose |
| --- | --- | --- |
| `/api/capabilities` | GET | Transport/API version and currently enabled operations. |
| `/api/status` | GET | Full live status snapshot: score, sensors, wifi, time, settings. |
| `/api/scan` | GET | Trigger/read a Wi-Fi scan, returns an array of nearby APs. |
| `/api/networks` | GET | List saved Wi-Fi networks (DCT-backed). |
| `/api/events` | GET | Server-Sent Events stream of the same status snapshot, pushed on change or every 15s. |
| `/api/join` | POST | Join a Wi-Fi network (and save credentials). In AP setup mode (see [AP Setup Mode](#ap-setup-mode)), stores credentials only (no live join probe) and triggers an async switch to STA. |
| `/api/forget` | POST | Remove a saved network. |
| `/api/priority` | POST | Reorder saved-network join priority. |
| `/api/settings` | POST | Update user settings (name, units, timezone, display mode). |
| `/api/mqtt` | GET/POST | Read MQTT runtime/config status or update broker, authentication, topic, and Home Assistant discovery settings. The saved password is never returned. |
| `/api/time` | POST | Set/override the device's wall-clock time. |
| `/api/disp` | POST | Set the F103 display mode. |
| `/api/update` | POST | Chunked F411 firmware OTA used by the web portal: begin, sequential data chunks, verify/commit, reboot. See [docs/ota.md](docs/ota.md). |
| `/api/reset` | POST | Clear all saved Wi-Fi credentials and settings, then reboot into AP setup mode. |
| `/api/debug/sflash` | GET | Debug route (compiled under `REWAIR_API_CORS_DEV`, currently enabled by default — see `web_api.h`) raw SPI-flash readback for debugging. |
| `/`, `/app.js`, `/rewair.css` | GET | The web UI itself, served from the RWFS image in external flash (or a small built-in fallback page for `/` if no image is flashed yet). |

## AP Setup Mode

The device is always in exactly one radio role — joined to a home network as
a station (STA), or hosting its own setup access point (AP) — never both at
once. `rewair_net_mode`
(`wiced/apps/rewair/local_bridge/rewair_net_mode.c`) owns the transitions:

- No stored Wi-Fi network at all: boot straight into the setup AP, no STA
  join attempted.
- Stored network(s) exist, but boot-time autojoin fails 3 consecutive
  attempts: fall back to the setup AP. A steady-state STA drop (link lost
  after having been up) does not count toward this and keeps retrying
  autojoin indefinitely instead of falling back.

The setup AP is an open network named `rewair-setup-<xxxx>`, where `<xxxx>`
is the last two bytes of the device's Wi-Fi MAC in lowercase hex (e.g.
`rewair-setup-30f4`), on channel 6, served at `192.168.0.1/24` via the WICED
internal DHCP server. While it is up, any HTTP request whose path isn't
exactly `/`, `/app.js`, or `/rewair.css` gets a DNS redirect (WICED's
`DNS_redirect` daemon) plus a `302` to `/`, so phones/laptops pop the
captive-portal sign-in page automatically. STA-mode HTTP behavior is
unchanged (no redirect, plain 404 for unknown paths).

Posting to `/api/join` while in setup/fallback AP mode stores the given
SSID/password only — security and channel are resolved from the `/api/scan`
result cache, not a live join probe — flushes an immediate success response,
then asynchronously switches to STA on the network thread's next tick
(within ~1 s). `POST /api/reset` clears saved credentials/settings and
reboots back into the setup AP, from either mode.

The fallback AP self-heals: every ~5 minutes it retries STA autojoin from
the saved credentials, unless a client is currently associated to the setup
AP (deferred so an in-progress phone/laptop configuration session isn't torn
down mid-flow).

The web API/UI status contract reflects AP mode already: `/api/status`'s
`wifi` object reports `"mode":"ap"` plus `ap_ssid`, `ap_ip`, and
`saved_count` (vs. `"mode":"sta"` plus `ssid`/`rssi`/`ip`/`gw`/`dns` when
joined).

## MQTT and Home Assistant

In the device portal, open **Settings → MQTT & Home Assistant → Configure**,
then enter the hostname/IP and port of the MQTT broker used by Home Assistant.
Username/password authentication is optional. Rewair currently supports MQTT
3.1.1 over unencrypted TCP (normally port `1883`), so the broker should stay on
a trusted local network; MQTT over TLS is not included in this firmware build.

With Home Assistant discovery enabled (the default), Rewair publishes retained
discovery records below the `homeassistant` prefix and creates seven entities:
temperature, humidity, carbon dioxide, VOC, dust, illuminance, and the Rewair
air-quality score. Every entity shares a stable device identifier derived from
the Wi-Fi MAC, so a rename or reboot does not create duplicates.

Telemetry is a retained JSON object at:

```text
rewair/rewair_<12-hex-mac>/state
```

The adjacent `availability` topic carries retained `online`/`offline` state and
is also configured as the MQTT last will. A custom state-topic prefix and Home
Assistant discovery prefix can be set in the same dialog. Broker credentials
and settings are persisted in the WICED DCT; factory reset clears them and,
when the broker is reachable, removes retained discovery records before
rebooting.

For a quick broker-side check:

```sh
mosquitto_sub -h <broker> -u <user> -P '<password>' -v \
  -t 'rewair/#' -t 'homeassistant/sensor/#'
```

## Current State

The web API, self-contained HTTP/BLE UI, read-only BI201 API, AP setup portal,
and F411 OTA implementation are built and bench-verified. BLE mutations remain
locked pending application-layer authentication, and OTA's destructive
power-loss/rollback gauntlet is still a bench checkpoint. The F103 sensor board's `SENS` stream
still occasionally stalls after `REDY`/`TEST` on boot — intermittent, not
fully root-caused. See [docs/status.md](docs/status.md) for the current
detailed state and diagnostics.

If an experimental flash leaves the module unable to boot, see
`tools/recovery` for the restore path back to stock F411 firmware.
