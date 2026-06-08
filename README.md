# Rewair

Custom drop-in firmware work for reviving original Awair Element units by
replacing the EMW3165 radio-module firmware.

What's inside of the Element?

- ST STM32F103VET6 — sensor board MCU
- ST STM32F103RBT6 — front panel daughterboard MCU
- Glead ZYM-BI201 — BLE 4.0 (TI CC2540, originally used for BLE GATT pairing)
- MXCHIP EMW3165 — WiFi module (has STM32F411 + Broadcom WiFi inside, the main interest of this repo)
- a bunch of sensors (Telaire T6703, Sharp GP2Y10, FIS QS-01, SHT30)

## Repository Layout

- `wiced/apps/rewair/local_bridge`: WICED 3.3.1 app currently used for radio
  bring-up and F103 UART protocol work.
- `wiced/platforms/AWAIR`: Awair/EMW3165 WICED platform pin map.
- `scripts`: wrappers for syncing into the external WICED SDK, building, and
  flashing.
- `tools/f103_debug`: SWD/GDB helpers for F103-side protocol tracing.
- `tools/emw3165_sensor_console`: small bare-metal F411 fallback console used
  before WICED networking was working.
- `docs`: status and compatibility notes.

Generated build output, local lab artifacts, and the WICED SDK are intentionally
not tracked here.

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

Override with `SDK_DIR=/path/to/wiced-emw3165` if needed.

## Build

```sh
scripts/build_local_bridge.zsh
```

This syncs `wiced/apps/rewair` and `wiced/platforms/AWAIR` into the external SDK,
then builds:

```text
rewair.local_bridge-AWAIR-FreeRTOS-LwIP-SDIO
```

## Flash

Located on the opposite side of EMW3165 are 4 test points. TP103/TP104 is the debug USART, TP80/TP81 is SWD.

```sh
scripts/flash_local_bridge_probe_rs.zsh
```

By default this flashes the WICED bootloader and app, but preserves DCT/Wi-Fi
credentials. To intentionally wipe/reseed DCT:

```sh
FLASH_DCT=1 scripts/flash_local_bridge_probe_rs.zsh
```

## Current State

See [docs/status.md](docs/status.md). Short version: Wi-Fi, DCT credential
persistence, DHCP, SNTP, time sync, and basic F103 frame TX/RX are in
place. The current unresolved issue is that under WICED the F103 emits
`REDY/TEST`, then often stops sending `SENS`.

Without Wi-Fi connected the sensor board reliably (more or less) sends data to the radio module every 5 seconds. 
