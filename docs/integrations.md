# Integrations contract: NUT, MQTT, Home Assistant, Connect API

Decided 2026-11-14. One contract so the stick (this repo), `tescom-agent`
(nut-app `crates/tescom-agent`), Tescom Connect (nut-app `cloud`) and the
Home Assistant integration (its own repository) agree. Change it here first,
then in the code.

Tescom Connect speaks the open standards (NUT, MQTT, Home Assistant, Zabbix,
REST, webhooks) and offers its own app and cloud on top. A claim goes on the
Connect landing page only once it works.

## 1. NUT (works today)

- The stick serves NUT on TCP 3493 and announces `_nut._tcp` over mDNS with the
  model in a TXT record. Home Assistant's built-in NUT integration finds it by
  zeroconf; nothing Tescom-specific is needed.
- UPS name `ups`. Variables are NUT's standard names (`ups.status`,
  `battery.charge`, ...), the same ones every other part of this contract uses.

## 2. MQTT

Same layout from the stick and from `tescom-agent`, so Home Assistant and any
other MQTT consumer (Node-RED, openHAB, ...) see the same thing whatever feeds it.

### Settings

`enabled`, `host`, `port` (1883; 8883 with TLS), `tls`, `username`, `password`,
`base_topic` (default `tescom`), `discovery` (default on), `discovery_prefix`
(default `homeassistant`), `interval` (seconds between full states, default 10;
a status change is published at once).

### Identifiers

- `node_id`: the publishing device, lowercase `[a-z0-9_-]`. Stick: its hostname
  (`tescom-stick-2c49c0`). Agent: `tescom-agent-<hostname>`.
- `ups_id`: the NUT UPS name on that node (`ups` on the stick). An agent with
  several UPSs publishes each under its own `ups_id`.
- `uid` = `<node_id>_<ups_id>`, used for Home Assistant unique ids.

### Topics (`<base>` = `base_topic`)

| Topic | Retained | Payload |
| --- | --- | --- |
| `<base>/<node_id>/availability` | yes | `online` / `offline` (also the MQTT last will) |
| `<base>/<node_id>/<ups_id>/state` | no | JSON object of NUT variables, values as NUT strings: `{"ups.status":"OL","battery.charge":"100",...}`, plus `"ts"` (Unix seconds) |
| `<base>/<node_id>/<ups_id>/status` | yes | `ups.status` alone (`OL`, `OB LB`, ...) |
| `<base>/<node_id>/<ups_id>/link` | yes | `ok`, `stale` or `down`: the UPS link (as Zabbix's `ups.link`) |
| `<base>/<node_id>/<ups_id>/info` | yes | JSON: `mfr`, `model`, `serial` (when known), `firmware`, `node_version`, `config_url` |
| `<base>/<node_id>/<ups_id>/command` | - | reserved for later (beeper, battery test); not subscribed yet |

### Home Assistant discovery

Per-entity discovery (works on every HA version with MQTT), retained, on
`<discovery_prefix>/<component>/<uid>/<object_id>/config`. Every entity has
`availability_topic` = the node's availability, `state_topic` = the state topic
with a `value_template` picking its variable, and one shared `device` block:

```json
{"identifiers": ["tescom_<uid>"], "manufacturer": "Tescom", "model": "Leo SmartBox",
 "name": "Tescom Leo SmartBox", "sw_version": "<node firmware/agent version>",
 "configuration_url": "http://<stick ip>/", "via_device": "tescom_<node_id>"}
```

Entities, created only for variables the UPS reports:

| object_id | Component | From | Unit / device_class |
| --- | --- | --- | --- |
| `status` | sensor | `ups.status` | - |
| `mains_power` | binary_sensor | on unless `OB` is in `ups.status` | `power` (on = mains present; HA reads "Detected" / "Cleared") |
| `low_battery` | binary_sensor | `LB` in `ups.status` | `battery` |
| `battery_charge` | sensor | `battery.charge` | `%`, `battery` |
| `battery_runtime` | sensor | `battery.runtime` | `s`, `duration` |
| `battery_voltage` | sensor | `battery.voltage` | `V`, `voltage` |
| `input_voltage` | sensor | `input.voltage` | `V`, `voltage` |
| `output_voltage` | sensor | `output.voltage` | `V`, `voltage` |
| `input_frequency` | sensor | `input.frequency` | `Hz`, `frequency` |
| `load` | sensor | `ups.load` | `%`, no device_class |
| `realpower` | sensor | `ups.realpower` | `W`, `power` |
| `temperature` | sensor | `ups.temperature` | `°C`, `temperature` |
| `link` | sensor (diagnostic) | link topic | - |

Removing a UPS or turning discovery off publishes empty retained configs.

`mains_power` replaced `on_battery` on 2026-11-15: with the `power` device class
an "on battery" sensor read "Detected power" during a power cut. Publishers send
an empty retained config for `<uid>/on_battery` once, so the old entity goes
away; its name is "Mains power".

**Names and the node device** (added 2026-11-15, after the first test in Home
Assistant showed "UPS by Tescom", "Connected via Unnamed device"):

- The UPS device's `model` is the Tescom model from the profile ("Leo SmartBox",
  the profile's short name, as the stick's `ups.model`), its `name` is
  "Tescom " + model ("Tescom Leo SmartBox"). Only when no profile fits: the
  UPS's own `device.model`, else "UPS". Entity names stay short ("Battery
  charge"); HA puts the device name in front.
- The UPS's own firmware, when the UPS reports it, goes in `hw_version`;
  `sw_version` is the publisher's version.
- The node (`tescom_<node_id>`) must be a device of its own, or HA shows
  "Unnamed device": the node publishes one diagnostic entity, object id
  `connection` (binary_sensor, `connectivity`, from the availability topic),
  whose `device` block is `{"identifiers":["tescom_<node_id>"],"manufacturer":"Tescom",
  "model":"Tescom Edge" (stick) | "Tescom Connect" (agent),"name":"Tescom Edge <last 6 of MAC>" |
  "Tescom Connect on <hostname>","sw_version":"...","configuration_url":"..."}`.
  UPS devices point at it with `via_device`.
- The brand logo next to these devices in Home Assistant is MQTT's: only an
  integration listed in Home Assistant's brands repository shows its own logo,
  which is what the Tescom Connect integration (section 4) is for.

## 3. Tescom Connect public API v1

A documented, stable subset of the Connect API for other systems. The routes the
web app uses keep their session sign-in; the public ones also accept tokens.

- **Base:** `https://connect.tescom.gr/v1` (staging: the staging Worker).
  Spec at `GET /v1/openapi.json` (OpenAPI 3.1), docs on a developer page.
- **Auth:** `Authorization: Bearer <token>`.
  - Personal API tokens, made and revoked in the account page, shown once,
    stored hashed, prefix `tc_pat_`.
  - OAuth 2.0 authorization code with PKCE for apps (Home Assistant):
    `/oauth/authorize`, `/oauth/token`, refresh tokens, registered clients.
  - Scopes: `read` (sites, devices, readings, events), `control` (commands,
    tests, later), `webhooks`.
- **Resources:** `GET /v1/me`; `GET /v1/sites`; `GET /v1/devices` (UPSs, sticks
  and agents with their site); `GET /v1/devices/{id}`; `GET /v1/devices/{id}/latest`
  (NUT variables, same names as above); `GET /v1/devices/{id}/history?from&to&vars`;
  `GET /v1/devices/{id}/events`; `GET /v1/devices/{id}/tests`.
- **Webhooks:** `GET/POST /v1/webhooks`, `DELETE /v1/webhooks/{id}`,
  `POST /v1/webhooks/{id}/test`. Events: `power.lost`, `power.restored`,
  `battery.low`, `device.offline`, `device.online`, `test.finished`. Body
  `{"id","type","created","device":{...},"data":{...}}`; header
  `Tescom-Signature: t=<unix>,v1=<hex HMAC-SHA256 of "t.body" with the webhook secret>`;
  retries with backoff for 24 h.
- **Conventions:** JSON; errors `{"error":{"code","message"}}`; cursor
  pagination (`?cursor=`, `next_cursor`); rate limits per token with
  `RateLimit-*` headers and 429; times in ISO 8601 UTC.

## 4. Home Assistant "Tescom Connect" integration

Its own repository, HACS first, Home Assistant core later.

- Config flow: sign in with the Tescom account (OAuth2 through HA's
  application credentials), or add a stick found on the LAN.
- Cloud: devices from `GET /v1/devices`, readings from `/latest` on a
  coordinator (60 s), events pushed by a webhook HA registers
  (`async_generate_url`), so a power cut shows at once.
- Local: sticks found by zeroconf `_nut._tcp` with a Tescom TXT; reads NUT
  directly so it works without internet.
- Entities: the same set as the MQTT discovery table, same names, so a user
  switching from MQTT to the integration keeps the look.
