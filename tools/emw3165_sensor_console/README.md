# EMW3165 F411 Sensor Console

Tiny bare-metal STM32F411 firmware for the Awair EMW3165 module.

It does not start WICED or Wi-Fi. It only:

- boots the F411 from internal flash;
- opens the debug console on stock WICED UART0 pins;
- opens the F103 sensor-board UART on stock WICED UART1 pins;
- lets the console send Awair UART frames to the F103;
- prints frames received from the F103;
- automatically answers complete `SENS` frames with `SCOR`, like the stock
  communications firmware did.

## Pins

Console, 115200 8N1:

- EMW3165 `PB6` / module pin 30 / `USART1_TX` -> USB-UART RX.
- EMW3165 `PA10` / module pin 29 / `USART1_RX` <- USB-UART TX.
- Common GND.

Sensor-board link, 115200 8N1:

- EMW3165 `PA2` / module pin 9 / `USART2_TX`.
- EMW3165 `PA3` / module pin 12 / `USART2_RX`.

These match the stock WICED platform table:

- WICED UART index 0: `USART1`, `PB6` TX, `PA10` RX.
- WICED UART index 1: `USART2`, `PA2` TX, `PA3` RX.

## Build

```sh
make -C tools/emw3165_sensor_console
```

Outputs:

- `build/emw3165_sensor_console.elf`
- `build/emw3165_sensor_console.bin`

## Flash

This overwrites the beginning of the F411 internal flash. Keep a known-good
restore path handy before doing this.

```sh
probe-rs download \
  --chip STM32F411CE \
  --protocol swd \
  --speed 950 \
  --non-interactive \
  --verify \
  tools/emw3165_sensor_console/build/emw3165_sensor_console.elf
```

or:

```sh
tools/emw3165_sensor_console/flash_sensor_console.zsh
```

To restore a full known-good image:

```sh
IMAGE=/path/to/known-good.bin tools/emw3165_sensor_console/restore_stock_f411.zsh
```

If the stock DCT/app sectors are still intact and only this tiny console image
was flashed, restoring just the first 16 KiB boot sector should be enough to
boot the original WICED application again:

```sh
IMAGE=/path/to/known-good-boot-sector.bin tools/emw3165_sensor_console/restore_stock_boot_sector.zsh
```

## Console Commands

- `help`
- `frame CMD [field ...]`
- `score VALUE [color]`
- `autoscore on|off`
- `net up`
- `net down`
- `time YYYYMMDDhhmmss`
- `raw HEXBYTES`
- `reset`

Examples:

```text
frame WVER
frame SVER
frame NETW net 1 rssi -40 ip 192.168.4.1 mac 70:88:6B:10:48:3B
time 20260607010101
net up
frame SCOR score 68 color amber index "" temp 0 humid 0 co2 0 voc 0 dust 0 sensor "" temp 0 humid 0 co2 0 voc 0 dust 0
score 68 amber
autoscore off
autoscore on
```

Frame payload fields are sent as NUL-separated argv entries, with the final NUL
included in the payload length, matching the stock Awair protocol.

The automatic `SENS` responder mirrors the stock field vocabulary: `score`,
`color`, empty `index`, per-sensor index values, empty `sensor`, and raw sensor
values. It prints a short `[auto] score=...` line for each complete `SENS` frame.
The scoring math is still table-free; the original Awair interpolation tables
need to be ported if exact stock scores matter.
