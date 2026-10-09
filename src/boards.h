// Board profiles: everything that differs between the supported M5Stick models.
//
// The firmware is the same for every model. At boot M5Unified detects the board (M5.getBoard()) and
// main.cpp picks the PROFILES row with the same `board`; an unknown board falls back to the first
// row and logs a warning. The row is kept in RTC memory, so wakes from deep sleep use it before
// M5.begin() runs.
//
// Adding a similar model (ESP32, AXP192, MPU6886, BM8563, buttons on GPIO37/39, IMU INT on GPIO35):
//   1. Add a row below with its m5::board_t id and a short display name. scripts/flash.py reads the
//      names from this file, so keep them in double quotes on one line.
//   2. Measure the accelerometer axes: flash, watch `pio device monitor` and put the device in each
//      pose. Every screen-on log line prints `acc=x,y,z`. Fill `axis` / `sign` so that our x, y, z
//      read +1 g for: z screen up, x long edge A up (power button down), -y standing with USB up.
//   3. Check every pose shows an upright image; fix `rotation` (M5GFX setRotation() values) if not.
//   4. Set `compact` for small (80x160) panels, `buzzerPin` = -1 if there is no buzzer.
// Models with a different power chip, IMU or pinout (e.g. StickC Plus2, StickS3) need code changes
// as well; see PORTING.md.
#pragma once
#include <M5Unified.h>

struct BoardProfile {
  m5::board_t board;    // M5Unified board id
  const char* name;     // shown in the boot log ("profile=...") and by scripts/flash.py
  bool    compact;      // small 80x160 panel: compact status line, thinner digit halo
  int8_t  buzzerPin;    // passive buzzer (powered from AXP192 EXTEN); -1 if none: the LED signals instead
  int8_t  ledPin;       // red LED, active low
  uint8_t rotation[5];  // setRotation() per layout: WIDE, WIDE_FLIP, TALL, TALL_FLIP, FLAT
  uint8_t axis[3];      // raw accelerometer axis (0 = x, 1 = y, 2 = z) used for our x, y, z
  int8_t  sign[3];      // and its sign
};

const BoardProfile PROFILES[] = {
  // board                          name             compact buzzer led  rotation          axis       sign
  {m5::board_t::board_M5StickCPlus, "M5StickC Plus", false,   2,     10, {1, 3, 2, 0, 1}, {0, 1, 2}, {1, 1, 1}},
  {m5::board_t::board_M5StickC,     "M5StickC",      true,   -1,     10, {1, 3, 2, 0, 1}, {0, 1, 2}, {1, 1, 1}},
};
