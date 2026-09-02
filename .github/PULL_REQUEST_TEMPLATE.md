## What this changes

<!-- One or two sentences. Link the issue if there is one. -->

## Why

<!-- The problem it solves. If it is a behaviour change, say what it was before. -->

## How it was tested

<!-- Be specific. "Builds" and "ran for a week driving a Daikin AC" are very
     different claims, and both are fine — just say which one this is. -->

- [ ] All four targets build: `pio run -e esp32dev -e esp32dev_oled -e esp32s3 -e esp32c3`
- [ ] `node --check data/app.js` passes (if the web UI changed)
- [ ] Tested on hardware — board and appliance:
- [ ] Documentation in `docs/` updated (if behaviour changed)
- [ ] `CHANGELOG.md` updated under Unreleased

## Notes for the reviewer

<!-- Anything you are unsure about, or deliberately left out of scope. -->
