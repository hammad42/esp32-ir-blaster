# Hardware guide

Everything here uses the parts you already have: a 3-pin 38 kHz IR receiver
module and an IR LED module, plus a transistor to give the LED real range.

---

## 1. Pin map

Defaults for the classic ESP32 DevKit v1 (WROOM-32). Every pin can be
overridden from `platformio.ini` with a `-D` flag — e.g. `-DPIN_IR_TX=17` —
without editing source.

| Function              | ESP32 | ESP32-S3 | ESP32-C3 | Notes |
|-----------------------|-------|----------|----------|-------|
| IR receiver OUT       | 14    | 4        | 3        | input, internal pull-up enabled |
| IR LED drive          | 4     | 5        | 4        | to transistor base resistor |
| Power LED             | 25    | 6        | 5        | optional |
| WiFi status LED       | 26    | 7        | 6        | optional |
| IR activity LED       | 27    | 15       | 7        | optional |
| Button                | 0     | 0        | 9        | the on-board **BOOT** button |
| OLED SDA (optional)   | 21    | 8        | 8        | |
| OLED SCL (optional)   | 22    | 9        | 10       | |
| Macro button 1        | 32    | 16       | 1        | only with `-DENABLE_MACRO_BUTTONS=1` |
| Macro button 2        | 33    | 17       | 2        | only with `-DENABLE_MACRO_BUTTONS=1` |

Pins were chosen to avoid strapping pins, flash/PSRAM pins, native-USB pins
(S3 GPIO 19/20) and input-only pins for outputs. Any GPIO can drive the RMT
peripheral on the ESP32, so the LED pin is a free choice.

Set an indicator you did not wire to `-1` (`-DPIN_LED_PWR=-1`) and the firmware
skips it silently.

> **BOOT button caveat.** GPIO 0 is also the bootloader strapping pin. Holding
> the button *while powering up* puts the chip in download mode instead of
> running the firmware. Press it only after boot. If that bothers you, wire an
> external button to another GPIO and pass `-DPIN_BUTTON=13`.

---

## 2. Parts

| Part | Notes |
|---|---|
| ESP32 DevKit v1 | or ESP32-S3 / ESP32-C3 |
| IR receiver module | VS1838B / TSOP38238 / HX1838 — your 3-pin 38 kHz module |
| IR LED, 940 nm | 5 mm; the emitter from a KY-005 module works |
| NPN transistor | 2N2222, BC547, S8050, or an equivalent |
| 1 kΩ resistor | transistor base |
| 33 Ω resistor, ½ W | IR LED current limit (see §5 for other values) |
| 100 nF ceramic | receiver supply decoupling |
| 100 Ω + 10 µF | receiver supply RC filter — **recommended, not optional** |
| 470 µF electrolytic, ≥10 V | bulk capacitor on the 5 V rail |
| 3 × LED + 3 × 330 Ω | status indicators, optional |

---

## 3. IR receiver wiring

Your module has three pins. **Check the silkscreen** — pinouts differ between
otherwise identical-looking boards:

- Bare **VS1838B** (domed face towards you, legs down): `OUT · GND · VCC`
- Common 3-pin breakout: usually marked `S` (signal), `+` or `VCC`, `−` or `G`

### The black "IR Receiver" 3-pin module

The common black-PCB module silkscreened **`IR Receiver`**, with the sensor at
one end and a 3-pin header at the other, is labelled per pin. Hold it with the
**sensor on the left and the header on the right**; the labels then read, from
the top edge down:

| Pin | Label | Connect to |
|---|---|---|
| 1 (nearest top edge) | `OUT` | GPIO 14 |
| 2 (middle) | `VCC` | **3V3** |
| 3 (nearest bottom edge) | `GND` | GND |

Trust the silkscreen over this table — it is printed next to the pins for
exactly this reason, and these boards get re-laid-out between batches.

This module already carries a few SMD parts, which on most versions include a
supply decoupling capacitor. That covers part of what the filter below does,
so try it plain first; add the 100 Ω + 10 µF only if you see phantom captures
or learns that time out with the remote clearly working.

```
                 100 Ω
   3V3 ─────────/\/\/\──────┬────── VCC (receiver)
                            │
                    10 µF ══╪══ 100 nF
                            │
   GND ─────────────────────┴────── GND (receiver)

   GPIO14 ───────────────────────── OUT (receiver)
```

**Power it from 3V3, not 5V.** A VS1838B runs happily on either, but at 5 V its
output idles at 5 V, and ESP32 GPIOs are *not* 5 V tolerant. At 3V3 the output
is already correct logic level and you can wire it straight to GPIO14. If you
must run it at 5 V, add a divider (10 kΩ from OUT to the pin, 20 kΩ from the pin
to GND) or a proper level shifter.

**The 100 Ω + 10 µF filter matters.** These receivers have enormous AGC gain and
will happily demodulate noise from the ESP32's own switching regulator, which
shows up as phantom captures and failed learns. It costs two components.

Keep the receiver **at least 5 cm away from the IR LED**, ideally facing the
other way, or the blaster deafens itself.

---

## 4. IR LED driver

A GPIO can source ~20 mA at 3.3 V; an IR LED wants 100 mA or more in pulses to
reach across a room. That is what the transistor is for.

```
                  5V
                   │
                   ├──────────────┐
                   │              │
                 ╍╍╍╍╍          470 µF   (bulk, close to the LED)
                  IR LED          │
                 ╍╍╍╍╍           GND
                   │
                  33 Ω  ½ W
                   │
                   ├─────────────── Collector
       1 kΩ        │
GPIO4 ─/\/\/\──── Base        2N2222
                   │
                  Emitter
                   │
                  GND  (shared with the ESP32)
```

### Transistor pinouts are NOT interchangeable

Flat face towards you, legs pointing down:

| | left | middle | right |
|---|---|---|---|
| **2N2222 / 2N2222A (TO-92)** | Emitter | Base | Collector |
| **BC547 / BC548** | Collector | Base | Emitter |
| **S8050** | Emitter | Base | Collector |

Getting this backwards is the single most common reason a home-built blaster
does not transmit. Check yours against its datasheet.

### If you use a ready-made transmitter module

**The 2-pin KY-005** is an IR LED with a 330 Ω resistor, driven straight from a
GPIO. It works — expect 1–2 m of range, enough to test with. For whole-room
range, take the LED off it and build the driver above.

**The black 3-pin `IR Transmitter` module** (silkscreened `DAT · VCC · GND`,
some marked `V1221`) is the more interesting case. Hold it with the **LED on
the left and the header on the right**; the labels read, from the top edge down:

| Pin | Label | Connect to |
|---|---|---|
| 1 (nearest top edge) | `DAT` | GPIO 4 |
| 2 (middle) | `VCC` | 3V3 to start — see below |
| 3 (nearest bottom edge) | `GND` | GND |

The presence of a `VCC` pin means it is *not* a bare LED-and-resistor board,
but these modules ship in two different designs and you cannot tell which you
have by looking:

- **With a driver transistor** — the LED is fed from `VCC` and `DAT` only
  switches the transistor's base. The LED current comes from your supply, not
  from the GPIO, so you can run `VCC` at 5 V and get real range with no extra
  parts.
- **Direct drive** — `DAT` feeds the LED through a resistor and `VCC` only
  powers an indicator. Current is capped by what the GPIO can source (~20 mA),
  so expect 1–2 m however you wire it.

#### Telling them apart (two minutes, multimeter)

With the module **unpowered and disconnected**, set the meter to resistance,
and measure to the LED's anode — the pad marked **`+`** next to the LED:

| Measure | Reading | Means |
|---|---|---|
| `VCC` → LED `+` | 0 Ω, or tens of ohms | LED is fed from VCC → **transistor driver**. Move `VCC` to 5 V for full range. |
| `DAT` → LED `+` | ~100–330 Ω | LED is fed from DAT → **direct drive**. Range is GPIO-limited. |

One of the two will read low and the other open or very high. That tells you
which design you have.

**If it turns out to be direct drive** and you want more than a couple of
metres, the module is still useful: leave its LED in place, cut or lift the
track from `DAT`, and drive the LED from a 2N2222 collector as in the schematic
above. Or just fit a separate 5 mm IR LED on the transistor and keep the module
as a spare.

Either way, **start with `VCC` on 3V3**. It is safe in both designs, and it
lets you confirm the firmware transmits before you change anything.

#### Upgrading a direct-drive module for range

If the test says direct drive and 1–2 m is not enough, note first **why the
obvious fix does not work**: the module's own series resistor (usually 330 Ω)
sits in the current path. Feeding the `DAT` pin from a transistor still leaves
that resistor in series, so the current is capped at roughly
`(5 V − 1.3 V) / 330 Ω ≈ 11 mA` — worse than driving it from the GPIO. The
resistor cannot be bypassed either: one end is the LED pad, the other is buried
in the board.

**The LED has to leave the module.** Desolder it (two joints — note which pad
is `+` first), or leave the module intact as a spare and fit a fresh 940 nm
5 mm LED. Either way, build the transistor driver from §4 above: 1 kΩ from
GPIO 4 to the base, 33 Ω from 5 V to the LED anode, LED cathode to the
collector, emitter to the shared ground, and the 470 µF bulk capacitor across
the 5 V rail.

No firmware change is needed. An NPN on the low side is non-inverting — GPIO
high turns the LED on — which is what `IRsend` already assumes.

---

## 5. Choosing the LED resistor

With a 940 nm LED (V<sub>f</sub> ≈ 1.3 V) and V<sub>CE(sat)</sub> ≈ 0.2 V on a
5 V rail:

| Resistor | Peak current | Range (typical) |
|---|---|---|
| 100 Ω | ~35 mA | 2–3 m, gentle on everything |
| 47 Ω | ~75 mA | 4–6 m |
| **33 Ω** | **~105 mA** | **6–8 m — the recommended default** |
| 22 Ω | ~155 mA | 8–12 m, check your transistor and supply |

The carrier is a 38 kHz square wave that is only on during marks, so average
dissipation is far below peak; a ½ W resistor is comfortable at any of these.
Two IR LEDs in series (V<sub>f</sub> ≈ 2.6 V) with 22 Ω is a good way to widen
coverage without raising current.

**Fit the 470 µF capacitor.** A 100 mA pulse train on a thin USB cable sags the
5 V rail; on a marginal supply that shows up as the ESP32 rebooting whenever you
send a command. The bulk capacitor is what keeps a 24/7 device from doing that.

---

## 6. Status LEDs and button

```
GPIO25 ──/\/\/\── LED ── GND      330 Ω, power   (solid)
GPIO26 ──/\/\/\── LED ── GND      330 Ω, WiFi
GPIO27 ──/\/\/\── LED ── GND      330 Ω, activity
```

LEDs are driven active-high. If you wire them anode-to-3V3 instead, build with
`-DLED_ACTIVE_LOW=1`.

| WiFi LED | Meaning |
|---|---|
| solid | connected to your network |
| slow blink (once per 2 s) | disconnected, retrying |
| fast blink | setup access point is up |
| double blink | learn mode armed, waiting for a signal |

The button is the DevKit's own **BOOT** button — nothing to wire.

| Hold | Action |
|---|---|
| < 1 s | start learn mode (name it later in the web UI) |
| 1–5 s | toggle the setup access point |
| **5–10 s, then release** | **factory reset** — the activity LED flashes to tell you that you are in the window |
| > 10 s | cancelled, nothing happens |

The "release within the window" design is deliberate: a stuck button or
something resting on the case cannot wipe your commands.

---

## 7. Assembly order

Do it in this order and each step proves the previous one.

1. **Flash first, wire second.** `pio run -t upload` then `pio run -t uploadfs`
   with nothing else attached. Confirm the boot banner on the serial monitor.
2. **Receiver only.** Wire it, reboot, open the web UI → **Learn** → turn on
   *Monitor incoming signals*, and press any remote at it. You should see a
   protocol name appear within a second. If nothing does, the receiver is
   miswired — fix that before going further.
3. **Transmitter.** Wire the driver. Learn one command from a remote you can
   see working (a TV power button), then press it in the UI. Aim the LED at the
   appliance from a metre away.
4. **Point a phone camera at the IR LED** while sending. Most phone cameras see
   940 nm as a faint purple flicker; if you see it, the driver works and the
   problem is elsewhere.
5. **LEDs, bulk capacitor, enclosure.**

---

## 8. Power

USB 5 V, from anything that can hold 500 mA. The ESP32 peaks around 250 mA on
WiFi transmit and the IR driver adds ~100 mA in bursts.

Give it a decent supply. Phone chargers with thin captive cables are the usual
cause of a device that "randomly reboots" — which the firmware will report
honestly, since the System tab shows the last reset reason and the log warns
after an abnormal restart.

---

## 9. Range and placement

- **Line of sight beats power.** A blaster on a shelf facing the room
  out-performs a stronger one behind the TV.
- **Bounce works.** IR reflects well off white ceilings and walls; aiming up
  often covers a room better than aiming at one appliance.
- **Two LEDs in series, angled apart**, is the cheapest way to cover a room
  corner-to-corner.
- **Keep the receiver shaded** from direct sunlight and away from CFL/plasma
  sources, which flood it with noise.
