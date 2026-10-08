# Porting to other M5Stick models

The firmware was developed and tested on the **M5StickC Plus** only. It talks to several chips directly
(AXP192, MPU6886, BM8563) and relies on ESP32-classic deep-sleep wake sources, so other models need changes.
This document lists what to check and change for each model.

> None of the steps below have been tested on real hardware. Treat them as a checklist, not as a verified recipe.

## Hardware-specific places in `src/main.cpp`

| What | Where | M5StickC Plus assumption |
|---|---|---|
| Pins | `PIN_INT`, `PIN_BTN_A`, `PIN_BTN_B`, `PIN_BUZZ` | GPIO35 IMU/RTC INT, GPIO37 button A, GPIO39 button B, GPIO2 buzzer |
| PMIC | `deepSleep()`, `setup()` (`AXP`, register `0x12`), `beep()` (`Axp192.setEXTEN`) | AXP192 at I2C `0x34`; LDO3 = LCD logic, LDO2 = backlight, EXTEN powers the buzzer |
| IMU | `imuInit()`, `imuTapSensitive()`, `readAccel()`, `imuMotion()` | MPU6886 at `0x68`, raw register access, wake-on-motion on GPIO35 |
| Axes | `layoutOf()`, `LAYOUT_ROTATION[]`, `DOWN_G` / `VISIBLE_G` | +z screen up, +x long edge A up, −y USB up |
| RTC | `rtcSeconds()`, `rtcInit()` | BM8563 at `0x51` |
| Deep-sleep wake | `deepSleep()` | ext0 on GPIO35 (IMU), ext1 on GPIO37 (button A) |
| Display geometry | `render()`, `drawTime()` | 135×240 panel; strip and padding values are tuned for it |

M5Unified detects the board automatically (`M5.getBoard()`), so the cleanest way to support several models
is to branch on the board type in the places above.

## Step 0: calibrate the axes on any new model

The accelerometer orientation depends on how the chip sits on the PCB, so measure it before changing any logic:

1. Flash, open `pio device monitor`, and put the device in each pose. Every time the screen turns on, the log prints a
   line like `screen on: 24:00 layout=0 ... acc=0.98,-0.01,0.05`. The pose changes make the screen turn on.
2. Note which axis reads ≈ +1 g in each pose: screen up, each long edge, each end.
3. Update `layoutOf()` (which axis selects which layout) and `LAYOUT_ROTATION[]` (which `setRotation()` value makes the
   image upright in that pose).
4. If "screen up" is not +z, change the z checks (`DOWN_G`, `VISIBLE_G`, `confirmPose()`, `runStep()`, `finishedStep()`).

## M5StickC (original)

Closest to the Plus: same ESP32-PICO-D4, AXP192, BM8563 and GPIO35/37/39 pinout.

- **Display** is an ST7735S 80×160. M5GFX handles the panel, but the layout constants in `render()` (18 px status strip,
  frame margins, `drawTime()` maximum height) were tuned for 135×240. Re-check them; a smaller status font or no
  battery percentage may be needed.
- **No buzzer.** `beep()` will be silent. Replace it with something visible, e.g. blink the red LED on GPIO10
  (active low), or rely on the blinking screen alone.
- **IMU:** most units have an MPU6886, but early units shipped with an **SH200Q**. Check `M5.Imu.getType()` or read
  `WHO_AM_I` (`0x75` on the MPU6886 returns `0x19`). The SH200Q needs a different `imuInit()` and has no equivalent
  wake-on-motion setup, so it would run in the polling fallback (`womOk = false`).
- The boot self-test (`IMU INT self-test`) tells you whether the IMU interrupt reaches GPIO35. If it fails, the firmware
  automatically polls the orientation once a second in light sleep instead of deep sleeping.
- Battery is smaller (≈95 mAh), so expect proportionally shorter runtime.

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
5. Running a short session (`pio run -e test -t upload`) ends with a beep (or the replacement signal) and a blinking
   screen.
