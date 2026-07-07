# Recovery Scripts

Safety-critical restore path back to the stock EMW3165 F411 firmware image,
in case an experimental flash (e.g. the bare-metal console in
`tools/legacy/emw3165_sensor_console`, or a WICED build) leaves the module
unable to boot its original communications firmware.

Both scripts write raw binary data to F411 internal flash starting at
`0x08000000` via `probe-rs download --binary-format bin --base-address
0x08000000`, then reset the target.

- `restore_stock_f411.zsh`: restores a full known-good F411 image.
- `restore_stock_boot_sector.zsh`: restores just the first 16 KiB boot
  sector. Enough to get the original WICED application booting again if the
  stock DCT/app sectors past the boot sector are still intact and only the
  boot sector was overwritten.

## Required input: `IMAGE`

Both scripts require `IMAGE=/path/to/*.bin` and refuse to run without it.
There is no image bundled in this repo. The golden dumps this project has
used are expected at:

```text
awair/
  rewair/                                   (this repo)
  dumps/emw3165_f411_golden_*/              (golden F411 dumps, outside the repo)
  third_party/wiced-emw3165/
```

i.e. a sibling `dumps/emw3165_f411_golden_*/` directory in the project root,
next to this repo and the WICED SDK checkout, not tracked in git. If you do
not have a golden dump captured from a known-good unit, do not proceed with
experimental flashing until you do.

## Usage

```sh
IMAGE=/path/to/../../dumps/emw3165_f411_golden_*/full.bin tools/recovery/restore_stock_f411.zsh
IMAGE=/path/to/../../dumps/emw3165_f411_golden_*/boot_sector.bin tools/recovery/restore_stock_boot_sector.zsh
```

Override `CHIP` (default `STM32F411CE`) or `SPEED` (default `950`) if needed.
