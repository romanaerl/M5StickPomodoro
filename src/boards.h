// Board profiles: everything that differs between the supported M5Stick models.
//
// The firmware is the same for every ESP32 model (StickC, StickC Plus, StickC Plus2); the StickS3
// (ESP32-S3) gets its own build of the same code (PlatformIO env m5stick-s3). At boot M5Unified detects
// the board (M5.getBoard()) and main.cpp picks the PROFILES row with the same `board`; an unknown
// board falls back to the first row built for this chip and logs a warning. The row is kept in RTC
// memory, so wakes from deep sleep use it before M5.begin() runs.
//
// Fingerprint: at the first boot the firmware checks that the board really has what its row says
// (power chip, panel size, IMU, RTC, audio codec). If the pieces do not fit any row, it refuses to run
// and says why (screen, five beeps where the sound is known, log line "UNSUPPORTED: ..."). This catches
// look-alikes such as the M5StickC Plus SE, which M5Unified reports as a Plus but which has no IMU.
//
// `tested` marks rows verified on a real device. Untested rows were prepared from the vendor docs and
// the M5Unified sources only; the boot log prints "UNTESTED" for them. See PORTING.md before trusting one.
//
// Adding a similar model:
//   1. Add a row below with its m5::board_t id and a short display name. scripts/flash.py reads the
//      names from this file, so keep them in double quotes on one line.
//   2. Measure the accelerometer axes: flash, watch `pio device monitor` and put the device in each
//      pose. Every screen-on log line prints `acc=x,y,z`. Fill `axis` / `sign` so that our x, y, z
//      read +1 g for: z screen up, x long edge A up (power button down), -y standing with USB up.
//   3. Check every pose shows an upright image; fix `rotation` (M5GFX setRotation() values) if not.
//   4. Set `compact` for small (80x160) panels, and the hardware fields below.
#pragma once
#include <M5Unified.h>

enum class Power : uint8_t {
  AXP192,      // AXP192 PMIC: LDO2 backlight, LDO3 LCD logic, EXTEN 5 V boost, power key in register 0x46
  HOLD_GPIO4,  // no PMIC: GPIO4 HIGH keeps the device on (latched in deep sleep), PWM backlight
  M5PM1,       // M5PM1 PMIC (StickS3): power button handled by the PMIC itself, PWM backlight
};
enum class Imu : uint8_t {
  MPU6886,     // raw registers: low-power cycle mode, wake-on-motion, tap detection
  M5UNIFIED,   // polled through M5.Imu (e.g. BMI270): no wake-on-motion, no tap detection
};
enum class Sound : uint8_t {
  BUZZER_EXTEN,  // passive buzzer powered from the AXP192 5 V boost (EXTEN)
  BUZZER,        // passive buzzer, always powered
  SPEAKER,       // codec + speaker through M5.Speaker
  LED,           // no sound: the red LED signals instead
};
enum class Clock : uint8_t {
  BM8563,      // crystal RTC on the internal I2C bus
  SYSTEM,      // ESP32 system time (RC sleep clock: less accurate across sleep)
};

struct BoardProfile {
  m5::board_t board;    // M5Unified board id
  const char* name;     // shown in the boot log ("profile=...") and by scripts/flash.py
  bool    tested;       // verified on a real device
  bool    s3;           // built for the ESP32-S3 (env m5stick-s3) rather than the ESP32
  bool    compact;      // small 80x160 panel: compact status line, thinner digit halo
  Power   power;
  Imu     imu;
  Sound   sound;
  Clock   clock;
  int8_t  buzzerPin;    // passive buzzer GPIO (Sound::BUZZER*), -1 if none
  int8_t  ledPin;       // red LED GPIO, -1 if none
  bool    ledActiveLow;
  int8_t  btnA, btnB;   // front and side button GPIOs, active low
  int8_t  intPin;       // IMU interrupt GPIO (light/deep-sleep wake), -1 if not wired to the ESP32
  int8_t  keyPin;       // power key on a GPIO (active low), -1: in the PMIC (AXP192 register) or none
  int8_t  sda, scl;     // internal I2C bus (IMU, RTC, PMIC)
  // Fingerprint checked at the first boot:
  uint8_t pmicAddr;     // power chip I2C address (0x34 AXP192, 0x6E M5PM1), 0 = must have neither
  uint16_t panelLong, panelShort;  // display size in pixels
  m5::imu_t imuType;    // expected IMU (MPU6886: WHO_AM_I 0x19 read directly; others through M5.Imu)
  bool    codec;        // ES8311 audio codec at I2C 0x18
  uint8_t rotation[5];  // setRotation() per layout: WIDE, WIDE_FLIP, TALL, TALL_FLIP, FLAT
  uint8_t axis[3];      // raw accelerometer axis (0 = x, 1 = y, 2 = z) used for our x, y, z
  int8_t  sign[3];      // and its sign
};

using P = Power; using I = Imu; using S = Sound; using C = Clock;
const BoardProfile PROFILES[] = {
  // Tested on devices: identical axes and rotations, measured.
  {m5::board_t::board_M5StickCPlus,  "M5StickC Plus",  true,  false, false, P::AXP192,     I::MPU6886,   S::BUZZER_EXTEN, C::BM8563,
   2, 10, true,  37, 39, 35, -1, 21, 22, 0x34, 240, 135, m5::imu_t::imu_mpu6886, false,
   {1, 3, 2, 0, 1}, {0, 1, 2}, {1, 1, 1}},
  {m5::board_t::board_M5StickC,      "M5StickC",       true,  false, true,  P::AXP192,     I::MPU6886,   S::LED,          C::BM8563,
   -1, 10, true, 37, 39, 35, -1, 21, 22, 0x34, 160,  80, m5::imu_t::imu_mpu6886, false,
   {1, 3, 2, 0, 1}, {0, 1, 2}, {1, 1, 1}},
  // UNTESTED, from the M5Stack docs and M5Unified: CH9102 bridge, GPIO4 power hold, button C (power) on
  // GPIO35, MPU6886 interrupt not documented (so polled), LED + IR on GPIO19 (active high). Axes and
  // rotations assumed to match the Plus (same case and panel).
  {m5::board_t::board_M5StickCPlus2, "M5StickC Plus2", false, false, false, P::HOLD_GPIO4, I::MPU6886,   S::BUZZER,       C::BM8563,
   2, 19, false, 37, 39, -1, 35, 21, 22, 0,    240, 135, m5::imu_t::imu_mpu6886, false,
   {1, 3, 2, 0, 1}, {0, 1, 2}, {1, 1, 1}},
  // Partly tested on a device (2026-10-14): ESP32-S3, M5PM1 PMIC (its button powers on / resets / powers
  // off by itself), BMI270 IMU whose INT1 goes to the PMIC, which signals it on GPIO13 (see pm1ImuInit()
  // in main.cpp), ES8311 codec + speaker (amplifier on PMIC G3), buttons on GPIO11/12, no RTC. Poses, the
  // speaker, taps and the M5 button work; deep sleep on battery is not verified yet.
  {m5::board_t::board_M5StickS3,     "M5StickS3",      false, true,  false, P::M5PM1,      I::M5UNIFIED, S::SPEAKER,      C::SYSTEM,
   -1, -1, false, 11, 12, 13, -1, 47, 48, 0x6E, 240, 135, m5::imu_t::imu_bmi270,  true,
   {1, 3, 2, 0, 1}, {0, 1, 2}, {1, 1, 1}},
};
