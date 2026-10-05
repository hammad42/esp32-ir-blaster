# Feature Proposals

Five additions to the IR blaster, described in enough detail to be built or
rejected on the evidence rather than on enthusiasm. Nothing here is implemented.
Every claim about current behaviour cites the file it came from.

Two structural facts shape all five:

**The device is write-only.** The receiver decodes perfectly well, but every
decode ends in a string — `lastSeen_` in [ir_service.cpp](src/ir_service.cpp) —
that the web UI polls every 400 ms and nothing else ever sees. Nothing is
published, stored, or acted on. The hardware to react to a remote has been
sitting there the whole time.

**Every trigger path ends at one `sendStored()`.** Button, REST, MQTT, schedule
— they all resolve to a single command. There is no sequencing primitive, no
deferred action, no conditional. That is a deliberate and largely good
simplification, and it is also why "turn the AC off in 45 minutes" cannot be
expressed today.

| # | Feature | Effort | Risk | Depends on |
|---|---------|--------|------|-----------|
| 1 | [Listening to remotes](#1-listening-to-remotes) | Medium | Medium | — |
| 2 | [Countdown timer](#2-countdown-timer) | Small | Low | — |
| 3 | [Backing up the schedules](#3-backing-up-the-schedules) | Small | Low | — |
| 4 | [SD card](#4-sd-card) | Large | Medium | 3 |
| 5 | [Capture-quality feedback](#5-capture-quality-feedback) | Small | Low | — |

Features 1 and 5 both modify the receiver state machine in `ir_service.cpp`.
Build them in the same sitting or one will fight the other.

---

## 1. Listening to remotes

Turn the receiver into an input, not just a learning aid. Press the button on
your existing TV remote; the blaster hears it and does something — sends another
command, publishes an MQTT message, dims a lamp.

### What exists today

The receiver already decodes reliably. In `IrService::loop()`:

```cpp
if (!rxEnabled_ || txBusy_) return;
...
lastSeen_ = typeToString(...) + " / " + String(results_.bits) + " bits / "
          + String(getCorrectedRawLength(&results_)) + " raw";
```

That string is returned by `/api/learn/state` as `lastSeen` and rendered into
`#monitor-out` by [app.js:353](data/app.js:353). That is its entire life. The
MQTT client subscribes to exactly three topics — `send`, `raw`, `learn`
(`subscribeAll()` in [mqtt_manager.cpp](src/mqtt_manager.cpp)) — and publishes
nothing about what it hears.

Critically, the matching logic already exists and is proven. `sendStoredVerified()`
compares an echo against an expectation:

- A/C state commands → `NotChecked`, because the reply cannot be re-derived
- `protocol > 0 && bits > 0` → exact value-and-bits comparison
- otherwise → raw-length comparison with `const int slack = 6`, accepting either
  a whole burst or a single frame of it

A trigger table needs precisely that comparison, pointed at a stored pattern
instead of at a just-transmitted one. This is not new logic; it is existing
logic given a second caller.

### What would be added

A `/triggers.json` file, structured like `schedules.json`, holding up to
`MAX_TRIGGERS` (8 is plenty) entries:

```json
{ "id": 1, "enabled": true, "label": "TV remote red button",
  "protocol": 3, "bits": 32, "value": "0x20DF10EF", "rawLen": 68,
  "action": { "type": "send", "commandId": "0000001a" },
  "cooldownMs": 1500 }
```

`action.type` starts as `send` and `publish`. Keep the door open for `timer`
(feature 2) without building it now.

Learning a trigger reuses the learn flow verbatim — capture, then "use as
trigger" instead of "save as command". No new capture path.

Matching runs in the monitor branch of `IrService::loop()`, using the same
three-tier comparison as `sendStoredVerified()`.

MQTT gains one publish topic, `irblaster/received`:

```json
{"protocol":"NEC","bits":32,"value":"0x20DF10EF","rawLen":68,"trigger":"TV remote red button"}
```

That topic alone — with no trigger table at all — is worth building. It turns
the blaster into an IR sensor for Home Assistant, and every automation you would
otherwise express in `triggers.json` can then be written in HA where it is
easier to edit.

### What it costs

**The receiver has to stay on.** Today the 50 µs timer ISR runs only during a
learn or with the monitor toggle checked; `setReceiverEnabled()` early-returns
when the state is unchanged, and this on-demand policy is deliberate. Triggers
require reversing it: the ISR runs forever. That is a permanent low-level CPU
tax and a small current draw, and it is the single biggest thing to weigh here.
Make it a setting — `alwaysListen` — defaulting off.

**Repeat frames.** Hold a remote button down and NEC sends a repeat frame every
~110 ms. Without a cooldown, one long press fires a trigger fifteen times. Hence
`cooldownMs` per trigger; 1500 ms is a sane default.

**Self-triggering.** The existing `if (!rxEnabled_ || txBusy_) return;` guard
already prevents the blaster hearing its own transmission during a send. Any new
code path must not bypass it. Note also HANDOFF §8's Finding #6 — a reported but
never-reproduced receiver lockout after transmitting with the monitor active.
Leaving the receiver on permanently is exactly the condition that report
describes, so budget time to chase it if it resurfaces.

**The panic.** From the comment in `ir_service.cpp`:

> `resume()` must never be called on a stopped receiver ... a null dereference
> that panics and reboots the device. Which is exactly what happened the first
> time a capture actually succeeded.

Every new path that touches the receiver goes through `setReceiverEnabled()`.
No exceptions.

### Open question

Whether trigger matching belongs on the device at all. The MQTT publish is ~30
lines and delegates all the logic to Home Assistant, which has a real rules
engine. The on-device table is the answer only if the device must work when the
broker is down. That is a legitimate requirement — decide it deliberately rather
than building both.

---

## 2. Countdown timer

"Turn the AC off in 45 minutes." A one-shot, relative-time action.

### What exists today

Nothing relative. `ScheduleManager` is strictly wall-clock: an hour, a minute,
and a day-of-week bitmask, matched in `loop()`:

```cpp
if (s.hour != t.tm_hour || s.minute != t.tm_min) continue;
if (!(s.dow & (1 << t.tm_wday))) continue;
if (s.lastFiredMin == nowMin) continue;
```

The surrounding machinery, though, is exactly right and should be reused:
the 1 Hz check (`if (now - lastCheck_ < 1000) return;`), the refusal to fire on
a bad clock (`if (!netManager.timeValid()) return;`), the minutes-since-epoch
edge trigger, and `recordFire()` writing a `FireLogEntry` with echo evidence.

### What would be added

A small parallel structure — `MAX_TIMERS 8` — in its own `/timers.json`:

```c
struct Timer {
  uint8_t  id;
  uint32_t fireAt;                 // absolute epoch seconds
  uint8_t  repeats;
  char     commandId[IR_ID_LEN];
  char     label[IR_NAME_MAX];
};
```

**Store the deadline, not the remaining time.** An absolute epoch second
survives a reboot; a counting-down millisecond value does not. This is the whole
design decision and everything else follows from it.

Checked in the same `loop()` pass as schedules, fired through the same
`sendStoredVerified()`, logged into the same firelog. A fired timer deletes
itself.

The API is one endpoint:

```
POST /api/timers  {"commandId":"0000001a","seconds":2700,"label":"AC off"}
GET  /api/timers  → [{"id":1,"label":"AC off","remaining":2543,...}]
DELETE /api/timers/1
```

MQTT gets `irblaster/timer` accepting `{"cmd":"AC_Power","seconds":2700}`, which
makes the feature usable from Home Assistant without any custom integration.

The UI is a command picker, a number, a unit dropdown, and a live "42 min
remaining" readout ticking client-side off the `remaining` field.

### The two cases that need a decision

**A missed deadline.** The device was unplugged at 21:00 with a timer due at
21:15, and comes back at 03:00. Firing "AC off" six hours late is worse than not
firing. Recommendation: a grace window — fire if the deadline passed less than
`TIMER_GRACE_S` (600) ago, otherwise drop it and write a firelog entry saying
`missed, expired`. The user sees what happened either way.

**No clock.** Schedules already refuse to fire when `timeValid()` is false, and
timers must follow the same rule. A timer set before NTP has landed cannot
compute a deadline at all — reject it at the API with a clear error rather than
storing a garbage `fireAt`.

### What it costs

Very little. ~200 lines, one new file, 8 × ~56 bytes of RAM. It reuses the
firing path, the echo verification, and the firelog. Of the five proposals this
has the best ratio of usefulness to risk, and it is the natural one to build
first.

---

## 3. Backing up the schedules

### What exists today

`/api/export` streams a backup document (`apiExport()`,
[web_ui_admin.cpp:198](src/web_ui_admin.cpp:198)):

```json
{"format":"ir-blaster-backup","version":1,"fw":"1.0.0","commands":[ ... ]}
```

It is well built — chunked so a 100-command backup never exists in RAM at once,
feeding the watchdog mid-stream, carrying `frameLens`, `forceRaw` and `acState`
so a restore is faithful. `/api/import` takes one command per request, keeping
peak RAM at a single command.

It covers commands and **nothing else**. `tools/ir-backup.sh` contains no
mention of schedules. `HANDOFF.md` §8 lists "Schedules are not backed up" as a
known gap. So a factory reset, a failed filesystem OTA, or a swapped board loses
every schedule with no way back.

### The thing that will break if you rush it

Schedules reference commands by `commandId`. On import, `irStore.add()`
**generates a fresh id** — `nextId()` counts up from an NVS-backed sequence and
has no idea what the id was on the old device. So a naively restored schedule
points at an id that does not exist, and then:

```cpp
if (storageOk) scheduleManager.pruneMissingCommands();
```

runs at the next boot and deletes every one of them. A backup that restores
cleanly and then silently erases itself overnight is worse than no backup.

**Therefore: export schedules by command _name_, and re-resolve to an id on
import.** `irStore.findByName()` already exists and is already case-insensitive
— it is what MQTT uses. This also means commands must be imported before
schedules, which the browser-side walk must enforce.

### What would be added

Extend the same document rather than inventing a second one:

```json
{ "format":"ir-blaster-backup", "version":2, "fw":"1.0.0",
  "commands":[ ... ],
  "schedules":[
    {"enabled":true,"hour":23,"minute":0,"dow":62,
     "repeats":0,"command":"AC_Power","label":"Bedtime"}
  ] }
```

Bump to version 2; accept version 1 (a missing `schedules` key is simply an
empty array). Old backups keep working.

`POST /api/import/schedule` takes one entry, resolves `command` → id, and calls
`upsert()`. If the name does not resolve, skip that schedule and return the
reason so the browser can report "3 of 4 schedules restored; 'AC_Power' not
found" instead of failing silently.

Deliberately **not** exported: `lastFiredMin` (meaningless on a restore) and the
firelog (history, not configuration).

### Settings

Settings are equally unbacked — a CRC-protected blob in NVS, lost on factory
reset. Worth adding to the same document, with one caveat: settings include the
WiFi password and MQTT credentials. Exporting them writes plaintext secrets into
a file in a Downloads folder.

Recommendation: export settings with secrets **omitted** by default, and a
separate explicit "include passwords" checkbox that says plainly what it does.
Restore then re-prompts for the passwords, which is a small annoyance in
exchange for not leaving credentials in a file the user forgets about.

### What it costs

Small — perhaps 150 lines split between firmware and `app.js`, plus a matching
update to `tools/ir-backup.sh`. The only real work is the name-resolution
discipline, and that is a design decision rather than a difficult implementation.

---

## 4. SD card

Worth being blunt about this one, because the usual reason for adding an SD card
does not apply here.

### Flash is not the constraint

The 4 MB partition table gives LittleFS 896 KB (`partitions_ir_4mb.csv`). The
web UI is roughly 48 KB of that. A long A/C command is about 1.2 KB on disk.
So the free space holds on the order of **700 commands**, and the 8 MB layout has
1.9 MB — more than twice that.

The 128-command ceiling is not a storage limit. It is `IR_MAX_COMMANDS`, a
constant in `config.h`, and what actually makes it expensive to raise is this
line in `ir_store.h`:

```cpp
IrCommandMeta index_[IR_MAX_COMMANDS];
```

A fixed in-RAM array. `IrCommandMeta` measures 96 bytes after padding (67 bytes
of strings, then an 8-byte-aligned `value`), so the index costs **12 KB of RAM,
always, whether you have stored 3 commands or 128**. Raising the ceiling to 700
would cost ~67 KB of permanently resident RAM.

**An SD card does not help with that at all.** The index is in RAM because
lookups by name happen on every MQTT message and every schedule fire; moving the
files to SD leaves the index exactly where it is. If more commands are genuinely
needed, the fix is a dynamically-sized or paged index, not different media.

So: do not add an SD card for capacity. There is no capacity problem.

### What an SD card is actually good for

Three real uses, in descending order of value:

**1. Automatic on-device backups.** This is the strongest case, and it is where
this feature meets feature 3. A nightly write of the full export document to
`/sd/backups/ir-YYYYMMDD.json` means the device backs itself up without anyone
remembering to click Export. It also survives the scenario that currently loses
everything: a filesystem OTA overwrites the whole LittleFS partition, and a card
in a slot is untouched by it.

**2. A real history.** The firelog is a 16-entry ring (`SCHED_LOG_MAX`) and the
serial log is 40 lines (`LOG_RING_LINES`), both sized for RAM. An SD card makes
months of "did the 23:00 schedule actually fire?" history practical, which is
the question the echo verification was built to answer and currently can only
answer about the last sixteen fires.

**3. Bulk import.** Dropping a folder of `.irb` files or a Flipper `.ir` file
onto a card and importing from the UI, instead of pushing them one HTTP request
at a time.

### Pin feasibility per target

An SPI card needs four pins (CS, SCK, MOSI, MISO).

| Target | Verdict |
|--------|---------|
| **Classic ESP32** | Straightforward. The entire VSPI set — 18, 19, 23, 5 — is free. IR is on 14/4, LEDs on 25/26/27, I²C on 21/22, macros on 32/33. Nothing collides. |
| **ESP32-S3** | Fine. IR 4/5, LEDs 6/7/15, I²C 8/9, macros 16/17; avoid 19/20 (native USB) and 26–32 (flash/PSRAM). Enough remains. |
| **ESP32-C3** | Genuinely tight. Usable pins are 0–10 and 18–21, with 20/21 being UART0. After IR (3/4), LEDs (5/6/7), BOOT (9) and I²C (8/10), only **0, 18, 19** remain — one short of four. |

The C3 is not blocked, but it requires a choice: give up a status LED, or accept
that the macro buttons (1/2) and the OLED (8/10) are both compile-time-optional
and reclaim those pins when they are disabled. Whichever way, it is a
configuration the user has to make consciously, so the docs must say so.

### The architectural rule

**LittleFS stays authoritative; SD is a mirror.** SD cards fail, and they fail
in the worst way — silently, mid-write, often after a year of working fine. A
card that has been removed, corrupted, or has simply died must not be able to
take the command store down with it.

That means: the device boots and runs identically with no card present; every SD
operation is best-effort with a logged failure; and no code path treats an SD
read as a source of truth. `IrStore` touches `LittleFS` in fifteen places in
`ir_store.cpp` and three in the header — making that a swappable backend is a
real refactor, and the mirror approach avoids needing it.

### What it costs

The largest of the five by a distance. New hardware and wiring, the SD library
and a FAT stack (a few KB of RAM per open file), card-presence detection,
hot-removal handling, a new failure mode in the boot path, and per-target pin
documentation. Against that, its single most valuable use — automatic backups —
is largely delivered by feature 3 plus a scheduled `curl` on any machine that is
already on the network.

Recommendation: build feature 3 first, live with it, and add the card only if
the "nobody ever remembers to press Export" problem is real in practice.

---

## 5. Capture-quality feedback

Right now the Learn tab reports *what* it captured. It never says whether that
capture is any good.

### What exists today

`consumeDecode()` already computes everything needed:

```cpp
if (results_.overflow) {
  learnError_ = "signal longer than the capture buffer -- raise IR_MAX_RAW";
  learnState_ = LearnState::Error; return true;
}
if (results_.rawlen <= IR_MIN_RAWLEN) return false;   // noise, keep listening
```

and `/api/learn/state` returns `protocol`, `bits`, `raw`, `frames` and `value`,
which [app.js:332](data/app.js:332) renders as:

> **Captured** — NEC · 32 bits · 68 timings · 1 part

A clean capture and a half-received one both produce that same green
**Captured**. The user has no way to tell them apart until the command fails to
work weeks later, and by then the remote is in another room.

`selfTest()` already establishes the right vocabulary — it probes the idle line
200 times at 250 µs intervals, transmits while listening, and judges on
**payload rather than protocol label** (NEC_LIKE and RC5X are accepted). Quality
feedback should speak the same language.

### The checks, cheapest first

**Silently-dropped noise.** Today `rawlen <= IR_MIN_RAWLEN` (8) returns false and
the user sees nothing — the UI just keeps saying "Listening…" while the device
is in fact receiving something too short to be a signal. One counter, surfaced as
"saw 4 short pulses — move closer, or check for sunlight on the receiver", turns
a mystifying timeout into an actionable one. Cheapest change here, and probably
the most useful.

**Near the buffer cliff.** `overflow` is already an error. But a capture at, say,
960 of `IR_MAX_RAW`'s 1024 entries succeeded by luck and the next one may not.
Warn above ~85%.

**UNKNOWN with a short payload.** A decode of `UNKNOWN` with 20 timings is
almost certainly a fragment. A decode of `UNKNOWN` with 400 timings is a
perfectly normal air conditioner. The rule must key on length, not on the
protocol label — flagging every UNKNOWN would mark most A/C captures as bad and
train the user to ignore the warning.

**The `value == 0` trap.** For an A/C, both `bits` and `value` are zero. The UI
already handles this (`st.bits ? ...`, `st.value !== '0x0'`), and any quality
rule must too. "No decoded value" is not a defect.

**Repeat consistency — the one that actually matters.** The real failure mode is
a partially-received frame that still decodes into *something* plausible. No
single-capture heuristic catches it. Two do:

Add a **"Verify"** button next to the existing "Test" button on a captured
signal. It captures the same button a second time and diffs the two:

- same frame count, and
- each timing within ~20 % or ±150 µs of its counterpart

Pass → "Verified — two captures agree." Fail → "The two captures differ; the
first may be incomplete. Try again, closer, and hold the remote still." This
reuses the whole existing capture path and adds a comparison loop. It is the
single highest-value item in this section.

**Pre-flight line check.** `selfTest()`'s idle-line probe already exists. Running
it at the start of a learn and warning "the receiver line is noisy — check for a
fluorescent lamp or direct sunlight" before the user wastes three attempts is
reusing working code for a new purpose.

### What it would look like

A quality line under the existing detail line, in the same three-state style the
Learn tab already uses (`ok` / `waiting` / `bad`):

```
Captured        NEC · 32 bits · 68 timings · 1 part
   Good capture — clean decode, well within buffer.        [Verify] [Test]
```

```
Captured        UNKNOWN · 22 timings · 1 part
   Looks incomplete — too short for a full remote frame.
   Move closer and press the button firmly.                [Verify] [Test]
```

### What it costs

Small, and almost entirely additive. A few counters in `consumeDecode()`, three
or four fields on `/api/learn/state`, one comparison buffer for Verify, and some
UI. No change to the storage format, no change to the send path, no change to
the receiver policy. It can ship independently of everything else here.

---

## Building order

1. **Countdown timer** — best value for effort, reuses proven machinery, no
   policy changes.
2. **Capture-quality feedback** — small, additive, immediately visible to the
   user, and touches nothing else.
3. **Schedule backup** — closes a gap `HANDOFF.md` already admits to. Get the
   name-resolution right and it is a day's work.
4. **Listening to remotes** — start with the MQTT publish only. That is 30 lines
   and lets Home Assistant do the rest. Add the on-device trigger table later,
   and only if it must work with the broker down.
5. **SD card** — last, and only if automatic backups are still a felt need after
   3 ships. Not a capacity fix; there is no capacity problem.

### Shared costs to keep an eye on

- **RAM.** The command index alone is a fixed 12 KB. Timers, triggers, and a
  Verify comparison buffer each want a few KB more. Watch the free-heap figure
  logged at boot.
- **The single-loop model.** Everything above must fit inside the existing loop
  and feed the 20 s watchdog. No new tasks; that constraint is why this firmware
  is stable and is not worth trading away.
- **Flash endurance.** A timer that writes `/timers.json` on every tick would
  wear the partition out. Write on change only — the firelog's separate-file
  design exists for exactly this reason.
