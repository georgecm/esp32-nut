#!/usr/bin/env python3
"""
The factory secret and QR code of a Tescom stick.

Each stick gets a secret made from its MAC and the factory key (the cloud's
STICK_QR_KEY, nut-app cloud): base32 of HMAC-SHA256(key, "aa:bb:cc:dd:ee:ff"),
first 16 characters. The same secret goes to two places:

- the sticker on the stick and its box, as the QR code
  https://connect.tescom.gr/?stick=AABBCCDDEEFF.SECRET : scanned, it adds the
  stick to the account in Tescom Connect, even before it was ever plugged in;
- the stick's NVS ("factory" namespace, key "qr"): the stick sends it with its
  first claim, and a stick already in an account is approved at once.

The key is read from the STICK_QR_KEY environment variable or --key-file, never
from the command line (shell history).

  STICK_QR_KEY=... scripts/stick_qr.py 14:c1:9f:2c:49:c0
  STICK_QR_KEY=... scripts/stick_qr.py 14:c1:9f:2c:49:c0 --nvs-csv build/factory_nvs.csv
  # then, for the NVS partition (ESP-IDF):
  #   python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \\
  #       generate build/factory_nvs.csv build/factory_nvs.bin 0x5000
  # and a PNG of the QR code, where qrencode is installed:
  #   qrencode -o sticker.png "$(STICK_QR_KEY=... scripts/stick_qr.py 14:c1:9f:2c:49:c0 --url-only)"
"""

import argparse
import base64
import hashlib
import hmac
import os
import re
import sys

APP_URL = "https://connect.tescom.gr"


def normal_mac(raw: str) -> str:
    h = re.sub(r"[^0-9a-f]", "", raw.lower())
    if len(h) != 12:
        raise ValueError(f"not a MAC address: {raw}")
    return ":".join(h[i : i + 2] for i in range(0, 12, 2))


def stick_secret(key: str, mac: str) -> str:
    digest = hmac.new(key.encode(), normal_mac(mac).encode(), hashlib.sha256).digest()
    return base64.b32encode(digest).decode().rstrip("=")[:16]


def stick_url(key: str, mac: str, app_url: str = APP_URL) -> str:
    return f"{app_url.rstrip('/')}/?stick={normal_mac(mac).replace(':', '').upper()}.{stick_secret(key, mac)}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mac", help="the stick's Wi-Fi station MAC, any spelling")
    ap.add_argument("--key-file", help="read the factory key from this file instead of STICK_QR_KEY")
    ap.add_argument("--app-url", default=APP_URL, help=f"Tescom Connect's address (default {APP_URL})")
    ap.add_argument("--nvs-csv", help="also write an NVS CSV (namespace factory, key qr) here")
    ap.add_argument("--url-only", action="store_true", help="print only the QR code's URL")
    args = ap.parse_args()

    if args.key_file:
        with open(args.key_file, encoding="utf-8") as f:
            key = f.read().strip()
    else:
        key = os.environ.get("STICK_QR_KEY", "")
    if not key:
        print("No factory key: set STICK_QR_KEY or pass --key-file", file=sys.stderr)
        return 2

    try:
        secret = stick_secret(key, args.mac)
        url = stick_url(key, args.mac, args.app_url)
    except ValueError as e:
        print(e, file=sys.stderr)
        return 2

    if args.nvs_csv:
        with open(args.nvs_csv, "w", encoding="utf-8") as f:
            f.write("key,type,encoding,value\nfactory,namespace,,\nqr,data,string," + secret + "\n")
    if args.url_only:
        print(url)
    else:
        print(f"MAC     {normal_mac(args.mac)}")
        print(f"secret  {secret}")
        print(f"QR      {url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
