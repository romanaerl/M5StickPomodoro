// Low-power Pomodoro timer for M5StickC Plus (ESP32-PICO-D4, AXP192, MPU6886, BM8563, ST7789).
// Also runs on the original M5StickC (80x160 ST7735S, no buzzer): detected at boot, it gets a compact
// status line and signals with the red LED instead of beeps (the LED also blinks with the FINISHED
// screen, and the power key's short press switches the LED instead of the sound). Same chips and
// axes otherwise. The M5StickC Plus2 (no AXP192) and the M5StickS3 (ESP32-S3, separate build) have
// UNTESTED profiles prepared from the docs; all hardware differences come from src/boards.h.
//
// RUNNING  : any orientation except face down. Light sleep between events. At every minute mark
//            the screen fades in and out over 5 s showing only the minutes; on a layout change or a
//            tap it shows MM:SS for 5 s. Layout follows the device pose:
//            flat / long edges -> landscape, standing on its end -> portrait. A bar in a rounded
//            frame behind the digits drains every second and fades green -> orange -> red.
//            On long edge A and on either end, a light tap or vibration also shows the screen.
// PAUSED   : screen face down -> deep sleep, woken by MPU6886 wake-on-motion on GPIO35.
// FINISHED : beep, blink 00:00 for 2 min (or until face down), deep sleep. Face down and back up
//            restarts the same mode; changing the pose for 1 s during the blink starts the other mode.
// Modes    : WORK (green -> orange -> red) and BREAK (blue -> light blue), each with its own length.
//            Power-on always starts WORK.
// Button A : restarts the current mode; pressed again within 5 s, switches to the other mode.
//            Every session starts with a 5 s grace period (5:00 starts at 5:05), yellow for WORK and
//            violet for BREAK, that matches this window.
// Button B : sets the current mode's length (1..60 min, +1 per press, auto-repeat when held); the new
//            session starts 1 s after the last press. The length is kept in NVS.
// Power key: while the screen is on, a short press toggles the sound and a long press (1.5 s)
//            toggles tap-to-wake in the horizontal poses (always on when standing on an end).
//            Both settings are kept in NVS.
// Battery  : a bright yellow dot below 40 %, red below 20 %, readable from a distance.
// Time is measured with the BM8563 crystal RTC, so ESP32 sleep-clock drift does not matter.

#include <M5Unified.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/adc.h>
#include <driver/uart.h>
#include <sys/time.h>

#ifndef SESSION_MIN
#define SESSION_MIN 25  // default WORK length, minutes
#endif
#ifndef BREAK_MIN
#define BREAK_MIN 5     // default BREAK length, minutes
#endif
constexpr uint32_t MODE_SWITCH_MS = 5000;  // second button-A press within this switches the mode
constexpr uint32_t HOLD_SOUND_MS = 1500;   // button A held this long: sound (LED) on/off
constexpr uint32_t HOLD_TAP_MS = 3000;     // held this long: tap-to-wake on/off
constexpr int32_t  GRACE_MS = 5000;        // shown in yellow before the session proper starts
constexpr uint32_t POSE_SWITCH_MS = 1000;  // FINISHED: a new pose held this long starts the other mode
constexpr uint32_t SET_TIMEOUT_MS = 1000;
constexpr uint32_t SCREEN_ON_MS = 5000;
constexpr uint32_t FINISHED_SCREEN_MS = 2 * 60 * 1000;  // blinking, unless turned face down
constexpr uint32_t BLINK_MS = 500;
constexpr uint32_t NOTICE_MS = 2000;  // "SOUND ON/OFF" banner
constexpr uint8_t  BRIGHTNESS_AXP = 96;  // AXP192 boards: M5GFX maps this to LDO2 ~2.8 V
constexpr uint8_t  BRIGHTNESS_PWM = 12;  // PWM backlights (StickS3): M5GFX makes this 54/512 duty,
                                         // picked on the device with env s3-brightness
// Minute-mark glance: backlight steps through these AXP192 LDO2 levels (1.8 V + 0.1 V * level), one
// per second. Level 7 is the first one visible on the device.
constexpr uint8_t  GLANCE_LEVELS[] = {7, 8, 8, 8, 7};
constexpr uint32_t GLANCE_STEP_MS  = 1000;
// PWM backlights: the glance fades smoothly to this raw 9-bit duty (picked on a StickS3: half of
// setBrightness(1), which M5GFX makes 34/512) on M5GFX's backlight LEDC channel.
constexpr uint32_t GLANCE_PWM_PEAK = 17;
constexpr uint8_t  M5GFX_BACKLIGHT_CH = 7;

constexpr uint8_t  MPU = 0x68, RTC_ADDR = 0x51, AXP = 0x34;
constexpr uint32_t I2C_HZ = 400000;

// Accelerometer axes (measured on the device; the axis pointing up reads +1 g):
//   +z screen up, +x long edge with landscape upright, -y standing on its end with USB up.
constexpr float DOWN_G    = -0.6f;  // z below this: face down -> pause
constexpr float VISIBLE_G = -0.3f;  // z above this: screen visible -> run (hysteresis band between)
constexpr float AXIS_G    =  0.7f;  // an axis above this decides the layout
constexpr int   CONFIRM_SAMPLES = 3;
constexpr uint32_t CONFIRM_STEP_MS = 150;
// Wake-on-motion: x4 mg between consecutive samples. In tap poses the IMU samples faster with a
// lower threshold so that a light tap is caught.
constexpr uint8_t WOM_THRESHOLD = 20, WOM_THRESHOLD_TAP = 10;
constexpr uint8_t ODR_DIV = 79, ODR_DIV_TAP = 9;  // 1 kHz / (1 + div): 12.5 Hz, 100 Hz

enum State : uint8_t { RUNNING = 0, PAUSED = 1, FINISHED = 2 };
enum Mode : uint8_t { WORK = 0, BREAK = 1 };
enum Layout : uint8_t { WIDE = 0, WIDE_FLIP = 1, TALL = 2, TALL_FLIP = 3, FLAT = 4 };

// ---------- board profiles: see boards.h ----------
#include "boards.h"

constexpr uint32_t MAGIC = 0x504F4D33;
RTC_DATA_ATTR uint32_t rtcMagic;
RTC_DATA_ATTR uint8_t  state;
RTC_DATA_ATTR int64_t  endMs;         // RUNNING: session end on the BM8563 ms timeline
RTC_DATA_ATTR int32_t  pausedRemMs;   // PAUSED: remaining time
RTC_DATA_ATTR uint8_t  mode = WORK;
RTC_DATA_ATTR uint8_t  lengthMin[2] = {SESSION_MIN, BREAK_MIN};
RTC_DATA_ATTR bool     soundOn = true;
RTC_DATA_ATTR bool     tapFlat = false;  // tap-to-wake in the horizontal poses (flat, long edges)
RTC_DATA_ATTR bool     womOk;         // GPIO35 interrupt line verified at cold boot
RTC_DATA_ATTR bool     finishedDown;  // FINISHED: has been face down since the session ended
RTC_DATA_ATTR uint8_t  profileIdx;    // PROFILES row; set at cold boot, used before M5.begin() after sleep
RTC_DATA_ATTR uint16_t sleepWakes;    // diagnostics: deep-sleep wakes that went back to sleep
static uint16_t motionEvents;         // diagnostics: WOM interrupts while running

static M5Canvas canvas(&M5.Display);
static uint8_t  layout = WIDE;
static bool     screenOn = false, setting = false, minutesOnly = false;
static uint32_t screenOffAt = 0;
static int      lastMinShown = -1, lastDrawn = -1, battery = -1;
static int      downCount = 0, layoutCount = 0, pendingLayout = -1;
static uint32_t downSince = 0, motionUntil = 0, finishedAt = 0, noticeUntil = 0, lastPressAt = 0;
static uint32_t poseSince = 0;
static int      finishLayout = -1;
static bool     inGrace = false;  // RUNNING within the grace period of a new session
static bool     poseChanging = false;
static bool     noticeShown = false;
static const char* noticeMsg = "";
static bool     noticeOn = false;

static const BoardProfile& prof() { return PROFILES[profileIdx]; }
static bool hasBuzzer() { return prof().sound != Sound::LED; }
static bool axp() { return prof().power == Power::AXP192; }
static uint8_t brightness() { return axp() ? BRIGHTNESS_AXP : BRIGHTNESS_PWM; }
// AXP192 calls that M5Unified only provides in ESP32 builds (the AXP192 boards are all ESP32).
static bool axpUsbPower() {
#if CONFIG_IDF_TARGET_ESP32
  return M5.Power.Axp192.isVBUS();
#else
  return true;
#endif
}
static void axpExten(bool on) {
#if CONFIG_IDF_TARGET_ESP32
  M5.Power.Axp192.setEXTEN(on);
#else
  (void)on;
#endif
}
// Pins from the profile (buttons and the IMU / power-key lines are active low).
static gpio_num_t PIN_BTN_A() { return (gpio_num_t)prof().btnA; }
static gpio_num_t PIN_BTN_B() { return (gpio_num_t)prof().btnB; }
static gpio_num_t PIN_INT() { return (gpio_num_t)prof().intPin; }
static bool btnDown(gpio_num_t pin) { return digitalRead(pin) == LOW; }

// ---------- low level ----------

// The USB-UART bridge is powered from USB. While the cable is out, a driven TX line (idle HIGH)
// back-powers it through its RX pin, and then it does not enumerate when the cable is plugged in
// (found on the device: the port only appeared after deep sleep released the pins). So TX is
// connected only while USB power is present; logs flow as usual whenever a computer is attached.
static bool txAttached = true;
// Waits for the log to go out before a sleep. On the ESP32-S3 the log goes over the chip's own USB
// (HWCDC), whose flush() never returns while no computer reads it: on battery it froze the StickS3 at
// its first sleep. There it flushes only while USB power is present and a computer has read the log
// (a charger never reads it; after unplugging, VBUS is gone).
static void flushLog() {
#if CONFIG_IDF_TARGET_ESP32S3
  if (Serial && M5.Power.getVBUSVoltage() > 4000) Serial.flush();
#else
  Serial.flush();
#endif
}

static void serialFollowUsb() {
  if (!axp()) return;  // needs the AXP192 to see USB power; other boards keep TX attached
  const bool usb = axpUsbPower();
  if (usb == txAttached) return;
  flushLog();
  if (usb) uart_set_pin(UART_NUM_0, 1, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  else pinMode(1, INPUT);
  txAttached = usb;
}
static void wr(uint8_t addr, uint8_t reg, uint8_t v) { M5.In_I2C.writeRegister8(addr, reg, v, I2C_HZ); }
static uint8_t rd(uint8_t addr, uint8_t reg) { return M5.In_I2C.readRegister8(addr, reg, I2C_HZ); }

// ESP32-S3 (StickS3) with USB power present (the M5PM1 measures VBUS).
static bool s3OnUsb() {
#if CONFIG_IDF_TARGET_ESP32S3
  return prof().power == Power::M5PM1 && M5.Power.getVBUSVoltage() > 4000;
#else
  return false;
#endif
}

// Light sleep. wakeOnIo: buttons wake it; imuWake: the IMU INT line too (off while already polling
// after motion, otherwise a re-latching INT turns the sleep into a busy loop).
static void nap(uint32_t ms, bool wakeOnIo, bool imuWake = true) {
  imuWake = imuWake && wakeOnIo && womOk && prof().intPin >= 0;
  // A power key on a GPIO wakes it too, unless it is held (a held level would wake it at once).
  const bool keyWake = wakeOnIo && prof().keyPin >= 0 && digitalRead(prof().keyPin) == HIGH;
  // A PWM backlight (boards without the AXP192) goes dark in light sleep, which made the screen flicker
  // (StickS3): while it is lit, wait awake instead, still returning early on a button press. The same on
  // the ESP32-S3 while USB is connected: its USB is inside the chip and stops answering in light sleep,
  // which would make flashing need the manual download mode.
  if ((!axp() && M5.Display.getBrightness() > 0) || s3OnUsb()) {
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      if (wakeOnIo && (digitalRead(PIN_BTN_A()) == LOW || digitalRead(PIN_BTN_B()) == LOW ||
                       (keyWake && digitalRead(prof().keyPin) == LOW) ||
                       (imuWake && digitalRead(PIN_INT()) == LOW)))
        return;
      delay(10);
    }
    return;
  }
  flushLog();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
  if (wakeOnIo) {
    gpio_wakeup_enable(PIN_BTN_A(), GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(PIN_BTN_B(), GPIO_INTR_LOW_LEVEL);
    if (imuWake) gpio_wakeup_enable(PIN_INT(), GPIO_INTR_LOW_LEVEL);
    if (keyWake) gpio_wakeup_enable((gpio_num_t)prof().keyPin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }
  esp_light_sleep_start();
  if (wakeOnIo) {
    gpio_wakeup_disable(PIN_BTN_A());
    gpio_wakeup_disable(PIN_BTN_B());
    if (imuWake) gpio_wakeup_disable(PIN_INT());
    if (keyWake) gpio_wakeup_disable((gpio_num_t)prof().keyPin);
  }
}

// ---------- BM8563 RTC ----------
static uint8_t bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

// System time in ms (Clock::SYSTEM): kept across light and deep sleep by the RC sleep clock.
static int64_t systemMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int64_t rtcSeconds(bool* valid = nullptr) {
  if (prof().clock == Clock::SYSTEM) { if (valid) *valid = true; return systemMs() / 1000; }
  uint8_t b[7];
  M5.In_I2C.readRegister(RTC_ADDR, 0x02, b, 7, I2C_HZ);
  if (valid) *valid = !(b[0] & 0x80);
  int y = 2000 + bcd(b[6]), m = bcd(b[5] & 0x1F), d = bcd(b[3] & 0x3F);
  y -= m <= 2;  // days-from-civil
  int era = y / 400, yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t days = (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  return days * 86400 + bcd(b[2] & 0x3F) * 3600 + bcd(b[1] & 0x7F) * 60 + bcd(b[0] & 0x7F);
}

// RTC time (ms) at the moment `eventMillis` happened: waits for the next second tick (< 1 s)
// to get sub-second precision without trusting the ESP32's RC sleep clock over long periods.
static int64_t rtcMsAt(uint32_t eventMillis) {
  if (prof().clock == Clock::SYSTEM) return systemMs() - (int64_t)(millis() - eventMillis);
  int64_t s0 = rtcSeconds(), s = s0;
  uint32_t t0 = millis();
  while (s == s0 && millis() - t0 < 1100) { nap(10, false); s = rtcSeconds(); }
  return s * 1000 - (int64_t)(millis() - eventMillis);
}

static void rtcInit() {
  if (prof().clock != Clock::BM8563) return;
  wr(RTC_ADDR, 0x00, 0x00);  // clock running
  wr(RTC_ADDR, 0x01, 0x00);  // no alarm/timer interrupts on the shared GPIO35 line
  bool valid;
  rtcSeconds(&valid);
  if (!valid) {  // never set / lost power: any consistent time base will do
    const uint8_t t[7] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x24};
    for (int i = 0; i < 7; i++) wr(RTC_ADDR, 0x02 + i, t[i]);
  }
}

// ---------- MPU6886 ----------
struct Accel { float x, y, z; };

// StickS3: BMI270 (I2C 0x69) whose INT1 goes to the M5PM1's G4; the PMIC raises its IRQ output G1,
// wired to the ESP32-S3's GPIO13. That is the "INT pin" of this board: wake-on-motion and taps work
// as with the MPU6886's INT on GPIO35. Register details from the M5PM1 library and the BMI270 datasheet.
constexpr uint8_t PM1 = 0x6E;
constexpr uint32_t PM1_HZ = 100000;
static bool pm1Imu() { return prof().power == Power::M5PM1 && prof().intPin >= 0; }
RTC_DATA_ATTR uint8_t bmiAddr;  // BMI270 I2C address: 0x68 or 0x69 (SDO), found once, kept in deep sleep
static uint8_t bmi() {
  if (!bmiAddr)
    for (uint8_t a : {0x68, 0x69})
      if (M5.In_I2C.readRegister8(a, 0x00, I2C_HZ) == 0x24) { bmiAddr = a; break; }  // CHIP_ID
  return bmiAddr ? bmiAddr : 0x68;
}

static Accel readAccel() {
  float raw[3];
  if (pm1Imu()) {  // read directly: works before M5.begin() (motion wake fast path)
    uint8_t b[6];
    M5.In_I2C.readRegister(bmi(), 0x0C, b, 6, I2C_HZ);
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = (int16_t)(b[2 * i] | b[2 * i + 1] << 8) * (8.0f / 32768.0f);  // +-8 g
    raw[0] = r[1]; raw[1] = -r[0]; raw[2] = r[2];  // the same axes as M5Unified gives (X=+Y, Y=-X, Z=+Z)
  } else if (prof().imu == Imu::M5UNIFIED) {
    M5.Imu.update();
    auto d = M5.Imu.getImuData();
    raw[0] = d.accel.x; raw[1] = d.accel.y; raw[2] = d.accel.z;
  } else {
    uint8_t b[6];
    M5.In_I2C.readRegister(MPU, 0x3B, b, 6, I2C_HZ);
    for (int i = 0; i < 3; i++) raw[i] = (int16_t)(b[2 * i] << 8 | b[2 * i + 1]) / 8192.0f;  // +-4 g
  }
  const BoardProfile& p = prof();
  return {p.sign[0] * raw[p.axis[0]], p.sign[1] * raw[p.axis[1]], p.sign[2] * raw[p.axis[2]]};
}
static bool mpu() { return prof().imu == Imu::MPU6886; }

// Layout for a visible pose, or -1 while tilted in between.
static int layoutOf(const Accel& a) {
  if (a.z > AXIS_G) return FLAT;
  if (a.x > AXIS_G) return WIDE;
  if (a.x < -AXIS_G) return WIDE_FLIP;
  if (a.y < -AXIS_G) return TALL;
  if (a.y > AXIS_G) return TALL_FLIP;
  return -1;
}

static bool imuMotion() {  // a motion event since the last call; clears it, releasing the INT line
  if (pm1Imu()) {
    const uint8_t st = M5.In_I2C.readRegister8(PM1, 0x40, PM1_HZ);  // IRQ_STATUS1: GPIO events
    if (!st) return false;
    M5.In_I2C.writeRegister8(PM1, 0x40, 0x00, PM1_HZ);              // write 0 clears
    return st & 0x10;                                               // G4 = BMI270 INT1
  }
  return mpu() && (rd(MPU, 0x3A) & 0xE0);  // reading INT_STATUS releases INT
}

// BMI270 any-motion (feature page 1, bytes 12..15 of 0x30..0x3F): fires when any axis changes by more
// than thrMg within durSamples x 20 ms. Used like the MPU6886's wake-on-motion threshold.
static void bmiAnyMotion(uint16_t thrMg, uint16_t durSamples) {
  uint8_t f[16];
  M5.In_I2C.writeRegister8(bmi(), 0x2F, 1, I2C_HZ);
  M5.In_I2C.readRegister(bmi(), 0x30, f, 16, I2C_HZ);
  const uint16_t w1 = (durSamples & 0x1FFF) | (7u << 13);  // duration, x/y/z selected
  const uint16_t thr = (uint32_t)thrMg * 2048 / 1000;     // 0.488 mg per step
  const uint16_t w2 = ((f[14] | f[15] << 8) & 0x7800) | (thr & 0x7FF) | 0x8000;  // threshold, enabled
  f[12] = w1 & 0xFF; f[13] = w1 >> 8; f[14] = w2 & 0xFF; f[15] = w2 >> 8;
  M5.In_I2C.writeRegister(bmi(), 0x30, f, 16, I2C_HZ);
  M5.In_I2C.writeRegister8(bmi(), 0x2F, 0, I2C_HZ);
}

// StickS3, every boot after M5.begin() (which resets the BMI270): any-motion on INT1 only, INT1 as an
// active-low push-pull output; the M5PM1 reports G4 edges on its IRQ output G1.
static void pm1ImuInit() {
  M5.In_I2C.writeRegister8(bmi(), 0x58, 0x00, I2C_HZ);  // INT_MAP_DATA: M5Unified maps all data IRQs
  M5.In_I2C.writeRegister8(bmi(), 0x53, 0x08, I2C_HZ);  // INT1_IO_CTRL: output, push-pull, active low
  M5.In_I2C.writeRegister8(bmi(), 0x55, 0x00, I2C_HZ);  // INT_LATCH: pulses (the PMIC latches)
  M5.In_I2C.writeRegister8(bmi(), 0x56, 0x40, I2C_HZ);  // INT1_MAP_FEAT: any-motion
  M5.In_I2C.writeRegister8(bmi(), 0x7D, 0x04, I2C_HZ);  // PWR_CTRL: accelerometer only (gyro off)
  bmiAnyMotion(80, 2);
  auto bits = [](uint8_t reg, uint8_t mask, uint8_t val) {
    for (int i = 0; i < 5; i++) {
      const uint8_t v = M5.In_I2C.readRegister8(PM1, reg, PM1_HZ);
      if ((v & mask) == val) return;
      M5.In_I2C.writeRegister8(PM1, reg, (v & ~mask) | val, PM1_HZ);
    }
  };
  bits(0x17, 0x03, 0x00);  // G4: GPIO function
  bits(0x10, 0x10, 0x00);  //     input
  bits(0x15, 0x03, 0x01);  //     pull-up
  bits(0x16, 0x0C, 0x04);  // G1: IRQ output function
  bits(0x10, 0x02, 0x02);  //     output
  bits(0x13, 0x02, 0x00);  //     push-pull
  bits(0x43, 0x1F, 0x0F);  // IRQ masks: G4 only; no system or button IRQs
  bits(0x44, 0x3F, 0x3F);
  bits(0x45, 0x07, 0x07);
  for (uint8_t reg = 0x40; reg <= 0x42; reg++) M5.In_I2C.writeRegister8(PM1, reg, 0x00, PM1_HZ);
}

// Accel-only low-power cycle mode (12.5 Hz) with wake-on-motion on the INT pin.
static void imuInit() {
  if (!mpu()) return;
  wr(MPU, 0x6B, 0x80); delay(10);  // reset
  wr(MPU, 0x6B, 0x01); delay(5);   // awake, PLL clock
  wr(MPU, 0x6C, 0x07);             // gyro off
  wr(MPU, 0x1C, 0x08);             // +-4 g
  wr(MPU, 0x1D, 0x07);             // 4-sample averaging; DLPF_CFG=7 (other values read ~0 g in cycle mode)
  wr(MPU, 0x19, ODR_DIV);          // 12.5 Hz
  wr(MPU, 0x37, 0xE0);             // active low, open drain, latched until INT_STATUS read
  wr(MPU, 0x20, WOM_THRESHOLD); wr(MPU, 0x21, WOM_THRESHOLD); wr(MPU, 0x22, WOM_THRESHOLD);
  wr(MPU, 0x69, 0xC2);             // WOM enabled, compare with previous sample
  wr(MPU, 0x38, 0xE0);             // WOM X/Y/Z -> INT
  wr(MPU, 0x6B, 0x21);             // cycle mode
  delay(100);
  imuMotion();
}

static void imuTapSensitive(bool on) {
  // StickS3: any-motion samples at 50 Hz and smooths a tap more than the MPU6886 at 100 Hz, so its
  // tap threshold is lower (40 and 20 mg felt less sensitive than the other models; 10 mg was close to the limit).
  if (pm1Imu()) { bmiAnyMotion(on ? 15 : 80, on ? 1 : 2); return; }
  if (!mpu()) return;
  uint8_t th = on ? WOM_THRESHOLD_TAP : WOM_THRESHOLD;
  wr(MPU, 0x19, on ? ODR_DIV_TAP : ODR_DIV);
  wr(MPU, 0x20, th); wr(MPU, 0x21, th); wr(MPU, 0x22, th);
}

// Verify the INT -> GPIO35 path using the data-ready interrupt (no physical motion needed).
static bool imuSelfTest() {
  if (pm1Imu()) {  // the same check through the PMIC: BMI270 data-ready -> G4 -> IRQ -> GPIO13
    pinMode(PIN_INT(), INPUT);
    imuMotion();
    M5.In_I2C.writeRegister8(bmi(), 0x58, 0x04, I2C_HZ);  // accel data ready -> INT1
    bool low = false;
    for (int i = 0; i < 50 && !low; i++) { delay(10); low = digitalRead(PIN_INT()) == LOW; }
    M5.In_I2C.writeRegister8(bmi(), 0x58, 0x00, I2C_HZ);
    delay(30);
    M5.In_I2C.writeRegister8(PM1, 0x40, 0x00, PM1_HZ);
    delay(5);
    const bool released = digitalRead(PIN_INT()) == HIGH;
    M5.Imu.update();
    const auto d = M5.Imu.getImuData();
    const Accel a = readAccel();
    Serial.printf("IMU INT self-test (via PMIC): asserted=%d released=%d; accel raw %.2f,%.2f,%.2f m5 %.2f,%.2f,%.2f\n",
                  low, released, a.x, a.y, a.z, d.accel.x, d.accel.y, d.accel.z);
    return low && released;
  }
  if (!mpu() || prof().intPin < 0) {
    Serial.println("IMU INT: not wired to the ESP32, orientation is polled");
    return false;
  }
  pinMode(PIN_INT(), INPUT);
  rd(MPU, 0x3A);
  wr(MPU, 0x38, 0x01);  // data ready -> INT
  delay(100);
  bool low = digitalRead(PIN_INT()) == LOW;
  rd(MPU, 0x3A);
  bool released = digitalRead(PIN_INT()) == HIGH;
  wr(MPU, 0x38, 0xE0);
  rd(MPU, 0x3A);
  Serial.printf("IMU INT self-test: asserted=%d released=%d\n", low, released);
  return low && released;
}

// Samples until the pose is confirmed: +1 visible (since = first visible sample), -1 face down,
// 0 undecided after ~2 s.
static int confirmPose(uint32_t* since) {
  int up = 0, down = 0;
  for (int i = 0; i < 14; i++) {
    float z = readAccel().z;
    if (z > VISIBLE_G) {
      down = 0;
      if (up++ == 0) *since = millis();
      if (up >= CONFIRM_SAMPLES) return 1;
    } else if (z < DOWN_G) {
      up = 0;
      if (++down >= CONFIRM_SAMPLES) return -1;
    } else {
      up = down = 0;
    }
    nap(CONFIRM_STEP_MS, false);
  }
  return 0;
}

// After a motion wake: the pose once the device has stopped moving. A wake comes at the start of a move
// (picked up, turned over), and a pose confirmed in mid-turn is not where the device ends up; if it then
// settles gently, below the wake-on-motion threshold, nothing wakes it again and the end pose is missed
// (e.g. turned face down after FINISHED, which then never counts). So the check repeats while the IMU
// still reports motion, for up to ~6 s.
static int settledPose(uint32_t* since) {
  imuMotion();  // clear the wake event itself
  int pose = 0;
  for (int i = 0; i < 3; i++) {
    pose = confirmPose(since);
    if (!imuMotion()) break;  // no motion during the check: this is the end pose
  }
  return pose;
}

// ---------- persistence ----------
// The running state lives in RTC memory across deep sleep; power-on always starts a new WORK session,
// so only the settings go to NVS, and only when they change.
static void saveSettings() {
  Preferences p;
  p.begin("pomo", false);
  p.putUChar("min", lengthMin[WORK]);
  p.putUChar("bmin", lengthMin[BREAK]);
  p.putBool("snd", soundOn);
  p.putBool("tap", tapFlat);
  p.end();
}

static void loadSettings() {
  Preferences p;
  p.begin("pomo", true);
  lengthMin[WORK] = p.getUChar("min", SESSION_MIN);
  lengthMin[BREAK] = p.getUChar("bmin", BREAK_MIN);
  soundOn = p.getBool("snd", true);
  tapFlat = p.getBool("tap", false);
  p.end();
  if (lengthMin[WORK] < 1 || lengthMin[WORK] > 60) lengthMin[WORK] = SESSION_MIN;
  if (lengthMin[BREAK] < 1 || lengthMin[BREAK] > 60) lengthMin[BREAK] = BREAK_MIN;
}

// ---------- display (rendered off-screen, pushed in one go) ----------
static int32_t sessionMs() { return lengthMin[mode] * 60 * 1000; }

static bool isTall() { return layout == TALL || layout == TALL_FLIP; }
static bool isTapPose() { return isTall() || tapFlat; }

// `narrow`: the 80 px wide StickC portrait status line, where the longest labels are shortened.
static const char* stateLabel(bool narrow = false) {
  if (setting) return mode == WORK ? (narrow ? "SET W" : "SET WORK") : (narrow ? "SET B" : "SET BREAK");
  if (state == RUNNING) return mode == WORK ? "WORK" : "BREAK";
  return state == PAUSED ? "PAUSED" : narrow ? "DONE" : "FINISHED";
}

static void drawTime(int remS, int cx, int cy, int maxW, int maxH) {
  char buf[8];
  if (minutesOnly) snprintf(buf, sizeof buf, "%02d", (remS + 59) / 60);
  else snprintf(buf, sizeof buf, "%02d:%02d", remS / 60, remS % 60);
  canvas.setFont(&fonts::Font7);
  canvas.setTextSize(1);
  float size = std::min((float)maxW / canvas.textWidth("88:88"), (float)maxH / canvas.fontHeight());
  if (minutesOnly) {  // two digits: as large as the landscape MM:SS, or as wide as the bar allows
    // Landscape MM:SS limits (216 x 90 px on the Plus), derived from the panel size.
    const int lng = std::max(canvas.width(), canvas.height()), sht = std::min(canvas.width(), canvas.height());
    const int wideW = lng - 24, wideH = std::min(90, sht - (prof().compact ? 10 : 18) - 20);
    float wide = std::min((float)wideW / canvas.textWidth("88:88"), (float)wideH / canvas.fontHeight());
    size = std::min((float)maxW / canvas.textWidth("88"), wide);
  }
  canvas.setTextSize(size);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_BLACK);  // black halo keeps digits readable on top of the bar
  const int h = prof().compact ? 2 : 3;
  for (int dx = -h; dx <= h; dx += h)
    for (int dy = -h; dy <= h; dy += h) canvas.drawString(buf, cx + dx, cy + dy);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(buf, cx, cy);
}

// Battery percentage, or -1 while unknown: right after power-on the AXP192 ADC still reads 0.
static int readBattery() {
  int b = M5.Power.getBatteryLevel();
  return b > 0 ? b : -1;
}

// Status: state label on the left; battery (with a bright dot when low) on the right, MUTE / TAP
// flags just left of the battery. The narrow StickC portrait line has no room for the flags and uses
// shorter labels.
static void drawStatus(int y, uint8_t datumL, uint8_t datumR) {
  const bool narrow = prof().compact && isTall();
  char buf[16], flags[16];
  // TAP only matters in landscape: standing on an end, tap-to-wake is always on.
  snprintf(flags, sizeof flags, "%s%s", soundOn ? "" : hasBuzzer() ? "MUTE" : "NO LED",
           tapFlat && !isTall() ? (soundOn ? "TAP" : " TAP") : "");
  if (narrow) flags[0] = 0;
  if (prof().compact) canvas.setFont(&fonts::Font0); else canvas.setFont(&fonts::Font2);
  canvas.setTextSize(1);
  canvas.setTextColor(state == FINISHED ? TFT_RED : TFT_LIGHTGREY);
  canvas.setTextDatum(datumL);
  canvas.drawString(stateLabel(narrow), 4, y);
  const int r = prof().compact ? 3 : 6;  // low-battery dot radius
  int x = canvas.width() - 4;  // right edge of what is still to be drawn
  if (battery >= 0) {  // low battery: bright dot + coloured percentage, visible from a distance
    uint16_t c = battery < 20 ? TFT_RED : battery < 40 ? TFT_YELLOW : TFT_LIGHTGREY;
    snprintf(buf, sizeof buf, "%d%%", battery);
    canvas.setTextColor(c);
    canvas.setTextDatum(datumR);
    canvas.drawString(buf, x, y);
    x -= canvas.textWidth(buf) + 4;
    if (battery < 40) {
      int cy = datumR == top_right ? y + canvas.fontHeight() / 2 : y - canvas.fontHeight() / 2;
      canvas.fillCircle(x - r, cy, r, c);
      x -= 2 * r + 4;
    }
  }
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setTextDatum(datumR);
  canvas.drawString(flags, x, y);
}

// WORK: green -> orange -> red as the session drains. BREAK: blue -> light blue.
static uint16_t drainColor(int remS) {
  if (remS * 1000 > sessionMs())  // grace period: yellow for WORK, violet for BREAK
    return mode == WORK ? canvas.color565(255, 210, 0) : canvas.color565(180, 90, 255);
  float f = (float)remS * 1000 / sessionMs();
  auto mix = [](float t, int a, int b) { return (int)(a + (b - a) * t); };
  if (mode == BREAK) return canvas.color565(mix(1 - f, 0, 160), mix(1 - f, 90, 225), 255);
  if (f >= 0.5f) {
    float t = (1 - f) / 0.5f;
    return canvas.color565(mix(t, 0, 255), mix(t, 200, 140), 0);
  }
  float t = (0.5f - f) / 0.5f;
  return canvas.color565(mix(t, 255, 230), mix(t, 140, 0), 0);
}

// Rounded frame with a bar inside whose level drops every second with the remaining time; time on
// top of it. The status line takes a strip at the top in portrait, at the bottom in landscape: 18 px
// on the Plus, 10 px on the StickC.
static void render(int remS) {
  if (battery < 0) battery = readBattery();
  constexpr int M = 3, PAD = 5;  // frame margin, frame-to-bar padding
  const int STRIP = prof().compact ? 10 : 18;
  const int W = canvas.width(), H = canvas.height();
  const int fy = (isTall() ? STRIP : 0) + M, fw = W - 2 * M, fh = H - STRIP - 2 * M;
  const int bx = M + PAD, by = fy + PAD, bw = fw - 2 * PAD, bh = fh - 2 * PAD;
  const int fill = (int)std::min<int64_t>(bh, (int64_t)bh * remS * 1000 / sessionMs());
  canvas.fillScreen(TFT_BLACK);
  canvas.drawRoundRect(M, fy, fw, fh, 8, TFT_WHITE);
  canvas.drawRoundRect(M + 1, fy + 1, fw - 2, fh - 2, 7, TFT_WHITE);
  canvas.fillRect(bx, by, bw, bh - fill, canvas.color565(30, 30, 30));
  canvas.fillRect(bx, by + bh - fill, bw, fill, drainColor(remS));
  drawTime(remS, W / 2, by + bh / 2, bw - 8, std::min(isTall() ? 60 : 90, bh - 4));
  if (isTall()) drawStatus(1, top_left, top_right);
  else drawStatus(H - 1, bottom_left, bottom_right);
  noticeShown = (int32_t)(noticeUntil - millis()) > 0;
  if (noticeShown) {  // banner over the time
    const char* msg = noticeMsg;
    canvas.setFont(&fonts::Font4);
    canvas.setTextSize(1);
    canvas.setTextSize(std::min(1.0f, (float)(bw - 16) / canvas.textWidth(msg)));
    canvas.setTextDatum(middle_center);
    int tw = canvas.textWidth(msg) + 16, th = canvas.fontHeight() + 12;
    canvas.fillRoundRect(W / 2 - tw / 2, by + bh / 2 - th / 2, tw, th, 6, TFT_BLACK);
    canvas.drawRoundRect(W / 2 - tw / 2, by + bh / 2 - th / 2, tw, th, 6, TFT_WHITE);
    canvas.setTextColor(noticeOn ? TFT_GREEN : TFT_RED);
    canvas.drawString(msg, W / 2, by + bh / 2);
  }
  canvas.pushSprite(0, 0);
  lastDrawn = remS;
}

static void setLayout(uint8_t l) {
  layout = l;
  imuTapSensitive(isTapPose());
  M5.Display.setRotation(prof().rotation[l]);
  canvas.deleteSprite();
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
}

// AXP192 power key events since the last call: 2 = short press, 1 = long press, 0 = none. Always
// clears the latched status, so presses made while the screen is off are dropped. The AXP192 flags a
// long press once, at 1.5 s while the key is still held, and reports no release.
enum : uint8_t { KEY_NONE = 0, KEY_LONG = 1, KEY_SHORT = 2 };
static uint32_t keyDownAt = 0;
static bool keyWasDown = false, keyLongSent = false;
static uint8_t powerKey() {
  if (axp()) {
    uint8_t st = rd(AXP, 0x46) & 0x03;
    if (st) wr(AXP, 0x46, 0x03);
    return st & KEY_SHORT ? KEY_SHORT : st;
  }
  if (prof().keyPin < 0) return KEY_NONE;  // e.g. the StickS3: its PMIC owns the power button
  // Power key on a GPIO (StickC Plus2 button C), polled: long press once held for 1.5 s, short press
  // on release. Like the AXP192 path, a long press is reported while the key is still held.
  const bool down = digitalRead(prof().keyPin) == LOW;
  uint8_t ev = KEY_NONE;
  if (down && !keyWasDown) { keyDownAt = millis(); keyLongSent = false; }
  if (down && !keyLongSent && millis() - keyDownAt >= 1500) { keyLongSent = true; ev = KEY_LONG; }
  if (!down && keyWasDown && !keyLongSent) ev = KEY_SHORT;
  keyWasDown = down;
  return ev;
}

static void notice(const char* msg, bool on) {
  noticeMsg = msg;
  noticeOn = on;
  noticeUntil = millis() + NOTICE_MS;
  Serial.println(msg);
}

// Short press: sound on/off, saved at once. Long press: tap-to-wake in the horizontal poses on/off,
// applied at once but saved only after the 6 s power-off hold would have cut the power. Holding the
// key to switch the device off therefore leaves the saved setting unchanged.
constexpr uint32_t TAP_SAVE_DELAY_MS = 6000;
static bool     tapSavePending = false;
static uint32_t tapSaveAt = 0;

static void applyPowerKey(uint8_t key) {
  if (key == KEY_SHORT) {
    soundOn = !soundOn;
    if (hasBuzzer()) notice(soundOn ? "SOUND ON" : "SOUND OFF", soundOn);
    else notice(soundOn ? "LED ON" : "LED OFF", soundOn);
    saveSettings();
  } else {
    tapFlat = !tapFlat;
    imuTapSensitive(isTapPose());
    notice(tapFlat ? "TAP ON" : "TAP OFF", tapFlat);
    tapSavePending = true;
    tapSaveAt = millis() + TAP_SAVE_DELAY_MS;
  }
}

// Saves a pending tap setting once due (or right away with `now`, e.g. before deep sleep).
static void flushTapSave(bool now = false) {
  if (tapSavePending && (now || (int32_t)(millis() - tapSaveAt) >= 0)) {
    tapSavePending = false;
    saveSettings();
  }
}

// Caps a light-sleep duration so that a pending tap setting is saved on time.
static uint32_t untilTapSave(uint32_t ms) {
  if (!tapSavePending) return ms;
  int32_t left = (int32_t)(tapSaveAt - millis());
  return std::min<uint32_t>(ms, left > 0 ? left : 1);
}

static void screenWake(int remS, uint32_t ms = SCREEN_ON_MS) {
  uint32_t until = millis() + ms;
  if (!screenOn || (int32_t)(until - screenOffAt) > 0) screenOffAt = until;  // extend, never shorten
  if (!screenOn) {
    powerKey();  // drop presses made while the screen was off
    battery = readBattery();
    M5.Display.wakeup();
    render(remS);
    M5.Display.setBrightness(brightness());
    screenOn = true;
    Accel a = readAccel();
    Serial.printf("screen on: %02d:%02d layout=%d bat=%d%% motion=%u acc=%.2f,%.2f,%.2f\n", remS / 60,
                  remS % 60, layout, battery, motionEvents, a.x, a.y, a.z);
  } else if (remS != lastDrawn) {
    render(remS);
  }
}

// Red LED, on boards that signal with it instead of sound (StickC). Released to input when off.
static void led(bool on) {
  if (hasBuzzer() || prof().ledPin < 0) return;
  if (on) { pinMode(prof().ledPin, OUTPUT); digitalWrite(prof().ledPin, prof().ledActiveLow ? LOW : HIGH); }
  else pinMode(prof().ledPin, INPUT);
}

static void screenSleep() {
  led(false);
  M5.Display.setBrightness(0);  // AXP192 LDO2 (backlight) off
  M5.Display.sleep();           // ST7789 SLPIN
  screenOn = false;
}

// ---------- state transitions ----------
static int remainingS() {
  int64_t r = endMs - rtcSeconds() * 1000;
  return r <= 0 ? 0 : (int)((r + 999) / 1000);
}

// Ends the FINISHED blink: steady backlight and the normal 5 s screen time.
static void steadyScreen() {
  led(false);
  if (!screenOn) return;
  M5.Display.setBrightness(brightness());
  screenOffAt = millis() + SCREEN_ON_MS;
}

// Starts the current mode from the current RTC second instead of rtcMsAt(): no 1 s wait, so a quick
// second button press is not missed. The display then starts exactly at MM:05; the session may be
// up to 1 s shorter, which does not matter.
static void startSession() {
  steadyScreen();
  rtcMagic = MAGIC;
  state = RUNNING;
  endMs = rtcSeconds() * 1000 + sessionMs() + GRACE_MS;
  inGrace = true;
  lastMinShown = lastDrawn = -1;
  Serial.printf("%s: new %d min session\n", mode == WORK ? "WORK" : "BREAK", lengthMin[mode]);
}

// Button B: the first press shows the current length, each further press adds 1 min (auto-repeat
// while held), wrapping 60 -> 1. After 1 s without presses: if the length was changed, it is saved
// and a new session starts; a single look-only press shows the length for that second, then the
// countdown for the usual 5 s, without touching the session or the FINISHED blink.
static void bumpLength() {
  lengthMin[mode] = lengthMin[mode] % 60 + 1;
  screenWake(lengthMin[mode] * 60);
}

static void adjustLength() {
  const bool wasOn = screenOn;
  const uint32_t prevOffAt = screenOffAt;
  steadyScreen();
  setting = true;
  const uint8_t before = lengthMin[mode];
  screenWake(before * 60);  // the first press only shows the current length
  render(before * 60);
  bool first = true, changed = false;
  uint32_t last = millis();
  do {
    if (digitalRead(PIN_BTN_B()) == LOW) {
      if (!first) { bumpLength(); changed = true; }
      first = false;
      uint32_t repeatAt = millis() + 500;
      while (digitalRead(PIN_BTN_B()) == LOW) {
        nap(20, false);
        if ((int32_t)(millis() - repeatAt) >= 0) { bumpLength(); changed = true; repeatAt = millis() + 120; }
      }
      last = millis();
    }
    nap(20, false);
  } while (millis() - last < SET_TIMEOUT_MS);
  setting = false;
  if (changed) {
    if (lengthMin[mode] != before) saveSettings();
    startSession();
    return;
  }
  // Look only: back to the countdown (it kept running) with the usual 5 s screen time; the FINISHED
  // blink goes on with its 2 min left unchanged.
  if (state == FINISHED && wasOn) {
    screenOffAt = prevOffAt;
    render(0);
  } else {
    screenOffAt = millis() + SCREEN_ON_MS;
    render(state == RUNNING ? remainingS() : 0);
  }
}

static void resume(uint32_t since) {
  endMs = rtcMsAt(since) + pausedRemMs;
  state = RUNNING;
  lastMinShown = -1;
  Serial.printf("RUNNING: resumed, %ld ms left\n", (long)pausedRemMs);
}

// PAUSED / FINISHED: deep sleep until the IMU sees motion (or button A when finished).
[[noreturn]] static void deepSleep() {
  flushTapSave(true);
  if (axp()) {
    M5.In_I2C.bitOff(AXP, 0x12, 1 << 3, I2C_HZ);  // LCD logic power (LDO3) off
    wr(AXP, 0x46, 0x03);                           // drop pending power-key presses
  }
  if (prof().power == Power::HOLD_GPIO4) {  // keep the power switched on through deep sleep
    gpio_hold_en(GPIO_NUM_4);
    gpio_deep_sleep_hold_en();
  }
  imuTapSensitive(false);
  imuMotion();                                   // release INT before arming
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (womOk) esp_sleep_enable_ext0_wakeup(PIN_INT(), 0);
  if (state == FINISHED) esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_A(), ESP_EXT1_WAKEUP_ALL_LOW);
  flushLog();
  esp_deep_sleep_start();
}

static void pause(uint32_t since) {
  pausedRemMs = (int32_t)std::max<int64_t>(0, endMs - rtcMsAt(since));
  state = PAUSED;
  screenSleep();
  Serial.printf("PAUSED: %ld ms left\n", (long)pausedRemMs);
}

// StickS3 speaker amplifier enable: M5PM1 G3 as a push-pull output, high = on. Each register is
// written and read back, up to 5 tries (100 kHz, as M5Unified talks to this PMIC).
static bool pm1Bits(bool on) {
  constexpr uint8_t PM1 = 0x6E, G3 = 1 << 3;
  struct { uint8_t reg; bool set; } steps[] = {{0x16, false}, {0x10, true}, {0x13, false}, {0x11, on}};
  bool ok = true;
  for (auto& st : steps) {
    bool done = false;
    for (int i = 0; i < 5 && !done; i++) {
      if (st.set) M5.In_I2C.bitOn(PM1, st.reg, G3, 100000);
      else M5.In_I2C.bitOff(PM1, st.reg, G3, 100000);
      const uint8_t v = M5.In_I2C.readRegister8(PM1, st.reg, 100000);
      done = ((v & G3) != 0) == st.set;
      if (!done) delay(2);
    }
    ok = ok && done;
  }
  return ok;
}

// The buzzer is powered from the AXP192 5 V boost (EXTEN), so it is enabled only while beeping.
static void beep(int n, int onMs, int offMs) {
  if (!soundOn) return;
  if (prof().sound == Sound::LED) {  // original M5StickC: the same pattern on the red LED
    if (prof().ledPin < 0) return;
    pinMode(prof().ledPin, OUTPUT);
    for (int i = 0; i < n; i++) {
      digitalWrite(prof().ledPin, prof().ledActiveLow ? LOW : HIGH);  delay(onMs);
      digitalWrite(prof().ledPin, prof().ledActiveLow ? HIGH : LOW); if (i + 1 < n) delay(offMs);
    }
    pinMode(prof().ledPin, INPUT);
    return;
  }
  if (prof().sound == Sound::SPEAKER) {  // codec + speaker (StickS3), started only for the beep
    if (!M5.Speaker.begin()) Serial.println("speaker: begin failed");
    // The amplifier is switched by the M5PM1's G3. M5Unified sets it with single writes, which the PMIC
    // sometimes misses (its I2C answers late after idling): the beep then was only a click. Set it here,
    // read back, retried.
    if (!pm1Bits(true)) Serial.println("speaker: amplifier enable failed");
    M5.Speaker.setVolume(90);  // quieter than the buzzer boards' piezo would allow; 255 = max
    delay(40);  // codec and amplifier settle; a tone at once was sometimes lost
    // The speaker clips the start of a tone, so the shortest signals (the 80 ms BREAK start) sounded
    // cut off: here they last at least 140 ms, still clearly shorter than the 250 ms WORK start.
    onMs = std::max(onMs, 140);
    for (int i = 0; i < n; i++) {
      M5.Speaker.tone(4000, onMs);
      delay(onMs + (i + 1 < n ? offMs : 0));
    }
    while (M5.Speaker.isPlaying()) delay(5);
    delay(20);
    M5.Speaker.end();
    pm1Bits(false);
    return;
  }
  const bool exten = prof().sound == Sound::BUZZER_EXTEN;
  if (exten) axpExten(true);
  delay(20);
  ledcSetup(0, 4000, 8);
  ledcAttachPin(prof().buzzerPin, 0);
  for (int i = 0; i < n; i++) {
    ledcWriteTone(0, 4000); delay(onMs);
    ledcWriteTone(0, 0);    if (i + 1 < n) delay(offMs);
  }
  ledcDetachPin(prof().buzzerPin);
  pinMode(prof().buzzerPin, INPUT);
  if (exten) axpExten(false);
}

// End: WORK three short beeps, BREAK two long ones. Start (after the grace period): one beep, longer
// for WORK, shorter for BREAK.
static void endBeep() { mode == WORK ? beep(3, 150, 120) : beep(2, 400, 200); }
static void startBeep() { beep(1, mode == WORK ? 250 : 80, 0); }

static void finish() {
  state = FINISHED;
  finishedDown = false;
  downCount = 0;
  finishLayout = layout;
  poseChanging = false;
  Serial.printf("FINISHED %s\n", mode == WORK ? "WORK" : "BREAK");
  finishedAt = millis();
  screenWake(0, FINISHED_SCREEN_MS);
  endBeep();
}

// ---------- first-boot self-identification ----------
// The board M5Unified detected is checked against the fingerprint in its PROFILES row. If anything
// does not fit, the timer does not run: refuse() says why. Codes (also in the README):
//   E1 no motion sensor (IMU)          E2 motion sensor reading implausible   E3 power chip mismatch
//   E4 display missing / other size    E5 RTC missing or not running          E6 audio codec missing
//   E9 unknown board (no profile)
struct Check { const char* code; const char* why; const char* looksLike; };

static Check identify(bool boardKnown) {
  const BoardProfile& p = prof();
  if (!boardKnown) return {"E9", "Unknown board", nullptr};
  const int lng = std::max(M5.Display.width(), M5.Display.height());
  const int sht = std::min(M5.Display.width(), M5.Display.height());
  if (lng != p.panelLong || sht != p.panelShort) return {"E4", "Display missing or of another size", nullptr};
  const bool hasAxp = M5.In_I2C.scanID(0x34), hasPm1 = M5.In_I2C.scanID(0x6E);
  if (p.pmicAddr ? !M5.In_I2C.scanID(p.pmicAddr) : (hasAxp || hasPm1))
    return {"E3", "Power chip does not match", nullptr};
  bool imuThere = p.imu == Imu::MPU6886 ? M5.In_I2C.scanID(MPU) && rd(MPU, 0x75) == 0x19
                                        : M5.Imu.getType() == p.imuType;
#ifdef FAKE_NO_IMU  // test build: behave like a board without an IMU (env test-unsupported)
  imuThere = false;
#endif
  if (!imuThere) {
    const char* like = nullptr;
    if (p.board == m5::board_t::board_M5StickCPlus) like = "M5StickC Plus SE, which has no motion sensor";
    if (p.board == m5::board_t::board_M5StickC && M5.In_I2C.scanID(0x6C)) like = "an early M5StickC with the SH200Q sensor";
    return {"E1", "No motion sensor (IMU)", like};
  }
  imuInit();
  // Give the sensor up to ~1 s for its first sample: right after M5.begin() the BMI270 (StickS3) has
  // no data yet and reads 0 g.
  Accel a = readAccel();
  float g = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
  for (int i = 0; i < 50 && (g < 0.6f || g > 1.4f); i++) {
    delay(20);
    a = readAccel();
    g = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
  }
  if (g < 0.6f || g > 1.4f) {
    Serial.printf("IMU reading: %.2f,%.2f,%.2f (%.2f g)\n", a.x, a.y, a.z, g);
    return {"E2", "Motion sensor reading is implausible", nullptr};
  }
  if (p.clock == Clock::BM8563) {
    if (!M5.In_I2C.scanID(RTC_ADDR)) return {"E5", "Clock chip (RTC) missing", nullptr};
    rtcInit();
    const int64_t s0 = rtcSeconds();
    const uint32_t t0 = millis();
    while (rtcSeconds() == s0 && millis() - t0 < 1300) nap(20, false);
    if (rtcSeconds() == s0) return {"E5", "Clock chip (RTC) is not running", nullptr};
  }
  if (p.codec && !M5.In_I2C.scanID(0x18)) return {"E6", "Audio codec missing", nullptr};
  Serial.printf("IDENTIFIED: %s%s (panel %dx%d, IMU ok %.2f g, RTC %s, power chip %s)\n", p.name,
                p.tested ? "" : " UNTESTED", lng, sht, g, p.clock == Clock::BM8563 ? "running" : "none (system time)",
                p.pmicAddr == 0x34 ? "AXP192" : p.pmicAddr == 0x6E ? "M5PM1" : "none");
  return {nullptr, nullptr, nullptr};
}

// Unsupported board: no timer. Says why on the screen (if there is one), beeps five times (only on a
// recognised board, whose buzzer / LED pins are known: nothing is driven on an unknown board), logs
// "UNSUPPORTED: ...". It never powers off or deep-sleeps while it may be on USB, so the port stays
// there for flashing something else; on battery (AXP192 boards) it powers off after 2 minutes.
// Refusal screen idle: light sleep, except on the ESP32-S3, whose USB is inside the chip and stops
// answering in light sleep, so the port could not be used to flash something else.
static void refuseWait(uint32_t ms) {
#if CONFIG_IDF_TARGET_ESP32S3
  delay(ms);
#else
  nap(ms, false);  // timer-only light sleep: no pins are armed
#endif
}

[[noreturn]] static void refuse(const Check& c, bool boardKnown) {
  Serial.printf("UNSUPPORTED: %s %s%s%s\n", c.code, c.why, c.looksLike ? " - looks like " : "",
                c.looksLike ? c.looksLike : "");
  const bool display = M5.Display.width() > 0;
  auto show = [&]() {
    if (!display) return;
    auto& d = M5.Display;
    d.setRotation(1);
    d.wakeup();
    d.fillScreen(TFT_BLACK);
    const bool small = d.width() < 200;
    int y = 4;
    // Draws `text`, wrapped to the screen width: after a space, or after a '/' in a long URL.
    auto line = [&](const char* text, uint16_t color, const lgfx::IFont* font) {
      d.setFont(font);
      d.setTextSize(1);
      d.setTextColor(color);
      d.setTextDatum(top_left);
      String rest = text;
      while (rest.length()) {
        int cut = rest.length();
        while (cut > 1 && d.textWidth(rest.substring(0, cut)) > d.width() - 8) {
          int sp = -1;
          for (int i = cut - 1; i > 0 && sp < 0; i--) if (rest[i] == ' ' || rest[i - 1] == '/') sp = i;
          cut = sp > 0 ? sp : cut - 1;
        }
        d.drawString(rest.substring(0, cut), 4, y);
        y += d.fontHeight() + 2;
        rest = rest.substring(cut);
        rest.trim();
      }
    };
    char buf[64];
    const lgfx::IFont* title = small ? (const lgfx::IFont*)&fonts::Font2 : (const lgfx::IFont*)&fonts::Font4;
    const lgfx::IFont* body = small ? (const lgfx::IFont*)&fonts::Font0 : (const lgfx::IFont*)&fonts::Font2;
    line("Unsupported device", TFT_RED, title);
    snprintf(buf, sizeof buf, "%s: %s", c.code, c.why);
    line(buf, TFT_WHITE, body);
    if (c.looksLike) { snprintf(buf, sizeof buf, "Looks like %s", c.looksLike); line(buf, TFT_YELLOW, body); }
    line("The Pomodoro timer cannot run here.", TFT_LIGHTGREY, body);
    line("github.com/romanaerl/M5StickPomodoro", TFT_LIGHTGREY, body);
    d.setBrightness(brightness());
  };
  show();
  soundOn = true;
  if (boardKnown) beep(5, 120, 120);
  uint32_t shownAt = millis(), lastBeep = millis();
  bool lit = display;
  for (;;) {
    if (!display && boardKnown && millis() - shownAt < 60000 && millis() - lastBeep >= 10000) {
      beep(5, 120, 120);  // no screen: repeat the signal for a minute
      lastBeep = millis();
    }
    if (millis() - shownAt >= 120000) {
      if (lit) { M5.Display.setBrightness(0); M5.Display.sleep(); lit = false; }
      if (boardKnown && axp() && !axpUsbPower()) M5.Power.powerOff();  // on battery: off
    }
    if (boardKnown && btnDown(PIN_BTN_A())) {  // M5 button: show the message and signal again
      show();
      lit = display;
      shownAt = lastBeep = millis();
      beep(5, 120, 120);
      while (btnDown(PIN_BTN_A())) refuseWait(20);
    }
    refuseWait(200);
  }
}

// ---------- main ----------
void setup() {
  setCpuFrequencyMhz(80);
  auto cause = esp_sleep_get_wakeup_cause();
  bool warm = rtcMagic == MAGIC && (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1);
#if CONFIG_IDF_TARGET_ESP32S3
  constexpr bool S3 = true;
  const int sda = warm ? prof().sda : 47, scl = warm ? prof().scl : 48;
#else
  constexpr bool S3 = false;
  const int sda = warm ? prof().sda : 21, scl = warm ? prof().scl : 22;
#endif
  // Internal I2C before M5.begin(): the profile chosen at cold boot after a wake, else this chip's bus.
  M5.In_I2C.begin(I2C_NUM_1, (gpio_num_t)sda, (gpio_num_t)scl);

  // Fast path after a motion wake: decide from the pose before touching the display.
  uint32_t since = millis();
  if (warm && cause == ESP_SLEEP_WAKEUP_EXT0) {
    int pose = settledPose(&since);
    if (state == FINISHED && pose == -1) finishedDown = true;
    bool wake = pose == 1 && (state == PAUSED || (state == FINISHED && finishedDown));
    if (!wake) { sleepWakes++; deepSleep(); }
  }

  if (!S3 && (!warm || axp())) {  // a cold boot does not know the board yet: a missing AXP192 just NACKs
    M5.In_I2C.bitOn(AXP, 0x12, 1 << 3, I2C_HZ);  // LCD logic power (LDO3) on
    delay(5);
  }
  Serial.begin(115200);
#if CONFIG_IDF_TARGET_ESP32S3
  Serial.setTxTimeoutMs(0);  // no computer reading the USB log: drop it rather than wait
#endif

  auto cfg = M5.config();
  cfg.output_power = false;  // 5 V boost (EXTEN) off
  cfg.internal_imu = S3;     // ESP32 boards: MPU6886 configured manually for low-power WOM; S3: M5.Imu
  cfg.internal_mic = false;
  cfg.internal_spk = S3;     // S3: speaker codec (started only for beeps)
  cfg.clear_display = true;
  M5.begin(cfg);
#if CONFIG_IDF_TARGET_ESP32S3
  M5.Speaker.end();
#else
  adc_power_release();  // M5Unified keeps the SAR ADC powered for GPIO36/39 debouncing
#endif
  if (!warm) {  // pick the board profile (deep-sleep wakes keep the one chosen at cold boot)
    int found = -1, fallback = -1;
    for (size_t i = 0; i < sizeof PROFILES / sizeof PROFILES[0]; i++) {
      if (PROFILES[i].s3 != S3) continue;
      if (fallback < 0) fallback = i;
      if (PROFILES[i].board == M5.getBoard()) found = i;
    }
    profileIdx = found >= 0 ? found : fallback;
    Serial.printf("\nPomodoro first boot: board=%d profile=%s\n", (int)M5.getBoard(), found >= 0 ? prof().name : "none");
    // Check the hardware before touching any board-specific pin or chip; refuse() does not return.
    const Check c = identify(found >= 0);
    if (c.code) refuse(c, found >= 0);
  }
  if (prof().power == Power::HOLD_GPIO4) gpio_hold_dis(GPIO_NUM_4);  // M5.begin() drives it HIGH again
  if (pm1Imu()) pm1ImuInit();
#ifdef BRIGHTNESS_SWEEP  // env s3-brightness: steps the normal-screen backlight, 3 s per raw duty value
  if (!axp()) {
    static const uint16_t duties[] = {17, 24, 34, 48, 64, 80, 96, 122, 160};  // of 512; 122 = setBrightness(48)
    auto& d = M5.Display;
    d.wakeup();
    d.setRotation(1);
    for (;;) {
      for (uint16_t duty : duties) {
        d.fillScreen(TFT_BLACK);
        d.drawRoundRect(3, 3, d.width() - 6, d.height() - 24, 8, TFT_WHITE);
        d.fillRect(8, 8 + (d.height() - 34) / 4, d.width() - 16, (d.height() - 34) * 3 / 4, TFT_GREEN);
        d.setTextDatum(middle_center);
        d.setFont(&fonts::FreeSansBold24pt7b);
        d.setTextColor(TFT_WHITE);
        d.drawString("18:42", d.width() / 2, (d.height() - 21) / 2);
        d.setFont(&fonts::Font2);
        d.setTextColor(TFT_LIGHTGREY);
        char buf[24];
        snprintf(buf, sizeof buf, "%u / 512", duty);
        d.drawString(buf, d.width() / 2, d.height() - 10);
        ledcWrite(7, duty);
        Serial.printf("backlight duty %u/512\n", duty);
        delay(3000);
      }
    }
  }
#endif
  serialFollowUsb();
  M5.Display.setBrightness(0);
  pinMode(PIN_BTN_A(), INPUT);
  pinMode(PIN_BTN_B(), INPUT);
  if (prof().intPin >= 0) pinMode(PIN_INT(), INPUT);
  if (prof().keyPin >= 0) pinMode(prof().keyPin, INPUT);

  Serial.printf("\nPomodoro boot: wake cause=%d warm=%d board=%d sleep wakes=%u\n", cause, warm,
                (int)M5.getBoard(), sleepWakes);
  sleepWakes = 0;

  if (!warm) {  // cold boot or reset: always a new WORK session
    if (!prof().tested) Serial.printf("Profile %s is UNTESTED on a real device: see PORTING.md\n", prof().name);
    if (axp()) wr(AXP, 0x36, (rd(AXP, 0x36) & ~0x30) | 0x10);  // power key long press = 1.5 s
    // identify() has already initialised the IMU and the RTC.
    womOk = imuSelfTest();
    loadSettings();
    mode = WORK;
    startSession();
    Serial.printf("profile=%s battery=%d%% womOk=%d work=%d break=%d sound=%d tap=%d\n", prof().name,
                  M5.Power.getBatteryLevel(), womOk, lengthMin[WORK], lengthMin[BREAK], soundOn, tapFlat);
  } else if (state == PAUSED) {
    resume(since);
  } else if (state == FINISHED) {  // button A, or face down and back up: same mode again
    startSession();
    if (cause == ESP_SLEEP_WAKEUP_EXT1) lastPressAt = millis();  // a second press switches the mode
  }

  int l = layoutOf(readAccel());
  setLayout(l < 0 ? FLAT : l);
}

// 0 = off, else AXP192 LDO2 = 1.8 V + 0.1 V * level. PWM backlights (no AXP192) get level * 4 / 255
// instead: dimmer than their normal brightness, as on the AXP192 boards.
static void backlightLevel(uint8_t level) {
  if (!axp()) { M5.Display.setBrightness(level * 4); return; }  // 28..32: below BRIGHTNESS_PWM, like LDO2 7..8
  if (!level) { M5.In_I2C.bitOff(AXP, 0x12, 1 << 2, I2C_HZ); return; }
  wr(AXP, 0x28, (rd(AXP, 0x28) & 0x0F) | (level << 4));
  M5.In_I2C.bitOn(AXP, 0x12, 1 << 2, I2C_HZ);
}

// Minute-mark glance: static minutes, backlight steps up then down. A button press or turning face
// down cuts it short so that runStep() can react.
static void glance(int remS) {
  powerKey();  // drop presses made while the screen was off
  minutesOnly = true;
  battery = readBattery();
  M5.Display.wakeup();
  render(remS);
  minutesOnly = false;
  Serial.printf("glance: %d min\n", (remS + 59) / 60);
  uint8_t key = KEY_NONE;
  bool stop = false;
  if (!axp()) {  // PWM backlight (StickS3): a smooth fade, written to the LEDC channel directly, since
                 // setBrightness(1) is already 34/512: 2 s up to GLANCE_PWM_PEAK, 1 s there, 2 s down
    constexpr uint32_t UP_MS = 2000, HOLD_MS = 1000, TICK_MS = 20;
    for (uint32_t t = 0; t <= 2 * UP_MS + HOLD_MS && !stop; t += TICK_MS) {
      const uint32_t ramp = t < UP_MS ? t : t < UP_MS + HOLD_MS ? UP_MS : 2 * UP_MS + HOLD_MS - t;
      ledcWrite(M5GFX_BACKLIGHT_CH, GLANCE_PWM_PEAK * ramp / UP_MS);
      delay(TICK_MS);  // awake: light sleep would stop the PWM
      if (t % 200 == 0) {
        key = powerKey();
        stop = key || digitalRead(PIN_BTN_A()) == LOW || digitalRead(PIN_BTN_B()) == LOW || readAccel().z < DOWN_G;
      }
    }
  } else
  for (uint8_t level : GLANCE_LEVELS) {
    backlightLevel(level);
    for (int i = 0; i < 5 && !stop; i++) {  // poll every 200 ms
      nap(GLANCE_STEP_MS / 5, true, false);
      key = powerKey();
      stop = key || digitalRead(PIN_BTN_A()) == LOW || digitalRead(PIN_BTN_B()) == LOW || readAccel().z < DOWN_G;
    }
    if (stop) break;
  }
  lastDrawn = -1;
  if (key) {  // the screen was visible: switch to the normal screen with the banner
    applyPowerKey(key);
    screenWake(remS);
    return;
  }
  backlightLevel(0);
  M5.Display.sleep();
}

// Button A restarts, button B sets the length (returns true: a new session was started). The power
// key works only while the screen is on.
static bool handleButtons(int remS) {
  uint8_t key = powerKey();
  if (key && screenOn) {
    applyPowerKey(key);
    screenWake(remS);
    render(remS);
  }
  if (digitalRead(PIN_BTN_A()) == LOW) {
    // M5 button, decided on release: a press restarts (a second one within 5 s switches the mode), held
    // 1.5 s switches the sound, held 3 s tap-to-wake. Works on every board; on the StickS3 it is the only
    // way, since its power button belongs to the PMIC. While held, the screen previews the change.
    const uint32_t t0 = millis();
    bool hintSound = false, hintTap = false;
    while (digitalRead(PIN_BTN_A()) == LOW) {
      const uint32_t held = millis() - t0;
      if (!hintSound && held >= HOLD_SOUND_MS) {
        hintSound = true;
        if (hasBuzzer()) notice(soundOn ? "SOUND OFF" : "SOUND ON", !soundOn);
        else notice(soundOn ? "LED OFF" : "LED ON", !soundOn);
        screenWake(remS);
        render(remS);
      }
      if (!hintTap && held >= HOLD_TAP_MS) {
        hintTap = true;
        notice(tapFlat ? "TAP OFF" : "TAP ON", !tapFlat);
        render(remS);
      }
      nap(20, false);
    }
    const uint32_t held = millis() - t0;
    if (held >= HOLD_SOUND_MS) {
      applyPowerKey(held >= HOLD_TAP_MS ? KEY_LONG : KEY_SHORT);
      screenWake(remS);
      render(remS);
      return true;
    }
    if (millis() - lastPressAt < MODE_SWITCH_MS) mode ^= 1;  // second press during the grace period
    startSession();
    lastPressAt = millis();
    return true;
  }
  if (digitalRead(PIN_BTN_B()) == LOW) {  // GPIO39 can glitch: require a held level
    nap(20, false);
    if (digitalRead(PIN_BTN_B()) == LOW) { adjustLength(); return true; }
  }
  return false;
}

// Layout follows the pose once it has been stable for a few samples.
static void followLayout(const Accel& a, int remS) {
  int l = layoutOf(a);
  if (l < 0 || l == layout) {
    layoutCount = 0;
  } else if (l != pendingLayout) {
    pendingLayout = l;
    layoutCount = 1;
  } else if (++layoutCount >= CONFIRM_SAMPLES) {
    layoutCount = 0;
    setLayout(l);
    if (screenOn) render(remS);
    screenWake(remS);
  }
}

static void runStep() {
  int remS = remainingS();
  if (remS == 0) { finish(); return; }
  if (inGrace && remS * 1000 <= sessionMs()) {  // grace over: the session proper starts
    inGrace = false;
    if (screenOn) render(remS);
    startBeep();
  }
  if (handleButtons(remS)) return;

  if (imuMotion()) {
    motionEvents++;
    motionUntil = millis() + 1500;
    if (isTapPose()) screenWake(remS);  // a tap shows the time
  }
  Accel a = readAccel();
  if (a.z < DOWN_G) {
    if (downCount++ == 0) downSince = millis();
    if (downCount >= CONFIRM_SAMPLES) { downCount = 0; pause(downSince); return; }
  } else {
    downCount = 0;
  }
  followLayout(a, remS);

  int ceilMin = (remS + 59) / 60;
  if (ceilMin != lastMinShown) {
    bool scheduled = lastMinShown >= 0 && !screenOn;  // not the first show of a session / resume
    lastMinShown = ceilMin;
    if (scheduled) { glance(remS); return; }  // ~5 s passed: recompute before sleeping
    screenWake(remS);
  }
  if (screenOn) {
    if (remS != lastDrawn || (noticeShown && (int32_t)(millis() - noticeUntil) >= 0)) render(remS);
    if ((int32_t)(millis() - screenOffAt) >= 0) screenSleep();
  }

  int toNextMin = remS - (ceilMin - 1) * 60;  // 1..60 s
  uint32_t ms = toNextMin <= 1 ? 100 : (toNextMin - 1) * 1000;
  if (screenOn) ms = 200;
  bool moving = (int32_t)(motionUntil - millis()) > 0;
  if (downCount || layoutCount || moving) ms = std::min<uint32_t>(ms, CONFIRM_STEP_MS);
  if (!womOk) ms = std::min<uint32_t>(ms, 1000);  // fallback: poll orientation
  flushTapSave();
  nap(untilTapSave(ms), true, !moving);
}

// FINISHED with the screen on (blinking): buttons and layout still work; face down or timeout -> deep sleep.
// Deep sleep needs wake-on-motion; on the ESP32-S3 it would also drop the USB port, so not while on USB.
static bool canDeepSleep() { return womOk && !s3OnUsb(); }

// FINISHED, screen off, without deep sleep (no wake-on-motion, or a StickS3 on USB): deep sleep could only wake on the
// M5 button, so the pose is polled instead, once a second: face down and back up, or the M5 button,
// starts the same mode again, as a motion wake from deep sleep does on the other boards.
static void finishedIdleStep() {
  if (btnDown(PIN_BTN_A())) {
    startSession();
    lastPressAt = millis();  // a second press switches the mode
    return;
  }
  uint32_t since;
  const int pose = confirmPose(&since);
  if (pose == -1) finishedDown = true;
  else if (pose == 1 && finishedDown) { startSession(); return; }
  nap(1000, true, false);
}

static void finishedStep() {
  if (!screenOn) {
    if (!canDeepSleep()) { finishedIdleStep(); return; }
    finishedDown = readAccel().z < DOWN_G;
    deepSleep();
  }
  if (handleButtons(0)) return;
  if (noticeShown && (int32_t)(millis() - noticeUntil) >= 0) render(0);
  imuMotion();
  Accel a = readAccel();
  if (a.z < DOWN_G && ++downCount >= CONFIRM_SAMPLES) {
    screenSleep();
    finishedDown = true;
    if (canDeepSleep()) deepSleep();
    return;
  }
  if (a.z >= DOWN_G) downCount = 0;
  int l = layoutOf(a);  // a new pose held for POSE_SWITCH_MS starts the other mode
  if (l < 0 || l == finishLayout) {
    poseChanging = false;
  } else if (!poseChanging) {
    poseChanging = true;
    poseSince = millis();
  } else if (millis() - poseSince >= POSE_SWITCH_MS) {
    mode ^= 1;
    startSession();
    return;
  }
  followLayout(a, 0);
  if ((int32_t)(millis() - screenOffAt) >= 0) { screenSleep(); return; }
  uint32_t t = millis() - finishedAt;
  const bool lit = (t / BLINK_MS) % 2 == 0;
  M5.Display.setBrightness(lit ? brightness() : 0);  // backlight only, panel stays awake
  led(lit && soundOn);                             // StickC: the LED blinks along
  flushTapSave();
  nap(std::min<uint32_t>(CONFIRM_STEP_MS, BLINK_MS - t % BLINK_MS), true, false);
}

static void pausedStep() {
  uint32_t since;
  if (confirmPose(&since) == 1) { resume(since); return; }
  if (canDeepSleep()) deepSleep();
  nap(1000, false);
}

void loop() {
  serialFollowUsb();
  switch (state) {
    case RUNNING:  runStep(); break;
    case PAUSED:   pausedStep(); break;
    default:       finishedStep();
  }
}
