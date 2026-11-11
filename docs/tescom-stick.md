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
| Tescom Connect claim: own key + CSR, code from bridge.connect.tescom.gr, poll, credentials in NVS, heartbeat, Zabbix over TLS with the stick's certificate, code banner in the web UI, `GET /api/claim` | `lib/StickCloud`, `src/network/cloud_client.cpp`, `stick_identity.cpp`; cloud side in nut-app branch `feature/stick-claim` (`cloud/bridge/`, `/v1/stick/*`) | Native tests; cloud flow end to end on local Workers (`scripts/stick-claim.mjs`) and in the browser; firmware builds, not run on the board yet |
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

### 1-2. Setup: Wi-Fi, then sign in to claim the stick

Decided 2026-11-11: the customer plugs the stick in, gives it the Wi-Fi from
a setup network, then signs in with their account (Tescom Connect; Metagrid
to be confirmed) to make the stick theirs. Until then the stick talks to the
cloud without credentials, and only to ask to be claimed. This is the OAuth
device authorization flow (RFC 8628, what TVs use), on top of what
`tescom-agent register` does today.

```
 Customer phone/PC          Stick                         Cloud (connect.tescom.gr)        Zabbix (zabbix.metagr.id)
 ─────────────────          ─────                         ─────────────────────────        ─────────────────────────
                            1. AP "Tescom-XXXX"
 2. join AP, captive page ─▶ save Wi-Fi
                            3. joins Wi-Fi, makes key pair + CSR
                            4. POST /v1/stick/claim/start ─▶ {userCode, verifyUrl,
                               (serial, MAC, model, CSR)       pollToken, expiresIn}
 5. sees code + link + QR ◀─ setup page / web UI /
    (or the app reads it      GET /api/claim on the LAN
    from the stick)
 6. opens verifyUrl, signs in, picks site/name ─────────────▶ approves: device row, Zabbix host
                                                              with the stick's cert subject,
                                                              signs the CSR
                            7. POST /v1/stick/claim/poll ──▶ {deviceId, agentToken,
                               every 5 s until approved        zabbix {server, port, host},
                                                               tls {ca, serverCa, cert}}
                            8. saves to NVS; Zabbix over TLS with its certificate ─────────▶ readings
                            9. heartbeat with agentToken ──▶ (live mode, diagnostics, events)
```

How the code reaches the person, most convenient first:

1. **The Tescom Connect app** already signed in on a computer on the same
   network finds the stick (port 3493 scan or mDNS), reads the pending code
   from the stick's `GET /api/claim`, and approves it with the user's session:
   no code to type.
2. **The setup page**: after the Wi-Fi is saved, the page waits on the setup
   network (AP and Wi-Fi run together for a minute) until the cloud gave a
   code, then shows a "Sign in to finish" link and the code. The link loads
   once the phone is back on its usual network.
3. **The stick's web UI** (`tescom-stick-xxxxxx.local`) shows the code until
   the stick is claimed.

Rules for the unauthenticated endpoints: the code is short-lived (15 min,
renewed by the stick), 8 characters without look-alikes, rate-limited per IP
and per serial; `start` gives nothing but the code; `poll` gives credentials
only once, to the holder of `pollToken`. A claimed stick that is reset goes
through the flow again; the cloud then replaces its certificate.

What is built (2026-11-11):

- **Bridge** (`nut-app/cloud/bridge`, its own Worker on
  bridge.connect.tescom.gr): the page where the owner signs in (or creates
  an account) with Tescom Connect, sees the stick (model, serial, MAC),
  picks the site (their home by default) and approves. It forwards `/v1` to
  the Connect Worker, so the stick talks only to the bridge.
- **Cloud** (`nut-app/cloud`, `migrations/0009_stick_claims.sql`):
  `POST /v1/stick/claim/start` and `/poll` for the stick,
  `GET /v1/stick/claims/:code` and `POST .../approve` for the page and the
  app. Approval does what `/agent/register` does: device row
  (`agent_kind = 'stick'`), Zabbix host accepting only the stick's
  certificate (`tls_accept = 4`), the CA signs the stick's request.
- **Firmware** (tescom-stick env, `ENABLE_CLOUD_CLAIM`): ECDSA P-256 key made
  on the stick (NVS `stick`), the claim task (`cloud_client.cpp`), the
  credentials in NVS `cloud`, heartbeat every 30 s, unlink when the cloud
  forgets the device, Zabbix over TLS with the certificate, the code banner
  in the web UI and `GET /api/claim` (public on the LAN; approving still
  needs the account). `POST /api/claim/unlink` (login) forgets the link.

Not built yet:

- the setup page showing the code before the setup network closes (the code
  shows in the stick's web UI and on `GET /api/claim` once it is on Wi-Fi);
- the app's "Add a stick" that reads `/api/claim` and approves in one click;
- a cloud-plan rule for sticks (approval checks none today);
- deployment: migration 0009, the Connect Worker, the bridge Worker and its
  custom domain.

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
| Claim on the board: key and CSR made by mbedTLS accepted by the cloud, HTTPS to the bridge with the certificate bundle, TLS to zabbix.metagr.id with the stick's certificate | The board on Wi-Fi with internet, the bridge deployed (staging) |
| Values reach Zabbix and fill the template's items; backlog after a Zabbix outage | Zabbix server + host with the template (a UPS for full readings; without one only `ups.link = driverDown` goes out) |

## Order of work

1. ~~Profiles export and model detection~~ (done).
2. ~~Board profile + env `tescom-stick`, mDNS with the NUT service and the model~~ (done).
3. ~~Zabbix sender, plain TCP~~ (done).
4. Claim flow: ~~cloud endpoints, bridge page, firmware key/CSR + claim
   client + Zabbix TLS + heartbeat + web UI code~~ (done, not on hardware);
   left: code on the setup page, app "Add a stick", deploy.
5. Per-unit AP and login from a `factory` NVS partition, with a fallback for
   dev boards that have none.
6. Signed OTA (app-level).
7. Factory flashing script and label.
8. Prime: CH34x VCP host source and Modbus port of `tescom_prime.c`.
9. LiON RT `krauler` mode.
