# Contributing

Thanks for taking an interest. This is a small project with a clear scope, so
the most useful contributions are usually **protocol reports from real
hardware** — the one thing that cannot be tested in CI.

---

## Reporting that a remote works (or doesn't)

This is genuinely the highest-value contribution. Open an issue with:

- the appliance make and model,
- what the Learn tab reported (protocol, bit count, timing count, parts),
- whether the replay worked, and what you had to change if it did not
  (raw replay, repeats, carrier frequency, mark-excess).

Failures are as useful as successes. A remote that captures but does not
replay tells us something a working one does not.

---

## Building

```bash
pip install platformio
pio run -e esp32dev            # firmware
pio run -e esp32dev -t buildfs # web UI image
```

Before opening a pull request, confirm **all four** targets still build — they
exercise different chips and the OLED code path:

```bash
pio run -e esp32dev -e esp32dev_oled -e esp32s3 -e esp32c3
```

CI does this too, but finding it locally is faster than finding it in review.

### Working on the web UI

There is no build step and there will not be one; the file that ships is the
file that runs. To iterate without hardware:

```bash
tools/make-demo.sh
# then serve build/demo over HTTP -- opening index.html directly works too,
# but a file:// origin blocks some browser features
python3 -m http.server -d build/demo 8000
```

`tools/demo-mock.js` replaces `window.fetch` with canned responses. If you add
an endpoint, add it to the mock as well, or the demo silently stops matching
the firmware.

Check that what you wrote parses before pushing:

```bash
node --check data/app.js
```

---

## Code style

C++ follows Google style at 80 columns, which is what the Arduino core and
IRremoteESP8266 use. A `.clang-format` is included:

```bash
clang-format -i src/*.cpp src/*.h include/*.h
```

Beyond formatting, a few conventions the existing code holds to:

- **No dynamic allocation in steady state.** Buffers are sized at compile time.
  A device that runs for years should not be able to fragment its heap.
- **No blocking longer than a watchdog period.** If something must block —
  a long IR burst, an OTA write, a WiFi scan — feed the watchdog inside it.
- **Errors are returned, not printed.** Functions that can fail return a
  `const char*` message (`nullptr` on success) so the caller can surface it in
  the UI rather than only in a serial log nobody is reading.
- **Comments explain why, not what.** The existing comments are a reasonable
  guide to the expected level.

---

## Dependencies are pinned exactly

`platformio.ini` pins the platform *and* every library to an exact version.
This is deliberate: the point of the project is a device that still rebuilds
identically in three years.

To bump one:

1. Change the version in `platformio.ini`.
2. Rebuild all four targets and note any new warnings.
3. **Re-test on hardware** — especially for IRremoteESP8266, where capture and
   send behaviour is the whole dependency.
4. Note the change in `CHANGELOG.md`, including what you verified it against.

A dependency bump with no hardware testing behind it will be asked for one.

---

## Scope

Things that fit:

- protocol and appliance compatibility fixes,
- reliability under long uptime,
- integrations that ride on what is already there (REST, MQTT),
- documentation, especially wiring photos and troubleshooting notes.

Things that probably do not:

- a second web framework or a JavaScript build step,
- cloud services or accounts,
- features that require the device to be reachable from the public internet.

If you are unsure, open an issue before writing the code. It is much less
annoying to discuss a paragraph than to decline a pull request.

---

## Pull requests

- One logical change per PR.
- Say what you tested it on. "Builds" and "works on my ESP32 with a Daikin
  remote" are very different claims, and both are welcome as long as which one
  you are making is clear.
- If it changes behaviour, update the affected file in `docs/`.

By contributing you agree your work is licensed under the project's MIT
licence.
