#!/usr/bin/env bash
# Build the web installer site (GitHub Pages) into dist/web: the installer page with the README below
# it, porting.html, the diagrams, manifest.json for ESP Web Tools, and the firmware parts.
# The docs are rendered by scripts/render_docs.py, which needs markdown-it-py and
# mdit-py-plugins (set DOCS_PYTHON to a Python that has them, if `python3` does not).
#
#   scripts/build_web.sh [VERSION]     VERSION defaults to `git describe --tags --always`
#
# The parts are written at their own offsets (bootloader 0x1000 on the ESP32 / 0x0 on the ESP32-S3,
# partition table 0x8000, boot_app0 0xe000, app 0x10000), so the settings partition (NVS, 0x9000) is
# left alone and saved settings survive an update. ESP Web Tools picks the ESP32 or ESP32-S3 build
# from the connected chip (the S3 build is for the StickS3 and is UNTESTED).
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-$(git describe --tags --always --dirty)}"
ENV=m5stick-c-plus
PIO="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
PKG="$HOME/.platformio/packages"

"$PIO" run -e "$ENV"
"$PIO" run -e m5stick-s3  # StickS3 (ESP32-S3, UNTESTED): ESP Web Tools picks the build by chip

BUILD=".pio/build/$ENV"
OUT=dist/web
rm -rf "$OUT"
mkdir -p "$OUT" "$OUT/s3"
cp "$BUILD/bootloader.bin" "$BUILD/partitions.bin" "$BUILD/firmware.bin" "$OUT/"
cp "$PKG/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$OUT/"
cp .pio/build/m5stick-s3/bootloader.bin .pio/build/m5stick-s3/partitions.bin .pio/build/m5stick-s3/firmware.bin "$OUT/s3/"
cp "$PKG/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$OUT/s3/"
# The pages: web/index.html with README.md below the install box, and PORTING.md.
"${DOCS_PYTHON:-python3}" scripts/render_docs.py "$OUT" "$VERSION"
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
    },
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "s3/bootloader.bin", "offset": 0 },
        { "path": "s3/partitions.bin", "offset": 32768 },
        { "path": "s3/boot_app0.bin", "offset": 57344 },
        { "path": "s3/firmware.bin", "offset": 65536 }
      ]
    }
  ]
}
JSON
echo "Built $OUT for $VERSION:"; ls -l "$OUT"
