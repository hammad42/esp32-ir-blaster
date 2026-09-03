# Using the IR Blaster

---

## First boot

Flash both images — the firmware and the web UI are separate uploads:

```bash
pio run -e esp32dev -t upload
pio run -e esp32dev -t uploadfs
```

With no WiFi credentials stored, the device starts an access point:

| | |
|---|---|
| SSID | `IR-Blaster-XXXXXX` (last 6 hex digits of its MAC) |
| Password | `irblaster` |
| Address | <http://192.168.4.1> |

Most phones open the setup page by themselves — the device answers every DNS
query with its own address and replies to the standard captive-portal probes.
If yours does not, browse to the address above.

Go to **Settings → WiFi**, press **Scan networks**, pick yours from the SSID
box, type the password, and **Save settings**. The device switches to your
network within a few seconds.

Afterwards reach it at:

- `http://ir-blaster.local` (mDNS — works on macOS, iOS, Windows 10+, and Linux
  with Avahi)
- or the IP address, which is printed on the serial monitor and shown in the
  header of the web UI

Give it a **fixed DHCP reservation** in your router. Nothing breaks without one,
but bookmarks and MQTT integrations are happier.

---

## Checking the hardware before you start

**Learn tab → Hardware self-test → Run self-test.**

The device transmits a known signal and listens for it coming back, so it
verifies the receiver, the transmitter and the wiring in one go — with no
remote and no appliance involved. Point the IR LED at the receiver, roughly
10–30 cm apart and facing each other, or bounce both off a nearby wall.

It tests in three stages, so a failure tells you *which half* is broken instead
of just "it didn't work":

| Result | Meaning |
|---|---|
| **PASS** | Both halves work. Go and learn a command. |
| *"receiver output is not idling high"* | The transmitter was never even tried. The receiver has no power or its `OUT` wire is not reaching the pin. Check VCC is on **3V3** and GND is shared. |
| *"receiver is alive but heard nothing"* | The receiver is fine, so the LED is not emitting. Check the transistor pinout (emitter to GND, collector to the LED), the LED polarity, and look at the LED through a phone camera while the test runs — you should see it flicker. |
| *"signal received but corrupted"* | Both halves work; this is an optics problem. Too close saturates the sensor just as surely as too far fades it. Try 10–30 cm. |

Worth running once when you first build the circuit, and again any time
something stops working — it takes two seconds and rules out half the possible
causes.

> The self-test is the only time the device listens while transmitting. In
> normal use the receiver is switched off during a send, so a blaster never
> captures its own output.

## Learning a command

1. Open the **Learn** tab.
2. Press **Start listening**. The headline counts down and the WiFi LED
   double-blinks.
3. Hold the remote 10–30 cm from the receiver, pointing at it, and press the
   button **once**.
4. The card turns green and reports what it heard — protocol, bit count, number
   of timings, number of parts.
5. Press **Test send** to check the appliance actually responds *before* you
   commit to a name.
6. Type a **Name** and a **Group**, then **Save command**.

Names must be unique. Groups are free text with autocomplete — use them as
rooms or devices (`Living Room AC`, `Bedroom TV`), because they become the
filter chips on the Remotes tab and show up in Home Assistant.

### Naming that stays usable at fifty commands

```
AC_Power           AC_Temp_24         AC_Mode_Cool      AC_Fan_High
TV_Power           TV_Volume_Up       TV_Input_HDMI1    TV_Mute
```

Prefix by device, suffix by function. It sorts correctly and reads well in
Home Assistant and MQTT.

### Air conditioners

A/C remotes do not send "temperature up". They send the **entire state** — mode,
temperature, fan speed, swing, timer — in one long burst, every time. So learn
each *state you actually use* as its own command:

```
AC_Off
AC_Cool_24_Auto
AC_Cool_26_Low
AC_Heat_22_Auto
```

Set the remote to exactly the state you want, press its power or send button
while the device is listening, and save. Four or five commands usually cover a
whole season.

These captures are large — 300 to 600 timings, sometimes across two or three
frames — and the firmware handles them as a unit. A capture reporting `UNKNOWN`
with 400+ timings is a *normal, working* A/C capture, not a failure. It replays
from raw timings, which is exactly right.

### Remotes that need several presses

Some remotes split a command across bursts separated by more than the 90 ms
capture window. After a successful capture, press **Capture another part** and
send the next burst. Up to 4 parts are stored as one command and replayed in
order with the configured gap between them.

---

## Sending

The **Remotes** tab lists everything, grouped. Tap a card to send it. Search
filters by name or group; the chips filter by group.

The gear icon opens per-command options:

| Option | What it does |
|---|---|
| Name / Group | rename and regroup |
| Repeats | send the signal N times (1–20) — useful for stubborn receivers |
| Carrier | 30–60 kHz, default 38 |
| Always replay raw timings | skip protocol regeneration and replay the capture verbatim |

**When to turn on "always replay raw":** if a command was recognised as a known
protocol but the appliance ignores it. The regenerated waveform is normally
cleaner, but the decoder occasionally mis-identifies a variant, and raw replay
is the ground truth.

---

## Schedules

**Schedules** tab: pick a command, a time, and the days of the week.

Schedules need a real clock, so set your **time zone** on the Settings tab
first, as a POSIX TZ string:

| Region | String |
|---|---|
| Pakistan | `PKT-5` |
| India | `IST-5:30` |
| UAE | `GST-4` |
| UK | `GMT0BST,M3.5.0/1,M10.5.0` |
| Central Europe | `CET-1CEST,M3.5.0,M10.5.0/3` |
| US Eastern | `EST5EDT,M3.2.0,M11.1.0` |
| UTC | `UTC0` |

The sign is inverted compared to what you might expect: UTC+5 is written `-5`.
That is the POSIX convention, not a typo.

The Schedules tab shows the device's current clock. If it reads blank, NTP has
not succeeded and **nothing will fire** — the firmware refuses to run schedules
on an unset clock rather than firing a burst of missed commands at boot.

---

## Backup and restore

**System → Backup → Download backup** saves `ir-backup.json`: every command with
its full timing data, human-readable.

**Restore backup** posts the entries back one at a time with a progress bar.
Existing commands are kept; anything whose name already exists is skipped and
listed in the log below the bar.

Take a backup before firmware updates and before experimenting. It is also how
you clone a second device, or move commands between rooms.

---

## Firmware updates

**Over the air, from the web UI** — System → Firmware update. Upload
`.pio/build/esp32dev/firmware.bin` for firmware, `littlefs.bin` for the web UI.
The device reboots when it finishes.

**Over the air, from PlatformIO:**

```bash
pio run -e esp32dev_ota -t upload
pio run -e esp32dev_ota -t uploadfs
```

**Over USB** — the same commands with `-e esp32dev`.

The partition table keeps two firmware slots, so an image that fails to boot
rolls back to the previous one automatically. Your learned commands live in a
separate partition and survive firmware updates; a *filesystem* update replaces
the web UI **and erases stored commands**, so take a backup first.

---

## Security

Off by default, since most people run this on a trusted LAN. Settings →
Security turns on a username and password for the web UI, the REST API and OTA.

This is HTTP basic auth over plain HTTP: it keeps housemates and casual devices
out. It is **not** a reason to expose this device to the internet. If you want
access from outside, put it behind a VPN or a reverse proxy that terminates TLS.

---

## If something does not work

### Learning times out every time

- Wrong pin, or receiver output not connected. Turn on **Monitor incoming
  signals** on the Learn tab and press any remote — if nothing appears, it is
  wiring, not software.
- Receiver powered from 5 V into a 3.3 V GPIO. See
  [HARDWARE.md §3](HARDWARE.md#3-ir-receiver-wiring).
- Remote's batteries are flat. Point a phone camera at its LED and press a
  button — you should see a faint flicker.
- Sunlight or a CFL lamp on the sensor.

### It learns, but the appliance ignores it

Work through these in order:

1. **Is the LED transmitting?** Point a phone camera at it while sending.
2. **Aim and distance.** Try 1 m, straight on.
3. **Raise repeats to 3.** Some receivers want a signal held.
4. **Turn on "always replay raw"** in the command's options.
5. **Try 36 or 40 kHz** carrier — a few brands are not on 38.
6. **Re-learn closer**, 10 cm, with fresh remote batteries. A weak capture makes
   a weak replay.
7. **Mark-excess**, Settings → IR defaults. Set 50, then 100 µs. Receivers
   report marks slightly long and spaces slightly short; this removes the bias
   at send time. Leave it at 0 unless you get here.

### The device drops off WiFi

Normal behaviour: it retries every 30 s, and after 5 minutes down it *also*
raises the setup AP so you can walk up and fix it — while still retrying, so it
recovers on its own when the router comes back. The System tab shows the last
reset reason; the log warns after any abnormal restart.

Random reboots while sending are almost always the power supply. Fit the 470 µF
capacitor and use a better cable.

### The web page is blank or 404s

The filesystem image was not uploaded. Run `pio run -t uploadfs`. The recovery
page the device serves in this state can also take `littlefs.bin` over the air.

### Forgot the password, or want to start over

Hold the BOOT button for 5–10 seconds and release. The LEDs flash six times and
the device restarts into setup mode with everything erased.
