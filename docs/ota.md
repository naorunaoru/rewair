# F411 firmware OTA

Rewair can update the STM32F411 application from the web portal with the raw
`.bin` produced by `scripts/build_local_bridge.zsh`. The deterministic RWFS
web UI is linked into that F411/WICED application image, so one OTA installs a
matching firmware and portal. F103 firmware remains separate.

## Architecture choice

The implementation uses the approved design's custom-minimal option. The
WICED-native WAF OTA path was rejected for this board because it expects an ELF
plus a programmed apps lookup table, performs no staged CRC enforcement, and
has no automatic trial rollback. Rewair instead owns fixed staging, backup,
and journal regions and runs a small raw-binary copier from the existing WAF
bootloader slot. This also avoids modifying the external WICED SDK fork: all
bootloader sources live under the tracked `wiced/platforms/AWAIR` overlay.

## One-time bootstrap

The OTA copier lives in the WICED bootloader region. A device running an older
Rewair bootloader must receive the OTA-capable bootloader and application once
over SWD:

```sh
scripts/build_local_bridge.zsh
scripts/flash_local_bridge_probe_rs.zsh
```

The flash script updates the bootloader, application, and embedded portal while
preserving DCT/Wi-Fi credentials by default. Do not set `FLASH_DCT=1` for this
bootstrap unless a credential reset is intentional.

The WICED WLAN firmware is kept in external SPI flash. Build the lookup table
alongside the application, then provision and fully read back both external
images before flashing an application that depends on them:

```sh
scripts/build_local_bridge.zsh
scripts/flash_wifi_firmware_sflash.zsh
scripts/flash_local_bridge_probe_rs.zsh
```

`flash_wifi_firmware_sflash.zsh` saves the previous `0x101000–0x135FFF`
contents under `/tmp`, writes the 4 KiB WICED lookup table and 210412-byte
BCM43362A2 image, then verifies the whole payload byte for byte. On first boot,
the application atomically updates only `DCT_WIFI_FIRMWARE_INDEX` from the old
lookup-table address to `0x101088`; saved Wi-Fi credentials are unchanged.

After that bootstrap, open the device UI, choose Settings → Firmware →
Update, and select:

```text
../third_party/wiced-emw3165/build/
  rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO/binary/
  rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO.bin
```

The image must be 8–475136 bytes and have a valid STM32F411 vector table for
the Rewair app region at `0x0800C000`.

For a headless bench test, the repository also includes an uploader that uses
the same begin/chunk/commit protocol as the portal:

```sh
python3 scripts/ota_upload.py 192.168.1.242 \
  ../third_party/wiced-emw3165/build/rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO/binary/rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO.bin
```

## Flash layout

| Region | Purpose |
| --- | --- |
| `0x000000–0x07FFFF` | Incoming F411 image; header at `0x000000`, data at `0x000100` |
| `0x080000–0x0FFFFF` | Full 464 KB known-good internal-app backup |
| `0x100000–0x100FFF` | Append-only, CRC-protected OTA state journal |
| `0x101000–0x101FFF` | WICED apps lookup table |
| `0x102000–0x135FFF` | BCM43362A2 Wi-Fi firmware (52 sectors, 210412 data bytes) |
| `0x136000–0x1FFFFF` | Reserved/free; the web UI no longer occupies external flash |

The browser calculates CRC32, begins a session, then POSTs sequential 16 KiB
chunks to `/api/update`. Chunking is required because the WICED 3.3.1 HTTP
daemon represents request-body length with 16 bits. The device calculates its
own running CRC, reads the staged bytes back, validates the vector table, writes
the staging header last, and only then records the image as ready to apply.

## Power-loss and rollback behavior

- A partial upload has no valid header or journal record and cannot affect the
  running app.
- On the next boot after a verified upload, the bootloader copies the complete
  current 464 KB app region to the backup slot and verifies its CRC before
  erasing internal app flash.
- The app region includes the packed RWFS web UI, so install and rollback always
  keep firmware and portal assets on the same release.
- The staged copy is idempotent. Power loss during the internal-flash copy
  leaves the journal at `BACKUP_READY`, so the next boot repeats the copy.
- A new image is a trial until its embedded RWFS passes every integrity check
  and its HTTP server has been running for 15 seconds. A six-minute system
  monitor covers slow STA-fallback boots. Three unconfirmed trial boots cause
  the bootloader to restore and CRC-verify the backup.
- Image authenticity is not checked. This is CRC/size/vector safety on a trusted
  local network, not signed firmware.

## AWAIR platform constraints

- The STM32F411CE has 512 KiB of internal flash. Its application occupies
  sectors 3–7; the bootloader uses an F411-specific erase routine rather than
  WICED 3.3.1's generic F4 helper, which incorrectly continues through sectors
  8–11 and hangs on this part.
- `WICED_DISABLE_MCU_POWERSAVE` is enabled for this platform. This selects the
  ordinary WFI idle hook with SysTick running; the SDK's tickless idle path was
  observed failing to wake from a 9 ms sleep and eventually tripping the
  hardware watchdog. Wi-Fi and the HTTP server remain active normally.
- SSE returns one complete status event per connection and asks EventSource to
  reconnect after 2.5 seconds. The stock HTTP daemon recycles socket objects
  without an application disconnect callback, so retaining raw socket pointers
  for a long-lived broadcaster is unsafe.

## Bench acceptance

Run these on the disposable bench unit before treating OTA as field-ready:

1. Bootstrap the OTA bootloader and combined app/portal over SWD, preserving DCT.
2. Upload a byte-identical `.bin`; confirm reboot, the same firmware version,
   portal availability, and `scripts/api_smoke.zsh` success.
3. Bump `REWAIR_FW_VERSION`, rebuild, upload from the portal, and confirm the
   new version appears after reboot.
4. Interrupt power during upload; confirm the old app still boots.
5. Interrupt power while the bootloader is copying; confirm the next boot
   repeats the copy and reaches the portal.
6. Boot an intentionally non-healthy test image; confirm three failed trial
   boots restore the prior version.

Keep the SWD recovery probe connected for steps 4–6. The final backstop remains
the complete internal-and-external backup restored with
`tools/recovery/restore_stock_emw3165.zsh`.

On 2026-07-09, step 2 passed on the bench device at `192.168.1.242`: a
426264-byte image with CRC32 `8af76b1b` was staged in 16 KiB chunks, installed,
entered trial boot 1/3, confirmed after the HTTP health window, and booted
normally after a subsequent reset. The 20-check API smoke suite passed on the
OTA-installed image.
