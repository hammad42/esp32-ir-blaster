# REST API

Base URL: `http://ir-blaster.local` (or the device's IP).

All request bodies are JSON. All responses are JSON with an `ok` field; on
failure the body is `{"ok":false,"error":"..."}` and the HTTP status is 4xx/5xx.

If authentication is enabled, every endpoint takes HTTP basic auth:

```bash
curl -u admin:secret http://ir-blaster.local/api/status
```

---

## Commands

### `GET /api/commands`

Lists everything, sorted by group then name.

```json
{
  "ok": true,
  "groups": ["Bedroom TV", "Living Room AC"],
  "commands": [
    {
      "id": "0000001a", "name": "AC_Cool_24", "group": "Living Room AC",
      "protocol": "UNKNOWN", "bits": 0, "raw": 583, "frames": 2,
      "khz": 38, "repeats": 1, "forceRaw": false, "created": 1756800000
    }
  ]
}
```

`created` is 0 if the clock was not yet set when the command was learned.

### `POST /api/commands/send`

Accepts an `id` **or** a `name`, so integrations can stay readable.

```bash
curl -X POST http://ir-blaster.local/api/commands/send \
     -H 'Content-Type: application/json' \
     -d '{"name":"AC_Power","repeats":2}'
```

| Field | Type | Notes |
|---|---|---|
| `id` | string | command id |
| `name` | string | used if `id` is absent; case-insensitive |
| `repeats` | int | optional, 1–20; omit to use the command's own setting |

→ `{"ok":true,"sent":"AC_Power"}`

### `POST /api/commands/raw`

Sends timings without storing them — handy for testing a code from elsewhere.

```json
{ "raw": [9000,4500,560,560,560,1690], "khz": 38, "repeats": 1 }
```

Maximum 1024 entries. Values are microseconds, alternating mark and space,
starting with a mark.

### `POST /api/commands/rename`

```json
{ "id": "0000001a", "name": "AC_Cool_24", "group": "Living Room AC" }
```

Only the header is rewritten; the timings are never touched.

### `POST /api/commands/options`

```json
{ "id": "0000001a", "repeats": 3, "khz": 38, "forceRaw": true }
```

### `POST /api/commands/delete`

```json
{ "id": "0000001a" }
```

Also clears the command's Home Assistant discovery entry and drops any schedule
that pointed at it.

---

## Learning

| Endpoint | Method | Body |
|---|---|---|
| `/api/learn/start` | POST | `{"timeoutMs":20000,"append":false}` — both optional |
| `/api/learn/state` | GET | — |
| `/api/learn/test` | POST | — sends the pending capture without saving |
| `/api/learn/save` | POST | `{"name":"AC_Power","group":"Living Room AC"}` |
| `/api/learn/cancel` | POST | — |

`append: true` adds another part to the pending capture instead of replacing it
(up to 4 parts).

`GET /api/learn/state`:

```json
{
  "ok": true, "state": "captured", "error": "", "remainingMs": 0,
  "protocol": "NEC", "bits": 32, "raw": 67, "frames": 1,
  "value": "0x20DF10EF", "lastSeen": ""
}
```

`state` is one of `idle`, `waiting`, `captured`, `timeout`, `error`.

### `POST /api/selftest`

Transmits a known NEC frame **with the receiver left on** and checks it comes
back. Needs the IR LED pointing at the receiver, 10–30 cm apart, or both
bounced off a wall. No remote and no appliance required.

This is the one operation that deliberately breaks the "never listen while
transmitting" rule, because hearing ourselves is the point.

```bash
curl -X POST http://ir-blaster.local/api/selftest
```

```json
{
  "ok": true, "pass": true,
  "rxIdleOk": true, "idleLowSamples": 0, "idleSamples": 200,
  "received": true, "matched": true, "attempts": 1,
  "verdict": "transmitter and receiver are both working",
  "expectedProtocol": "NEC", "expectedValue": "0x20DF10EF",
  "protocol": "NEC", "value": "0x20DF10EF", "bits": 32, "raw": 67
}
```

`ok` reports that the *request* succeeded; **`pass` is the test result** — a
failed test still returns HTTP 200.

The test runs in three stages so a failure says which half is broken:

| Stage | What it checks | If it fails |
|---|---|---|
| `rxIdleOk` | the receiver's output sits idle-high before anything is sent | receiver has no power, or `OUT` is not reaching the pin — the transmitter is never even exercised |
| `received` | any signal came back | receiver is alive but the LED is not emitting: transistor pinout, LED polarity, LED current |
| `matched` | protocol, value and bit count all round-tripped | optics — too close and the sensor saturates, too far and it fades. 10–30 cm facing each other |

It blocks for up to ~1.5 s (three attempts, each waiting out the 90 ms receive
timeout) and feeds the watchdog while it waits.

### `POST /api/monitor`

`{"on":true}` — enables the receiver and reports what it decodes in the
`lastSeen` field of `/api/learn/state`, without storing anything. Turn it off
when you are done; it keeps the receive interrupt running.

---

## Built-in library

Air conditioners do not send "temperature up" -- every button transmits the
unit's whole state, so teaching one by hand means capturing every temperature
separately. These endpoints let the device build the codes instead, using
IRremoteESP8266's encoders, so any temperature and mode is available whether or
not it was ever captured.

### `GET /api/library/protocols`

```json
{"ok":true,"protocols":[{"id":6,"name":"AIRTON"},{"id":32,"name":"GREE"}]}
```

Sorted by name. Only protocols this build can *generate* appear; a remote whose
protocol is missing is still perfectly usable via Learn.

The list is the 65 IRremoteESP8266 supports plus **`DAWLANCE`**, which is
implemented here because the library does not know it. Its frame is 72 bits over
a 6.7/3.3 ms header and matches no protocol upstream, so captures from those
remotes decode as `UNKNOWN`. All five modes are verified against captures from
a real remote, including the two bytes those captures corrected — see the
Dawlance section of `HANDOFF.md`.

### Preset packs

For remotes that cannot be generated at all, `data/presets/index.json` lists
ready-made packs of captured commands, each a static file served from the
device. They install through `/api/import` one command at a time -- the same
path a restore takes -- so a pack needs no firmware support:

```bash
curl -s http://ir-blaster.local/presets/index.json
jq -c '.commands[]' pack.json | while read -r c; do
  curl -sS -X POST http://ir-blaster.local/api/import \
       -H 'Content-Type: application/json' -d "$c"; echo
done
```

### `POST /api/library/ac/preview` · `/send` · `/save`

All three take the same body and differ only in what they do: **preview**
transmits nothing and just echoes what the device understood, **send**
transmits without storing, **save** stores it as an ordinary command.

```bash
curl -X POST http://ir-blaster.local/api/library/ac/save \
  -H 'Content-Type: application/json' \
  -d '{"protocol":"GREE","power":true,"mode":"cool","degrees":24,
       "fan":"auto","name":"AC Cool 24","group":"Air Conditioner"}'
```

| Field | Default | Notes |
|---|---|---|
| `protocol` | — | **required**, e.g. `GREE`, case-insensitive |
| `model` | −1 | for brands with variants |
| `power` | `true` | |
| `mode` | `"cool"` | `auto` `cool` `heat` `dry` `fan` |
| `degrees` | `24` | 10–35 °C, or 50–95 with `celsius:false` |
| `celsius` | `true` | |
| `fan` | `"auto"` | `auto` `min` `low` `medium` `high` `max` |
| `swingv` `swingh` | `"off"` | |
| `quiet` `turbo` `econo` `light` `filter` `clean` `beep` | `false` | not every protocol honours every flag |
| `sleep` | −1 | minutes, −1 for off |
| `name` `group` | — | **save only**; group defaults to `Air Conditioner` |

→ `{"ok":true,"summary":"GREE · on · Cool · 24C · fan Auto","id":"0000003a"}`

A saved entry stores the standard state struct rather than timings — about
40 bytes against ~1.2 KB for the equivalent capture — and is replayed by handing
it back to the library, so the checksum is always the library's. It behaves like
any other command everywhere else: Remotes, schedules, MQTT, backup.

A temperature far outside thermostat range is rejected rather than encoded, on
the grounds that it is far more likely to be Fahrenheit sent with
`celsius:true` than a real request.

### `GET /api/library/tv/models`

Televisions work the opposite way to air conditioners. A/C frames carry the
unit's whole state, so they are *generated*; a TV button is one fixed code with
no state in it, so there is nothing to derive and the firmware simply holds a
table of measured values.

→ `{"ok":true,"models":[…],"buttons":[…]}`

Each model lists every button with `true` or `false` for whether a code is
known:

```json
{ "id": "tcl-nikai", "brand": "TCL", "model": "Smart TV",
  "protocol": "NIKAI", "note": "…",
  "buttons": { "power": true, "vol_up": true, "vol_down": false, … } }
```

`buttons` at the top level is the canonical id → label list, in display order.

Codes are never guessed. A button with no capture reports `false` and the
endpoints below refuse it, because a wrong IR code is worse than a missing one:
a missing button is visibly missing, while a wrong one looks fine and silently
does nothing.

### `GET /api/schedules/log` · `POST /api/schedules/log/clear`

What scheduled fires actually did, newest first. A schedule runs unattended, so
each fire records both halves: what went out, and what the receiver overheard
coming back while it went.

```json
{ "at": 1772668800, "scheduleId": 1, "label": "Bedtime AC off",
  "command": "AC_Off", "txOk": true, "heard": true,
  "protocol": "UNKNOWN", "bits": 0, "rawLen": 291, "value": "0x0",
  "error": "" }
```

| field | means |
|---|---|
| `txOk` | the firmware transmitted without error |
| `heard` | the receiver decoded something during the listening window |
| `match` | `match`, `mismatch`, or `unchecked` -- whether what came back is what went out |
| `protocol` `bits` `value` `rawLen` | what came back; `UNKNOWN` with a `rawLen` is normal for an A/C |
| `error` | why the send failed, when `txOk` is false |

Three tiers, and they are not the same claim:

- `txOk` is **blind** -- the send function returned without error.
- `heard` is physical evidence: the demodulator produced pulses and the
  decoder made sense of them. It does not prove they were *ours*; another
  remote pressed at that instant would also satisfy it.
- `match` closes that gap. For a simple protocol it compares value and bit
  count exactly, the same test the self-test uses. For a raw capture it
  compares frame length, allowing a few entries of slack and accepting a
  single frame of a multi-frame command. For a generated A/C command it
  reports `unchecked`: the stored payload is a state struct, so there is no
  frame length to compare against.

**None of them can say the appliance reacted** -- an air conditioner sends no
reply -- so a confirmed fire means the blaster worked, not that the room got
cooler. It also depends on the receiver being able to hear the emitter; aim
the LED somewhere the sensor cannot see and every fire reads as not heard.

The log holds the last 16 fires in `/firelog.json`, kept apart from
`/schedules.json` so rewriting it after every fire cannot endanger the
schedules themselves. It survives a reboot.

---

### `POST /api/library/tv/send` · `/save`

| field | required | notes |
|---|---|---|
| `model` | yes | an `id` from `/api/library/tv/models` |
| `button` | yes | `power`, `vol_up`, `vol_down`, `ch_up`, `ch_down`, … |
| `name` `group` | — | **save only**; group defaults to `TV` |

```bash
curl -X POST http://ir-blaster.local/api/library/tv/send \
     -H 'Content-Type: application/json' \
     -d '{"model":"tcl-nikai","button":"power"}'
```

→ `{"ok":true,"label":"Power"}`

Asking for a button the model has no code for returns
`no code captured for that button yet` rather than transmitting anything.

A saved TV button is stored with both its decoded value and its timings, so it
replays from the protocol (a clean regenerated frame) and still carries raw
timings for backup and for `forceRaw`.

---

## Backup

### `GET /api/export`

Streams the whole archive as a download. Chunked, so a hundred commands never
need to exist in the device's memory at once.

```json
{
  "format": "ir-blaster-backup", "version": 1, "fw": "1.0.0",
  "commands": [
    { "name": "AC_Power", "group": "Living Room AC", "protocol": -1,
      "bits": 0, "value": "0x0", "khz": 38, "repeats": 1, "forceRaw": false,
      "frameLens": [291, 292], "raw": [3400, 1700, 450, 1250, ...] }
  ]
}
```

### `POST /api/import`

Takes **one** command object — the same shape as an element of `commands`
above. Post them in a loop to restore an archive; that is what the web UI does.

```bash
jq -c '.commands[]' ir-backup.json | while read -r cmd; do
  curl -sS -X POST http://ir-blaster.local/api/import \
       -H 'Content-Type: application/json' -d "$cmd"
  echo
done
```

Duplicate names are rejected with a 400 rather than overwriting.

---

## Settings

### `GET /api/settings`

Returns the configuration. Secrets are never returned — you get
`wifiPassSet`, `apPassSet`, `mqttPassSet`, `authPassSet` booleans instead.

### `POST /api/settings`

Send only the fields you want to change.

Secrets follow ordinary REST semantics:

| Field | Effect |
|---|---|
| **absent** | left exactly as it is |
| **present, non-empty** | set to that value |
| **present, empty string** | **cleared** |

So `{"wifiPass": ""}` removes the stored WiFi password, which is how you join
an open network. The same applies to `apPass`, `mqttPass` and `authPass` --
clearing `authPass` also forces `authEnabled` off, so the API cannot lock you
out of your own device.

The web UI never sends an empty string by accident: typing nothing in a password
box still means "no change", and clearing requires ticking that field's
**Clear it** box.

```json
{
  "hostname": "ir-blaster",
  "wifiSsid": "MyNetwork", "wifiPass": "secret",
  "mqttEnabled": true, "mqttHost": "192.168.1.10", "mqttPort": 1883,
  "mqttBase": "irblaster", "haDiscovery": true, "haPrefix": "homeassistant",
  "tz": "PKT-5", "ntp1": "pool.ntp.org",
  "defaultRepeats": 1, "defaultFreqKhz": 38, "repeatGapMs": 40,
  "frameGapMs": 25, "markExcessUs": 0, "learnTimeoutMs": 20000,
  "ledEnabled": true,
  "authEnabled": false, "authUser": "admin", "authPass": "..."
}
```

→ `{"ok":true,"wifiChanged":true}` — when `wifiChanged` is true the device
reconnects immediately and may come back on a different address.

Out-of-range numbers are clamped to their defaults rather than rejected.
`authEnabled` is forced off if no password is set, so the API cannot lock you
out of your own device.

### `GET /api/wifi/scan`

```json
{"ok":true,"networks":[{"ssid":"MyNetwork","rssi":-52,"secure":true}]}
```

Blocks for a few seconds while the radio scans.

---

## Schedules

### `GET /api/schedules`

```json
{
  "ok": true, "now": "2026-09-02 23:11:04",
  "schedules": [
    { "id": 1, "enabled": true, "hour": 23, "minute": 0, "dow": 127,
      "repeats": 0, "cmd": "0000001a", "label": "Bedtime AC off" }
  ]
}
```

`now` is empty when the clock is unset — schedules do not fire in that state.

### `POST /api/schedules`

Creates when `id` is 0, updates otherwise.

| Field | Notes |
|---|---|
| `hour`, `minute` | local time |
| `dow` | bitmask, **bit 0 = Sunday** … bit 6 = Saturday. `127` = every day, `62` = weekdays, `65` = weekends |
| `repeats` | 0 = use the command's own setting |
| `cmd` | command **id** (not name) |

### `POST /api/schedules/delete`

```json
{ "id": 1 }
```

---

## System

### `GET /api/status`

```json
{
  "ok": true, "fw": "1.0.0", "host": "ir-blaster", "mdns": "ir-blaster.local",
  "ip": "192.168.1.42", "wifi": "connected", "ssid": "MyNetwork", "rssi": -52,
  "portal": false, "apSsid": "IR-Blaster-A1B2C3",
  "uptime": 864210, "heap": 148320, "heapMin": 131044, "heapMax": 110592,
  "fsUsed": 61440, "fsTotal": 917504,
  "commands": 37, "maxCommands": 128, "schedules": 3,
  "lastSent": "AC_Cool_24", "lastSentAgo": 12, "txCount": 1841, "rxCount": 96,
  "mqtt": "connected", "mqttError": "", "time": "2026-09-02 23:11:04",
  "resetReason": 1, "learn": "idle", "monitor": false, "authEnabled": false
}
```

Good things to graph: `heapMin` (a slow decline means a leak), `uptime` (resets
you did not order), `rssi`.

### `GET /api/logs`

The last 40 log lines, oldest first, each prefixed with an uptime stamp and a
level letter.

### Other

| Endpoint | Method | Notes |
|---|---|---|
| `/api/system/reboot` | POST | replies first, then restarts |
| `/api/system/portal` | POST | toggles the setup access point |
| `/api/system/factory-reset` | POST | requires `{"confirm":"ERASE"}` |
| `/api/mqtt/discovery` | POST | republishes Home Assistant discovery |
| `/api/ota/firmware` | POST | multipart, field name `image` |
| `/api/ota/filesystem` | POST | multipart, field name `image` |

```bash
curl -X POST -F "image=@.pio/build/esp32dev/firmware.bin" \
     http://ir-blaster.local/api/ota/firmware
```

---

## Notes for integrators

**One command at a time.** The transmitter is single-threaded by design. A send
while another is in flight returns `{"ok":false,"error":"transmitter busy"}`
rather than queueing — retry after a moment. A long A/C burst with repeats can
occupy the device for a second or two.

**Ids are stable.** They come from a counter persisted in NVS and are never
reused, even after every command is deleted. Renaming does not change the id.

**Names are the friendlier handle.** `/api/commands/send` accepts either, so
scripts can use names and survive a device rebuild from a backup — where ids
will differ but names will not.
