#!/usr/bin/env python3
"""Make what the board's access point needs on the phone side: the NFC tag's
contents and a QR code. Reads WIFI_AP_SSID / WIFI_AP_PASSWORD from
firmware/components/imx_wifi/include/wifi_credentials.h (gitignored).

Writes into wifi-app/nfc-out/ (gitignored, it holds the password):
  wifi-qr.png    WIFI:T:WPA;S:...;P:...;;  - the iPhone / Android camera joins from it
  url-qr.png     http://192.168.4.1/       - opens the page once joined
  tag.ndef       the NDEF message (Wi-Fi record + URL record), for apps that
                 write a raw NDEF file; also shows the size against NTAG213
and prints the values to type into a phone NFC app.

  python3 tools/nfc_tag.py            (QR codes need: pip install "qrcode[pil]")
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CREDS = HERE.parent / "firmware/components/imx_wifi/include/wifi_credentials.h"
OUT = HERE.parent / "nfc-out"
URL = "http://192.168.4.1/"   # esp_netif's default SoftAP address


def define(text: str, name: str) -> str | None:
    m = re.search(r'^\s*#define\s+' + name + r'\s+"([^"]*)"', text, re.M)
    return m.group(1) if m else None


def tlv(t: int, v: bytes) -> bytes:
    return struct.pack(">HH", t, len(v)) + v


def wsc_payload(ssid: str, pw: str) -> bytes:
    """Wi-Fi Simple Configuration credential, as Android reads it."""
    cred = (tlv(0x1026, b"\x01")                     # network index
            + tlv(0x1045, ssid.encode())              # SSID
            + tlv(0x1003, b"\x00\x20")                # auth: WPA2-Personal
            + tlv(0x100F, b"\x00\x08")                # encryption: AES
            + tlv(0x1027, pw.encode())                # network key
            + tlv(0x1020, b"\xff" * 6))               # MAC: any
    return tlv(0x104A, b"\x10") + tlv(0x100E, cred)  # version 1.0, credential


def record(tnf: int, rtype: bytes, payload: bytes, first: bool, last: bool) -> bytes:
    short = len(payload) < 256
    hdr = (0x80 if first else 0) | (0x40 if last else 0) | (0x10 if short else 0) | tnf
    ln = struct.pack(">B", len(payload)) if short else struct.pack(">I", len(payload))
    return bytes([hdr, len(rtype)]) + ln + rtype + payload


def main() -> None:
    text = CREDS.read_text(encoding="utf-8") if CREDS.exists() else ""
    ssid, pw = define(text, "WIFI_AP_SSID"), define(text, "WIFI_AP_PASSWORD")
    if not ssid or not pw:
        sys.exit(f"WIFI_AP_SSID / WIFI_AP_PASSWORD not defined in {CREDS}")
    OUT.mkdir(exist_ok=True)

    # URL first (iPhone reads the first record), Wi-Fi second (Android finds it anywhere).
    url_rec = record(0x01, b"U", b"\x03" + URL.removeprefix("http://").encode(), True, False)
    wifi_rec = record(0x02, b"application/vnd.wfa.wsc", wsc_payload(ssid, pw), False, True)
    msg = url_rec + wifi_rec
    (OUT / "tag.ndef").write_bytes(msg)

    print(f"SSID {ssid}   password {pw}   security WPA2-Personal (AES)   page {URL}")
    print(f"NDEF message {len(msg)} bytes (NTAG213 holds 137, NTAG215 496)")

    try:
        import qrcode
    except ImportError:
        print('QR codes skipped: pip install "qrcode[pil]"')
        return
    esc = lambda s: re.sub(r'([\;,:"])', r'\\\1', s)
    qrcode.make(f"WIFI:T:WPA;S:{esc(ssid)};P:{esc(pw)};;").save(OUT / "wifi-qr.png")
    qrcode.make(URL).save(OUT / "url-qr.png")
    print(f"wrote {OUT}/wifi-qr.png, url-qr.png, tag.ndef")


if __name__ == "__main__":
    main()
