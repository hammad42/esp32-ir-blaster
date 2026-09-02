<div align="center">

# ESP32 Universal IR Blaster & Receiver

**Learn any infrared remote. Replay it from a web page, REST, MQTT, or a schedule.**

Built to be plugged in and forgotten about — including the air-conditioner
remotes that most DIY blasters give up on.

[![CI](https://github.com/hammad42/esp32-ir-blaster/actions/workflows/ci.yml/badge.svg)](https://github.com/hammad42/esp32-ir-blaster/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/hammad42/esp32-ir-blaster?sort=semver)](https://github.com/hammad42/esp32-ir-blaster/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32%20%7C%20S3%20%7C%20C3-informational)](platformio.ini)
[![Framework](https://img.shields.io/badge/arduino--esp32-2.0.17-teal)](platformio.ini)

[**Try the web UI →**](https://hammad42.github.io/esp32-ir-blaster/) ·
[Hardware](docs/HARDWARE.md) ·
[Usage](docs/USAGE.md) ·
[REST API](docs/API.md) ·
[MQTT](docs/MQTT.md)

</div>

---

```
   IR remote  ──►  VS1838B receiver ──► GPIO14 ──┐
                                                 │   ESP32
   Appliance  ◄──  IR LED + 2N2222  ◄── GPIO4  ──┘     │
                                                       │
                          web UI · REST · MQTT · schedules
```

Handles the easy protocols (NEC, Samsung, LG, Sony, RC5/RC6, Panasonic, JVC,
Sharp…) and the hard ones — air-conditioner remotes that send 400–600 timing
pairs across two or three frames, carrying the entire unit state in every burst.

## Contents

- [Why this one](#why-this-one)
- [Status](#status)
- [Quick start](#quick-start)
- [What it does](#what-it-does)
- [Documentation](#documentation)
- [Project layout](#project-layout)
- [Design decisions worth knowing](#design-decisions-worth-knowing)
- [Contributing](#contributing)
- [Licence](#licence)

---

## Why this one

Most ESP32 IR projects capture NEC codes from a TV remote and stop there. The
parts that take real work — and that this project does — are:

| | |
|---|---|
| **Air conditioners actually work** | A 90 ms silence threshold keeps a multi-frame burst together, gaps included, so it replays as one signal instead of three broken ones. |
| **Storage that survives** | One CRC32-protected file per command, with no index to corrupt: the index is rebuilt by scanning at boot, so a damaged file can only lose itself — and is deleted rather than blasted at your appliance as noise. |
| **Recovers by itself** | Watchdog on the main loop, WiFi retries every 30 s, a rescue access point after 5 minutes down *while still retrying*, MQTT reconnect with capped backoff. |
| **You can get back in** | Flashed the firmware but forgot the web UI? It serves a recovery page that says so and accepts the filesystem image over the air. Forgot the password? Hold the BOOT button 5–10 s. |
| **Honest about limits** | Every trade-off is written down, including [why this is a mains device](docs/BATTERY.md) and [what the security model does not cover](SECURITY.md). |

## Status

Every target builds clean with **zero warnings** against arduino-esp32 **2.0.17**
(`espressif32@6.13.0`):

| Target | Board | RAM | Flash (of OTA slot) |
|---|---|---|---|
| `esp32dev` | ESP32 DevKit v1 / WROOM-32 | 24.2 % | 67.8 % of 1.5 MB |
| `esp32dev_oled` | + SSD1306 display | 24.3 % | 69.7 % of 1.5 MB |
| `esp32s3` | ESP32-S3 DevKitC-1 | 23.8 % | 32.0 % of 3 MB |
| `esp32c3` | ESP32-C3 DevKitM-1 | 21.9 % | 71.4 % of 1.5 MB |

The web UI image is 48 KB, leaving ~848 KB of LittleFS for learned codes —
far more than the 128-command firmware ceiling needs.

> [!IMPORTANT]
> **This firmware has not yet been run on physical hardware.** It compiles
> clean, the web UI has been exercised end-to-end against a mocked API, and the
> logic has been reviewed — but the IR timing paths, WiFi reconnect policy and
> OTA flow are unproven on a real device. Treat your first session as
> commissioning, use the [troubleshooting checklist](docs/USAGE.md#if-something-does-not-work),
> and please [report what you find](https://github.com/hammad42/esp32-ir-blaster/issues/new?template=device_report.yml) —
> that is the single most useful contribution to this project.

## Quick start

```bash
git clone https://github.com/hammad42/esp32-ir-blaster.git
cd esp32-ir-blaster

pio run -e esp32dev -t upload      # firmware
pio run -e esp32dev -t uploadfs    # web UI  <-- do not skip this
pio device monitor
```

Both uploads are required — the web UI lives in the filesystem image. Prebuilt
binaries for every target are attached to each
[release](https://github.com/hammad42/esp32-ir-blaster/releases).

On first boot there are no WiFi credentials, so the device raises an access
point:

1. Join **`IR-Blaster-XXXXXX`**, password **`irblaster`**.
2. A setup page opens by itself (captive portal). If not, browse to
   <http://192.168.4.1>.
3. **Settings → WiFi → Scan networks**, pick yours, enter the password, Save.
4. It joins your network and is then at `http://ir-blaster.local`.

Then open **Learn**, press *Start listening*, and point a remote at the receiver.

Wiring is in [docs/HARDWARE.md](docs/HARDWARE.md) — start there if you have not
built the circuit yet. Two things worth knowing before you do: **power the
receiver from 3V3, not 5V** (ESP32 GPIOs are not 5 V tolerant), and **2N2222 and
BC547 have mirrored pinouts**, which is the most common reason a home-built
blaster does not transmit.

## What it does

**Capture.** Raw mark/space timings at 2 µs resolution into a 1024-entry buffer,
with a 90 ms silence threshold so multi-frame A/C bursts are captured as one
signal. The decoder runs alongside and records the protocol when it recognises
one. Explicit multi-part capture for remotes whose bursts are separated by more
than that.

**Storage.** Up to 128 commands as self-describing binary files in LittleFS,
each with a CRC32 over its timings.

**Transmission.** Simple protocols are regenerated from the decoded value — a
cleaner waveform than a re-radiated capture. A/C and unknown protocols replay
the stored timings. Per-command carrier frequency (30–60 kHz), repeat count, and
a force-raw override for the one appliance that argues.

**Reach.** Responsive web UI, a [REST API](docs/API.md), [MQTT with Home
Assistant auto-discovery](docs/MQTT.md) (every command becomes a button entity),
and wall-clock schedules with day-of-week masks.

**Staying up.** Task watchdog, self-healing WiFi, capped MQTT backoff, web OTA
for firmware *and* filesystem, ArduinoOTA, and a factory reset that requires
releasing the button inside a 5–10 second window so a stuck button cannot wipe
your commands.

## Documentation

| | |
|---|---|
| [docs/HARDWARE.md](docs/HARDWARE.md) | Pin maps for all three chips, schematics, parts list, assembly order, range tuning |
| [docs/USAGE.md](docs/USAGE.md) | First boot, learning, air conditioners, schedules, backup, troubleshooting |
| [docs/API.md](docs/API.md) | Every REST endpoint with `curl` examples |
| [docs/MQTT.md](docs/MQTT.md) | Topics, Home Assistant discovery, a climate template, Node-RED |
| [docs/CASE.md](docs/CASE.md) | 3D-printed enclosure notes |
| [docs/BATTERY.md](docs/BATTERY.md) | Why this is a mains device, and what a battery build would take |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Building, code style, dependency policy, scope |
| [SECURITY.md](SECURITY.md) | Threat model, stated plainly |

## Project layout

```
platformio.ini            build environments for ESP32 / S3 / C3 / OLED
partitions_ir_4mb.csv     1.5 MB x2 OTA + 896 KB LittleFS + coredump
partitions_ir_8mb.csv     3 MB x2 OTA + 1.9 MB LittleFS

include/
  config.h                limits, timeouts, defaults
  board_pins.h            per-chip pin map, all overridable with -D

src/
  main.cpp                setup/loop, watchdog, button policy, factory reset
  settings.{h,cpp}        CRC-protected settings blob in NVS
  ir_store.{h,cpp}        command storage: one CRC'd file each, index by scan
  ir_service.{h,cpp}      capture and transmit
  net_manager.{h,cpp}     WiFi, captive portal, reconnect policy, mDNS, NTP
  web_ui.{h,cpp}          server, static files, commands, learning
  web_ui_admin.cpp        backup, settings, schedules, system, OTA, routes
  mqtt_manager.{h,cpp}    MQTT + Home Assistant discovery
  schedules.{h,cpp}       wall-clock schedules
  indicators.{h,cpp}      non-blocking status LEDs
  button.{h,cpp}          debounced button with press-duration reporting
  display.{h,cpp}         optional SSD1306 (compiled out by default)
  log_ring.{h,cpp}        in-RAM log shown on the System tab

data/                     web UI, flashed with `-t uploadfs`
tools/                    demo generator for the GitHub Pages preview
```

## Design decisions worth knowing

**The synchronous `WebServer`, not `ESPAsyncWebServer`.** An IR blaster serves a
handful of tiny requests a minute; async buys nothing here, and costs a callback
context with its own stack plus a third-party fork whose compatibility has to be
re-established against every core release. `WebServer` ships with the core and
keeps everything in one loop task — which also means the transmitter can never
be entered re-entrantly.

**Codes in LittleFS, settings in NVS.** One A/C command is ~1.2 KB of timings;
the default NVS partition is 20 KB *total*. Settings are small and benefit from
NVS's wear levelling, so they stay there as a single CRC-checked blob.

**Raw is always stored.** Raw timings are the ground truth and replay protocols
nobody has reverse engineered. Decoded protocol/value is kept alongside and
preferred at send time for simple remotes; state-based A/C protocols always fall
back to raw, because their state arrays cannot be reconstructed from one 64-bit
value.

**The receiver is off unless you are using it.** A blaster idling for years has
no reason to run a 50 µs timer interrupt, and a demodulator staring at a plasma
TV would refill the capture buffer forever.

**Import is one command per request.** A 100-command backup is ~300 KB of JSON,
which will not parse on this device. The browser walks the file and posts each
entry, so peak memory is one command instead of the whole archive.

**Dependencies are pinned exactly.** The platform *and* every library. The point
of the project is a device that still rebuilds identically in three years — see
[CONTRIBUTING.md](CONTRIBUTING.md#dependencies-are-pinned-exactly) for how to
bump one.

## Contributing

Compatibility reports are the most valuable contribution — protocol behaviour is
the one thing CI cannot test. If you build this, please
[tell us what your remote did](https://github.com/hammad42/esp32-ir-blaster/issues/new?template=device_report.yml),
whether it worked or not.

See [CONTRIBUTING.md](CONTRIBUTING.md) for building, style and scope.

## Acknowledgements

Built on [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) by
crankyoldgit and contributors, which does the genuinely hard part — the protocol
decoding. Also uses [ArduinoJson](https://arduinojson.org/) and
[PubSubClient](https://github.com/knolleary/pubsubclient).

## Licence

[MIT](LICENSE) — do what you like with it.
