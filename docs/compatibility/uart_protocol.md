# F103 <-> EMW3165 UART Protocol

Working notes for the UART protocol between the STM32F103 sensor/display MCU and
the EMW3165 STM32F411 WICED module.

This is the vocabulary map used by Rewair's local bridge firmware in
`wiced/apps/rewair/local_bridge/local_bridge.c`.

## Frame Format

Frames use:

```text
*CMDLLLLLLLL\0field\0value\0...#
```

- `CMD`: four ASCII command bytes.
- `LLLLLLLL`: eight uppercase hexadecimal payload length bytes.
- Payload: NUL-separated fields.
- Trailer: `#`.

## Architecture

- F103 owns sensors, local display state, score display behavior, and the LED
  matrix.
- F411/EMW3165 owns Wi-Fi, time sync, local network state, and cloud-facing
  behavior in the stock firmware.
- Rewair replaces the F411/EMW3165 application with a local bridge while keeping
  the stock F103 firmware in place.

## Direction Groups

Likely F411 to F103 commands:

- `NETW`: network state, RSSI, IP, MAC.
- `TIME`: current time.
- `TINF`: timezone/DST information.
- `DISP`: display mode/preference.
- `SLEP`: sleep/display dimming state.
- `NOTI`: notification command.
- `SUND`: notification sound setting.
- `KNCK`: knocking gesture setting.
- `ALRM`: alarm command.
- `TUTO`: tutorial/demo command.
- `MESG`: text message command.
- `LEDS`: LED color/blink command.
- `FDEF`: factory default/reset command.
- `DEBG`: debug command.
- `HASH`, `UPGR`, `DOWN`: special update/control path.

Likely F103 to F411 messages:

- `SENS`: raw/processed sensor readings.
- `SCOR`: current score and derived sensor index/color values.
- `REDY`: sensor board ready.
- `TEST`: test result/status.
- `TEND`: test end.
- `VERS`: sensor firmware or product version string.
- `SCCD`: score/color code status.
- `DVID`: device id.
- `MACA`: MAC address.
- `BTST`: boot status or board test.
- `RUNN`: running/heartbeat state.
- `SSID`: Wi-Fi SSID status/request result.
- `NETD`: network down request/status.
- `NETR`: network restart request/status.
- `BOOT`: reboot/boot command.
- `WVER`: Wi-Fi firmware version request/result.
- `SVER`: sensor firmware version request/result.
- `FNOW`: firmware update now.
- `WLOG`: Wi-Fi log/status.
- `WSTA`: Wi-Fi state/status.

Some commands are request/response style, so "direction" means the side with
clearly observed ownership in normal boot and telemetry flows, not the only
direction the token can ever appear.

## Vocabulary Table

| Token | Likely direction | Purpose | Payload hints | Notes |
| --- | --- | --- | --- | --- |
| `NETW` | F411 -> F103 | Network state update | `net`, `rssi`, `ip`, `mac` | Lets F103 know whether networking is up and what address/MAC to display/log. |
| `TIME` | F411 -> F103 | Wall-clock time | `time` | Needed for clock display and timestamped behavior. |
| `TINF` | F411 -> F103 | Timezone/DST information | `offs`, `dst_on`, `dst_off`, `dst_offs` | Rewair currently sends Lisbon timezone data. |
| `HASH` | F411 -> F103, special | Firmware update validation | Update-path fields | Deferred until update behavior is deliberately implemented. |
| `UPGR` | F411 -> F103, special | Firmware update transfer/control | Update-path fields | Deferred for the MVP. |
| `SCOR` | Both / display feedback | Score and interpreted sensor values | `score`, `color`, `index`, `sensor`, `temp`, `humid`, `co2`, `voc`, `dust` | F103 accepts this to update the visible score. Empty `index` and `sensor` separators matter. |
| `DISP` | F411 -> F103 | Display mode/preference | `mode` | Live test: `frame DISP mode clock` shows a clock; `frame DISP mode default` blanks the display on the current setup. |
| `SLEP` | F411 -> F103 | Sleep or low-brightness mode | TBD | Not needed for the first local bridge. |
| `NOTI` | F411 -> F103 | Notification | color/message/sound/display/mode-style fields | Related to user-facing notifications. |
| `SUND` | F411 -> F103 | Notification sound | TBD | Not needed for the first local bridge. |
| `KNCK` | F411 -> F103 | Knock gesture setting/action | TBD | Not needed for the first local bridge. |
| `ALRM` | F411 -> F103 | Alarm | TBD | Not needed for the first local bridge. |
| `TUTO` | F411 -> F103 | Tutorial/demo | TBD | Not needed for the first local bridge. |
| `NOPR` | Internal/no-op | No operation | None | Filler/default behavior. |
| `MESG` | F411 -> F103 | Display text message | `message`, `timeout` | Try `frame MESG message hello timeout 5`; `text`/`duration` are not parser keys. |
| `LEDS` | F411 -> F103 | LED color/blink | `color`, `blink` | Live test: `frame LEDS color white blink 0` sets a steady white LED. |
| `DOWN` | F411 -> F103, special | Download/update state or network-down event | TBD | Ambiguous; deferred for MVP. |
| `REDY` | F103 -> F411 | Sensor board ready | `version` observed | Important boot synchronization token. |
| `TEST` | F103 -> F411 | Test command/result | `mode` observed | Boot/test lifecycle token. |
| `TEND` | F103 -> F411 | Test end | TBD | Manufacturing/self-test vocabulary. |
| `FDEF` | F411 -> F103 | Factory default reset | TBD | Keep disabled until reset behavior is understood. |
| `SENS` | F103 -> F411 | Sensor readings | `temp`, `humid`, `co2`, `voc`, `dust`, `light`, raw values | Primary payload for Home Assistant/MQTT integration. |
| `VERS` | F103 -> F411 | Version string | version text | May overlap with `SVER`/`WVER` request-specific tokens. |
| `SCCD` | F103 -> F411 | Score/color code data | TBD | Name suggests score color code. |
| `DVID` | F103 -> F411 | Device id | identity text | Local firmware can probably keep or ignore. |
| `MACA` | F103 -> F411 | MAC address | MAC text | Identity/status path. |
| `BTST` | F103 -> F411 | Boot/self-test status | TBD | Boot/test lifecycle token. |
| `RUNN` | F103 -> F411 | Running state/heartbeat | TBD | Useful liveness signal. |
| `SSID` | F103 -> F411 | SSID status/request | SSID text | Possibly lets F103 display current Wi-Fi name. |
| `DEBG` | F411 -> F103 | Debug mode/log command | TBD | Useful for local experiments, not needed for MVP. |
| `NETD` | F103 -> F411 | Network down request/status | TBD | Could request network teardown/reconnect. |
| `NETR` | F103 -> F411 | Network restart request/status | TBD | Could request network restart. |
| `BOOT` | F103 -> F411 | Reboot/boot command | TBD | Likely asks Wi-Fi module to reboot, or reports boot cause. |
| `WVER` | F103 -> F411 | Wi-Fi firmware version | version text | Custom firmware should answer with its own version. |
| `SVER` | F103 -> F411 | Sensor firmware version | version text | Sensor identity/status. |
| `FNOW` | F103 -> F411 | Firmware update now | TBD | Deferred for MVP. |
| `WLOG` | F103 -> F411 | Wi-Fi log/status | TBD | Diagnostics. |
| `WSTA` | F103 -> F411 | Wi-Fi state/status | TBD | Present on the F103 side; not yet important to Rewair. |

## Minimal Local Bridge Scope

For a cloud-free firmware, keep the useful center of the protocol implemented
and tested:

1. Parse inbound from F103:
   - `REDY`, `RUNN`, `BTST` for lifecycle.
   - `SENS` for local telemetry.
   - `SCOR` if mirrored by the F103.
   - `NETD`, `NETR`, `BOOT` for network/reboot requests.
   - `WVER`, `SVER`, `SSID`, `MACA`, `DVID` for identity/status queries.
2. Send outbound to F103:
   - `NETW` whenever Wi-Fi or local network state changes.
   - `TIME` after SNTP/local RTC sync and periodically after that.
   - `TINF` when timezone/DST configuration is known.
   - Optionally `DISP`, `MESG`, and `LEDS` for local controls.
3. Defer for later:
   - `HASH`, `UPGR`, `DOWN`, `FNOW` until the update path is intentionally
     supported.
   - Manufacturing/test commands unless the board refuses to run without them.

This is the current Rewair direction: make the EMW3165 act like a local
MQTT/Home Assistant bridge while preserving the F103 sensor/display firmware.

## Open Questions

- Whether `SCOR` is sent spontaneously by the F103 or only built internally on
  one side after `SENS`.
- Whether the F103 expects a boot script of `NETW`, `TIME`, `TINF`, version
  answers, or if those can arrive lazily.
- Which commands are mandatory to silence errors on the F103 display.
- Whether `DOWN` means network-down, download state, or both depending on
  payload/context.
