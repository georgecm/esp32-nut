#!/usr/bin/env python3
"""
Flash one Tescom stick at the factory, and make its sticker.

For the stick on --port:

1. read its MAC with esptool (or take --mac);
2. make its factory secret and QR link (scripts/stick_qr.py, the key is the
   cloud's STICK_QR_KEY);
3. build the NVS partition with only "factory"/"qr" = the secret
   (ESP-IDF's NVS partition generator), at the size of the "nvs" partition of
   the env's partition table;
4. put it into the build's firmware.factory.bin at the "nvs" offset and write
   that one image at 0x0, erasing the whole flash first (a clean stick);
5. write the sticker (SVG, 40 x 25 mm: QR code, "Tescom Connect", the MAC's
   last 6 hex digits) and add a line to the factory list (MAC, secret hash,
   firmware hash).

The key comes from the STICK_QR_KEY environment variable or --key-file, never
from the command line (shell history). It must equal the cloud's STICK_QR_KEY
(nut-app/cloud: npx wrangler secret put STICK_QR_KEY), or the QR codes and the
stick's own secret are refused.

  ~/.platformio-venv/bin/pio run -e tescom-stick
  STICK_QR_KEY=... scripts/factory_flash.py --port /dev/cu.usbserial-0001
  # no board: print the commands, write the files, flash nothing
  STICK_QR_KEY=... scripts/factory_flash.py --mac 14:c1:9f:2c:49:c0 --dry-run

Needs the NVS generator: the pip package esp-idf-nvs-partition-gen (in the
Python that runs this script or in PlatformIO's ~/.platformio/penv), or
$IDF_PATH / a framework-espidf package with nvs_partition_gen.py. The QR code
of the sticker uses the segno or qrcode package, else the qrencode command;
without any the sticker has the URL as text only.

Flashing over a used stick wipes its settings, its key and its claim: this is
for new sticks (the cloud lets a reset stick back in with its factory secret).
"""

import argparse
import configparser
import csv
import datetime
import hashlib
import importlib.util
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import stick_qr  # noqa: E402

PIO_PYTHON = Path.home() / ".platformio" / "penv" / "bin" / "python"
NVS_GEN_MODULE = "esp_idf_nvs_partition_gen"


# --- the build and its partition table -------------------------------------------------


def env_option(env: str, option: str, ini: Path = ROOT / "platformio.ini") -> str:
    """An option of a PlatformIO env, following `extends` (and [env] last)."""
    cp = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";",))
    cp.read(ini)
    section, seen = f"env:{env}", set()
    while section and section not in seen:
        seen.add(section)
        if not cp.has_section(section):
            raise SystemExit(f"platformio.ini has no [{section}]")
        if cp.has_option(section, option):
            return cp.get(section, option).strip()
        section = cp.get(section, "extends").strip() if cp.has_option(section, "extends") else ""
    if cp.has_option("env", option):
        return cp.get("env", option).strip()
    return ""


def partition(csv_path: Path, name: str) -> tuple:
    """(offset, size) of a partition in an ESP-IDF partition CSV."""
    for line in csv_path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if cols[0] == name:
            if not cols[3] or not cols[4]:
                raise SystemExit(f"{csv_path}: partition {name} has no fixed offset and size")
            return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit(f"{csv_path}: no partition named {name}")


# --- tools ------------------------------------------------------------------------------


def has_module(python: str, module: str) -> bool:
    if python == sys.executable:
        return importlib.util.find_spec(module) is not None
    if not Path(python).exists():
        return False
    return subprocess.run([python, "-c", f"import {module}"], capture_output=True).returncode == 0


def esptool_cmd() -> list:
    for py in (sys.executable, str(PIO_PYTHON)):
        if has_module(py, "esptool"):
            return [py, "-m", "esptool"]
    exe = shutil.which("esptool.py") or shutil.which("esptool")
    if exe:
        return [exe]
    raise SystemExit("esptool not found: build once with PlatformIO (~/.platformio/penv) or pip install esptool")


def nvs_gen_cmd() -> list:
    for py in (sys.executable, str(PIO_PYTHON)):
        if has_module(py, NVS_GEN_MODULE):
            return [py, "-m", NVS_GEN_MODULE]
    scripts = []
    if os.environ.get("IDF_PATH"):
        scripts.append(Path(os.environ["IDF_PATH"]) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py")
    scripts += sorted((Path.home() / ".platformio" / "packages").glob(
        "framework-espidf*/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"))
    for s in scripts:
        if s.exists():
            return [sys.executable, str(s)]
    raise SystemExit(
        "ESP-IDF's NVS partition generator not found. Install it next to esptool:\n"
        f"  {PIO_PYTHON.parent / 'pip'} install esp-idf-nvs-partition-gen\n"
        "(or in a venv that runs this script), or set IDF_PATH to an ESP-IDF checkout."
    )


def show(cmd: list) -> str:
    return " ".join(shlex.quote(str(c)) for c in cmd)


def read_mac(esptool: list, port: str) -> str:
    cmd = esptool + ["--chip", "esp32s3", "--port", port, "read-mac"]
    print("$ " + show(cmd))
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(out.stdout + out.stderr)
        raise SystemExit(f"could not read the MAC on {port}")
    # "MAC: 14:c1:9f:2c:49:c0" (the eFuse base MAC = the Wi-Fi station's on the S3)
    m = re.search(r"^MAC:\s*([0-9a-fA-F:]{17})", out.stdout, re.M)
    if not m:
        sys.stderr.write(out.stdout)
        raise SystemExit("no MAC in esptool's answer")
    return m.group(1)


# --- sticker ----------------------------------------------------------------------------


def qr_matrix(data: str):
    """Rows of booleans (dark modules, no quiet zone) and how they were made; None without a QR library."""
    try:
        import segno

        qr = segno.make(data, error="m", micro=False)
        return [[bool(v) for v in row] for row in qr.matrix], "segno"
    except ImportError:
        pass
    try:
        import qrcode

        q = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, border=0)
        q.add_data(data)
        q.make(fit=True)
        return [[bool(v) for v in row] for row in q.get_matrix()], "qrcode"
    except ImportError:
        return None, ""


def qr_svg_group(data: str, x: float, y: float, size: float):
    """SVG for the QR code in a size x size mm square, and what made it ("" when nothing could)."""
    matrix, how = qr_matrix(data)
    if matrix:
        n = len(matrix)
        quiet = 2  # modules; the sticker's own margin adds the rest
        cell = size / (n + 2 * quiet)
        d = []
        for r, row in enumerate(matrix):
            c = 0
            while c < n:
                if row[c]:
                    start = c
                    while c < n and row[c]:
                        c += 1
                    d.append(f"M{x + (start + quiet) * cell:.3f} {y + (r + quiet) * cell:.3f}h{(c - start) * cell:.3f}v{cell:.3f}h{-(c - start) * cell:.3f}z")
                else:
                    c += 1
        return f'<path fill="#000" d="{"".join(d)}"/>', how
    exe = shutil.which("qrencode")
    if exe:
        out = subprocess.run([exe, "-t", "SVG", "-l", "M", "-m", "2", "-o", "-", data], capture_output=True, text=True)
        if out.returncode == 0 and "<svg" in out.stdout:
            inner = out.stdout[out.stdout.index("<svg"):]
            # Nested <svg> placed and scaled into the square: its own size dropped, its viewBox kept
            end = inner.index(">")
            head = re.sub(r'\s(width|height|x|y)="[^"]*"', "", inner[:end])
            inner = head.replace("<svg", f'<svg x="{x}" y="{y}" width="{size}" height="{size}"', 1) + inner[end:]
            return inner, "qrencode"
    return "", ""


def sticker_svg(url: str, mac: str) -> tuple:
    """The 40 x 25 mm label; returns (svg, how the QR code was made, "" when it is not there)."""
    w, h, qr_size = 40, 25, 23
    last6 = mac.replace(":", "").upper()[-6:]
    qr, how = qr_svg_group(url, 1, 1, qr_size)
    if not qr:
        # No QR library or qrencode: the URL as text, for a person to type or another tool to encode
        qr = (f'<rect x="1" y="1" width="{qr_size}" height="{qr_size}" fill="none" stroke="#000" stroke-width="0.2"/>'
              f'<text x="{1 + qr_size / 2}" y="12" font-size="1.4" text-anchor="middle" font-family="Helvetica, Arial, sans-serif">'
              f'<tspan x="{1 + qr_size / 2}" dy="0">{escape(url[:26])}</tspan>'
              f'<tspan x="{1 + qr_size / 2}" dy="1.8">{escape(url[26:52])}</tspan>'
              f'<tspan x="{1 + qr_size / 2}" dy="1.8">{escape(url[52:])}</tspan></text>')
    tx = 25.5
    svg = f"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="{w}mm" height="{h}mm" viewBox="0 0 {w} {h}">
  <title>Tescom stick {escape(last6)}</title>
  <rect width="{w}" height="{h}" fill="#fff"/>
  {qr}
  <g font-family="Helvetica, Arial, sans-serif" fill="#000">
    <text x="{tx}" y="7" font-size="3.2" font-weight="700">Tescom</text>
    <text x="{tx}" y="10.6" font-size="3.2">Connect</text>
    <text x="{tx}" y="15.5" font-size="1.7">Scan to add</text>
    <text x="{tx}" y="21.5" font-size="3" font-family="Menlo, Consolas, monospace" font-weight="700">{escape(last6)}</text>
  </g>
</svg>
"""
    return svg, how


# --- main -------------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="the stick's serial port (UART USB-C)")
    ap.add_argument("--mac", help="the stick's MAC, instead of reading it from the board")
    ap.add_argument("--dry-run", action="store_true", help="write the files and print the esptool commands; flash nothing")
    ap.add_argument("--out-dir", default=str(ROOT / "factory-out"), help="sticker, NVS CSV/bin and factory list (default factory-out/)")
    ap.add_argument("--key-file", help="read the factory key from this file instead of STICK_QR_KEY")
    ap.add_argument("--env", default="tescom-stick", help="PlatformIO env whose build is flashed (default tescom-stick)")
    ap.add_argument("--app-url", default=stick_qr.APP_URL, help=f"Tescom Connect's address (default {stick_qr.APP_URL})")
    ap.add_argument("--baud", default="921600", help="flashing speed (default 921600)")
    args = ap.parse_args()

    if args.key_file:
        key = Path(args.key_file).read_text(encoding="utf-8").strip()
    else:
        key = os.environ.get("STICK_QR_KEY", "")
    if not key:
        print("No factory key: set STICK_QR_KEY or pass --key-file", file=sys.stderr)
        return 2
    if not args.mac and not args.port:
        print("Pass --port (or --mac with --dry-run)", file=sys.stderr)
        return 2
    if not args.dry_run and not args.port:
        print("Flashing needs --port", file=sys.stderr)
        return 2

    build = ROOT / ".pio" / "build" / args.env
    factory_bin = build / "firmware.factory.bin"
    if not factory_bin.exists():
        print(f"{factory_bin} is missing: run ~/.platformio-venv/bin/pio run -e {args.env}", file=sys.stderr)
        return 2
    table = ROOT / (env_option(args.env, "board_build.partitions") or "partitions.csv")
    nvs_offset, nvs_size = partition(table, "nvs")

    esptool = esptool_cmd()
    if args.mac:
        mac = args.mac
    elif args.dry_run:
        print("$ " + show(esptool + ["--chip", "esp32s3", "--port", args.port, "read-mac"]))
        print("Dry run without --mac: the MAC is not read from the board", file=sys.stderr)
        return 2
    else:
        mac = read_mac(esptool, args.port)
    try:
        mac = stick_qr.normal_mac(mac)
    except ValueError as e:
        print(e, file=sys.stderr)
        return 2
    secret = stick_qr.stick_secret(key, mac)
    url = stick_qr.stick_url(key, mac, args.app_url)
    mac12 = mac.replace(":", "").upper()

    out = Path(args.out_dir).resolve() / mac12
    out.mkdir(parents=True, exist_ok=True)

    # The NVS partition: namespace "factory", key "qr" (CloudClient::factorySecret)
    nvs_csv = out / "nvs.csv"
    nvs_csv.write_text("key,type,encoding,value\nfactory,namespace,,\nqr,data,string," + secret + "\n", encoding="utf-8")
    nvs_bin = out / "nvs.bin"
    gen = nvs_gen_cmd() + ["generate", str(nvs_csv), str(nvs_bin), hex(nvs_size)]
    print("$ " + show(gen))
    r = subprocess.run(gen, capture_output=True, text=True)
    if r.returncode != 0 or not nvs_bin.exists() or nvs_bin.stat().st_size != nvs_size:
        sys.stderr.write(r.stdout + r.stderr)
        print("could not build the NVS partition", file=sys.stderr)
        return 1

    # One image at 0x0: the build's merged image (bootloader, table, otadata, app) with the
    # NVS partition in place of its blank range (two writes would overlap)
    image = bytearray(factory_bin.read_bytes())
    if len(image) < nvs_offset + nvs_size or any(b != 0xFF for b in image[nvs_offset:nvs_offset + nvs_size]):
        print(f"{factory_bin}: the nvs range {nvs_offset:#x}+{nvs_size:#x} is not blank in the image", file=sys.stderr)
        return 1
    image[nvs_offset:nvs_offset + nvs_size] = nvs_bin.read_bytes()
    flash_bin = out / "flash.bin"
    flash_bin.write_bytes(image)

    flash = esptool + ["--chip", "esp32s3", "--port", args.port or "PORT", "--baud", args.baud,
                       "write-flash", "--erase-all", "0x0", str(flash_bin)]
    print("$ " + show(flash))
    if not args.dry_run:
        r = subprocess.run(flash)
        if r.returncode != 0:
            print(f"flashing failed on {args.port}", file=sys.stderr)
            return 1
        flash_bin.unlink()  # 2.4 MB per stick; the build and nvs.bin make it again

    svg, how = sticker_svg(url, mac)
    (out / "sticker.svg").write_text(svg, encoding="utf-8")

    # The factory list: one line per stick (no secret, its hash)
    listing = Path(args.out_dir).resolve() / "factory_list.csv"
    new = not listing.exists()
    with listing.open("a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["time", "mac", "label", "secret_sha256", "firmware_sha256", "env", "flashed"])
        w.writerow([
            datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            mac, mac12[-6:],
            hashlib.sha256(secret.encode()).hexdigest()[:16],
            hashlib.sha256((build / "firmware.bin").read_bytes()).hexdigest()[:16],
            args.env,
            "no (dry run)" if args.dry_run else "yes",
        ])

    print(f"MAC      {mac}  (label {mac12[-6:]})")
    print(f"QR       {url}")
    print(f"NVS      {nvs_bin} at {nvs_offset:#x} ({nvs_size:#x} bytes)")
    print(f"sticker  {out / 'sticker.svg'}" + ("" if how else "  (URL as text only: no segno/qrcode package or qrencode for the QR code)"))
    if args.dry_run:
        print(f"dry run: nothing flashed; image {flash_bin}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
