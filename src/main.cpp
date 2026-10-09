// Low-power Pomodoro timer for M5StickC Plus (ESP32-PICO-D4, AXP192, MPU6886, BM8563, ST7789).
// Also runs on the original M5StickC (80x160 ST7735S, no buzzer): detected at boot, it gets a compact
// status line and signals with the red LED instead of beeps (the LED also blinks with the FINISHED
// screen, and the power key's short press switches the LED instead of the sound). Same chips and
// axes otherwise.
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

#ifndef SESSION_MIN
#define SESSION_MIN 25  // default WORK length, minutes
#endif
#ifndef BREAK_MIN
#define BREAK_MIN 5     // default BREAK length, minutes
#endif
constexpr uint32_t MODE_SWITCH_MS = 5000;  // second button-A press within this switches the mode
constexpr int32_t  GRACE_MS = 5000;        // shown in yellow before the session proper starts
constexpr uint32_t POSE_SWITCH_MS = 1000;  // FINISHED: a new pose held this long starts the other mode
constexpr uint32_t SET_TIMEOUT_MS = 1000;
constexpr uint32_t SCREEN_ON_MS = 5000;
constexpr uint32_t FINISHED_SCREEN_MS = 2 * 60 * 1000;  // blinking, unless turned face down
constexpr uint32_t BLINK_MS = 500;
constexpr uint32_t NOTICE_MS = 2000;  // "SOUND ON/OFF" banner
constexpr uint8_t  BRIGHTNESS   = 96;
// Minute-mark glance: backlight steps through these AXP192 LDO2 levels (1.8 V + 0.1 V * level), one
// per second. Level 7 is the first one visible on the device.
constexpr uint8_t  GLANCE_LEVELS[] = {7, 8, 8, 8, 7};
constexpr uint32_t GLANCE_STEP_MS  = 1000;

constexpr gpio_num_t PIN_INT   = GPIO_NUM_35;  // MPU6886 INT (shared with BM8563 INT), active low
constexpr gpio_num_t PIN_BTN_A = GPIO_NUM_37;  // front "M5" button, active low
constexpr gpio_num_t PIN_BTN_B = GPIO_NUM_39;  // side button, active low

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
static bool hasBuzzer() { return prof().buzzerPin >= 0; }

// ---------- low level ----------
static void wr(uint8_t addr, uint8_t reg, uint8_t v) { M5.In_I2C.writeRegister8(addr, reg, v, I2C_HZ); }
static uint8_t rd(uint8_t addr, uint8_t reg) { return M5.In_I2C.readRegister8(addr, reg, I2C_HZ); }

// Light sleep. wakeOnIo: buttons wake it; imuWake: the IMU INT line too (off while already polling
// after motion, otherwise a re-latching INT turns the sleep into a busy loop).
static void nap(uint32_t ms, bool wakeOnIo, bool imuWake = true) {
  imuWake = imuWake && wakeOnIo && womOk;
  Serial.flush();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
  if (wakeOnIo) {
    gpio_wakeup_enable(PIN_BTN_A, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(PIN_BTN_B, GPIO_INTR_LOW_LEVEL);
    if (imuWake) gpio_wakeup_enable(PIN_INT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }
  esp_light_sleep_start();
  if (wakeOnIo) {
    gpio_wakeup_disable(PIN_BTN_A);
    gpio_wakeup_disable(PIN_BTN_B);
    if (imuWake) gpio_wakeup_disable(PIN_INT);
  }
}

// ---------- BM8563 RTC ----------
static uint8_t bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static int64_t rtcSeconds(bool* valid = nullptr) {
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
  int64_t s0 = rtcSeconds(), s = s0;
  uint32_t t0 = millis();
  while (s == s0 && millis() - t0 < 1100) { nap(10, false); s = rtcSeconds(); }
  return s * 1000 - (int64_t)(millis() - eventMillis);
}

static void rtcInit() {
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

static Accel readAccel() {
  uint8_t b[6];
  M5.In_I2C.readRegister(MPU, 0x3B, b, 6, I2C_HZ);
  auto g = [&](int axis) { return (int16_t)(b[2 * axis] << 8 | b[2 * axis + 1]) / 8192.0f; };  // +-4 g
  const BoardProfile& p = prof();
  return {p.sign[0] * g(p.axis[0]), p.sign[1] * g(p.axis[1]), p.sign[2] * g(p.axis[2])};
}

// Layout for a visible pose, or -1 while tilted in between.
static int layoutOf(const Accel& a) {
  if (a.z > AXIS_G) return FLAT;
  if (a.x > AXIS_G) return WIDE;
  if (a.x < -AXIS_G) return WIDE_FLIP;
  if (a.y < -AXIS_G) return TALL;
  if (a.y > AXIS_G) return TALL_FLIP;
  return -1;
}

static bool imuMotion() { return rd(MPU, 0x3A) & 0xE0; }  // reading INT_STATUS releases INT

// Accel-only low-power cycle mode (12.5 Hz) with wake-on-motion on the INT pin.
static void imuInit() {
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
  uint8_t th = on ? WOM_THRESHOLD_TAP : WOM_THRESHOLD;
  wr(MPU, 0x19, on ? ODR_DIV_TAP : ODR_DIV);
  wr(MPU, 0x20, th); wr(MPU, 0x21, th); wr(MPU, 0x22, th);
}

// Verify the INT -> GPIO35 path using the data-ready interrupt (no physical motion needed).
static bool imuSelfTest() {
  pinMode(PIN_INT, INPUT);
  rd(MPU, 0x3A);
  wr(MPU, 0x38, 0x01);  // data ready -> INT
  delay(100);
  bool low = digitalRead(PIN_INT) == LOW;
  rd(MPU, 0x3A);
  bool released = digitalRead(PIN_INT) == HIGH;
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
static uint8_t powerKey() {
  uint8_t st = rd(AXP, 0x46) & 0x03;
  if (st) wr(AXP, 0x46, 0x03);
  return st & KEY_SHORT ? KEY_SHORT : st;
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
    M5.Display.setBrightness(BRIGHTNESS);
    screenOn = true;
    Accel a = readAccel();
    Serial.printf("screen on: %02d:%02d layout=%d bat=%d%% motion=%u acc=%.2f,%.2f,%.2f\n", remS / 60,
                  remS % 60, layout, battery, motionEvents, a.x, a.y, a.z);
  } else if (remS != lastDrawn) {
    render(remS);
  }
}

// Red LED (StickC only; active low). Released to input when off.
static void led(bool on) {
  if (hasBuzzer()) return;
  if (on) { pinMode(prof().ledPin, OUTPUT); digitalWrite(prof().ledPin, LOW); }
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
  M5.Display.setBrightness(BRIGHTNESS);
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
// while held), wrapping 60 -> 1. The session starts 1 s after the last press.
static void bumpLength() {
  lengthMin[mode] = lengthMin[mode] % 60 + 1;
  screenWake(lengthMin[mode] * 60);
}

static void adjustLength() {
  steadyScreen();
  setting = true;
  const uint8_t before = lengthMin[mode];
  screenWake(before * 60);  // the first press only shows the current length
  render(before * 60);
  bool first = true;
  uint32_t last = millis();
  do {
    if (digitalRead(PIN_BTN_B) == LOW) {
      if (!first) bumpLength();
      first = false;
      uint32_t repeatAt = millis() + 500;
      while (digitalRead(PIN_BTN_B) == LOW) {
        nap(20, false);
        if ((int32_t)(millis() - repeatAt) >= 0) { bumpLength(); repeatAt = millis() + 120; }
      }
      last = millis();
    }
    nap(20, false);
  } while (millis() - last < SET_TIMEOUT_MS);
  setting = false;
  if (lengthMin[mode] != before) saveSettings();
  startSession();
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
  M5.In_I2C.bitOff(AXP, 0x12, 1 << 3, I2C_HZ);  // LCD logic power (LDO3) off
  wr(AXP, 0x46, 0x03);                           // drop pending power-key presses
  imuTapSensitive(false);
  imuMotion();                                   // release INT before arming
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (womOk) esp_sleep_enable_ext0_wakeup(PIN_INT, 0);
  if (state == FINISHED) esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_A, ESP_EXT1_WAKEUP_ALL_LOW);
  Serial.flush();
  esp_deep_sleep_start();
}

static void pause(uint32_t since) {
  pausedRemMs = (int32_t)std::max<int64_t>(0, endMs - rtcMsAt(since));
  state = PAUSED;
  screenSleep();
  Serial.printf("PAUSED: %ld ms left\n", (long)pausedRemMs);
}

// The buzzer is powered from the AXP192 5 V boost (EXTEN), so it is enabled only while beeping.
static void beep(int n, int onMs, int offMs) {
  if (!soundOn) return;
  if (!hasBuzzer()) {  // original M5StickC: the same pattern on the red LED
    pinMode(prof().ledPin, OUTPUT);
    for (int i = 0; i < n; i++) {
      digitalWrite(prof().ledPin, LOW);  delay(onMs);
      digitalWrite(prof().ledPin, HIGH); if (i + 1 < n) delay(offMs);
    }
    pinMode(prof().ledPin, INPUT);
    return;
  }
  M5.Power.Axp192.setEXTEN(true);
  delay(20);
  ledcSetup(0, 4000, 8);
  ledcAttachPin(prof().buzzerPin, 0);
  for (int i = 0; i < n; i++) {
    ledcWriteTone(0, 4000); delay(onMs);
    ledcWriteTone(0, 0);    if (i + 1 < n) delay(offMs);
  }
  ledcDetachPin(prof().buzzerPin);
  pinMode(prof().buzzerPin, INPUT);
  M5.Power.Axp192.setEXTEN(false);
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

// ---------- main ----------
void setup() {
  setCpuFrequencyMhz(80);
  auto cause = esp_sleep_get_wakeup_cause();
  M5.In_I2C.begin(I2C_NUM_1, GPIO_NUM_21, GPIO_NUM_22);
  bool warm = rtcMagic == MAGIC && (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1);

  // Fast path after a motion wake: decide from the pose before touching the display.
  uint32_t since = millis();
  if (warm && cause == ESP_SLEEP_WAKEUP_EXT0) {
    int pose = confirmPose(&since);
    if (state == FINISHED && pose == -1) finishedDown = true;
    bool wake = pose == 1 && (state == PAUSED || (state == FINISHED && finishedDown));
    if (!wake) { sleepWakes++; deepSleep(); }
  }

  M5.In_I2C.bitOn(AXP, 0x12, 1 << 3, I2C_HZ);  // LCD logic power (LDO3) on
  delay(5);
  Serial.begin(115200);

  auto cfg = M5.config();
  cfg.output_power = false;  // 5 V boost (EXTEN) off
  cfg.internal_imu = false;  // configured manually for low-power WOM
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  cfg.clear_display = true;
  M5.begin(cfg);
  adc_power_release();  // M5Unified keeps the SAR ADC powered for GPIO36/39 debouncing
  if (!warm) {  // pick the board profile (deep-sleep wakes keep the one chosen at cold boot)
    profileIdx = 0;
    for (size_t i = 0; i < sizeof PROFILES / sizeof PROFILES[0]; i++)
      if (PROFILES[i].board == M5.getBoard()) profileIdx = i;
    if (PROFILES[profileIdx].board != M5.getBoard())
      Serial.printf("Unknown board %d: using the %s profile\n", (int)M5.getBoard(), PROFILES[0].name);
  }
  M5.Display.setBrightness(0);
  pinMode(PIN_BTN_A, INPUT);
  pinMode(PIN_BTN_B, INPUT);
  pinMode(PIN_INT, INPUT);

  Serial.printf("\nPomodoro boot: wake cause=%d warm=%d board=%d sleep wakes=%u\n", cause, warm,
                (int)M5.getBoard(), sleepWakes);
  sleepWakes = 0;

  if (!warm) {  // cold boot or reset: always a new WORK session
    wr(AXP, 0x36, (rd(AXP, 0x36) & ~0x30) | 0x10);  // power key long press = 1.5 s
    rtcInit();
    imuInit();
    Serial.printf("IMU WHO_AM_I=0x%02x (MPU6886 = 0x19)\n", rd(MPU, 0x75));
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

static void backlightLevel(uint8_t level) {  // 0 = off, else AXP192 LDO2 = 1.8 V + 0.1 V * level
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
  for (uint8_t level : GLANCE_LEVELS) {
    backlightLevel(level);
    for (int i = 0; i < 5 && !stop; i++) {  // poll every 200 ms
      nap(GLANCE_STEP_MS / 5, true, false);
      key = powerKey();
      stop = key || digitalRead(PIN_BTN_A) == LOW || digitalRead(PIN_BTN_B) == LOW || readAccel().z < DOWN_G;
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
  if (digitalRead(PIN_BTN_A) == LOW) {
    if (millis() - lastPressAt < MODE_SWITCH_MS) mode ^= 1;  // second press during the grace period
    startSession();
    while (digitalRead(PIN_BTN_A) == LOW) nap(20, false);
    lastPressAt = millis();
    return true;
  }
  if (digitalRead(PIN_BTN_B) == LOW) {  // GPIO39 can glitch: require a held level
    nap(20, false);
    if (digitalRead(PIN_BTN_B) == LOW) { adjustLength(); return true; }
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
static void finishedStep() {
  if (!screenOn) {
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
    deepSleep();
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
  M5.Display.setBrightness(lit ? BRIGHTNESS : 0);  // backlight only, panel stays awake
  led(lit && soundOn);                             // StickC: the LED blinks along
  flushTapSave();
  nap(std::min<uint32_t>(CONFIRM_STEP_MS, BLINK_MS - t % BLINK_MS), true, false);
}

static void pausedStep() {
  uint32_t since;
  if (confirmPose(&since) == 1) { resume(since); return; }
  if (womOk) deepSleep();
  nap(1000, false);
}

void loop() {
  switch (state) {
    case RUNNING:  runStep(); break;
    case PAUSED:   pausedStep(); break;
    default:       finishedStep();
  }
}
