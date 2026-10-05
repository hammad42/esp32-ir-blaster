# ESP32 IR Blaster — Single-Cell LiPo Veroboard Design

Pin map = firmware defaults for classic ESP32 DevKit ([include/board_pins.h](include/board_pins.h)):
IR TX **GPIO4**, IR RX **GPIO14**, LEDs **25/26/27**, BOOT button **GPIO0**, I²C **21/22**, macro buttons **32/33**.

---

## 1. Design decisions (and why)

| Problem on a 1S cell (3.0–4.2 V) | Decision |
|---|---|
| DevKit's AMS1117 needs ≥ 4.5 V in (1.1 V dropout) — dies at ~3.9 V | Bypass it. Feed the **3V3 pin** from a **buck-boost 3.3 V** converter (works from 2.7 V to 4.2 V+) |
| Charging while the device runs (UPS-style) confuses a bare TP4056 | **Power-path:** USB → TP4056 → battery *only*. The load hangs off `VSYS = max(USB, BAT)` through two Schottky diodes, so the charger always sees a clean battery and terminates properly |
| LED driver from 5 V (original design) | Drive LEDs from **VSYS** (3.4–4.6 V). NPN low-side switch, **one resistor per LED** |
| ESP32 WiFi peaks ≈ 400 mA | Buck-boost ≥ 1 A, 470 µF bulk on VSYS and 100 µF on 3V3 |
| Cell abuse | Protected cell, or TP4056 module **with DW01 + 8205** protection (the 6-pin-IC version) |

> [!WARNING]
> My earlier "emitter-degeneration constant current" tip does **not** work on a LiPo. It burns ~2.6 V across the emitter resistor, which a 3.0–4.2 V rail cannot spare. Use per-LED resistors as below.

> [!IMPORTANT]
> This firmware uses `WiFi.setSleep(false)` plus an always-on web server, so it draws ~100–130 mA. Battery is best used as **UPS / portable power**, not a weeks-long sensor. See §6.

---

## 2. Block diagram

```
 USB-C ──► [TP4056 + DW01/8205 module] ──► BAT+ ──► [Cell 1S, protected]
   │             (charges the cell only)       │
   │ 5V                                         │ VBAT (also → ADC divider)
   ▼                                            ▼
  D1 (SS34) ─────────┐              ┌───────── D2 (SS34)
                     ▼              ▼
                   ┌──────── VSYS ────────┐   (3.4 V on battery / 4.6 V on USB)
                   │  C1 470µF + 100nF    │
                   ├──► SW1 ──► [Buck-boost 3.3V ≥1A] ──► 3V3 rail ──► ESP32 3V3 pin
                   │
                   ├──► R_L1 22Ω ─► IR LED1 ─┐
                   └──► R_L2 22Ω ─► IR LED2 ─┴─► Q1 collector   (Q1 = 2N2222/BC337, emitter→GND)
                                                     ▲ base ◄─ 330Ω ◄─ GPIO4   (+10k base→GND)
```

---

## 3. Bill of materials

| Ref | Part | Notes |
|---|---|---|
| U1 | ESP32 DevKit v1 (30/38-pin) on female headers | Reference target |
| U2 | **3.3 V buck-boost, ≥ 1 A, Vin 2.7–5.5 V** | e.g. Pololu S7V8F3, or TPS63802/TPS63020 module. Avoid MT3608-style boost-only boards |
| U3 | **TP4056 USB-C module with protection** (DW01A + FS8205A) | Set charge current ≤ 0.5 C (1 kΩ Rprog = 1 A; use 2 kΩ for ≤ 600 mAh cells) |
| BT1 | 1S LiPo 1000–2000 mAh **with protection PCB**, or 18650 + holder | Max pulsed draw ≈ 0.7 A |
| D1, D2 | SS34 Schottky (1N5819 acceptable) | Power-path OR-ing |
| Q1 | 2N2222A / BC337 / S8050 (TO-92) | Low-side IR switch |
| R1 | 330 Ω | GPIO4 → Q1 base (≈ 8 mA base drive) |
| R2 | 10 kΩ | Q1 base → GND (keeps LED off during boot) |
| RL1, RL2 | 22 Ω ¼ W | ≈ 85 mA/LED on battery, ≈ 135 mA on USB (pulsed, 1/3 duty at 38 kHz) |
| LED1, LED2 | 940 nm 5 mm IR LEDs | One narrow (±10–20°), one wide (±25–35°) |
| U4 | VS1838B / **TSOP38238** | TSOP preferred (better AGC) |
| R3 / C4 / C5 | 100 Ω / 10 µF / 100 nF | RX supply RC filter |
| C1 | 470 µF 6.3 V+ low-ESR electrolytic | At VSYS, beside Q1/LEDs |
| C2 | 100 nF ceramic | VSYS decoupling |
| C3 | 100 µF + 100 nF | Across 3V3 rail near ESP32 |
| R4, R5 | 100 kΩ, 100 kΩ | Battery-voltage divider → **GPIO35** |
| C6 | 100 nF | ADC filter (GPIO35 → GND) |
| LED3–5 + R | 3 status LEDs + **1 kΩ** each | GPIO25/26/27. 1 kΩ (~1 mA) saves battery on efficient LEDs |
| SW1 | SPDT/slide switch | VSYS → buck-boost EN/Vin |
| SW2 (opt.) | Tactile switch to GND | BOOT button already on DevKit |
| J1 (opt.) | 4-pin header | OLED SSD1306: 3V3, GND, SDA 21, SCL 22 |

---

## 4. Netlist (wire-by-wire)

```
USB 5V (TP4056 IN+) ── D1 anode;  D1 cathode ── VSYS
BAT+ (TP4056 B+/cell) ── D2 anode; D2 cathode ── VSYS
BAT+ ── R4 100k ── [node ADC] ── R5 100k ── GND ;  [node ADC] ── GPIO35 ; C6 node→GND
GND: cell −, TP4056 −, buck-boost GND, ESP32 GND, LED cathodes/Q1 emitter, RX GND — one star

VSYS ── SW1 ── buck-boost VIN ;  buck-boost VOUT(3.3V) ── 3V3 rail
3V3 rail ── ESP32 "3V3" pin ;  C3 across 3V3 / GND

IR TX:  GPIO4 ── R1 330Ω ── Q1 base ;  R2 10k base→GND ;  Q1 emitter → GND
        VSYS ── RL1 22Ω ── LED1 anode ; LED1 cathode ── Q1 collector
        VSYS ── RL2 22Ω ── LED2 anode ; LED2 cathode ── Q1 collector
        C1 470µF + C2 across VSYS/GND, within ~1 cm of Q1

IR RX:  3V3 ── R3 100Ω ── [RXVCC] ; C4 10µF + C5 100nF [RXVCC]→GND
        U4 VCC=[RXVCC], GND=GND, OUT ── GPIO14 (firmware enables the pull-up)

Status: GPIO25 / 26 / 27 ── 1kΩ ── LED ── GND
OLED  : 3V3, GND, SDA=GPIO21, SCL=GPIO22
```

> [!CAUTION]
> **Never plug the DevKit's own USB while SW1 is ON.** The AMS1117 would fight your 3.3 V converter. Flash with **SW1 OFF** (USB powers the board), or use OTA afterwards. Alternatively, desolder the AMS1117 and its USB-power path becomes irrelevant.

---

## 5. Stripboard layout

Board: **~70 × 95 mm stripboard, 2.54 mm pitch, strips running horizontally** (≈ 27 × 37 holes).
Strips run left↔right, so **every strip crossing the ESP32 must be cut between its two header columns** (otherwise the left and right pins short). Mount the ESP32 on female headers; measure your header-row spacing (usually 9–10 holes) before cutting.

```
   ◄──────────────── ≈ 95 mm (37 holes) ────────────────►
   ┌──────────────────────────────────────────────────────┐ ▲
   │ [BT1 holder/JST]  [TP4056 USB-C ]                    │ │
   │   (edge, easy to swap)  USB port at board edge  ─►   │ │
   │                                                      │ │
   │  D1 D2  SW1   [Buck-boost]      ┌────────────────┐   │ │
   │  C1 C2         C3               │  ESP32 DevKit  │   │ │ 70 mm
   │  R4 R5 C6                       │  (female hdrs) │   │ │
   │ ─────────── power zone ──────── │ USB faces edge │   │ │
   │                                 └────────────────┘   │ │
   │  LED3 LED4 LED5 (+1k)      [OLED hdr J1]             │ │
   │                                                      │ │
   │  Q1 R1 R2  RL1 RL2         RX: U4 R3 C4 C5           │ │
   │  LED1 ═══╗ LED2            (sensor at edge, away     │ │
   │  (board edge, facing out)    from the TX LEDs)       │ ▼
   └──────────────────────────────────────────────────────┘
```

Layout rules that matter:
1. **Star ground** at the buck-boost GND pad. Keep Q1's emitter and C1 on a short, fat ground track (double up with a solder bead or bare wire). IR pulse current must not run through the RX/ESP32 ground.
2. **IR LEDs on the board edge**, pointed outward and angled apart (e.g. 30° and 0°). Put the **RX sensor on a *different* edge or behind a small card baffle**, otherwise your own LED floods it.
3. **C1 within ~1 cm of Q1**, and keep VSYS/GND tracks to the LED/Q1 loop short.
4. **Separate RX supply**: tap [RXVCC] after R3/C4/C5, not directly from the 3V3 rail.
5. Keep the USB-C of the TP4056 and the **DevKit antenna end** at opposite ends. **Nothing under or beside the ESP32 antenna** (no copper, no wires, no battery).
6. Put the **battery holder at an edge** so you can swap cells without stressing the solder joints.
7. Use ≥ 0.5 mm² wire for VSYS, GND and battery (peaks ≈ 0.7 A).

---

## 6. Battery life, honestly

| Mode | Average draw (from cell) | 2000 mAh LiPo | 3000 mAh 18650 |
|---|---|---|---|
| As shipped (`WiFi.setSleep(false)`, OLED off, LEDs off) | ≈ 110–130 mA | ~15 h | ~22 h |
| + OLED + 3 LEDs | ≈ 135–150 mA | ~13 h | ~19 h |
| **Modem sleep** (`WiFi.setSleep(true)`, DTIM 3) | ≈ 35–50 mA | ~2 days | ~3 days |

Recommended use: leave **USB plugged in permanently**. The cell makes the device a **UPS**: it keeps WiFi, MQTT and *schedules* alive across short power cuts. Only run on battery for outages.

---

## 7. Firmware changes needed (small)

| Feature | Change |
|---|---|
| **Battery %** | `analogRead(35)` (ADC1, input-only, safe with WiFi). Voltage = reading × 2 (divider). Use `analogSetPinAttenuation(35, ADC_11db)`. Average 16 samples. Report it in `/api/status`, on the OLED, and via HA MQTT discovery as a battery sensor |
| **Low-battery shutdown** | At < 3.30 V for 30 s: toast, stop IR TX, then `esp_deep_sleep_start()`. The protection IC cuts at ~2.4 V, but sag during WiFi TX can brown out the ESP32 first, so shut down gracefully and **before** that |
| **On-battery policy** | Detect "on USB" as `VBAT_raw` > 4.25 V is unreliable. Cleaner: tie USB 5 V to a spare GPIO via a divider (100 k + 220 k → GPIO34) and read it. Then switch `WiFi.setSleep(true)` and dim LEDs when USB is absent |
| **Disable status LEDs on battery** | Already supported via `cfg().ledEnabled` |

---

## 8. Smaller variant: ESP32-C3 SuperMini (optional)

Use the C3 env (`pio run -e esp32c3`). Pins from [board_pins.h](include/board_pins.h): TX **GPIO4**, RX **GPIO3**, LEDs **5/6/7**, I²C **8/10**.
Its onboard LDO has only ~0.1 V dropout, so you may feed **VSYS → "5V" pin** and **drop U2 entirely** (the C3 runs fine down to ~3.0 V). Draw is ~25–30% lower than classic ESP32. Disadvantage: below ~3.5 V the 3V3 rail tracks the cell, so the RX supply and IR timing see more variation. This is acceptable, but the buck-boost is cleaner.

---

## 9. Bring-up checklist

1. **No ESP32, no cell:** plug USB into TP4056 → confirm CHRG LED, VSYS ≈ 4.6 V.
2. Insert the cell, unplug USB → VSYS ≈ cell − 0.3 V; check D1 blocks (nothing flows back to the USB port).
3. SW1 on → measure **3.30 V** at the 3V3 rail *before* inserting the ESP32 into its headers.
4. Insert the ESP32 (SW1 off, USB connected) → flash firmware + `uploadfs`.
5. Disconnect USB, SW1 on → boots on battery. Check `/api/status` for stable WiFi.
6. Run the firmware **loopback self-test** (blaster + receiver healthy), then check the LED glows on a phone camera.
7. Scope or DMM across RL1 during a long mark: ≥ 70 mA on battery. If lower, check Q1's gain (the base current should be ≈ 8 mA).
8. **Brownout test:** from a full cell, fire 10 AC commands back to back with WiFi active, and confirm no resets (`boot: reset reason` stays `power-on`).
