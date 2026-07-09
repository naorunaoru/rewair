#!/usr/bin/env python3
"""Upload a raw Rewair application image using the portal OTA protocol."""

from __future__ import annotations

import argparse
import http.client
import json
from pathlib import Path
import sys
import zlib


CHUNK_SIZE = 16 * 1024
MAX_IMAGE_SIZE = 464 * 1024


def post(
    connection: http.client.HTTPConnection, path: str, body: bytes = b""
) -> dict[str, object] | None:
    headers = {"Content-Length": str(len(body))}
    if body:
        headers["Content-Type"] = "text/plain"

    connection.request("POST", path, body=body, headers=headers)
    response = connection.getresponse()
    payload = response.read()
    if not 200 <= response.status < 300:
        message = payload.decode("utf-8", errors="replace")
        raise RuntimeError(f"POST {path}: HTTP {response.status}: {message}")
    if not payload:
        return None
    return json.loads(payload)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Upload a raw application .bin through /api/update"
    )
    parser.add_argument("host", help="device IP address or hostname")
    parser.add_argument("firmware", type=Path, help="raw application .bin")
    args = parser.parse_args()

    image = args.firmware.read_bytes()
    if len(image) < 8 or len(image) > MAX_IMAGE_SIZE:
        parser.error(
            f"firmware must be 8..{MAX_IMAGE_SIZE} bytes (got {len(image)})"
        )

    crc = zlib.crc32(image) & 0xFFFFFFFF
    print(f"image: {args.firmware} ({len(image)} bytes, crc32={crc:08x})")

    connection = http.client.HTTPConnection(args.host, 80, timeout=30)
    try:
        post(connection, f"/api/update?op=begin&size={len(image)}&crc={crc:08x}")
        print("staging: ready")

        for offset in range(0, len(image), CHUNK_SIZE):
            chunk = image[offset : offset + CHUNK_SIZE]
            result = post(connection, f"/api/update?offset={offset}", chunk)
            received = int((result or {}).get("received", -1))
            expected = offset + len(chunk)
            if received != expected:
                raise RuntimeError(
                    f"device acknowledged {received} bytes; expected {expected}"
                )
            percent = (received * 100) // len(image)
            print(f"upload: {received}/{len(image)} bytes ({percent}%)")

        result = post(connection, "/api/update?op=commit")
        if (result or {}).get("status") != "staged":
            raise RuntimeError(f"unexpected commit response: {result!r}")
        print("commit: staged; device is rebooting")
    finally:
        connection.close()

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"ota_upload: {error}", file=sys.stderr)
        raise SystemExit(1)
