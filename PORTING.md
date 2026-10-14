# Porting to other M5Stick models

## Supported models

| Model | Status |
|---|---|
| **M5StickC** (original) | Supported and tested; see [its notes](#m5stickc-original). |
| **M5StickC Plus** | Main target; developed and tested on it. |
| **M5StickC Plus2** | **Untested.** Profile and code prepared from the docs; see [its notes](#m5stickc-plus2). |
| **M5StickS3** | Tested on a device; its own build (`m5stick-s3`); see [its notes](#m5sticks3). |

Untested profiles have `tested = false` in `src/boards.h`. The boot log then prints
`Profile ... is UNTESTED on a real device`, and `scripts/flash.sh` warns before flashing.

One firmware runs on all of them. At boot M5Unified detects the board and the firmware picks the matching **board
profile** from [`src/boards.h`](src/boards.h). The boot log shows it, e.g. `profile=M5StickC`.

## Adding a board profile (similar models)

A model that has the same chips and pinout as the M5StickC (ESP32, AXP192, MPU6886, BM8563, buttons on GPIO37/39, IMU
interrupt on GPIO35) only needs a new row in `PROFILES` in [`src/boards.h`](src/boards.h):

| Field | Meaning |
|---|---|
| `board` | the M5Unified board id (`m5::board_t::...`) that `M5.getBoard()` returns for it |
| `name` | short display name; used in the boot log and by `scripts/flash.sh` (`--expect`) |
| `compact` | `true` for a small 80×160 panel: compact status line, thinner digit halo |
| `buzzerPin` | passive buzzer GPIO, or `-1` if there is none: the red LED then signals instead, and the power button's short press switches the LED |
| `ledPin` | red LED GPIO (active low) |
| `rotation` | `setRotation()` value for each layout: WIDE, WIDE_FLIP, TALL, TALL_FLIP, FLAT |
| `axis` / `sign` | which raw accelerometer axis, with which sign, gives this firmware's x, y, z |

Steps:

1. Add the row, build, flash with `scripts/flash.sh`, and check the boot log shows `profile=<your name>`.
2. Calibrate the axes (step 0 below). Every pose must give the expected axis with +1 g.
3. Check that every pose shows an upright image. Fix `rotation` if one does not.
4. Check the status line fits the panel, and that the minute glance is visible. On the Plus the first visible
   backlight level is 7; adjust `GLANCE_LEVELS` in `main.cpp` if your panel differs.
5. Run a short session (`scripts/flash.sh --env test`) to the end: the signal (beep or LED) and the blinking screen.

Models with a different power chip, IMU or pinout need code changes as well; the sections below list them.

> The Plus2 section has not been tested on real hardware. Treat them as a checklist, not as a verified recipe.

## Hardware-specific places in `src/main.cpp`

| What | Where | M5StickC Plus assumption |
|---|---|---|
| Pins | `PIN_INT`, `PIN_BTN_A`, `PIN_BTN_B`; `buzzerPin` / `ledPin` in the profile | GPIO35 IMU/RTC INT, GPIO37 button A, GPIO39 button B, GPIO2 buzzer, GPIO10 LED |
| PMIC | `deepSleep()`, `setup()` (`AXP`, register `0x12`), `beep()` (`Axp192.setEXTEN`), `backlightLevel()` (LDO2), `powerKeyPressed()` (register `0x46`) | AXP192 at I2C `0x34`; LDO3 = LCD logic, LDO2 = backlight, EXTEN powers the buzzer |
| IMU | `imuInit()`, `imuTapSensitive()`, `readAccel()`, `imuMotion()` | MPU6886 at `0x68`, raw register access, wake-on-motion on GPIO35 |
| Axes | `axis` / `sign` / `rotation` in the profile; `layoutOf()`, `DOWN_G` / `VISIBLE_G` | +z screen up, +x long edge A up, −y USB up |
| RTC | `rtcSeconds()`, `rtcInit()` | BM8563 at `0x51` |
| Deep-sleep wake | `deepSleep()` | ext0 on GPIO35 (IMU), ext1 on GPIO37 (button A) |
| Display geometry | `render()`, `drawTime()`; `compact` in the profile | 135×240 panel (80×160 with `compact`) |

## Step 0: calibrate the axes on any new model

The accelerometer orientation depends on how the chip sits on the PCB, so measure it before changing any logic:

1. Flash, open `pio device monitor`, and put the device in each pose. Every time the screen turns on, the log prints a
   line like `screen on: 24:00 layout=0 ... acc=0.98,-0.01,0.05`. The pose changes make the screen turn on.
2. Note which axis reads ≈ +1 g in each pose: screen up, each long edge, each end.
3. Set `axis` / `sign` in the board profile so that the firmware's axes match the convention (+z screen up, +x long
   edge A up, −y USB up), and `rotation` so that each pose shows an upright image.
4. If "screen up" is not +z, change the z checks (`DOWN_G`, `VISIBLE_G`, `confirmPose()`, `runStep()`, `finishedStep()`).

## M5StickC (original)

**Supported and tested.** Same ESP32-PICO-D4, AXP192, MPU6886, BM8563 and GPIO35/37/39 pinout as the Plus, and the same
accelerometer axes and display rotations (measured on the device). Its profile differs in:

- **Display:** ST7735S 80×160 (`compact`). The status line uses a smaller font. In portrait the 80 px line shows only
  the mode and the battery: the option flags (`NO LED`, `TAP`) are left out, and the longest labels are shortened
  (`DONE`, `SET W`, `SET B`). Digit sizes follow the panel size.
- **No buzzer** (`buzzerPin = -1`). Signals use the same patterns on the red LED (GPIO10). The LED also blinks with the
  screen after a session ends. The power button's short press switches the LED (`LED ON` / `LED OFF`, flag `NO LED`).
- **Battery** is smaller (≈95 mAh), so expect proportionally shorter runtime.
- **Early units** shipped with an **SH200Q** IMU instead of the MPU6886. The boot log prints `IMU WHO_AM_I`
  (`0x19` = MPU6886). The SH200Q is not supported: it needs a different `imuInit()` and has no equivalent
  wake-on-motion.

## M5StickC Plus2

**Prepared, untested.** It runs the same ESP32 build as the StickC and StickC Plus; its profile in
`src/boards.h` selects the differences. Sources: the M5Stack docs and the M5Unified board code.

| | M5StickC Plus2 | How the firmware handles it |
|---|---|---|
| Chip | ESP32-PICO-V3-02, 8 MB flash | Same build; `scripts/flash.sh` knows the chip |
| Power | No AXP192. GPIO4 (HOLD) must stay HIGH | `Power::HOLD_GPIO4`: M5Unified drives it; it is latched with `gpio_hold_en` in deep sleep |
| Backlight | PWM on GPIO27 | `M5.Display.setBrightness()`; the minute glance uses `level × 8` (a guess) |
| Buzzer | GPIO2, no 5 V boost needed | `Sound::BUZZER` |
| Power button (C) | GPIO35 | Polled: short press = sound, 1.5 s = TAP, like the AXP192 boards |
| LED | GPIO19 (with the IR emitter), active high | Not used: it has a buzzer |
| IMU | MPU6886; interrupt line not documented | Polled once a second: no deep sleep while paused, no tap-to-wake |
| RTC | BM8563 | As on the Plus |

**Check first on a real device:**

1. The boot log shows `profile=M5StickC Plus2` and stays on. If it switches itself off, the GPIO4 hold is wrong.
2. Calibrate the axes (step 0). The profile assumes the same as the Plus.
3. Face down pauses and face up resumes (polled, within ~1 s).
4. A finished session deep-sleeps and **wakes on the M5 button without powering off**. This tests the GPIO4 latch.
5. The minute glance is visible but soft; tune the PWM levels in `backlightLevel()`.
6. If the MPU6886 interrupt turns out to be wired (to a pin such as GPIO35 shared with button C, or another RTC
   GPIO), set `intPin` to get deep sleep while paused and tap-to-wake.

## M5StickS3

**Tested** on a device (2026-10-14). ESP32-S3, so it has its own build of the same code (PlatformIO
env `m5stick-s3`; `scripts/flash.sh` picks it from the chip, and the web installer from the chip family).

| | M5StickS3 | How the firmware handles it |
|---|---|---|
| Chip | ESP32-S3-PICO-1-N8R8, 8 MB flash, native USB | Env `m5stick-s3` (USB CDC on boot). The log's `flush()` never returns without a computer reading it, so it is skipped then (it froze the device on battery). |
| Power | M5PM1 PMIC (I2C 0x6E); its button powers on / resets, double press powers off, hold = flashing mode | `Power::M5PM1`: the firmware never sees that button; the M5 button holds (2 s sound, 5 s tap) replace it |
| USB | Inside the chip: stops in light and deep sleep | No sleep while the PMIC measures VBUS, so the port always answers |
| Backlight | PWM on GPIO38: dark during light sleep | No light sleep while the backlight is lit (it flickered) |
| Sound | ES8311 codec + speaker; amplifier on PMIC G3 | `Sound::SPEAKER`, started for each beep; G3 is set and read back with retries (single writes were sometimes missed: a click, no tone). Volume 90/255, beeps at least 140 ms. |
| Buttons | GPIO11 (front), GPIO12 (side) | From the profile |
| IMU | BMI270 at 0x68; INT1 to PMIC G4; the PMIC's IRQ output G1 goes to GPIO13 | Read directly (works before `M5.begin()` after a wake). Any-motion feature on INT1 (80 mg; taps 10 mg), PMIC IRQ on G4 only; GPIO13 is the wake-on-motion line, so deep sleep and taps work as on the Plus. Self-test: data-ready through the same path. |
| RTC | None found | `Clock::SYSTEM`: ESP32 time, less accurate across sleep |
| Internal I2C | SDA 47 / SCL 48 | From the profile |

## Verifying a port

1. The boot log shows the expected board and `IMU INT self-test: asserted=1 released=1` (or a working fallback).
2. Every pose shows the correct layout.
3. Face down pauses; turning it back resumes from the same time.
4. On battery (USB unplugged), the device survives deep sleep and wakes up. This is critical on the Plus2 because of the
   GPIO4 power hold.
5. Running a short session (`scripts/flash.sh --env test`) ends with a beep (or the replacement signal) and a blinking
   screen.
