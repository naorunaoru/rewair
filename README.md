# Rewair

Bring an original Awair Element back to life with local, open firmware.

Rewair replaces the firmware on the device's Wi-Fi module while keeping its
original sensors and display. Everything runs locally, including a small web
app served by the device itself.

**[Install Rewair](https://github.com/naorunaoru/rewair/wiki/Getting-Started)** ·
**[Read the Wiki](https://github.com/naorunaoru/rewair/wiki)** ·
**[Download a release](https://github.com/naorunaoru/rewair/releases)**

## Features

- Simple Wi-Fi setup from a captive portal
- Live readings and settings in the built-in web app
- MQTT and Home Assistant discovery
- Firmware updates from the browser, with rollback protection
- Optional read-only access over Bluetooth

## Build it yourself

Place this repository beside the
[WICED SDK](https://github.com/kamejoko80/wiced-emw3165), then run:

```sh
scripts/build_local_bridge.zsh
scripts/flash_local_bridge_probe_rs.zsh
```

The [building guide](https://github.com/naorunaoru/rewair/wiki/Building-and-Flashing)
covers the required tools, wiring, and flashing options.

## Learn more

The [Wiki](https://github.com/naorunaoru/rewair/wiki) has guides for setup,
development, networking, MQTT, the web API, SPI flash, and recovery. Technical
notes that need to evolve alongside the code remain in [`docs`](docs).

> Rewair is experimental firmware. **Back up your device's stock firmware
> before flashing**—Rewair does not distribute it. Start with the
> [installation guide](https://github.com/naorunaoru/rewair/wiki/Getting-Started).
