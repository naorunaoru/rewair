#!/usr/bin/env python3
"""Scan for the BI201, enumerate GATT, and listen on notify characteristics."""

import argparse
import asyncio
from collections.abc import Sequence

from bleak import BleakClient, BleakScanner


def hex_bytes(data: bytes | bytearray | memoryview) -> str:
    return bytes(data).hex(" ")


def printable(data: bytes | bytearray | memoryview) -> str:
    raw = bytes(data)
    return "".join(chr(byte) if 0x20 <= byte <= 0x7E else "." for byte in raw)


def print_advertisement(device, adv) -> None:
    name = adv.local_name or device.name or "<unnamed>"
    print(f"{name}  id={device.address}  rssi={adv.rssi} dBm")
    if adv.service_uuids:
        print("  advertised services:", ", ".join(adv.service_uuids))
    for company, data in adv.manufacturer_data.items():
        print(f"  manufacturer 0x{company:04x}: {hex_bytes(data)}")
    for uuid, data in adv.service_data.items():
        print(f"  service data {uuid}: {hex_bytes(data)}")


async def scan(name_prefix: str, seconds: float, show_all: bool):
    print(f"Scanning for {seconds:g} seconds...")
    found = await BleakScanner.discover(timeout=seconds, return_adv=True)
    rows = sorted(found.values(), key=lambda item: item[1].rssi, reverse=True)
    matches = []
    for device, adv in rows:
        name = adv.local_name or device.name or ""
        if show_all or name.startswith(name_prefix):
            print_advertisement(device, adv)
        if name.startswith(name_prefix):
            matches.append((device, adv))
    return matches


async def inspect(
    device,
    listen_seconds: float,
    write_text: str | None,
    write_characteristic: str,
) -> None:
    print(f"\nConnecting to {device.name or device.address}...")
    async with BleakClient(device, timeout=30.0) as client:
        print(f"Connected; ATT MTU: {client.mtu_size}")
        notify_characteristics = []

        for service in client.services:
            print(f"\nservice {service.uuid}  {service.description}")
            for characteristic in service.characteristics:
                properties = ",".join(characteristic.properties)
                print(
                    f"  characteristic {characteristic.uuid} "
                    f"handle=0x{characteristic.handle:04x} [{properties}] "
                    f"{characteristic.description}"
                )
                for descriptor in characteristic.descriptors:
                    print(
                        f"    descriptor {descriptor.uuid} "
                        f"handle=0x{descriptor.handle:04x} {descriptor.description}"
                    )
                if "notify" in characteristic.properties or "indicate" in characteristic.properties:
                    notify_characteristics.append(characteristic)

        if write_text is not None:
            transparent = client.services.get_characteristic(write_characteristic)
            if transparent is None:
                raise RuntimeError(
                    f"BI201 write characteristic {write_characteristic} not found"
                )
            payload = write_text.encode("utf-8")
            print(
                f"\nWriting {len(payload)} bytes to {transparent.uuid}: "
                f"{payload!r}"
            )
            await client.write_gatt_char(transparent, payload, response=True)

        if not notify_characteristics or listen_seconds <= 0:
            return

        def on_notification(characteristic, data: bytearray) -> None:
            print(
                f"notify {characteristic.uuid}: {hex_bytes(data)}  "
                f"|{printable(data)}|"
            )

        print(
            f"\nSubscribing to {len(notify_characteristics)} notify/indicate "
            f"characteristic(s) for {listen_seconds:g} seconds..."
        )
        active = []
        for characteristic in notify_characteristics:
            try:
                await client.start_notify(characteristic, on_notification)
                active.append(characteristic)
            except Exception as exc:  # Keep probing the remaining characteristics.
                print(f"  could not subscribe to {characteristic.uuid}: {exc}")

        await asyncio.sleep(listen_seconds)
        for characteristic in active:
            try:
                await client.stop_notify(characteristic)
            except Exception:
                pass


async def async_main(args: argparse.Namespace) -> int:
    matches = await scan(args.name_prefix, args.seconds, args.all)
    if not matches:
        print(f"No device name beginning with {args.name_prefix!r} was found.")
        return 1
    if args.scan_only:
        return 0

    await inspect(
        matches[0][0], args.listen, args.write, args.write_characteristic
    )
    return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name-prefix", default="AWAIR-001956")
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--listen", type=float, default=8.0)
    parser.add_argument("--write", help="UTF-8 text to write once to the selected BI201 characteristic")
    parser.add_argument(
        "--write-characteristic",
        default="2f2dfff5-2e85-649d-3545-3586428f5da3",
        help="GATT characteristic UUID used by --write (default: BI201 ...fff5)",
    )
    parser.add_argument("--scan-only", action="store_true")
    parser.add_argument("--all", action="store_true", help="print every discovered BLE device")
    return parser.parse_args(argv)


def main() -> int:
    return asyncio.run(async_main(parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
