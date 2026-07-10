# Recovery Scripts

Back up and restore the stock EMW3165 F411 firmware around experimental
flashing.

## Step 0: back up your firmware

Do this **before flashing anything**. Stock firmware is not distributed with
Rewair. Without a dump from your own device, you cannot restore it to stock.

With the CMSIS-DAP probe connected:

```sh
tools/recovery/backup_stock_f411.zsh ../dumps/my-element-stock-f411.bin
```

The script reads all 512 KiB of F411 internal flash, verifies the file size,
and prints its SHA-256 checksum. Keep the dump outside the repository and make
a second copy somewhere safe.

## Restore scripts

If an experimental flash (such as the bare-metal console in
`tools/legacy/emw3165_sensor_console` or a WICED build) leaves the module
unable to boot, restore the dump you made at Step 0.

Both scripts write raw binary data to F411 internal flash starting at
`0x08000000` via `probe-rs download --binary-format bin --base-address
0x08000000`, then reset the target.

- `backup_stock_f411.zsh`: creates the required full-device backup.
- `restore_stock_f411.zsh`: restores your full F411 backup.
- `restore_stock_boot_sector.zsh`: restores just the first 16 KiB boot
  sector. Enough to get the original WICED application booting again if the
  stock DCT/app sectors past the boot sector are still intact and only the
  boot sector was overwritten.

## Required input: `IMAGE`

Both restore scripts require `IMAGE=/path/to/*.bin` and refuse to run without
it. There is no stock image bundled in this repository. A suggested layout is:

```text
awair/
  rewair/                                   (this repo)
  dumps/my-element-stock-f411.bin            (your backup, outside the repo)
  third_party/wiced-emw3165/
```

If you do not have a dump from your own known-good unit, do not proceed with
experimental flashing. Rewair does not provide a replacement stock image.

## Usage

```sh
IMAGE=../dumps/my-element-stock-f411.bin tools/recovery/restore_stock_f411.zsh
IMAGE=/path/to/my-boot-sector.bin tools/recovery/restore_stock_boot_sector.zsh
```

Override `CHIP` (default `STM32F411CE`) or `SPEED` (default `950`) if needed.
