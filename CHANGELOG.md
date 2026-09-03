# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

Because this is firmware, each release notes **what it was verified against** —
"builds" and "tested on hardware" are different claims and the distinction
matters when you are deciding whether to flash a device that is working today.

## [Unreleased]

### Fixed

- **Out-of-bounds write when storing a capture.** The bound was computed over a
  different range of the receive buffer than the write loop walked, so an
  interval above 65535 us in the final slot could push the write past the end of
  the capture buffer. The leading gap is normally the largest value present, and
  its surplus had been masking the shortfall.
- **LittleFS left unmounted after a failed filesystem OTA.** If `Update.begin()`
  failed, nothing remounted the filesystem and the device ran without storage
  until the next reboot. The remount now happens on every path that can end an
  update, and no longer formats on failure -- silently reformatting a
  half-written partition destroyed every learned command with no confirmation.
- **Schedules deleted by a storage glitch.** Orphan cleanup ran at boot whether
  or not the command store had mounted, and persisted the result, turning a
  transient fault into permanent data loss. It is now gated on a successful
  mount and refuses to prune when the store is empty but schedules exist.
- **"Discard" did nothing to a finished capture.** It only acted while still
  listening, so the capture and its UI survived -- and a following "capture
  another part" appended to the signal that had just been rejected.
- **Stored passwords could not be cleared.** Both the firmware and the browser
  refused empty secrets, so joining an open WiFi network, using an anonymous
  MQTT broker or reopening the setup AP were all impossible without a factory
  reset. Each field now has an explicit *Clear it* option.
- **WiFi scan showed only one network.** The SSID box used a `<datalist>`, which
  filters against whatever is already typed in its input -- and that box is
  pre-filled with the current network. Replaced with an explicit tap-to-pick
  list showing signal strength and open networks.
- `IrStore::begin()` no longer formats the filesystem silently when a mount
  fails; it says what it is about to erase first.

### Added

- `tools/ir-backup.sh` -- save, list and restore learned commands over the REST
  API from a terminal, so a backup can sit in a flashing script rather than
  requiring a trip through the browser.

## [1.0.0] — 2026-09-03

Initial release.

### Added

- **IR capture** — raw mark/space timings at 2 µs resolution into a 1024-entry
  buffer, with a 90 ms silence threshold so multi-frame air-conditioner bursts
  are captured as one signal, inter-frame gaps included. Explicit multi-part
  capture for remotes whose bursts are separated by more than that.
- **IR transmission** — protocol regeneration for recognised simple remotes,
  raw replay for A/C and unknown protocols, per-command carrier frequency
  (30–60 kHz), repeat count, and a force-raw override.
- **Storage** — up to 128 commands as self-describing CRC32-protected files in
  LittleFS. No index file: the in-RAM index is rebuilt by scanning headers at
  boot, so a damaged file can only lose itself, and is deleted rather than
  transmitted as noise.
- **Web UI** — responsive single-page interface served from the device, with no
  external requests: remotes, learning, schedules, settings, and a system tab
  with logs, backup and OTA.
- **WiFi** — captive-portal setup, mDNS, 30 s reconnect retries, and a rescue
  access point raised after 5 minutes down while still retrying the configured
  network.
- **REST API** — full control surface, documented in `docs/API.md`.
- **MQTT** — control topics, retained state, last-will availability, and Home
  Assistant discovery publishing every command as a button entity.
- **Schedules** — up to 24 wall-clock schedules with day-of-week masks, gated
  on a valid NTP clock so nothing fires on an unset time.
- **OTA** — firmware and filesystem updates from the web UI and from
  PlatformIO, over two OTA slots with automatic rollback.
- **Reliability** — task watchdog on the main loop, CRC-protected settings blob
  in NVS, brownout and reset-reason reporting, and a hardware factory reset
  that requires release inside a 5–10 second window so a stuck button cannot
  trigger it.
- **Optional** — SSD1306 status display, and two macro buttons for local
  control, both compiled out by default.
- Build targets for ESP32, ESP32-S3, ESP32-C3 and an OLED variant.
- Documentation: hardware wiring, usage and troubleshooting, REST API, MQTT and
  Home Assistant, 3D-printed case notes, and an honest analysis of why this is
  a mains-powered device.

### Verified

- All four PlatformIO targets build with **zero warnings** against
  `espressif32@6.13.0` (arduino-esp32 2.0.17), IRremoteESP8266 2.9.0,
  ArduinoJson 7.4.3, PubSubClient 2.8.0.
- Flash usage: 67.8 % of the 1.5 MB OTA slot on `esp32dev`; RAM 24.2 %.
- The web UI was exercised in a browser against a mocked API — every tab, the
  command options modal, and the full learn flow.
- **Not yet verified on physical hardware.** The IR timing paths, the WiFi
  reconnect policy and the OTA flow are unproven against a real device. Treat
  the first session with a real remote as commissioning, and please report what
  you find.

[Unreleased]: https://github.com/hammad42/esp32-ir-blaster/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/hammad42/esp32-ir-blaster/releases/tag/v1.0.0
