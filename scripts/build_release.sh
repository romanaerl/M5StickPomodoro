#!/usr/bin/env bash
# Build the firmware and merge it into one image that flashes at offset 0x0:
# bootloader (0x1000), partition table (0x8000), boot_app0 (0xe000) and the app (0x10000).
#
#   scripts/build_release.sh [VERSION]     -> dist/M5StickPomodoro-VERSION.bin
#
# VERSION defaults to `git describe --tags --always`. Used locally and by the GitHub release workflow.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-$(git describe --tags --always --dirty)}"
ENV=m5stick-c-plus
PIO="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
PY="$(command -v python3)"; [ -x "$HOME/.platformio/penv/bin/python" ] && PY="$HOME/.platformio/penv/bin/python"
PKG="$HOME/.platformio/packages"

"$PIO" run -e "$ENV"

BUILD=".pio/build/$ENV"
OUT="dist/M5StickPomodoro-$VERSION.bin"
mkdir -p dist
"$PY" "$PKG/tool-esptoolpy/esptool.py" --chip esp32 merge_bin -o "$OUT" \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 "$BUILD/bootloader.bin" \
  0x8000 "$BUILD/partitions.bin" \
  0xe000 "$PKG/framework-arduinoespressif32/tools/partitions/boot_app0.bin" \
  0x10000 "$BUILD/firmware.bin"
echo "Built $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
