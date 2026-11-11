# Tescom USB-to-Wi-Fi stick

An ESP32-S3 that Tescom hands to customers in place of a Raspberry Pi with NUT.
The UPS's USB cable plugs into the stick; the stick joins the customer's Wi-Fi
and serves NUT on port 3493. The Tescom Connect app (`nut-app`) already finds
NUT servers by scanning the subnet for port 3493, so it needs no change to
read a stick.

This fork (`georgecm/esp32-nut`, branch `feature/tescom-megatec`) holds the
firmware. Tescom-only parts stay behind their own PlatformIO environment and
board profile, so upstream changes keep merging.

Status: 2026-11-11. Nothing here has run against a real UPS yet.

## What is done

| Piece | Where | Verified |
| --- | --- | --- |
| Megatec Q1 over the MEC0003 bridge (`hunnox`, `fabula`) | `lib/USBHostUPS/src/Megatec*` | Native tests only |
| Model detection from the shared profiles | `lib/USBHostUPS/src/UpsProfiles*`, `scripts/gen_profiles.py` | Native tests only |
| `esp32-s3-n16r8` env (16MB flash, octal PSRAM) | `platformio.ini` | Builds; not flashed yet |
| `tescom-stick` env: board profile, hostname `tescom-stick-xxxxxx`, NUT description `Tescom stick (<model>)`, mDNS `_nut._tcp` with model TXT | `src/board/tescom_stick.cpp`, `src/network/mdns_announcer.*` | Builds; not flashed yet |
| Zabbix sender: the "Tescom UPS by NUT" items over the trapper protocol, backlog while unreachable, SNTP time, settings page | `lib/ZabbixSender`, `src/network/zabbix_client.cpp`, NUT Server page | Native tests; builds; not yet sent to a real Zabbix |
| Native tests on macOS/Linux (`native-host` env) | `platformio.ini` | 875 of 878 pass on the Mac; `test_nut_table`, `test_rw_catalog` (clang `bind` ambiguity) and `test_record_replay` (float rounding) fail on the base commit too |

Building: PlatformIO needs Python 3.10-3.14 (Homebrew's 3.15 is refused), so
on the Mac it lives in a venv:

```bash
python3.14 -m venv ~/.platformio-venv && ~/.platformio-venv/bin/pip install platformio
~/.platformio-venv/bin/pio run -e tescom-stick
~/.platformio-venv/bin/pio test -e native-host
```

## Shared UPS profiles

`nut-app/profiles/*.conf` stays the one place a model is described (one NUT
`ups.conf` section with `# Model / Link / USB ID / Match` headers). The app,
the Pi (`tools/nut-profile`) and the stick all read it:

```
nut-app/profiles/*.conf
  └─ nut-app/tools/export-profiles      (JSON, stdlib Python)
       └─ esp32-nut/profiles/tescom-profiles.json   (committed copy)
            └─ scripts/gen_profiles.py   (PlatformIO pre-script)
                 └─ lib/USBHostUPS/src/UpsProfiles.gen.h   (committed)
```

- The pre-script re-exports from `nut-app` when it finds it (`$TESCOM_NUT_APP`,
  else `../nut-app`), so a profile added there reaches the next firmware
  build. Without `nut-app` the committed JSON is used, so CI and upstream
  builds need nothing extra.
- `python scripts/gen_profiles.py --check` fails when the JSON or the header
  is stale (for CI on this fork).
- At run time `MegatecDriver` reads F (ratings) and Q1, then
  `UpsProfiles::identify()` picks the one profile with the UPS's USB ID whose
  `# Match:` rules the reading fits (`battery.voltage.nominal = 24` is the
  LEO, `12` the Smartbox), exactly as the app does. The profile then names the
  model (`device.mfr = Tescom`, `ups.model = Leo LCD`) and supplies what NUT
  would take from `ups.conf`: `override.*`, `default.*`, the battery limits for
  the charge estimate, and `battery_voltage_reports_one_pack` with
  `battery.packs` (the LiON).
- With no fitting profile, or two, the stick reads the UPS as before, with no
  model name, and logs `[Megatec] no single profile fits`.

## Zabbix

The stick sends to Zabbix itself, so a site with only a stick (no Pi, no PC
running `tescom-agent`) still shows up in Zabbix.

- Same items, keys and rules as `tescom-agent` (`nut-app/crates/tescom-agent`,
  `items.rs`): the template `nut-app/zabbix/template_tescom_ups_by_nut.yaml`
  works unchanged. Only `ups.link` is sent while the UPS link is down or stale,
  so the template's nodata triggers fire.
- Zabbix sender (trapper) protocol, plain TCP to port 10051 by default, in its
  own task. Unreachable server: values wait (1500 on PSRAM boards, 300
  otherwise, oldest dropped) and go out in batches of 200 when it answers.
- Time from SNTP (`pool.ntp.org`, `time.google.com`) once synced; until then
  the values carry no clock and Zabbix stamps them on arrival.
- Settings: NUT Server page of the web UI, or `GET/POST /api/zabbix`
  (`enabled`, `server`, `port`, `host`, `interval`), `POST /api/zabbix/send`.
  A build can preset the server: `-DZABBIX_DEFAULT_SERVER=\"...\"`.
- Not yet: TLS. Plain TCP is fine on a LAN or VPN, not over the internet.
  `tescom-agent` uses certificates; on the ESP32, Zabbix's PSK mode is the
  lighter option (mbedTLS has the PSK cipher suites rustls lacks). Also not
  yet: getting the settings from the Connect cloud's registration, as
  `tescom-agent register` does (the cloud creates the Zabbix host there).

## Plug-and-play pieces

### 1. Wi-Fi provisioning

Today the firmware opens the access point `NUT_ESP32_Config` with the shared
password `12345678`, and a web page sets the Wi-Fi. That is fine for a
developer, not for a box sold to customers.

Options:

- **A. Per-unit access point, credentials on a label.** SSID `Tescom-XXXX`
  (last four hex digits of the MAC), a random WPA2 password generated at the
  factory and printed on the stick with a Wi-Fi QR code
  (`WIFI:S:Tescom-XXXX;T:WPA;P:...;;`). The customer scans it with a phone,
  a captive portal opens the existing Wi-Fi page. Works with any phone or
  laptop, no app change, reuses the AP code that exists.
- **B. BLE provisioning from the app.** ESP-IDF `wifi_provisioning` over BLE
  with Security 1 and a proof-of-possession code on the label. Smoother when
  the app is on a phone, but nut-app is a desktop Tauri app today (BLE on
  desktop needs `btleplug` and OS permissions), the Arduino core ships BLE
  but the provisioning manager costs flash and RAM, and it still needs the
  label for the PoP.
- C. WPS button. Many routers have it disabled; no.

**Recommendation: A now, B when the app has a mobile version.** Both need the
same per-unit secret on a label, so the factory step is shared.

### 2. Per-unit secrets and login

One random secret per unit, made at the factory, never derived from the MAC
(the MAC is broadcast):

- the access-point password;
- the first web UI password (user `admin`), instead of trust-on-first-use,
  which on an open AP lets whoever connects first take the stick;
- the NUT control user (`tescom-connect`, as the Pi's `nut-profile` makes)
  for instant commands. The app asks for the label code once when adding a
  stick, or later gets it through the cloud as it does for a Pi.

Stored in a separate read-only NVS partition (`factory`), so "reset to
defaults" and a double-reset AP recovery go back to the label values instead
of to no password. Flash encryption would protect it against someone holding
the board; see section 5 for why that waits.

### 3. Naming: mDNS and what the app shows

- Hostname `tescom-xxxx` (already configurable, US-113); mDNS answers
  `tescom-xxxx.local`.
- Advertise `_nut._tcp` on 3493 and `_http._tcp` on 80, with TXT records
  `vendor=Tescom`, `model=Leo LCD`, `profile=leo`, `fw=<version>`. The app's
  port scan keeps working; mDNS lets it name the stick before connecting and
  find it on networks where the scan is slow.
- The NUT UPS description (`LIST UPS`) becomes `Tescom stick (Leo LCD)` once
  a profile matches, so the app shows that with no change.

### 4. Branding

A `tescom-stick` PlatformIO environment with its own board profile
(`src/board/tescom_stick.cpp`, ADR 0010 already supports one profile per
env): AP SSID prefix, hostname prefix, board label "Tescom USB-to-Wi-Fi
stick". Web UI: Tescom logo and colours behind a build flag in the
`embed_web.py` step, so upstream's UI stays the default. The onboard RGB LED
of the dev boards is not wired (unbridged jumper); the product board should
have one status LED.

### 5. Signed OTA

- **Now:** the `/update` handler checks an ECDSA P-256 signature appended to
  the image (mbedTLS, already linked) against a public key compiled in,
  before `Update.end()` marks the slot bootable. No eFuse is touched, so it
  is reversible. The release script signs with a key kept off the repo.
- **Later, for production:** ESP-IDF Secure Boot v2 and flash encryption.
  Both burn eFuses (irreversible) and need a custom sdkconfig build
  (`docs/plans/custom-sdkconfig.md`), because the precompiled Arduino core
  ignores `sdkconfig.defaults`.
- **GPLv3 caution.** The firmware is GPL-3.0. A stick sold to consumers is
  likely a "User Product" (GPLv3 §6): Tescom must give the "Installation
  Information" needed to run a modified version. Locked secure boot with
  only Tescom's key would break that. Keep a documented way for the owner to
  accept their own images (a web UI setting behind login that allows
  unsigned or owner-signed firmware, with a warning), and have this checked
  by someone who does licensing before shipping.

### 6. GPL source offer

- Every box (leaflet) and the web UI's About page: the licence, the source
  URL (this fork, the exact tag of the firmware version) and a written offer
  valid three years, as GPLv3 §6 asks for object code shipped on a device.
- Release tags match the version string the firmware reports.
- `LICENSE` and `NOTICE` stay in the repo; `nut-app` (not GPL) only talks to
  the stick over NUT and HTTP, so the two stay separate works.

### 7. Production flashing

A `scripts/factory_flash.py` for the line:

1. read the MAC with esptool, derive `XXXX`;
2. generate the per-unit secret, build the `factory` NVS image
   (`nvs_partition_gen.py`), flash `firmware.factory.bin` and it;
3. watch the UART log until the AP is up, then record MAC, serial, firmware
   version and the secret's hash in a CSV for the factory list;
4. print the label (SSID, password, QR code, serial).

The serial number joins the factory list the cloud already checks
(`nut-app/docs/ACTIVATION.md`), so a stick can be activated like a UPS.

## Hardware notes

- ESP32-S3 N16R8 DevKitC-1 clones: use the `esp32-s3-n16r8` env.
- The native USB port becomes the UPS port: it needs 5 V on VBUS (bridge the
  board's IN-OUT pads, or use a powered OTG adapter). Once the firmware runs,
  that port no longer shows a serial device; logs and flashing move to the
  UART USB-C port (or BOOT + RESET for the ROM loader on the native port).

## Needs a real UPS

| Test | Needs |
| --- | --- |
| Does the MEC0003 enumerate as HID so `USBHostUPS` can claim it | LEO or Smartbox; Export USB Diagnostics |
| `hunnox` lock-step and 102-byte reads give real Q1/F replies | LEO, Smartbox |
| Profile match: LEO reads `battery.voltage.nominal` 24, Smartbox 12 | LEO, Smartbox |
| Charge estimate on battery against the Pi's NUT readings | LEO or Smartbox, power cut |
| Status bits OL/OB/LB during a power cut | Any of them |
| `krauler` + warm-up for the LiON RT (not written yet) | LiON RT |
| CH34x serial host + Modbus for the Prime (not written yet) | Prime 1102 XL |
| 5 V VBUS from the board's pads under the UPS's load | Any UPS |
| App finds the stick by its 3493 scan, reads, shows the model | Any UPS + app |
| Values reach Zabbix and fill the template's items; backlog after a Zabbix outage | Zabbix server + host with the template (a UPS for full readings; without one only `ups.link = driverDown` goes out) |

## Order of work

1. ~~Profiles export and model detection~~ (done).
2. ~~Board profile + env `tescom-stick`, mDNS with the NUT service and the model~~ (done).
3. Per-unit AP and login from a `factory` NVS partition, with a fallback for
   dev boards that have none.
4. Signed OTA (app-level).
5. Factory flashing script and label.
6. Prime: CH34x VCP host source and Modbus port of `tescom_prime.c`.
7. LiON RT `krauler` mode.
