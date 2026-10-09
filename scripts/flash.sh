#!/usr/bin/env bash
# Pick a connected M5Stick, check it, and flash it. See scripts/flash.py --help.
# Runs with PlatformIO's Python, which already has pyserial and esptool.
set -euo pipefail
PY="$HOME/.platformio/penv/bin/python"
[ -x "$PY" ] || PY="python3"
exec "$PY" "$(dirname "$0")/flash.py" "$@"
