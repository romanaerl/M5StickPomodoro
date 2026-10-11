#!/usr/bin/env bash
# Build the firmware and merge each build into one image that flashes at offset 0x0:
#   ESP32 (StickC, StickC Plus, StickC Plus2): bootloader 0x1000, partitions 0x8000, boot_app0 0xe000,
#     app 0x10000                                      -> dist/M5StickPomodoro-VERSION.bin
#   ESP32-S3 (StickS3, UNTESTED): bootloader 0x0, the rest as above
#                                                      -> dist/M5StickPomodoro-VERSION-S3.bin
#
#   scripts/build_release.sh [VERSION]
#
# VERSION defaults to `git describe --tags --always`. Used locally and by the GitHub release workflow.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-$(git describe --tags --always --dirty)}"
PIO="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
PY="$(command -v python3)"; [ -x "$HOME/.platformio/penv/bin/python" ] && PY="$HOME/.platformio/penv/bin/python"
PKG="$HOME/.platformio/packages"

merge() {  # ENV CHIP FLASH_SIZE BOOTLOADER_OFFSET OUT
  local env="$1" chip="$2" size="$3" boot="$4" out="$5" build=".pio/build/$1"
  "$PIO" run -e "$env"
  "$PY" "$PKG/tool-esptoolpy/esptool.py" --chip "$chip" merge_bin -o "$out" \
    --flash_mode dio --flash_freq 40m --flash_size "$size" \
    "$boot" "$build/bootloader.bin" \
    0x8000 "$build/partitions.bin" \
    0xe000 "$PKG/framework-arduinoespressif32/tools/partitions/boot_app0.bin" \
    0x10000 "$build/firmware.bin"
  echo "Built $out ($(wc -c < "$out" | tr -d ' ') bytes)"
}

mkdir -p dist
merge m5stick-c-plus esp32   4MB 0x1000 "dist/M5StickPomodoro-$VERSION.bin"
merge m5stick-s3     esp32s3 8MB 0x0    "dist/M5StickPomodoro-$VERSION-S3.bin"
