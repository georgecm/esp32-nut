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
| Adding the stick in Tescom Connect: own key + CSR, code or box QR (factory secret), poll, credentials in NVS, heartbeat turning Zabbix on once the UPS is added and on a plan, Zabbix over TLS with the stick's certificate, banner in the web UI, `GET /api/claim` | `lib/StickCloud`, `src/network/cloud_client.cpp`, `stick_identity.cpp`, `scripts/stick_qr.py`; cloud and web app in nut-app branch `feature/stick-claim` (`/v1/stick/*`, `/v1/sticks/*`, `StickPage.tsx`) | Native tests; cloud end to end on the local Worker (`scripts/stick-claim.mjs`); web app clicked through locally (code and QR); firmware builds, not run on the board yet |
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
- Not yet: TLS. zabbix.metagr.id hosts made by the cloud accept certificate
  connections only, so the plain-TCP sender is for a local test Zabbix until
  the claim flow (above) gives the stick its certificate and settings.

## Plug-and-play pieces

### 1-2. Setup: Wi-Fi, then add the stick and its UPS in Tescom Connect

Decided 2026-11-11: the customer plugs the stick in, gives it the Wi-Fi from
a setup network, then adds it in **Tescom Connect** (connect.tescom.gr, the
same web app, sign-in and account creation as everywhere; no separate page).
Until then the stick talks to the cloud without credentials, and only to ask
to be added.

**The stick is a gateway, not the UPS.** Adding the stick puts it in the
account; the UPS it is plugged into is added on its own, by the serial number
on its label (typed, or read with the phone's camera), exactly like any UPS is
registered. The stick sends readings only once that UPS is on a cloud plan
(the same rule as an agent key).

Two ways to add a stick:

1. **Its code** (OAuth device flow, RFC 8628, what TVs use). Once on Wi-Fi the
   stick asks for a code and shows `connect.tescom.gr/?link-stick=CODE` in its
   web UI (`GET /api/claim`). The owner opens it, signs in, approves.
2. **The QR code on the stick or its box**: `connect.tescom.gr/?stick=MAC.SECRET`,
   the secret an HMAC of the MAC with the cloud's `STICK_QR_KEY`, made at the
   factory by `scripts/stick_qr.py` and also written into the stick (NVS
   `factory`/`qr`). Scanned any time, even before the stick was ever plugged
   in: it puts the stick in the account and asks for the UPS's serial. When the
   stick then comes online it sends the same secret with its start and is
   approved at once, with no code.

```
 Customer phone/PC              Stick                          Cloud (connect.tescom.gr)         Zabbix (zabbix.metagr.id)
 ─────────────────              ─────                          ─────────────────────────         ─────────────────────────
 (a) scans the box's QR ──────────────────────────────────────▶ stick in the account (no cert yet)
     signs in, UPS serial ────────────────────────────────────▶ UPS registered, linked to the stick
                                1. AP "Tescom-XXXX"
 2. joins AP, captive page ───▶ saves Wi-Fi
                                3. joins Wi-Fi, key pair + CSR
                                4. POST /v1/stick/claim/start ─▶ {userCode, verifyUrl, pollToken,
                                   (MAC, CSR, model, secret?)     approved: true when (a) happened}
 (b) or opens verifyUrl from ◀── web UI / GET /api/claim
     the stick's page, signs in,
     approves, then the UPS serial ───────────────────────────▶ device row, UPS linked
                                5. POST /v1/stick/claim/poll ──▶ {deviceId, agentToken, tls {ca, cert},
                                                                 zabbix: null | {server, port, host}, next}
                                6. heartbeat every 30 s ───────▶ zabbix: null until the UPS is on a plan,
                                                                 then {server, port, host} (host made then)
                                7. Zabbix over TLS with its certificate ─────────────────────────▶ readings
```

Rules: the code is short-lived (15 min, renewed by the stick), 8 characters
without look-alikes, rate-limited per IP and per MAC; `start` gives nothing but
the code (or, with a valid factory secret for a stick already in an account,
approves it); `poll` gives credentials only to the holder of `pollToken`. The
first account keeps a stick until its owner removes it; another account's
approval is refused. A reset stick (new key) reconnects to the same device:
its owner approves the new code, or it does so by itself with the factory secret.

What is built (2026-11-11):

- **Cloud** (nut-app `cloud`, `migrations/0009_stick_claims.sql`):
  `/v1/stick/claim/start` and `/poll` for the stick; `GET /v1/stick/claims/:code`
  and `POST .../approve`, `GET`/`POST /v1/stick/qr` for the app;
  `GET /v1/sticks/:id`, `PUT /v1/sticks/:id/ups` (the UPS by serial, same rules
  as `POST /registrations`), `DELETE /v1/sticks/:id`. The heartbeat answers a
  stick with `zabbix` and `next`; the Zabbix host (certificate only,
  `tls_accept = 4`) is made at the first heartbeat that needs it.
  `scripts/stick-claim.mjs` checks it all end to end.
- **Tescom Connect web app** (nut-app `src/pages/StickPage.tsx`): `?link-stick=`
  and `?stick=` open "Add a stick to Tescom Connect" before anything else
  (plan or not): sign in or create the account, where, then the UPS's serial
  (camera scan where the browser has `BarcodeDetector`), then what is left.
- **Firmware** (tescom-stick env, `ENABLE_CLOUD_CLAIM`): ECDSA P-256 key made
  on the stick (NVS `stick`), the claim task (`cloud_client.cpp`) against
  `CLOUD_URL` = https://connect.tescom.gr, credentials in NVS `cloud`, the
  heartbeat turning Zabbix on and off, unlink when the cloud forgets the
  device, the banner in the web UI (the code, then "Add the UPS" / "Choose a
  plan" with the link), `GET /api/claim` (public on the LAN; approving still
  needs the account), `POST /api/claim/unlink` (login).
- **Factory**: `scripts/stick_qr.py MAC` gives the secret, the QR link and an
  NVS CSV for the `factory` namespace.

Not built yet:

- the setup page showing the code before the setup network closes (the code
  shows in the stick's web UI and on `GET /api/claim` once it is on Wi-Fi);
- the desktop app's "Add a stick": find sticks on the LAN, read `/api/claim`,
  approve in one click; and a list of the account's sticks there (today the
  QR code or the stick's own link reopens one to add its UPS later);
- printing the sticker (the URL is there; `qrencode` makes the image) and
  writing the NVS CSV in the factory flashing script;
- deployment: migration 0009, the Connect Worker, `STICK_QR_KEY` (staging and
  production; keep it secret: it makes every stick's QR code).

### Per-unit secrets on the label

Still needed for the setup network: SSID `Tescom-XXXX` (last four hex digits
of the MAC) and a random WPA2 password generated at the factory, printed with
a Wi-Fi QR code (`WIFI:S:Tescom-XXXX;T:WPA;P:...;;`). The same secret is the
first web UI password (instead of trust-on-first-use, which on an open setup
network lets whoever connects first take the stick). Stored in a read-only
`factory` NVS partition so a reset goes back to the label values.

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
| Claim on the board: key and CSR made by mbedTLS accepted by the cloud, HTTPS to connect.tescom.gr with the certificate bundle, Zabbix switched on by the heartbeat, TLS to zabbix.metagr.id with the stick's certificate | The board on Wi-Fi with internet, the Connect Worker deployed (staging) |
| Values reach Zabbix and fill the template's items; backlog after a Zabbix outage | Zabbix server + host with the template (a UPS for full readings; without one only `ups.link = driverDown` goes out) |

## Order of work

1. ~~Profiles export and model detection~~ (done).
2. ~~Board profile + env `tescom-stick`, mDNS with the NUT service and the model~~ (done).
3. ~~Zabbix sender, plain TCP~~ (done).
4. Claim flow: ~~cloud endpoints, Add a stick in Tescom Connect (code and box
   QR), the UPS by serial, firmware key/CSR + claim client + Zabbix from the
   heartbeat + TLS + web UI banner, factory secret script~~ (done, not on
   hardware); left: code on the setup page, desktop app "Add a stick", deploy.
5. Per-unit AP and login from a `factory` NVS partition, with a fallback for
   dev boards that have none.
6. Signed OTA (app-level).
7. Factory flashing script and label.
8. Prime: CH34x VCP host source and Modbus port of `tescom_prime.c`.
9. LiON RT `krauler` mode.
