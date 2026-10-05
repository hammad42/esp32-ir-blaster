# Feature Proposals — Architectural Review & Implementation Roadmap

> **Target Document:** [FEATURE-PROPOSALS.md](FEATURE-PROPOSALS.md)  
> **Review Date:** September 2026  
> **Status:** Evaluated & prioritized for future implementation

---

## Executive Summary

[FEATURE-PROPOSALS.md](FEATURE-PROPOSALS.md) proposes five functional extensions for the universal IR blaster firmware:
1. **Listening to Remotes** (IR triggers & MQTT publish)
2. **Countdown Timer** (relative one-shot scheduling)
3. **Backing Up the Schedules** (export/import v2 with name resolution)
4. **SD Card** (external storage evaluation)
5. **Capture-Quality Feedback** (heuristics & verification in learn flow)

The proposals are exceptionally grounded, realistic, and sensitive to the embedded constraints of the ESP32 (RAM budget, cooperative single-loop execution, watchdog timeouts, LittleFS reliability, and past crash traps like `IRrecv::resume()` on a stopped receiver).

This document captures the critical findings, overlooked hardware and software edge cases, recommended design adjustments, and an optimized implementation order for when work begins.

---

## Section-by-Section Technical Review

### 1. Listening to Remotes

*Proposal:* Turn the IR receiver into an input device that either triggers local actions via a `/triggers.json` table or publishes incoming codes to MQTT (`irblaster/received`).

#### Strengths
- Reuses proven matching logic from `sendStoredVerified()` in [`ir_service.cpp`](src/ir_service.cpp).
- Recommends starting with the MQTT publish topic before committing to an on-device rules engine.
- Acknowledges the repeat frame issue (`cooldownMs`) and respects the `txBusy_` guard.

#### Critical Risks & Edge Cases
1. **Interrupt & CPU Load of 24/7 Reception**:
   - `IRremoteESP8266` uses a hardware timer ISR ticking every **50 µs** (20,000 interrupts/sec) while the receiver is enabled.
   - Leaving this on permanently incurs a continuous CPU tax (~5–10% of Core 0/1) and introduces interrupt latency that competes with the WiFi/TCP network stack.
   - *Mitigation:* Keep `alwaysListen` strictly as an explicit opt-in setting (default `false`).
2. **Ambient Optical Noise Churn**:
   - Ambient room conditions (sunlight, fluorescent ballast flicker, plasma screens) produce continuous spurious pulses on VS1838/TSOP demodulators.
   - Although [`IR_MIN_RAWLEN`](include/config.h#L40) (8 transitions) filters noise out, `consumeDecode()` will be executed frequently inside `IrService::loop()`. Ensure that noise drops occur early without allocating memory or doing string operations.
3. **Transmit/Receive Collision during Loopback**:
   - Transmitting via `sendStoredVerified()` arms the receiver to capture its own echo. If `alwaysListen` is active, the system must never mistake an echo of its own blast for an incoming remote trigger.
   - Post-transmission tail reflections or multi-part frame gaps must be isolated so triggers do not self-fire.

#### Architectural Recommendation
- **Phase 1: MQTT Publish Only (`irblaster/received`)**: Implement the ~30 lines needed to publish decoded frames to MQTT. Home Assistant has a robust automation engine, making an on-device `/triggers.json` table largely redundant for smart-home setups.
- **Phase 2: Local Trigger Table**: Implement only if autonomous, broker-free operation is an absolute requirement.

---

### 2. Countdown Timer

*Proposal:* "Turn AC off in 45 minutes" relative one-shot action, storing absolute epoch `fireAt` in `/timers.json`.

#### Strengths
- Storing absolute epoch seconds (`fireAt`) prevents time drift and handles transient reboot recovery cleanly.
- Reuses `ScheduleManager` 1 Hz loop, `sendStoredVerified()`, and the `FireLogEntry` ring log.
- Defines clear policies for missed deadlines (`TIMER_GRACE_S = 600s`) and uncalibrated clocks (`netManager.timeValid()`).

#### Critical Risks & Edge Cases
1. **Flash Wear & Fragmentation from Ephemeral Timers**:
   - Unlike recurring schedules (which are configured once and left for months), countdown timers are created, changed, and deleted frequently.
   - Writing to LittleFS (`/timers.json`) for every timer set/cancel cycle accelerates SPI flash wear and creates filesystem churn.
   - *Recommended Refinement:* **Keep timers in RAM only.** Consumer devices (TVs, AC units, microwaves) universally treat sleep timers as ephemeral state that cancels on power loss. Making timers RAM-resident eliminates LittleFS flash wear, avoids LittleFS corruption risk, and simplifies cancellation.
2. **NTP Step Adjustments**:
   - If an SNTP sync steps the system clock backwards or forwards (e.g. initial sync or network drift correction), an absolute `fireAt` could either expire instantly or trigger the "missed/expired" grace window.
   - *Mitigation:* Document that active timers require clock stability, and clamp timers against drastic clock jumps.

---

### 3. Backing Up the Schedules (& Settings)

*Proposal:* Extend `/api/export` to version 2 to include schedules, resolving commands by name on restore to prevent ID desynchronization.

#### Strengths
- **Critical Catch:** Accurately identifies that `irStore.add()` re-generates IDs via NVS (`nextId()`), which would cause naive schedule restores to point to non-existent IDs and be wiped by `pruneMissingCommands()` on the next boot.
- Chunked JSON streaming maintains the project's zero-RAM-spike export discipline.
- Backward compatibility: existing v1 backups remain valid.

#### Critical Risks & Edge Cases
1. **Command Name Ambiguity across Groups**:
   - `irStore.findByName()` looks for the first matching command name. If multiple commands have the same name in different groups (e.g. `Power` in `TV` vs. `Power` in `AC`), name-only resolution can link the wrong command.
   - *Mitigation:* Export both `group` and `command` in the schedule backup:
     ```json
     {"command":"Power", "group":"AC", "hour":23, "minute":0, ...}
     ```
     Resolve using both `name` and `group`.
2. **Settings Restore Security & Defaults**:
   - Redacting WiFi/MQTT passwords in exported backups is the right security default.
   - *Crucial Restore Rule:* When importing a settings backup with omitted passwords, the restore endpoint must **preserve existing NVS credentials**, rather than overwriting them with blank strings.
3. **Tool Parity**:
   - Ensure `tools/ir-backup.sh` is updated at the same time as `web_ui_admin.cpp` and `app.js`.

---

### 4. SD Card

*Proposal:* Evaluate external SPI SD card storage for backups, extended logging, and bulk import.

#### Strengths
- **Debunks the Flash Myth:** Explicitly demonstrates that LittleFS (896 KB) can hold 700+ commands; the 128-command limit is strictly due to the 12 KB in-RAM index (`IrCommandMeta index_[IR_MAX_COMMANDS]` in [`ir_store.h`](include/ir_store.h)).
- Accurate pin viability evaluation across Classic ESP32, S3, and C3.
- Sound architectural principle: LittleFS remains authoritative; SD card is only a passive mirror.

#### Critical Risks & Verdict
1. **SPI Latency & Watchdog Stalls**:
   - MicroSD cards have internal wear-leveling controllers that occasionally block writes for 200–800 ms.
   - In a non-preemptive single-loop architecture with a 20s watchdog, blocking SPI I/O poses a risk to IR timing and network servicing.
2. **RAM Overhead of FAT Filesystem**:
   - Arduino `SD.h` / `FatFS` requires 512-byte sector buffers and task stacks (often 4–10 KB heap). On a device where free heap is tightly monitored, this is substantial.
3. **Verdict: Do Not Implement (or Keep Deprioritized Indefinitely)**:
   - Automated backups are better solved by a simple external `curl` cron job hitting the `/api/export` endpoint.
   - Long-term logging is better handled over the network via Syslog or MQTT event streaming.

---

### 5. Capture-Quality Feedback

*Proposal:* Add quality heuristics (noise pulse counters, buffer ceiling warnings, length-based UNKNOWN validation, pre-flight line test, and a "Verify" second-capture diff) to the Learn tab.

#### Strengths
- **Highest ROI / Lowest Risk:** Purely additive to the Learn state machine; touches no stored data and introduces no regression risk to core blasting.
- Surfacing silent noise (`rawlen <= IR_MIN_RAWLEN`) as "short pulses detected" directly resolves the frustrating "why is it stuck on Listening..." experience.
- Using payload length rather than `UNKNOWN` label prevents false alarms on valid AC captures.

#### Critical Risks & Edge Cases
1. **Toggle Bits on Repeated Presses (RC-5, RC-6, Philips)**:
   - For protocols with toggle bits, press #1 and press #2 intentionally flip a bit in the frame.
   - A bitwise diff or raw timing comparison will flag a false mismatch unless the verification logic specifically recognizes or masks toggle bits for these protocols.
2. **AC Remote State Drift**:
   - If a user presses a button twice on an AC remote during verification, the state payload changes (e.g. temperature increases from 24°C to 25°C), altering checksums and payload bytes.
   - *Mitigation:* The UI must instruct: *"For AC remotes, confirm the remote display is set to identical settings before pressing Verify."*
3. **RAM Allocation for Verify Buffer**:
   - Comparing two raw captures requires holding the first capture (~1024 `uint16_t` = 2 KB) while capturing the second.
   - *Rule:* Allocate this buffer statically or inside a dedicated verification session—do not place a 2 KB array on the stack.

---

## Cross-Cutting Constraints Matrix

| Constraint | Impact on Proposals | Mitigation / Rule |
| :--- | :--- | :--- |
| **RAM (Heap & BSS)** | Timers, Triggers, Verify buffer, SD FAT stack | Keep timers/triggers small (8 slots max). Allocate verify buffers statically or session-scoped. Avoid SD card. |
| **Single Cooperative Loop** | All operations must yield within milliseconds | No blocking I/O (SD card writes, synchronous DNS). Break multi-part operations into state machine steps. |
| **Flash Endurance** | Frequent writes on LittleFS wear flash blocks | **Timers in RAM.** Settings and schedules written strictly on change, never periodically. |
| **20s Task Watchdog (WDT)** | Long loops or blocked network/disk calls trigger reboot | Ensure chunked responses yield and call `esp_task_wdt_reset()` during streaming. |
| **`IRrecv::resume()` Trap** | Panics if called on a stopped receiver | Every receiver state change must route exclusively through `IrService::setReceiverEnabled()`. |

---

## Recommended Implementation Roadmap

Prioritize based on **risk mitigation**, **user value**, and **architectural safety**:

```
[Stage 1: Technical Debt & Safety]
  └─► Feature 3: Schedule Backup (v2 export/import with Name + Group resolution)

[Stage 2: UX & Learning Accuracy]
  └─► Feature 5: Capture-Quality Feedback (Noise counter, buffer cliff, Verify diff)

[Stage 3: Core Functional Extensions]
  └─► Feature 2: Countdown Timer (RAM-only implementation, integrated with ScheduleManager loop)

[Stage 4: Smart Home Integration]
  └─► Feature 1: Remote Listening (MQTT publish irblaster/received ONLY; alwaysListen opt-in)

[Deferred / Low Priority]
  └─► Feature 4: SD Card (Superseded by network backups and Syslog/MQTT logging)
```

---

## Pre-Implementation Checklist

Before writing code for any of these features, verify:

- [ ] **Feature 3:** Test `POST /api/export` with multi-group commands to confirm name uniqueness requirements.
- [ ] **Feature 5:** Verify that `results_.rawbuf` remains intact between consecutive Learn captures or allocate a dedicated `verifyRaw[IR_MAX_RAW]` buffer.
- [ ] **Feature 2:** Decide whether timers should be RAM-only (strongly recommended) or LittleFS-backed.
- [ ] **Feature 1:** Measure CPU utilization and WiFi ping jitter with `irService.setReceiverEnabled(true)` running continuously for 1 hour.
- [ ] **General:** Confirm free heap baseline via `/api/status` before adding new static arrays.
