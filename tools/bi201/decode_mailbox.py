#!/usr/bin/env python3
import struct
import sys
from pathlib import Path


HEADER_FIELDS = (
    "magic",
    "version",
    "size",
    "heartbeat",
    "phase",
    "gpio_result",
    "uart_init_result",
    "last_uart_result",
    "tx_bytes",
    "rx_bytes",
    "rx_overwritten",
    "last_rx_ms",
    "rx_write",
    "beacon_seq",
)

PHASES = {
    0: "boot",
    1: "uart-ready",
    2: "module-settling",
    3: "querying",
    4: "transparent",
    5: "failed",
}

HEADER_SIZE = 4 * len(HEADER_FIELDS)
RX_LOG_SIZE = 1024
EXPECTED_MAGIC = 0x30324942


def escaped(data: bytes) -> str:
    out = []
    for byte in data:
        if byte == 0x0D:
            out.append(r"\r")
        elif byte == 0x0A:
            out.append(r"\n" + "\n")
        elif byte == 0x09:
            out.append(r"\t")
        elif 0x20 <= byte <= 0x7E:
            out.append(chr(byte))
        else:
            out.append(f"\\x{byte:02x}")
    return "".join(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} MAILBOX.bin", file=sys.stderr)
        return 2

    data = Path(sys.argv[1]).read_bytes()
    if len(data) < HEADER_SIZE + RX_LOG_SIZE:
        print(f"short mailbox read: {len(data)} bytes", file=sys.stderr)
        return 1

    raw_values = struct.unpack_from("<5I3i6I", data)
    values = dict(zip(HEADER_FIELDS, raw_values))

    print(f"magic:            0x{values['magic']:08x}")
    if values["magic"] != EXPECTED_MAGIC:
        print("mailbox magic mismatch; the probe app may not be running", file=sys.stderr)
        return 1
    print(f"version/size:     {values['version']} / {values['size']} bytes")
    print(f"phase:            {values['phase']} ({PHASES.get(values['phase'], 'unknown')})")
    print(f"heartbeat:        {values['heartbeat']}")
    print(f"GPIO/UART init:   {values['gpio_result']} / {values['uart_init_result']}")
    print(f"last UART result: {values['last_uart_result']}")
    print(f"TX/RX bytes:      {values['tx_bytes']} / {values['rx_bytes']}")
    print(f"RX overwritten:   {values['rx_overwritten']}")
    print(f"last RX time:     {values['last_rx_ms']} ms")
    print(f"beacon sequence:  {values['beacon_seq']}")

    ring = data[HEADER_SIZE:HEADER_SIZE + RX_LOG_SIZE]
    valid = min(values["rx_bytes"], RX_LOG_SIZE)
    if valid == 0:
        log = b""
    elif values["rx_bytes"] <= RX_LOG_SIZE:
        log = ring[:valid]
    else:
        start = values["rx_write"] % RX_LOG_SIZE
        log = ring[start:] + ring[:start]

    print("\nRX log (oldest to newest):")
    print(escaped(log) if log else "<empty>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
