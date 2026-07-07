#!/usr/bin/env python3
"""Regression check for the split-packet POST body bug.

Browsers routinely send the request headers and body in separate TCP segments.
The WICED HTTP daemon only parses the header packet, so the handler must pull
the continuation body off the socket itself (web_api.c api_read_body). curl
coalesces small bodies into one segment and therefore never exercised the split
path -- this check does, by writing the headers, pausing, then the body.

Sends POST /api/disp {"mode":"score"} two ways and requires HTTP 204 for both.
Uses "score" (the device's normal display mode) so the device is left unchanged.

Usage: REWAIR_IP=<ip> scripts/check_post_body.py    (exit 0 = pass)
"""
import os
import socket
import sys
import time

IP = os.environ.get("REWAIR_IP")
if not IP:
    print("REWAIR_IP not set", file=sys.stderr)
    sys.exit(2)

BODY = '{"mode":"score"}'
HEADERS = (
    "POST /api/disp HTTP/1.1\r\n"
    f"Host: {IP}\r\n"
    "Content-Type: text/plain\r\n"
    f"Content-Length: {len(BODY)}\r\n"
    "Connection: close\r\n"
    "\r\n"
)


def post(split):
    s = socket.create_connection((IP, 80), timeout=8)
    try:
        if split:
            s.sendall(HEADERS.encode())
            time.sleep(0.15)          # force the body into a separate segment
            s.sendall(BODY.encode())
        else:
            s.sendall((HEADERS + BODY).encode())
        resp = b""
        while b"\r\n" not in resp:
            chunk = s.recv(256)
            if not chunk:
                break
            resp += chunk
        status_line = resp.split(b"\r\n", 1)[0].decode(errors="replace")
        code = int(status_line.split()[1]) if len(status_line.split()) > 1 else 0
        return code, status_line
    finally:
        s.close()


ok = True
for split, label in ((False, "coalesced"), (True, "split headers|body")):
    code, line = post(split)
    verdict = "ok" if code == 204 else "FAIL"
    if code != 204:
        ok = False
    print(f"  {verdict}: {label} POST /api/disp -> {line}")

sys.exit(0 if ok else 1)
