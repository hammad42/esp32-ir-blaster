# Project handoff — ESP32 IR Blaster

Working state as of **2026-09-04**. Written so a new session can pick this up
without re-deriving anything.

**To resume:** point the new chat at this file. Everything below was verified
first-hand on the actual hardware unless it says otherwise.

---

## 1. Hardware in hand

### Board

| | |
|---|---|
| Chip | **ESP32-D0WDQ6 rev 1.0**, dual core, 240 MHz, 4 MB flash |
| MAC | `<device-mac>` |
| USB bridge | Silicon Labs **CP2102**, appears as **COM7** |
| Build target | `esp32dev` |

### IR receiver — black 3-pin "IR Receiver" module

Sensor on the left, header on the right. Labels read top → bottom:

| Pin | Wired to |
|---|---|
| `OUT` | **GPIO 14** |
| `VCC` | **3V3** — *not 5 V*, ESP32 GPIOs are not 5 V tolerant |
| `GND` | GND |

The module carries its own SMD decoupling, so the extra 100 Ω + 10 µF filter from
the docs was **not** needed. No phantom captures observed.

### IR transmitter — bare LED + 2N2222 driver

The LED was **removed from the 3-pin transmitter module**. That module was
direct-drive (its `DAT` pin fed the LED through an onboard 330 Ω), and that
resistor cannot be bypassed — it sits between the header and the LED pad, so any
external transistor still ends up current-limited to ~11 mA.

Current circuit — **low-side switch**:

```
5V ──── [33 Ω] ──── LED(+)
                    LED(−) ──── Collector
                                   │
                                Emitter ──── GND
                                   │
        GPIO4 ── [1 kΩ] ──────── Base

        470 µF electrolytic across the 5 V rail, close to the LED
```

2N2222 in TO-92, flat face toward you, legs down: **E · B · C** left to right.

### ⚠️ The LED position fix — this mattered

It was originally wired as an **emitter follower** (5 V → collector, emitter →
LED, LED → 33 Ω → GND). That topology clamps the emitter to `V_base − 0.7` ≈
2.6 V, so it never reaches the 5 V rail and self-limits to ~30 mA. Range was
**~10 cm**.

Moving the transistor **below** the LED (between LED and ground) makes it a
proper switch: it saturates at ~0.2 V, the LED and resistor get the full 4.8 V,
and current rises to ~106 mA. **Range improved significantly.**

If range ever regresses, check this first — it is the single highest-impact
thing in the whole build.

### Not built / not connected

- Status LEDs (GPIO 25/26/27) — firmware drives them harmlessly regardless
- SSD1306 OLED — `ENABLE_OLED` is off in the default build
- Macro buttons — `ENABLE_MACRO_BUTTONS` is off
- External reset button — the on-board **BOOT** button is used instead

### Bench notes

- Auto-reset over USB is **intermittent**. When `esptool` says
  `Wrong boot mode detected (0x13)`, do: hold **BOOT** → tap **EN** → release
  **BOOT**, then flash. It has also worked with no button at all — genuinely
  flaky, not a firmware issue.
- Once on WiFi, prefer **HTTP OTA** (§6) and the problem disappears.
- `espota` / `pio run -e esp32dev_ota` **fails** on this machine: it needs the
  device to connect *back* to the PC on a high port and Windows Firewall blocks
  it. The firmware's own HTTP OTA endpoint has no such problem.

---

## 2. Network and access

Identifying values are redacted because this repo is public. Recover the real
ones from the device itself:

```bash
pio device list                               # which COM port
curl -s http://ir-blaster.local/api/status    # ip, ssid, rssi, ap name
```

| | |
|---|---|
| WiFi SSID | `<your-network>` |
| Device IP | **<device-ip>** (DHCP — no reservation yet, may move) |
| mDNS | `ir-blaster.local` — resolves from Windows |
| RSSI | −41 to −49 dBm |
| Setup AP | `IR-Blaster-XXXXXX` / password `irblaster` / http://192.168.4.1 |
| Web auth | **disabled** |
| MQTT | **disabled**, never configured |
| Stored schedules | **0** |
| Timezone | `UTC0` — **not yet set to local**, so schedule times are UTC |

---

## 3. Firmware

Repo: **https://github.com/hammad42/esp32-ir-blaster** · v1.0.0 · built against
`espressif32@6.13.0` (arduino-esp32 **2.0.17**).

Flash on `esp32dev`: RAM 24.2 %, **Flash 73.8 %** of the 1.5 MB OTA slot. The
jump from 68 % is the A/C encoder library (§4), which is worth it.

### Branch state — read this before starting work

| Branch | Contains | Merged? |
|---|---|---|
| `main` | everything through PR #6 (seven bug fixes) | — |
| `docs/handoff` | the first version of this file | **no** |
| `feat/ac-library` | built-in A/C library, preset packs, Dawlance encoder, **and this file** | **no** |

`feat/ac-library` is the interesting one and supersedes `docs/handoff` — this
document lives there now, so **merge `feat/ac-library` and delete
`docs/handoff`** rather than merging both.

The device is running `feat/ac-library`; every change was OTA'd as it was
verified.

### Stored commands (18)

All in the group **Dawlance AC**: temperatures 16–28, `Dawlance AC Power ON` /
`OFF`, `eco`, `turbo`, `lamp off`. Every one is a raw capture, 147 timings,
single frame.

Exported to `ir-recordings.json` in the project root (gitignored — it is user
data). Restore with:

```bash
tools/ir-backup.sh restore <device-ip> ir-recordings.json
```

---

## 4. The built-in library — the big addition

**Problem it solves.** An air conditioner never sends "temperature up" — every
button transmits the unit's whole state. Teaching one by hand therefore means
capturing every temperature separately, and you still cannot use a combination
you never pressed.

**What it does.** Pick a brand, set mode / temperature / fan, and the device
*encodes the frame itself* with the correct checksum. **66 protocols**: the 65
IRremoteESP8266 supports via `IRac` (Gree, Coolix, Midea, Daikin, Electra,
Fujitsu, Haier, Hitachi, LG, Samsung, TCL, Toshiba, Argo, Airton…) plus
**DAWLANCE**, written for this project.

The **Library** tab has a "save a whole temperature range" action that creates
one command per degree in a single click. That is the part that replaces an
afternoon at the receiver.

### How generated commands are stored

Not as timings. A generated command holds the standard `stdAc::state_t` struct
(~56 bytes against ~1.2 KB for a capture), flagged `IR_FLAG_AC_STATE`, and is
replayed by handing the struct back to the encoder. So the checksum stays the
library's rather than a reconstruction of it, and a library upgrade improves
existing commands for free. This needed **no change to the store** — the struct
rides in the existing payload, one byte per entry.

Everything else about them is ordinary: Remotes, schedules, MQTT, backup.

### The Dawlance protocol — fully reverse engineered

Not in IRremoteESP8266, and it does not match anything there: 72 bits over a
6.7 / 3.3 ms header. Airton is a near-perfect *timing* match (6630/3350 against
our 6690/3345) but is 56 bits, so the decoder rejects it — which is why captures
report `UNKNOWN`.

Derived from 18 captures off the unit:

```
AA 11 <mode|power|turbo> <temp-16> 00 <flags> 00 00 <checksum>

byte 0,1   constant AA 11
byte 2     bits 0-2 mode, bit 3 power (09 on / 01 off), bit 4 dry, bit 7 turbo
           mode: auto 0, cool 1, dry 2, fan 3, heat 4
byte 3     temperature - 16   (16 -> 0x00 ... 28 -> 0x0C)
byte 4     00, except 01 in auto
byte 5     base 0x44, bit 7 display light, bit 0 economy
byte 6,7   always 00
byte 8     checksum = sum(bytes 0..7) XOR 0xAA
```

Timings: header 6720/3300, bit mark 460, zero space 390, one space 1200, stop
mark 460, 38 kHz.

**Verification** — this is why it is trusted:

| Check | Result |
|---|---|
| Checksum formula against 18 captures | **18 / 18** |
| Encoder bytes against captures | **8 / 8 identical** |
| On device after OTA, 13 temperatures | **all match** |
| Power on/off, eco, lamp off, turbo | **5 / 5 match** |

So `16 °C` from the Library produces `AA 11 09 00 00 C4 00 00 22` — the exact
frame the remote sends.

### All five modes are now measured

The mode field used to be the one guess in the protocol. It has since been
captured one mode at a time off the same remote, and the guess was right —
**auto 0, cool 1, dry 2, fan 3, heat 4**. Reference frames:

```
auto  AA 11 08 0A 01 44 00 00 B8      dry   AA 11 1A 09 00 44 00 00 88
cool  AA 11 09 07 00 44 00 00 A5      fan   AA 11 0B 0B 00 44 00 00 BF
heat  AA 11 0C 0C 00 44 00 00 BD
```

Those captures corrected **two things the encoder had wrong**:

- **byte 2 bit 4 (`0x10`) is set in dry, and only in dry.** The remote sends
  `1A`; the encoder used to send `0A`.
- **byte 4 is `0x01` in auto**, not the constant `0x00` it was documented as.

Both ride in the checksum and it validates, so they are real bits rather than a
decode artefact. Each rests on a **single capture per mode**, though, so what
they *mean* is open — they are reproduced because that is what the remote
sends, not because the field is understood.

After the fix the encoder reproduces all five frames exactly, and the 13 cool
temperatures are unchanged (13/13).

**Still not encoded: fan speed.** Byte 5 read `0x44` in all five captures, so
nothing here separates it. That is the next field to chase.

`tools/dawlance-decode.pl` turns a captured frame back into its nine bytes and
checks the checksum — this is how the above was derived:

```bash
curl -s http://$IP/api/export | perl tools/dawlance-decode.pl
perl tools/dawlance-decode.pl data/presets/dawlance-inverter.json   # 18/18 ok
```

### The TV half of the Library tab

The Library tab has two sub-tabs, **Air conditioner** and **TV**, and they work
in opposite directions:

| | A/C | TV |
|---|---|---|
| A frame carries | the unit's whole state | one fixed button code |
| So the firmware | **generates** it | **remembers** it |
| Source of truth | `IRac` + the Dawlance encoder | a table of measured values |

A TV code cannot be derived — manufacturers assign them arbitrarily — so the
table only ever holds values read off a real remote. `TV_NO_CODE` marks a button
that has not been captured; the UI draws it disabled and the endpoints refuse
it. **Nothing in that table is guessed**, deliberately: a missing button is
visibly missing, while a wrong one looks fine and silently does nothing.

Adding a model is a row in `kModels` in `src/tv_library.cpp` — protocol,
timings and the code array. No other firmware change.

#### The 17 models, and how they got there

Sixteen brands were imported from **Flipper-IRDB** (CC0-1.0, ~400 TV files),
plus TCL from captures. About **2 KB of flash** for the lot.

| Protocol | Brands |
|---|---|
| NEC | LG, Hisense, Toshiba, Sharp, Vizio, Hitachi, Philips, Insignia, Element, Sanyo, Westinghouse |
| SAMSUNG | Samsung, JVC |
| SONY | Sony, Sceptre |
| NIKAI | TCL, RCA |

```bash
perl tools/flipper-import.pl <file.ir>                       # decode to values
perl tools/flipper-table.pl <file.ir> <id> <Brand> <label>   # emit the C table
```

**The importer mirrors IRremoteESP8266's own encoders** — `encodeNEC`,
`encodeSAMSUNG`, `encodeSony` — rather than the protocol documentation, because
the library encoder is what actually transmits. Anything whose layout has not
been worked out is **refused, not guessed**: that is why **Panasonic
(Kaseikyo)** and **Grundig (RC5/RC6)** are absent. Adding them means working
out those layouts and checking them against a capture.

**Three independent checks, all passed:**

1. Generated **Samsung** codes (`Power E0E040BF`, `Vol+ E0E0E01F`,
   `Ch+ E0E048B7`…) and **LG** `Power 20DF10EF` match the well-known published
   values exactly.
2. Every protocol round-trips through the device's **loopback self-test** — real
   IR out of the LED, decoded back by the receiver. NEC, SAMSUNG, SONY and
   NIKAI all pass.
3. Saved commands' stored timings decode back to their own value: Samsung
   `0xE0E040BF` (67 entries), Sony `0xA90` (80 entries, 3 frames) — which is
   what verifies the **mark-modulated** generator, since Sony puts the bit in
   the mark rather than the space.

> **The database has errors, and the generator now catches one class of them.**
> `Samsung.ir` lists `Ch_next` and `Ch_prev` on the *same* command (`0x10`).
> `flipper-table.pl` drops any button sharing a code with another and says so —
> otherwise that would have shipped as a channel-down button that changed
> channel the wrong way. A different Samsung file was used instead.

**Presses go through the library encoder**, not the locally generated timings,
so each protocol gets its own minimum repeat count (Sony will not act on fewer
than three frames). `TvLibrary::encode()` still builds timings for `save()`,
because the store needs them.

> **None of the 16 has been tested against a real television.** They are
> verified as correct waveforms for the codes in the database; whether those
> codes match any particular set is a separate question. A brand ships many
> remotes.

#### TCL — the first model, all 16 buttons

The four captures off the remote turned out to be enough to identify the whole
remote, and this is the method worth reusing for the next brand.

**The protocol is RCA, sent as NIKAI.** IRremoteESP8266 has no RCA encoder, but
it has NIKAI, and they are the same waveform — 4000/4000 header, 500 µs mark,
1000/2000 µs spaces. They differ only in which space means one:

| | one | zero |
|---|---|---|
| RCA | long (2000 µs) | short (1000 µs) |
| **NIKAI** | **short (1000 µs)** | **long (2000 µs)** |

So an RCA frame is transmitted by handing the **bitwise complement** of the RCA
value to NIKAI. Same light, opposite bookkeeping.

> This cost a pass. Decoding a capture with the usual "long space = 1" rule
> gives the exact complement of the truth, and because the frame carries its own
> complement it *still self-validates*. It looks right and is entirely wrong.

**The frame** is 24 bits: `[addr:4][cmd:8][~addr:4][~cmd:8]`, every field **LSB
first**. TCL is address `0x0F`.

**How the table was confirmed.** Flipper-IRDB (CC0-1.0) carries a TCL table
under protocol RCA, address `0x0F`. Re-encoding its entries with
`tools/flipper-import.pl` reproduces **all four captured 24-bit values exactly**:

| Captured | Reproduced from the table | Saved as |
|---|---|---|
| `0xFEF010` | **Netflix** `F7` | `tcl netflix` ✓ |
| `0xF2F0D0` | **Vol_up** `F4` | `tcl vol+` ✓ |
| `0xF3F0C0` | **Mute** `FC` | `tcl power` ✗ |
| `0xF580A7` | **Down** `1A` | `tcl -` ✗ |

Four exact waveform reproductions, two of which also match the label they were
saved under. That is what makes the rest of the table trustworthy: it is
confirmed to be the right family and address, so its other buttons describe the
same remote.

**Two captures were mislabelled**, which is worth knowing rather than quietly
fixing: `tcl power` is really **Mute**, and `tcl -` is the d-pad **Down**, not
volume-down. Real power is cmd `0x54`. The original captures are still in the
store under their old names.

**Power is a toggle** — one code for on and off, normal for a TV. `power_on` and
`power_off` stay unknown rather than being aliased onto it, which would make
"turn off" turn the set on half the time.

> **Still unconfirmed: the television has never been seen reacting.** Every code
> is verified as a *waveform*, not as an effect. See §6.

### Preset packs

For remotes the encoder cannot generate, `data/presets/` holds installable
packs of captured codes. `index.json` lists them; the browser fetches a pack and
posts each command to `/api/import` — the same path a restore takes, so **adding
a device to the library needs no firmware change**, only a new file.

`data/presets/dawlance-inverter.json` holds the original 18 captures. Now
partly redundant given the native encoder, but it is the reference data the
protocol was derived from, so it is worth keeping.

Build one from any backup:

```bash
tools/make-preset.sh <backup.json> <out.json> <id> <brand> <model> <group>
```

### New API

| Endpoint | Purpose |
|---|---|
| `GET /api/library/protocols` | the 66, sorted by name |
| `POST /api/library/ac/preview` | encode and describe, transmit nothing |
| `POST /api/library/ac/send` | encode and transmit, store nothing |
| `POST /api/library/ac/save` | encode and store as a normal command |

Body: `protocol` (required) plus `power` `mode` `degrees` `celsius` `fan`
`swingv` `swingh` `quiet` `turbo` `econo` `light` `filter` `clean` `beep`
`sleep`, and `name`/`group` for save. Full table in `docs/API.md`.

---

## 5. Verified working on hardware

- Boot, LittleFS mount, command index rebuild
- WiFi station, captive-portal AP fallback, network scan
- Learn: arm, capture, multi-part append, save, cancel/discard
- Send: raw replay, multi-frame with gap
- **AC responds** — the original goal
- Self-test loopback: NEC, SAMSUNG, SONY, RC5 round-trip; state-based A/C
  protocols correctly refused
- Custom hex-code loopback from the self-test card
- HTTP OTA — firmware **and** filesystem
- Backup / restore, including a full wipe-and-restore of 18 commands
- Schedules: create, persist, survive reboot **(never actually fired — see §6)**
- Clearing a stored password
- **Library: 66 protocols listed, preview / send / save all work**
- **Generated Gree command stores as 56 bytes and sends via the normal path**
- **Dawlance generation matches captures for every tested combination** — 13
  cool temperatures, power on/off, eco, lamp, turbo, and one frame per mode
  (auto / cool / dry / fan / heat), all byte-exact
- Watchdog armed; no crashes or brownouts since the 470 µF went in

---

## 6. NOT yet tested — the list to work through

### Highest value first

- [x] ~~**Dawlance modes other than cool.**~~ Done — all five captured and the
      encoder corrected (§4). The bytes match; **the AC has not yet been made to
      actually respond in heat / dry / fan / auto**, so send each one at the unit
      and confirm the display agrees.
- [x] ~~Capture the three missing TCL buttons.~~ Not needed -- the full 16-button
      set came from Flipper-IRDB, confirmed against the captures (see §4).
- [ ] **Press Power on the TV panel with the television in view.** Every code
      is confirmed *as a waveform* -- four reproduce the captures exactly -- but
      **the TV has never been observed reacting to any of them**. Power, volume
      and channel are the ones to watch.
- [ ] **Find where fan speed lives.** Nothing in the five mode captures moves
      with it. Capture low / medium / high at a fixed mode and temperature and
      diff the bytes — byte 5, 6 or 7 are the candidates.
- [ ] **Measure the range** now the transistor is wired as a low-side switch.
      Walk backwards from the unit and note where it stops.
- [ ] **Set the timezone.** Still `UTC0`; Pakistan is `PKT-5`. Schedules fire on
      UTC until this is done.
- [ ] **Confirm a schedule actually fires.** Created and persisted, never fired.
      Now much easier to check: the Schedules tab has a **What actually fired**
      card, and the device records every fire whether or not anyone is watching.
      Set one a couple of minutes out, walk away, and read the card.

### Schedule fire acknowledgement

A schedule runs unattended, so being configured correctly was previously the
only assurance that anything happened. Every fire is now recorded.

The trick is the one the self-test already used: a transmit normally deafens
the receiver so the blaster does not capture its own output, but a *scheduled*
send deliberately leaves it listening. `IrService::sendStoredVerified()` sets
`listenWhileSending_`, which makes the four transmit paths skip the usual
`setReceiverEnabled(false)`, then polls the decoder for `SCHED_ECHO_WAIT_MS`
afterwards.

Three outcomes, and they point at different faults:

| Verdict | Means | Suspect |
|---|---|---|
| **heard** | the frame came back | the blaster is fine — look at the appliance |
| **not heard** | the send succeeded but nothing returned | the emitter: transistor, LED, current |
| **failed** | the send was refused | firmware or storage; the reason is logged |

> **`heard` does not mean the appliance obeyed.** An air conditioner sends no
> reply, so nothing can confirm that from here. It means the transistor
> switched and the LED lit — which is the failure that actually happens.

Stored in `/firelog.json`, deliberately **not** in `/schedules.json`: it is
rewritten after every fire, and a bad write must not be able to take the
schedules with it. Holds the last `SCHED_LOG_MAX` (16) fires and survives a
reboot. An armed learn keeps the receiver, so a fire during one goes out
unverified rather than stealing the capture.

### Library
- [ ] Bulk "save a temperature range" from the UI against the real AC
- [ ] A generated command driven from a schedule
- [ ] A generated command driven over MQTT
- [ ] Backup/restore round-trip of a *generated* command (the `acState` flag)
- [ ] Install a preset pack from the Library tab

### IR / range
- [ ] `repeats` = 2 or 3 on a stubborn command
- [ ] Carrier at 36 and 40 kHz
- [ ] `markExcessUs` at 50 and 100
- [ ] Learn a second appliance — a TV or fan — and confirm a known protocol
      decodes properly rather than UNKNOWN
- [ ] Bounce off a ceiling instead of line of sight

### Web UI paths never clicked
- [ ] Rename / regroup, per-command options, delete from the UI
- [ ] Group chips and the search box
- [ ] Export and Import through the browser (only ever done via curl)
- [ ] OTA through the web form (only ever done via curl)
- [ ] Logs and Reboot on the System tab
- [ ] Dark mode — theme-aware but only ever seen in light

### MQTT / Home Assistant — completely untested
- [ ] Point it at a broker, confirm connect
- [ ] `irblaster/send` by name and by id
- [ ] Last will: pull power, confirm `offline` appears
- [ ] Home Assistant discovery — commands as button entities
- [ ] Deleting a command removes its HA entity

### Reliability
- [ ] **Factory reset**: hold BOOT 5–10 s, release
- [ ] Hold BOOT >10 s — should cancel, not reset
- [ ] Short tap — starts learn; 1–5 s — toggles the setup portal
- [ ] Pull the router; confirm reconnect and the rescue AP after 5 minutes
- [ ] Enable web auth; confirm the prompt and that OTA still works
- [ ] Long uptime soak — check `heapMin` after a few days

### Optional hardware never built
- [ ] Status LEDs on GPIO 25/26/27
- [ ] SSD1306 OLED (`esp32dev_oled`)
- [ ] Macro buttons (`-DENABLE_MACRO_BUTTONS=1`)

---

## 7. Commands worth having

```bash
# PlatformIO is installed but NOT on PATH
export PATH="$HOME/.platformio/penv/Scripts:$PATH"

pio run -e esp32dev                 # build firmware
pio run -e esp32dev -t buildfs      # build the web UI image

# Flash over WiFi -- preferred. No cable, no BOOT button, no firewall issues.
IP=<device-ip>
curl -X POST -F "image=@.pio/build/esp32dev/firmware.bin" http://$IP/api/ota/firmware
curl -X POST -F "image=@.pio/build/esp32dev/littlefs.bin" http://$IP/api/ota/filesystem

# Back up FIRST -- a filesystem OTA erases every command.
tools/ir-backup.sh save    $IP ir-recordings.json
tools/ir-backup.sh list    ir-recordings.json
tools/ir-backup.sh restore $IP ir-recordings.json

# Poke it
curl -s http://$IP/api/status
curl -s http://$IP/api/commands
curl -s http://$IP/api/library/protocols
curl -s -X POST http://$IP/api/selftest -d '{}'

# Generate a Dawlance frame without sending it
curl -s -X POST http://$IP/api/library/ac/preview -H 'Content-Type: application/json' \
  -d '{"protocol":"DAWLANCE","power":true,"mode":"cool","degrees":24}'
# -> {"summary":"DAWLANCE · on · Cool · 24C · fan Auto",
#     "hex":"AA 11 09 08 00 C4 00 00 3A"}

pio device monitor -p COM7 -b 115200
```

**Firmware OTA keeps commands. Filesystem OTA erases them.** Separate
partitions. This cost real captures twice.

---

## 8. Open issues

**Dawlance fan speed is not encoded.** No captured byte moves with it yet. See
§4 and §6. This is now the top open protocol question; the modes are done.

**Finding #6 — receiver lockout after transmit with the monitor active.**
Reported but never reproduced. Every path was traced and they are balanced.
Left unfixed deliberately: editing the receiver state machine on an
unreproduced report is how the null-timer crash got in. Repro to try: monitor
on → press a remote (line updates) → send a command → press the remote again.
If the monitor stops updating, that is it.

**README is out of date.** It still says in a prominent callout that the
firmware has never run on hardware. Long since untrue. The CHANGELOG's
"Verified" section needs the same treatment.

**Schedules are not backed up.** `tools/ir-backup.sh` covers commands only;
`/schedules.json` has no backup path. A filesystem OTA loses them silently.

**Timezone is `UTC0`.** Schedules fire on UTC until it is set.

**No DHCP reservation** — the IP can move. `ir-blaster.local` is the stable
handle.

---

## 9. Things learned the hard way

- **`getCorrectedRawLength()` budgets a different index range than an obvious
  write loop walks.** It covers `rawbuf[0..rawlen-2]`; a loop that drops the
  leading gap covers `rawbuf[1..rawlen-1]`.
- **`IRrecv::resume()` dereferences a timer that `disableIRIn()` frees and
  NULLs.** Never resume a stopped receiver — this panicked the device on the
  first capture that ever succeeded.
- **`LittleFS.begin(true)` formats on any mount failure**, silently, and the
  boot log looks normal afterwards.
- **A `<datalist>` filters against whatever is already typed in its input.**
  Useless for "pick from scan results" when the field is pre-filled.
- **A 24-hour `Cache-Control` on a device with OTA** pairs new HTML with old JS:
  buttons that exist but do nothing.
- **`IRsend::mark()`/`space()` are only virtual under `-DTEST`**, so the encoder
  output cannot be intercepted in a production build. Store the state struct
  instead of trying to capture timings.
- **The shell keeps eating escapes.** `printf` swallowed a `\25B8` CSS escape; a
  double-quoted `perl -e` turned `$('#id')` into command substitution; a
  *single*-quoted perl string wrote `\n` literally and collapsed a comment block
  onto one line, swallowing the statement after it — and it still compiled. Use
  quoted heredocs, `--body-file`, or the editing tools. Never push code through
  shell quoting.
- **An AC button sends the whole state, not a delta.** This is why a code
  database needs one entry per *combination*, and why generating beats storing.
