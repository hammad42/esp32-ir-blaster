# esp32-ir-blaster — Principal Architecture Review

> **Method:** I checked the project profile against the actual source in the workspace (`src/`, `include/`, `platformio.ini`, partition tables). In a few places the code and the profile disagree. Those places are listed in §0, because they change some of the conclusions.

---

## 0. Where the Profile and the Code Disagree

| Profile claim | What the code actually does | Impact |
|---|---|---|
| "TX driven via ESP32 hardware **RMT** peripheral" | TX goes through `IRsend::sendRaw()` / `IRsend::send()` from IRremoteESP8266. On ESP32 this is a **software-timed carrier**: GPIO toggling plus `delayMicroseconds`. Nothing in `src/` uses RMT. | Interrupts can add carrier jitter, and the CPU is pinned for the whole burst (see §3.2) |
| "**Rollback-safe** dual OTA" | It is an A/B scheme with atomic switch-over. Nothing calls `esp_ota_mark_app_valid_cancel_rollback()`, and the prebuilt Arduino 2.0.17 bootloader ships with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` off. | An image that boots and then crash-loops is **not** reverted automatically |
| "Deliberately **version-pinned**" | The core libs are pinned exactly, but the `esp32dev_oled` env uses `^2.5.9` / `^1.11.9` ([platformio.ini:L60-63](platformio.ini#L60-L63)) | The OLED build is not reproducible |
| "1024-entry **ring buffer**" | IRremoteESP8266 uses a linear, double-buffered `rawbuf` (`save_buffer=true`), copied into a linear `capBuf_` | Naming only. The design is fine. |
| Carrier frequency per learned command | Captures are saved with `cfg().defaultFreqKhz` ([src/ir_service.cpp:L263](src/ir_service.cpp#L263)). The carrier is **assumed, not measured**. | Learned 36/40/56 kHz remotes are replayed at 38 kHz |

---

## 1. Overall Rating & Executive Summary

| Dimension | Score | One-line justification |
|---|:-:|---|
| Architecture & System Design | **7.5 / 10** | Clean modules, a deliberate single-loop concurrency model, self-describing storage. Held back by one shared FS partition for UI and user data, and by blocking TX. |
| Firmware Robustness & Failure-Domain Isolation | **7 / 10** | Unusually disciplined defensive code. Gaps: the header has no CRC, files that fail validation are *deleted*, boot formats the FS on mount failure, and there is no real rollback. |
| Hardware & Electrical Design | **7 / 10** | Correct, well-reasoned basics (bulk cap, RX supply filter). Single narrow-beam emitter, no hardware fail-safe for a stuck-HIGH TX pin, and a cheap demodulator. |
| Developer Experience & Tooling | **8 / 10** | Multi-target envs, pinning, HW test script, CHANGELOG, very good comments. No host-side unit tests (`test/` is absent), `FW_VERSION` is hard-coded, and the OLED deps float. |
| *Bonus: Security posture* | **4 / 10** | Auth is off by default, the rescue AP is open by default and stays up after recovery, and OTA is unsigned. |

**Maturity: upper-tier prosumer / "serious hobbyist".** The code quality is ahead of most commercial IR blasters I have reviewed. The comment density, and the fact that real failure post-mortems are written into the code, both say "maintained by an engineer." It is **not commercial-IoT grade** yet. That gap is mostly security defaults, OTA integrity/rollback, and data-vs-asset partition separation, not code quality.

---

## 2. Key Strengths & Architectural Highlights

1. **A single-loop concurrency model, chosen on purpose** ([src/main.cpp:L10-15](src/main.cpp#L10-L15)). There are no cross-core shared state, mutexes, or async callbacks. That removes the most common class of long-uptime ESP32 bugs (races in AsyncWebServer and MQTT callbacks). It is the right call for a device that has to work for years, and the cost is accepted knowingly.
2. **Per-file, self-describing storage with no central index.** Corruption stays inside one file, and boot rebuilds the index by walking headers. The payload CRC is checked *before transmitting* ([src/ir_store.cpp:L368-373](src/ir_store.cpp#L368-L373)). Refusing to blast corrupted timings at an AC is exactly the right instinct.
3. **Defensive arithmetic at interrupt-data boundaries.** In `consumeDecode()` ([src/ir_service.cpp:L130-208](src/ir_service.cpp#L130-L208)) an exact pre-count is paired with a "should be unreachable" guard. The code even notes *why* the library's `getCorrectedRawLength()` budget was wrong. This is reviewer-grade code.
4. **Tribal knowledge is written into the code.** The `resume()`-after-`disableIRIn()` null-deref trap is documented at every call site. Auth is checked in the `UPLOAD_FILE_START` callback rather than the completion handler ([src/web_ui_admin.cpp:L634-642](src/web_ui_admin.cpp#L634-L642)). That is a subtle point most projects get wrong.
5. **Staged loopback self-test.** It samples the idle line *before* transmitting, which separates "RX miswired" from "TX not emitting." A naive loopback test cannot tell those two apart. It also judges on payload rather than protocol label (NEC vs NEC_LIKE, RC5 vs RC5X).
6. **Honest verification semantics.** `EchoMatch::NotChecked` is used for AC-state commands instead of a fake "Match." Systems that never lie in their telemetry are rare.
7. **Data-loss-averse recovery paths.** `remountFsIfNeeded()` refuses to format after a failed FS OTA. Orphan-schedule pruning is skipped when storage didn't mount. `WiFi.persistent(false)` avoids SDK NVS churn.
8. **AC handling built on state structs** (IRac `stdAc::state_t` plus the Dawlance encoder). It regenerates waveforms and checksums instead of replaying 1.2 KB of timings. Users get a full temp/mode/fan matrix from a single "learn."
9. **millis()-rollover-safe arithmetic** throughout (`(int32_t)(millis() - deadline)`). It survives the 49.7-day wrap.

---

## 3. Weaknesses, Edge Cases & Hidden Bottlenecks

### 3.1 🔴 Critical: a Web-UI update wipes learned commands
UI assets, `/ir/*.irb`, schedules and presets all live in the **same** `spiffs` LittleFS partition ([partitions_ir_4mb.csv:L10](partitions_ir_4mb.csv#L10)). A filesystem OTA (`U_SPIFFS`, [src/web_ui_admin.cpp:L655](src/web_ui_admin.cpp#L655)) or `pio run -t uploadfs` overwrites the whole partition image, so **every learned command and schedule is erased** by what looks like a cosmetic UI update. Of everything in this review, this is the most likely path to user data loss.

### 3.2 Blocking transmit and the synchronous HTTP server
- A verified multi-frame AC send costs `N × frames × ~150 ms` + `frameGap`/`repeatGap` + the 400 ms echo wait + a 40 ms RX re-arm. During that time **the HTTP server, MQTT, DNS (portal), the scheduler and the buttons are frozen.** That is fine for one command. It is a problem for HA scenes that fire 4–6 commands in a row: each HTTP/MQTT call queues behind the previous burst.
- The core `WebServer` is synchronous. `HTTP_MAX_DATA_WAIT` / `HTTP_MAX_SEND_WAIT` default to about 5 s, so **one slow or malicious client (phone on weak WiFi, a Slowloris script) can stall the whole device**, including scheduled fires, for seconds per request.
- **Self-induced TX jitter:** in `sendStoredVerified()` the receiver ISR stays enabled during the send (`listenWhileSending_`). It lives on the same core as the bit-banged carrier, so every echoed edge preempts the transmitter that produced it. It usually works, but it degrades timing on long AC frames, which are exactly the ones with the least tolerance.

### 3.3 Storage failure modes
| Issue | Where | Consequence |
|---|---|---|
| **Header has no CRC** | [src/ir_store.cpp:L102-118](src/ir_store.cpp#L102-L118) | A bit-flip in `value`/`bits`/`protocol` passes validation. For **protocol-regenerated** sends, `loadRaw()` (and its CRC check) is never called, so the wrong code goes out silently. |
| **A file that fails validation is deleted** | [src/ir_store.cpp:L178-183](src/ir_store.cpp#L178-L183) | `version != IR_FILE_VERSION` counts as "corrupt". A future format bump, or an OTA *downgrade*, **deletes every command at boot**. A transient read error is also permanent. |
| **Boot formats the FS on mount failure** | [src/ir_store.cpp:L131-138](src/ir_store.cpp#L131-L138) | This contradicts the careful no-format policy in `remountFsIfNeeded()`. A brownout during boot or a flaky SPI flash read means a silent wipe. |
| `remove()` never decrements `bytesUsed_` | [src/ir_store.cpp:L320-331](src/ir_store.cpp#L320-L331) | Cosmetic: usage stats drift until reboot. |
| 128-command ceiling | `IR_MAX_COMMANDS` | Limited by RAM, not flash (about 12 KB of BSS). Power users with 4–5 devices reach it. |

**Flash wear is *not* a real risk here.** Writes happen only on learn, rename or schedule edit. LittleFS wear-levels, and the NVS `idseq` write per add is negligible. Even with daily edits the flash lasts for decades. Don't spend effort here.

### 3.4 Long-uptime memory behaviour
- Arduino 2.0.17 runs on IDF 4.4, which uses `multi_heap` rather than the TLSF allocator added in IDF 5. Hot-path `String` use (`lastSeen_`, `String err` on every send, JSON assembled by `String +=`, WebServer header parsing) **fragments the heap gradually over months.** Total free heap looks healthy while `getMaxAllocHeap()` shrinks, until a 1 KB MQTT discovery publish or a TLS-free HTTP response fails.
- You already expose `heapMin`/`heapMax`, which is the right telemetry. What's missing is an **action policy** for when they degrade.
- **FreeRTOS contention is a non-issue by design.** The loop runs on core 1 and the WiFi/LWIP tasks on core 0. The real trade-off is *latency*, covered in 3.2.

### 3.5 Network, RF & security
- 🔴 **The rescue portal is open by default** (`apPass` < 8 chars means an open AP, [src/net_manager.cpp:L73-75](src/net_manager.cpp#L73-L75)), **auth is off by default**, and the portal **stays up after the STA link recovers** ([src/net_manager.cpp:L144-145](src/net_manager.cpp#L144-L145)). Taken together: any router outage longer than 5 minutes leaves an open AP with unauthenticated admin, WiFi reconfiguration and **firmware OTA**, and it stays that way until someone notices. A neighbour, or anyone in the street, can flash arbitrary firmware.
- **AP_STA channel thrash.** The 30 s `WiFi.disconnect()`+`begin()` retry makes the radio scan across channels. The soft-AP has to follow the STA channel, so phones on the rescue portal get dropped every 30 s, and the portal is least usable at exactly the moment it is needed. Suspend STA retries while a portal client is associated (`WiFi.softAPgetStationNum() > 0`).
- **NTP cold-start after a power cut.** The ESP32 boots in about 2 s, but the router/ONT WAN often takes 1–3 minutes. Schedules can't fire until NTP succeeds. If the WAN is down (common in outage-prone regions), **timed AC schedules never fire.** Persisting the last-known epoch (plus elapsed `esp_timer`) or adding an optional DS3231 fixes this.
- **DST edges.** Wall-clock cron schedules at 02:00–03:00 fire twice or never on transition days. This needs a test case and a documented policy.
- **OTA integrity.** There is no image signing and no rollback. ArduinoOTA is passwordless unless auth is enabled.
- **Learned RC5/RC6 commands don't toggle.** Re-sending a stored RC5/RC6 frame repeats the same toggle bit, and some Philips TVs ignore the second press as a "held key."

### 3.6 Electrical vulnerabilities
| Risk | Detail | Mitigation |
|---|---|---|
| **Stuck-HIGH TX pin** | A panic or WDT hang *during a mark* (or a strapping-pin glitch at boot) leaves the LED at about 105 mA **DC** until reset. Many generic 5 mm 940 nm LEDs are rated 50–100 mA continuous, so over time this ages or kills them. | AC-couple the base drive (series cap with a base pull-down) so DC cannot hold the LED on. Add a 10 kΩ base-emitter pull-down for boot. |
| **Supply-dependent output** | LED current is `(Vbus − Vf − Vce)/33 Ω`. USB rails range from 4.6 to 5.25 V, so output varies by about ±10% from one charger to the next. | Use the constant-current emitter-degeneration trick (§4.3). It costs nothing. |
| **Single 5 mm LED, ±20° beam** | Range is fine, coverage is not. Placing the unit off-axis from the AC plus the TV commonly fails. | Multi-emitter array (§4.3). |
| **VS1838B clone demodulator** | Weak AGC. White LED drivers and CFL ballasts cause bursts of false edges, which pollutes learn mode and future "listen" features. | TSOP38238 / TSOP4438 (AGC2/AGC4 for noisy lighting). |
| **470 µF inrush** | Exceeds the 10 µF USB hot-plug guideline. It can brown out the ESP32 on weak hubs at plug-in. | 10 Ω NTC or a soft-start P-FET, or move bulk C behind a small series resistor (the 33 Ω path already limits the pulse). |
| **TX→RX optical saturation** | With the LED next to the RX on the PCB, verified sends see a saturated, distorted echo. | Optical baffle/tube on the RX, or point the emitter away. Verify with a scope. |

---

## 4. Improvement Proposals & Roadmap

### 4.1 Firmware & Protocols (priority order)
1. **Partition split: `ui` vs `irdata`** (fixes 3.1). Shrink `spiffs` to about 320 KB for assets and add a 576 KB `irdata` LittleFS mounted with `LittleFS.begin(false, "/data", 5, "irdata")` (supported by Arduino 2.0.17). FS-OTA then only touches the UI. *This requires a one-time serial reflash: ship a migration that exports to JSON first.*
2. **Storage hardening:** add `headerCrc`; rename failures to `.bad` instead of deleting them (quarantine); accept older `IR_FILE_VERSION` values with a migrator; never auto-format at boot unless the partition is genuinely blank (check for the erased `0xFF` superblock).
3. **Real OTA rollback.** Build a custom bootloader with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (or move to arduino-esp32 3.x / IDF 5 as an ESP-IDF component). Call `esp_ota_mark_app_valid_cancel_rollback()` only after WiFi + FS + IR self-check succeed within 60 s.
4. **RMT transmit backend for raw replays.** Convert `uint16_t` mark/space arrays to `rmt_item32_t` with hardware carrier (`rmt_config.tx_config.carrier_en`). That gives zero jitter, no CPU pinned during the burst, and non-blocking sends. Keep IRremoteESP8266 for *encoding*.
5. **Measured carrier frequency.** Either add a carrier-output receiver (TSMP58000) on a second pin, or estimate the carrier from a raw photodiode via PCNT. Store the measured kHz per command instead of `defaultFreqKhz`.
6. **Raw timing clustering / quantization.** Bucket durations within ±12% into about 4–8 symbols and replace each bucket with its median. Then **retry protocol decode on the cleaned signal**: many "UNKNOWN" captures are just noisy NEC/Samsung. Bonus: a symbol table plus indices compresses files 3–5×.
7. **TX job queue with async API.** Keep the single loop, but make transmit a *state machine* (one frame per `loop()` iteration, RMT completion-driven). HTTP/MQTT return `202 + jobId`, and scenes queue rather than block.
8. **RC5/RC6 toggle-bit management**: flip on each distinct press, hold on repeats.
9. **Interop formats:** Pronto Hex, Flipper `.ir`, and LIRC import/export. You already source from Flipper-IRDB, so close the loop.
10. **Heap-health policy:** if `maxAlloc < 16 KB` for more than 10 minutes, schedule a reboot at the next quiet window (03:00, with no schedule within ±5 min) and log the reason.
11. **ESP-NOW satellites (later).** Turn cheap C3 SuperMinis into per-room emitters, with the hub keeping the store and scheduler. This addresses whole-house line-of-sight.

### 4.2 Smart Home & Connectivity
1. **HA `climate` entity via MQTT discovery** for AC-library devices. You already hold the full `stdAc::state_t`, so publish an *assumed state* climate entity (mode/temp/fan/swing). In HA this is far more useful than N buttons, and it's mostly mapping code.
2. **HA diagnostics and an `update` entity:** RSSI, heap min/max, uptime, last reset reason, and firmware version, with OTA triggered from HA.
3. **`irblaster/received` publish** (already in your proposal backlog). Use device triggers so physical remotes can drive HA automations.
4. **Improv-WiFi over serial + ESP Web Tools.** With a browser-based flasher plus provisioning, a non-technical user can go from box to configured in under 2 minutes. Serial Improv costs a few KB. Improv over BLE needs NimBLE (about 100 KB flash, 30–40 KB RAM), so it is only viable after the partition rework, or on S3.
5. **Live push for learn/monitor.** The core `WebServer` can't hold connections. Add `links2004/WebSockets` (polled from `loop()`, compatible with your concurrency model) on port 81 for learn countdown, captured waveform, and logs. Avoid AsyncWebServer: it breaks your single-context guarantee.
6. **Matter / Thread: assess honestly.** It is **not feasible** on 4 MB WROOM with Arduino 2.0.17. The Matter SDK needs IDF 5.x, about 1.5–2 MB of app and substantial RAM. Thread needs an 802.15.4 radio, which the classic ESP32/S3 don't have. Plan it as a separate **ESP32-C6 (Thread+WiFi) or S3-8MB SKU** on arduino-esp32 3.x / ESP-Matter, exposing AC as a Matter Thermostat and TV as an On/Off + Media device. Until then, HA MQTT covers 95% of users.
7. **Remote syslog / MQTT log streaming** of the `log_ring`, for unattended fleet debugging.

### 4.3 Hardware & Electrical Upgrades
1. **Zero-cost constant-current driver:** keep the NPN, drive the base directly from the GPIO (with a small base resistor), and put the current-set resistor in the **emitter**. `I ≈ (3.3 − 0.7)/Re`, so Re ≈ 22–26 Ω gives about 100–120 mA, independent of the 5 V rail and LED Vf spread.
2. **Logic-level MOSFET plus multi-emitter array:** AO3400 / SI2302 (Rds(on) < 50 mΩ at Vgs = 2.5 V) driving **3–4 LEDs, each with its own resistor**, at 150–300 mA pulsed each. Mix beam angles: one narrow (TSAL6100, ±10°) for range and 2–3 wide (TSAL6400 ±25°, or SMD VSMB2948 ±60°) arranged radially for about 270° coverage. Add 100 kΩ gate pull-down and 100 Ω gate series resistors.
3. **Hardware stuck-ON protection:** AC-coupled drive or a monostable/RC timeout so the LED cannot conduct DC for more than about 10 ms. This makes the emitter immune to firmware hangs.
4. **Receiver upgrade:** TSOP38238/TSOP4438, plus an **optional second wideband sensor** (TSMP58000, 20–60 kHz carrier-out) for frequency measurement and 56 kHz learning. A plain 38 kHz demodulator learns 56 kHz remotes poorly.
5. **Soft-start for the bulk capacitor** (NTC or P-FET) to stay USB-polite. Use a 105 °C, low-ESR part for life in warm enclosures.
6. **Optional DS3231 RTC** (I²C, shares the OLED bus) so schedules survive outages with no WAN.
7. **Reference PCB + enclosure** (KiCad, JLC-assemblable) with an IR-transparent window and an RX light baffle. This moves the project from "build guide" to "product."

### 4.4 Web UI & User Experience
1. **Waveform visualizer:** a canvas render of mark/space from a `/api/commands/:id/raw` endpoint, with frame boundaries, zoom, and an **overlay diff of two captures**. That makes "verify capture" visual and debuggable.
2. **Capture-quality score** (your Proposal #5): noise pulses, buffer headroom, decode confidence, shown as a traffic light.
3. **Remote builder:** a drag-and-drop grid of stored commands into per-device "remotes" (layout JSON stored in `/data/layouts`), with an icon set and a long-press repeat.
4. **Scenes/macros in the UI:** ordered command lists with per-step delays, executed through the TX job queue.
5. **PWA manifest + offline shell:** installable on phones, home-screen tiles per remote.
6. **Mandatory first-run wizard:** set the admin password and AP password, run the self-test, and pick a TV/AC library brand. This doubles as the security fix.
7. **Backup reminders:** "You have 34 commands and no export in 90 days," plus one-click export before any FS OTA.

---

## 5. Top 3 Immediate Quick Wins

### 🥇 1. Secure the defaults (≈ 1 day)
- Derive a per-device AP password (for example the last 8 chars of `SHA256(efuse MAC + salt)`) and show it on the OLED, the serial log, and a printed label. **Never run an open AP.**
- **Drop the rescue portal automatically** when STA reconnects and no AP client is associated for 2 minutes. Pause STA retries while a portal client is connected.
- Refuse `/api/ota` and ArduinoOTA until an admin password is set (force it in a first-run wizard).

*This closes the only remote-takeover path a stranger can reach physically.*

### 🥈 2. Stop the data-loss paths (≈ 1–2 days)
- Add a `headerCrc`; **quarantine** (rename to `.bad`) instead of deleting; accept older file versions; **no auto-format at boot** unless the partition is blank.
- Until the partition split ships, make FS-OTA **refuse to proceed** unless the client sent an `X-Backup-Done` header (the UI auto-downloads `/api/export` first). Do the same in the PIO workflow docs.

*This turns the two silent wipe scenarios into recoverable ones.*

### 🥉 3. Post-mortem and drift visibility (≈ half a day)
- Add a **`/api/coredump`** download endpoint (`esp_core_dump_image_get()` plus a partition read) and a "Download crash dump" button whenever the reset reason was panic/WDT. You reserved 64 KB for coredumps but users can't retrieve them without esptool.
- Inject `FW_VERSION` from `git describe` via a PIO `extra_script`, pin the OLED libs exactly, and add a CI check that fails if the app exceeds 90% of the 1.5 MB slot. You're at about 73%, and the AC library plus future features will eat that quickly.

*Every future field bug becomes diagnosable instead of anecdotal.*

---

### Honourable mentions (next tier)
- HA `climate` entity from AC state: the largest user-visible upgrade per line of code.
- RMT TX backend: fixes jitter and blocking in one change.
- Host-native unit tests (`pio test -e native`) for `consumeDecode` bounds, the Dawlance checksum, header parsing, and cron/DST logic.
