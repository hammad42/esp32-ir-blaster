# Battery operation and deep sleep

Short version: **don't.** This firmware is built for a mains-powered device, and
the reasons are structural rather than a missing feature. Here is the honest
arithmetic and what a battery build would actually require.

---

## Why the current design is mains-only

A blaster has to be **reachable at any moment** — that is its whole job. Being
reachable over WiFi means the radio is associated, and an associated ESP32 draws
80–120 mA average with modem sleep disabled (which this firmware does on
purpose, because sleep adds hundreds of milliseconds to every HTTP request and
makes the UI feel broken).

On a 2000 mAh 18650:

| Mode | Current | Runtime |
|---|---|---|
| Associated, modem sleep off (this firmware) | ~100 mA | **~20 hours** |
| Associated, modem sleep on | ~40 mA | ~2 days |
| Light sleep, DTIM 3 | ~5 mA | ~2 weeks, ~300 ms latency |
| Deep sleep, waking every 30 min | ~0.05 mA | months, but unreachable between wakes |

Twenty hours is not a product. Two weeks with a third of a second of latency is
tolerable but fragile. Months of runtime means the device is asleep when you
press the button, which defeats the point.

The IR LED itself is not the problem — a burst is ~100 mA for maybe 100 ms, so
even a hundred commands a day is under 1 mAh. **The radio is the entire budget.**

---

## What a battery version would need to change

If you have a use case that genuinely needs it — a blaster in a room with no
socket, triggered only on a schedule — here is what to modify. None of it is
hard; it is just a different device.

### 1. Turn the always-on server off

Deep sleep and an HTTP server are mutually exclusive. A sleeping device cannot
serve `/api/status`. Pick one of:

- **Scheduled-only.** Wake on the RTC timer, fire the due command, sleep again.
  No web UI except during a configuration window.
- **MQTT-pull.** Wake every N minutes, connect, check a retained topic for a
  pending command, execute, sleep. Latency equals the wake interval.
- **Wake on a button.** Deep sleep until GPIO 0 goes low, then run normally for
  five minutes with the full web UI, then sleep again. This is the most usable
  compromise for a physically-present remote.

### 2. Keep state across sleeps

RTC slow memory survives deep sleep and is the right place for a wake counter
and a "next schedule due" timestamp:

```cpp
RTC_DATA_ATTR uint32_t wakeCount = 0;
RTC_DATA_ATTR int32_t  nextDueMinute = -1;
```

Learned commands already live in LittleFS and survive everything, so nothing has
to change there.

### 3. Cut the WiFi association cost

A full DHCP association takes 3–6 seconds and dominates the energy of a short
wake. Store a static IP, gateway, subnet, DNS, and the AP's BSSID and channel,
then:

```cpp
WiFi.config(ip, gw, mask, dns);
WiFi.begin(ssid, pass, channel, bssid);   // skips the scan
```

That takes association down to under a second — often a 4x reduction in the
energy of a wake cycle.

### 4. Feed the IR LED properly

At 3.7 V from a cell, an IR LED with V<sub>f</sub> ≈ 1.3 V needs the resistor
recalculated: `R = (3.7 − 1.3 − 0.2) / 0.1 ≈ 22 Ω`. And the cell must supply the
100 mA burst without sagging enough to brown out the ESP32 — keep the 470 µF
bulk capacitor, it matters more on battery than on USB.

### 5. Pick the right board

Most DevKits are wrong for battery use: their USB-serial bridge and power LED
draw 5–20 mA continuously, which swamps a deep-sleep current of 10 µA. Use a
board designed for it — a bare ESP32-WROOM module with your own LDO, a
FireBeetle, or a LOLIN32 — and remove the power LED.

An **ESP32-C3** is also meaningfully more efficient than a classic ESP32 for
this workload, and this firmware already builds for it.

---

## The recommendation

Run it from a **USB wall adapter**. It draws about half a watt, which is roughly
30 cents of electricity a year, and it is instantly reachable — which is what
makes it useful.

If the problem is that there is no socket where the blaster needs to be, a **5 m
USB extension** is a far better engineering answer than a battery, and it will
still be working in three years.
