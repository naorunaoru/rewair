# Recovery Scripts

Back up and restore the stock EMW3165 internal and external flash around
experimental flashing.

## Step 0: back up your firmware

Do this **before flashing anything**. Stock firmware is not distributed with
Rewair. Without a dump from your own device, you cannot restore it to stock.

Download `rewair-sflash-loader.bin` from a Rewair release, then connect the
CMSIS-DAP probe. Use that loader to capture both memories:

```sh
STUB_IMAGE=/path/to/rewair-sflash-loader.bin \
  tools/recovery/backup_stock_emw3165.zsh ../dumps/my-element-stock
```

The backup directory contains:

- `f411-internal-flash.bin` — all 512 KiB of F411 internal flash
- `external-spi-flash.bin` — all 2 MiB of external SPI flash

Both are required: the stock firmware depends on data in external SPI flash.
The script verifies both file sizes and prints their SHA-256 checksums. Keep
the resulting directory somewhere safe.

## Restore scripts

If an experimental flash (such as the bare-metal console in
`tools/legacy/emw3165_sensor_console` or a WICED build) leaves the module
unable to boot, restore the dump you made at Step 0.

- `backup_stock_emw3165.zsh`: creates the complete internal-and-external backup.
- `restore_stock_emw3165.zsh`: restores both memories from that backup.
- `backup_stock_f411.zsh` / `restore_stock_f411.zsh`: internal-flash helpers.
- `restore_stock_boot_sector.zsh`: restores just the first 16 KiB boot
  sector. Enough to get the original WICED application booting again if the
  stock DCT/app sectors past the boot sector are still intact and only the
  boot sector was overwritten.

The combined restore writes external SPI flash first through the SRAM loader,
then writes F411 internal flash with `probe-rs` and resets the target.

## Backup layout

There is no stock image bundled in this repository. A suggested layout is:

```text
awair/
  rewair/                                   (this repo)
  dumps/my-element-stock/                     (your backup, outside the repo)
    f411-internal-flash.bin
    external-spi-flash.bin
  third_party/wiced-emw3165/
```

If you do not have a dump from your own known-good unit, do not proceed with
experimental flashing. Rewair does not provide a replacement stock image.

## Usage

```sh
STUB_IMAGE=/path/to/rewair-sflash-loader.bin \
  tools/recovery/restore_stock_emw3165.zsh ../dumps/my-element-stock
```

The `STUB_IMAGE` can be the `rewair-sflash-loader.bin` included in a release
bundle or a locally built `waf_sflash_write-NoOS-NoNS-AWAIR.elf`. Override
`SPEED` (default `950`) or `OPENOCD` if needed.
