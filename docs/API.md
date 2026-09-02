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

### `POST /api/monitor`

`{"on":true}` — enables the receiver and reports what it decodes in the
`lastSeen` field of `/api/learn/state`, without storing anything. Turn it off
when you are done; it keeps the receive interrupt running.

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

Send only the fields you want to change. **Omitting a password field leaves the
stored one alone**; there is no way to accidentally blank a secret by sending an
empty string.

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
