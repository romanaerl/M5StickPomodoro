# Porting to other M5Stick models

## Supported models

| Model | Status |
|---|---|
| **M5StickC Plus** | Main target; developed and tested on it. |
| **M5StickC** (original) | Supported and tested; see [its notes](#m5stickc-original). |

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

> The Plus2 and StickS3 sections have not been tested on real hardware. Treat them as a checklist, not as a verified recipe.

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

- **Display:** ST7735S 80×160 (`compact`). The status line uses a smaller font, and in portrait it takes two lines:
  the mode on the first, battery and flags on the second. Digit sizes follow the panel size.
- **No buzzer** (`buzzerPin = -1`). Signals use the same patterns on the red LED (GPIO10). The LED also blinks with the
  screen after a session ends. The power button's short press switches the LED (`LED ON` / `LED OFF`, flag `NO LED`).
- **Battery** is smaller (≈95 mAh), so expect proportionally shorter runtime.
- **Early units** shipped with an **SH200Q** IMU instead of the MPU6886. The boot log prints `IMU WHO_AM_I`
  (`0x19` = MPU6886). The SH200Q is not supported: it needs a different `imuInit()` and has no equivalent
  wake-on-motion.

## M5StickC Plus2

Different power architecture. It needs real changes.

- **No AXP192.** Power is held on by **GPIO4**, which must stay HIGH while running on battery. M5Unified drives it HIGH
  in `M5.begin()`, but in deep sleep the pin must be latched or the device powers off:
  ```cpp
  gpio_hold_en(GPIO_NUM_4);
  gpio_deep_sleep_hold_en();
  ```
  Also drive GPIO4 HIGH at the very start of `setup()`, before the pre-`M5.begin()` fast path.
- **Remove or guard every AXP192 access:** the LDO3 `bitOn` / `bitOff` on `0x34`, `M5.Power.Axp192.setEXTEN()`
  and the `cfg.output_power` assumption. On the Plus2 they target a chip that does not exist.
- **Backlight** is PWM on GPIO27; `M5.Display.setBrightness()` already handles it. There is no LCD logic-supply switch,
  so in deep sleep just call `M5.Display.sleep()`.
- **Buzzer** is on GPIO2 and does not need EXTEN; `beep()` works without the `setEXTEN` lines.
- **Sound toggle:** `powerKeyPressed()` reads the AXP192 power-key register. On the Plus2 the power button is GPIO35
  (button C), so read that pin instead.
- **GPIO35 is the power button (button C)** on the Plus2, not an IMU interrupt. Check whether the MPU6886 INT is routed to
  any ESP32 pin. Without it the firmware falls back to orientation polling: light sleep with 1 s wake-ups while paused.
  That is reliable but uses noticeably more power than deep sleep with wake-on-motion. Deep sleep in `FINISHED` would
  then wake on a button only.
- **Battery level** comes from the ADC on GPIO38; `M5.Power.getBatteryLevel()` handles it.
- Flash is 8 MB, so use a Plus2 board definition or set the flash size in `platformio.ini`.
- Recalibrate the axes (step 0).

## M5StickS3 and other ESP32-S3 models

These use an ESP32-S3, a different PMIC (M5PM1) and different internal I2C pins (SDA 47 / SCL 48 in M5Unified). Expect
to rewrite the hardware layer:

- Replace the raw AXP192 / MPU6886 / BM8563 register code with M5Unified APIs where possible: `M5.Imu`, `M5.Rtc`,
  `M5.Power`, `M5.BtnA` / `M5.BtnB`. Keep raw access only for features M5Unified does not expose, such as IMU
  wake-on-motion.
- Check which IMU M5Unified detects and whether its interrupt pin is wired to an RTC-capable GPIO for deep-sleep wake.
- ESP32-S3 deep-sleep wake works differently from the ESP32 classic: ext1 supports "any low", so both buttons can wake
  it. Adjust `deepSleep()` accordingly.
- Set `board` and `platform` in `platformio.ini` for the S3 target.

## Verifying a port

1. The boot log shows the expected board and `IMU INT self-test: asserted=1 released=1` (or a working fallback).
2. Every pose shows the correct layout.
3. Face down pauses; turning it back resumes from the same time.
4. On battery (USB unplugged), the device survives deep sleep and wakes up. This is critical on the Plus2 because of the
   GPIO4 power hold.
5. Running a short session (`scripts/flash.sh --env test`) ends with a beep (or the replacement signal) and a blinking
   screen.
