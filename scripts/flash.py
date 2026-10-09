#!/usr/bin/env python3
"""Pick a connected M5Stick, check what it is, and flash the firmware onto it.

    scripts/flash.sh                          list devices, choose one, confirm, flash
    scripts/flash.sh --port PORT              use this port instead of choosing
    scripts/flash.sh --expect "M5StickC"      refuse unless the device is identified as this model
    scripts/flash.sh --port PORT --yes        no questions (for scripts)
    scripts/flash.sh --env test               flash another PlatformIO environment
    scripts/flash.sh --list                   only list the connected devices

Only the chosen device is touched. Checking it restarts it once (to read its boot log) before
flashing. The firmware is the same for every supported model: it detects the board at boot and
picks its profile from src/boards.h, which is also where this script gets the model names.

On macOS the VoiceOver braille daemon (scrod) opens new USB serial ports looking for braille
displays and blocks flashing. If it holds the chosen port it is stopped (macOS restarts it later).
Any other process holding the port is reported and left alone.
"""
import argparse
import os
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARDS_H = os.path.join(ROOT, "src", "boards.h")
ESPTOOL = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
# Chips the firmware is built for. StickC and StickC Plus both use the ESP32-PICO-D4.
SUPPORTED_CHIPS = ("ESP32-PICO-D4",)
# M5GFX board ids printed by the firmware ("board=N"), for boot logs without "profile=".
BOARD_IDS = {3: "M5StickC", 4: "M5StickC Plus", 5: "M5StickC Plus2"}


def known_models():
    """Model names from the PROFILES table in src/boards.h."""
    text = open(BOARDS_H, encoding="utf-8").read()
    return re.findall(r'\{\s*m5::board_t::\w+\s*,\s*"([^"]+)"', text)


def usb_ports():
    ports = []
    for p in list_ports.comports():
        if p.vid is None:
            continue
        if sys.platform == "darwin" and not p.device.startswith("/dev/cu."):
            continue
        ports.append(p)
    return sorted(ports, key=lambda p: p.device)


def free_port(port):
    """Stop scrod if it holds the port. Returns an error string if something else holds it."""
    try:
        pids = subprocess.run(["lsof", "-t", port], capture_output=True, text=True).stdout.split()
    except FileNotFoundError:
        return None  # no lsof (not macOS/Linux): nothing to check
    for pid in pids:
        name = os.path.basename(
            subprocess.run(["ps", "-p", pid, "-o", "comm="], capture_output=True, text=True).stdout.strip())
        if name == "scrod":
            print(f"  stopping scrod (VoiceOver braille daemon, pid {pid}) that holds the port")
            subprocess.run(["kill", pid])
            time.sleep(1)
        elif name:
            return f"in use by {name} (pid {pid}); close it and try again"
    return None


def chip_info(port):
    cmd = [sys.executable, ESPTOOL] if os.path.exists(ESPTOOL) else [sys.executable, "-m", "esptool"]
    out = subprocess.run(cmd + ["--port", port, "--baud", "115200", "flash_id"],
                         capture_output=True, text=True).stdout
    info = {}
    for key, pattern in (("chip", r"Chip is (\S+)"), ("flash", r"Detected flash size: (\S+)"),
                         ("mac", r"MAC: (\S+)")):
        m = re.search(pattern, out)
        if m:
            info[key] = m.group(1)
    if "chip" not in info:
        info["error"] = (out.strip().splitlines() or ["no answer"])[-1]
    return info


def boot_log(port, seconds=4.0):
    """Restart the device and return what it prints right after boot."""
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.dtr = s.rts = False
    s.open()
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    data, end = b"", time.time() + seconds
    while time.time() < end:
        data += s.read(512)
    s.close()
    return data.decode(errors="replace")


def identify(log, models):
    """(model, firmware) from a boot log."""
    m = re.search(r"profile=(.+?) battery=", log)
    if m:
        return m.group(1), "M5StickPomodoro"
    if "Pomodoro boot" in log:
        b = re.search(r"board=(\d+)", log)
        return (BOARD_IDS.get(int(b.group(1))) if b else None), "M5StickPomodoro (older version)"
    # Other firmware: look for a known model name in what it prints, longest name first.
    for name in sorted(models + list(BOARD_IDS.values()), key=len, reverse=True):
        if re.search(re.escape(name).replace(r"\ ", r"\s*"), log, re.IGNORECASE):
            return name, "other firmware"
    return None, "other or no firmware"


def ask(question):
    try:
        return input(question).strip()
    except EOFError:
        return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port to flash (skips the choice)")
    ap.add_argument("--expect", help="refuse unless the device is identified as this model")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    ap.add_argument("--force", action="store_true", help="flash even if the model or chip is not recognised")
    ap.add_argument("--env", default="m5stick-c-plus", help="PlatformIO environment (default: %(default)s)")
    ap.add_argument("--list", action="store_true", help="only list the connected devices")
    args = ap.parse_args()

    models = known_models()
    print("Supported models (src/boards.h): " + ", ".join(models))
    if args.expect and args.expect.lower() not in (m.lower() for m in models):
        sys.exit(f'--expect "{args.expect}" is not a known model')

    ports = usb_ports()
    if args.port:
        port = args.port
    else:
        if not ports:
            sys.exit("No USB serial device found. Is it connected and switched on? "
                     "(the port disappears while the device is off)")
        print("\nConnected devices:")
        for i, p in enumerate(ports, 1):
            print(f"  {i}. {p.device}   {p.description or ''}   serial {p.serial_number or '?'}")
        if args.list:
            return
        if args.yes and len(ports) > 1:
            sys.exit("Several devices are connected: choose one with --port")
        choice = "1" if len(ports) == 1 and args.yes else ask(f"\nWhich one to flash? [1-{len(ports)}] ")
        if not choice.isdigit() or not 1 <= int(choice) <= len(ports):
            sys.exit("Nothing chosen, nothing flashed.")
        port = ports[int(choice) - 1].device

    print(f"\nChecking {port} (it restarts once) ...")
    err = free_port(port)
    if err:
        sys.exit(f"{port} is {err}")
    info = chip_info(port)
    if "error" in info:
        sys.exit(f"Could not talk to the chip on {port}: {info['error']}")
    model, firmware = identify(boot_log(port), models)
    print(f"  chip:     {info['chip']}, {info.get('flash', '?')} flash, MAC {info.get('mac', '?')}")
    print(f"  model:    {model or 'not identified'}")
    print(f"  firmware: {firmware}")

    problems = []
    if info["chip"] not in SUPPORTED_CHIPS:
        problems.append(f"chip {info['chip']} is not supported by this firmware")
    if model and model.lower() not in (m.lower() for m in models):
        problems.append(f"{model} has no profile in src/boards.h (see PORTING.md)")
    if args.expect and (model or "").lower() != args.expect.lower():
        problems.append(f'expected "{args.expect}", found "{model or "an unidentified device"}"')
    if not model and not args.expect:
        print("  note:     the model could not be read from the device; an ESP32-PICO-D4 with 4MB is a\n"
              "            StickC or StickC Plus, and the firmware picks the right profile at boot.")
    if problems:
        for p in problems:
            print(f"  PROBLEM:  {p}")
        if not args.force:
            sys.exit("Not flashed (use --force to flash anyway).")

    if not args.yes and ask(f"\nFlash M5StickPomodoro ({args.env}) onto {port}? [y/N] ").lower() not in ("y", "yes"):
        sys.exit("Not flashed.")
    err = free_port(port)  # scrod may have grabbed it again
    if err:
        sys.exit(f"{port} is {err}")
    pio = os.path.expanduser("~/.platformio/penv/bin/pio")
    if not os.path.exists(pio):
        pio = "pio"
    sys.exit(subprocess.run([pio, "run", "-e", args.env, "-t", "upload", "--upload-port", port], cwd=ROOT).returncode)


if __name__ == "__main__":
    main()
