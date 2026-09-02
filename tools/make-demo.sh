#!/usr/bin/env bash
# Assembles a hardware-free demo of the web UI into build/demo/.
#
# It copies data/ verbatim -- the demo runs the exact CSS and JS that ship to
# the device -- and only rewrites the absolute asset paths (the ESP32 serves
# from /, GitHub Pages serves from a subdirectory) and injects the API mock.
#
# Usage: tools/make-demo.sh [output-dir]      default: build/demo
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/build/demo}"

rm -rf "$out"
mkdir -p "$out"

cp "$root/data/app.css" "$out/app.css"
cp "$root/data/app.js"  "$out/app.js"
cp "$root/tools/demo-mock.js" "$out/demo-mock.js"

sed -e 's#href="/app.css"#href="app.css"#' \
    -e 's#<script src="/app.js"></script>#<script src="demo-mock.js"></script>\n<script src="app.js"></script>#' \
    "$root/data/index.html" > "$out/index.html"

# Fail loudly rather than publishing a demo that silently 404s its own assets.
grep -q 'src="demo-mock.js"' "$out/index.html" \
  || { echo "make-demo: failed to inject the API mock" >&2; exit 1; }
grep -q 'href="app.css"' "$out/index.html" \
  || { echo "make-demo: failed to rewrite the stylesheet path" >&2; exit 1; }

echo "demo written to $out"
