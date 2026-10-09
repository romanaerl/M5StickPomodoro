#!/usr/bin/env bash
# Build the web installer site (GitHub Pages) into dist/web:
#   index.html (web/index.html), manifest.json for ESP Web Tools, and the firmware parts.
#
#   scripts/build_web.sh [VERSION]     VERSION defaults to `git describe --tags --always`
#
# The parts are written at their own offsets (bootloader 0x1000, partition table 0x8000,
# boot_app0 0xe000, app 0x10000), so the settings partition (NVS, 0x9000) is left alone and
# saved settings survive an update.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-$(git describe --tags --always --dirty)}"
ENV=m5stick-c-plus
PIO="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
PKG="$HOME/.platformio/packages"

"$PIO" run -e "$ENV"

BUILD=".pio/build/$ENV"
OUT=dist/web
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$BUILD/bootloader.bin" "$BUILD/partitions.bin" "$BUILD/firmware.bin" "$OUT/"
cp "$PKG/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$OUT/"
sed "s/__VERSION__/$VERSION/g" web/index.html > "$OUT/index.html"
cat > "$OUT/manifest.json" <<JSON
{
  "name": "M5StickPomodoro",
  "version": "$VERSION",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "bootloader.bin", "offset": 4096 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
JSON
echo "Built $OUT for $VERSION:"; ls -l "$OUT"
