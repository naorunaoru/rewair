#!/usr/bin/env python3
"""Request capabilities and status through the BI201 transparent GATT link."""

import argparse
import asyncio
import json
import struct
import zlib

from bleak import BleakClient, BleakScanner


SERVICE_UUID = "2f2dfff0-2e85-649d-3545-3586428f5da3"
NOTIFY_UUID = "2f2dfff4-2e85-649d-3545-3586428f5da3"
WRITE_UUID = "2f2dfff5-2e85-649d-3545-3586428f5da3"
FRAME_REQUEST = 1
FRAME_RESPONSE = 2
FRAME_ACK = 4
FLAG_FIRST = 0x01
FLAG_MORE = 0x02
OP_CAPABILITIES = 1
OP_STATUS = 2
OP_SCAN = 3
OP_NETWORKS = 4


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    output = bytearray()
    offset = 0
    while offset < len(data):
        code = data[offset]
        offset += 1
        if code == 0 or offset + code - 1 > len(data):
            raise ValueError("bad COBS frame")
        output.extend(data[offset : offset + code - 1])
        offset += code - 1
        if code != 0xFF and offset < len(data):
            output.append(0)
    return bytes(output)


def encode_frame(
    frame_type: int, operation: int, request_id: int, sequence: int = 0, flags: int = 0
) -> bytes:
    header = struct.pack(
        "<BBBBHHHH", 1, frame_type, flags, operation, request_id, sequence, 0, 0
    )
    crc = struct.pack("<I", zlib.crc32(header) & 0xFFFFFFFF)
    return cobs_encode(header + crc) + b"\x00"


def decode_frame(encoded: bytes) -> tuple[int, int, int, int, int, int, bytes]:
    raw = cobs_decode(encoded)
    if len(raw) < 16:
        raise ValueError("short frame")
    version, frame_type, flags, operation, request_id, sequence, status, length = (
        struct.unpack_from("<BBBBHHHH", raw)
    )
    if version != 1 or len(raw) != 12 + length + 4:
        raise ValueError("bad frame header")
    expected = struct.unpack_from("<I", raw, 12 + length)[0]
    if zlib.crc32(raw[: 12 + length]) & 0xFFFFFFFF != expected:
        raise ValueError("bad frame CRC")
    return frame_type, flags, operation, request_id, sequence, status, raw[12 : 12 + length]


async def find_device(name_prefix: str, seconds: float):
    found = await BleakScanner.discover(timeout=seconds, return_adv=True)
    matches = []
    for device, advertisement in found.values():
        name = advertisement.local_name or device.name or ""
        services = [item.lower() for item in advertisement.service_uuids]
        if name.startswith(name_prefix) or SERVICE_UUID in services:
            matches.append((advertisement.rssi, device, name))
    if not matches:
        raise RuntimeError("BI201 advertisement not found")
    matches.sort(key=lambda item: item[0], reverse=True)
    return matches[0][1], matches[0][2]


class ApiSession:
    def __init__(self, client: BleakClient, verbose_frames: bool = True):
        self.client = client
        self.verbose_frames = verbose_frames
        self.encoded = bytearray()
        self.future = None
        self.operation = 0
        self.request_id = 0
        self.chunks: list[bytes] = []
        self.expected_sequence = 0
        self.response_status = 0

    async def start(self) -> None:
        await self.client.start_notify(NOTIFY_UUID, self.on_notification)
        await asyncio.sleep(1.0)

    async def stop(self) -> None:
        await self.client.stop_notify(NOTIFY_UUID)

    async def send_ack(self, operation: int, request_id: int, sequence: int) -> None:
        wire = encode_frame(FRAME_ACK, operation, request_id, sequence)
        for offset in range(0, len(wire), 20):
            await self.client.write_gatt_char(
                WRITE_UUID, wire[offset : offset + 20], response=True
            )

    def on_notification(self, _characteristic, data: bytearray) -> None:
        for byte in data:
            if byte != 0:
                self.encoded.append(byte)
                if len(self.encoded) > 50:
                    self.encoded.clear()
                continue
            if not self.encoded:
                continue
            try:
                frame = decode_frame(bytes(self.encoded))
            except ValueError:
                self.encoded.clear()
                continue
            self.encoded.clear()
            frame_type, flags, frame_op, frame_id, sequence, status, payload = frame
            if self.verbose_frames:
                print(
                    f"frame type={frame_type} flags=0x{flags:02x} op={frame_op} "
                    f"id={frame_id} seq={sequence} status={status} bytes={len(payload)}"
                )
            if (
                self.future is None
                or frame_type != FRAME_RESPONSE
                or frame_op != self.operation
                or frame_id != self.request_id
            ):
                continue
            if flags & FLAG_FIRST:
                self.chunks = []
                self.expected_sequence = 0
                self.response_status = status
            if sequence != self.expected_sequence or status != self.response_status:
                if not self.future.done():
                    self.future.set_exception(RuntimeError("response fragment mismatch"))
                return
            self.chunks.append(payload)
            self.expected_sequence += 1
            if flags & FLAG_MORE:
                asyncio.get_running_loop().create_task(
                    self.send_ack(frame_op, frame_id, sequence)
                )
                continue
            try:
                body = json.loads(b"".join(self.chunks))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                if not self.future.done():
                    self.future.set_exception(exc)
                return
            if self.response_status < 200 or self.response_status >= 300:
                if not self.future.done():
                    self.future.set_exception(
                        RuntimeError(body.get("error", str(self.response_status)))
                    )
                return
            if not self.future.done():
                self.future.set_result(body)

    async def request(self, operation: int, request_id: int) -> dict:
        if self.future is not None:
            raise RuntimeError("request already in progress")
        self.future = asyncio.get_running_loop().create_future()
        self.operation = operation
        self.request_id = request_id
        self.chunks = []
        self.expected_sequence = 0
        self.response_status = 0
        wire = encode_frame(FRAME_REQUEST, operation, request_id, flags=FLAG_FIRST)
        for offset in range(0, len(wire), 20):
            await self.client.write_gatt_char(
                WRITE_UUID, wire[offset : offset + 20], response=True
            )
        try:
            return await asyncio.wait_for(self.future, timeout=30.0)
        finally:
            self.future = None


async def async_main(
    name_prefix: str,
    seconds: float,
    status_only: bool,
    read_all: bool,
    verbose_frames: bool,
) -> int:
    device, name = await find_device(name_prefix, seconds)
    print(f"Connecting to {name or device.address}...")
    async with BleakClient(device, timeout=30.0) as client:
        print(f"Connected; ATT MTU: {client.mtu_size}")
        session = ApiSession(client, verbose_frames)
        await session.start()
        try:
            if not status_only:
                capabilities = await session.request(OP_CAPABILITIES, 1)
                print("capabilities:", json.dumps(capabilities, separators=(",", ":")))
            status = await session.request(OP_STATUS, 2 if not status_only else 1)
            print("status:", json.dumps(status, separators=(",", ":")))
            if read_all:
                scan = await session.request(OP_SCAN, 3)
                print(f"scan: {len(scan)} network(s)")
                networks = await session.request(OP_NETWORKS, 4)
                print("networks:", json.dumps(networks, separators=(",", ":")))
        finally:
            await session.stop()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name-prefix", default="AWAIR-")
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--status-only", action="store_true")
    parser.add_argument("--read-all", action="store_true")
    parser.add_argument("--quiet-frames", action="store_true")
    args = parser.parse_args()
    return asyncio.run(
        async_main(
            args.name_prefix,
            args.seconds,
            args.status_only,
            args.read_all,
            not args.quiet_frames,
        )
    )


if __name__ == "__main__":
    raise SystemExit(main())
