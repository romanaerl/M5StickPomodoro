# simplePomodoro

A low-power, standalone Pomodoro timer for the **M5StickC Plus** (ESP32-PICO-D4, AXP192, MPU6886, BM8563, ST7789 135×240).
It has two modes, **WORK** and **BREAK**, each with its own length. You control it by how the device is placed and with its three buttons. It uses no Wi-Fi, no Bluetooth and no cloud.

> Tested on the M5StickC Plus only. Other models (StickC, StickC Plus2, …) need changes; see [PORTING.md](PORTING.md).

## Usage

### Modes

| Mode | Default length | Bar colour | Start signal | End signal |
|---|---|---|---|---|
| WORK | 25 min | green → orange → red | one short beep | three short beeps |
| BREAK | 5 min | blue → light blue | one longer beep | two long beeps |

Every session starts with a **5-second grace period**: a 5-minute session starts at `5:05`, and for those 5 seconds the bar is yellow (WORK) or violet (BREAK). When the grace period ends, the start signal sounds and the bar takes the mode's colour. Power-on always starts a new WORK session.

### Controls

| Action | Result |
|---|---|
| Power on | A new WORK session starts. |
| Place **face down** | Pause: the screen turns off and the device goes into deep sleep. |
| Turn the **screen visible** again (any pose below) | Resumes exactly where it paused; the screen shows for 5 s. |
| **M5** button (big, below the screen) | Restarts the current mode. Pressed again during the grace period (within 5 s), it switches to the other mode and starts it. |
| **B** button (right side) | +1 minute to the current mode's length (1…60, wraps around; hold to repeat quickly). Shows `SET WORK` / `SET BREAK`. A new session starts 1 s after the last press. Both lengths are saved. |
| **Power** button (left side), short press while the screen is on | Toggles the sound. `SOUND ON` / `SOUND OFF` shows for 2 s and `MUTE` appears in the status line. The setting is saved. Presses while the screen is off are ignored. |
| Session ends | The end signal sounds, then `00:00 FINISHED` blinks for 2 minutes (or until turned face down). |
| During the blinking: move to another pose and hold it for 1 s | Starts the **other** mode, e.g. from long edge A to long edge B, or from standing on its end to lying on its side. |
| After the end: face down, then visible again | Starts the **same** mode again. The M5 button works too. |

### Poses

| Pose | Display | A tap on the table shows the screen |
|---|---|---|
| Flat, screen up | landscape | no |
| On long edge A | landscape | yes |
| On long edge B | landscape, rotated 180° | no |
| On its end, USB up | portrait | yes |
| On its end, USB down | portrait, rotated 180° | yes |

The screen shows the remaining time `MM:SS` on top of a bar inside a rounded frame. The bar drops every second in proportion to the remaining time and changes colour as described in [Modes](#modes). A status line (`WORK` / `BREAK` / `SET …` / `FINISHED`) and the battery level are shown at the bottom, or at the top in portrait.

To save battery and stay unobtrusive, the screen shows a **5-second glance once a minute**: only the remaining minutes (`MM`, static), with the backlight stepping one level per second (7 → 8 → 8 → 8 → 7). When the pose changes, a session starts or resumes, or you tap the table in a tap pose, the screen instead shows the full `MM:SS` for 5 s.

The fade is stepped because the AXP192 drives the backlight through the LDO2 voltage in 0.1 V steps, and level 7 (2.5 V) is the first visible one. Smoother fading would need software PWM, which keeps the CPU awake and costs noticeably more battery.

## Build and flash

Install [PlatformIO](https://platformio.org/), either the CLI or the VS Code extension. PlatformIO downloads the dependencies itself: the `espressif32@6.4.0` platform and the `M5Unified@0.2.25` library (with M5GFX).

1. Connect the M5StickC Plus over USB.
2. Build and flash. PlatformIO auto-detects the port when a single board is connected:
   ```bash
   pio run -t upload
   ```
   If several serial devices are present, list them and pass the port explicitly:
   ```bash
   pio device list
   ```
   ```bash
   pio run -t upload --upload-port /dev/cu.usbserial-XXXX
   ```
3. Optionally, watch the serial log:
   ```bash
   pio device monitor
   ```
   On boot you should see `IMU INT self-test: asserted=1 released=1` and `WORK: new 25 min session`.

A test build with a 2-minute default WORK session:
```bash
pio run -e test -t upload
```
Lengths already chosen with the B button are stored in flash and override the defaults.

The defaults are set by `-DSESSION_MIN=25` and `-DBREAK_MIN=5` (see `src/main.cpp` and `platformio.ini`).

## Power design

- Wi-Fi and Bluetooth are never initialised, and the CPU runs at 80 MHz. The AXP192 5 V boost (EXTEN) is on only while beeping, because the buzzer is powered from it.
- **Running:** the ESP32 is in light sleep. It is woken by a hardware timer at the next minute mark, by the buttons, or by the accelerometer interrupt (GPIO35).
- **Paused / finished:** deep sleep with the backlight (LDO2) and the LCD logic supply (LDO3) off. The device wakes on the MPU6886 wake-on-motion interrupt (ext0) and, once finished, on the M5 button (ext1). After a wake, the pose is checked *before* the display is initialised, so an accidental bump never lights the screen.
- **Accelerometer:** accel-only low-power cycle mode. Flat and on edge B it samples at 12.5 Hz with an 80 mg threshold. In tap poses it uses 100 Hz and 40 mg.
- **Timekeeping:** the BM8563 crystal RTC keeps time, not the ESP32's RC sleep clock. Pause and resume are aligned to the RTC second tick, so they are accurate to milliseconds.
- **Flash (NVS)** holds only the settings (both lengths and the sound switch) and is written only when they change. The running state lives in RTC memory, which survives deep sleep.

**Estimated battery life** (calculated, not measured; 120 mAh battery): about 3.5–4 mA while running, so roughly 30 h of continuous counting. About 0.1–0.5 mA while paused or finished, so weeks. Every pose change and every tap adds 5 s of screen time.

## Hardware notes (found on the device)

- In low-power cycle mode the MPU6886 reads ~0 g unless `ACCEL_CONFIG2` has `DLPF_CFG = 7`.
- The buzzer (GPIO2) is powered from the AXP192 5 V boost (EXTEN) and stays silent without it.
- GPIO35 is shared by the MPU6886 INT (configured open-drain) and the BM8563 INT (its interrupts are disabled).
- Deep sleep can wake on the IMU or on one button, not on "either of two buttons": ESP32 ext1 cannot wake on "any of these pins low". That is why the B button does not wake the device after a session has finished.
- The power button is wired to the AXP192, not to the ESP32. Its short and long presses are latched in AXP192 register `0x46` and read over I2C, which is why the sound toggle works only while the screen is on and the CPU is polling.

## Layout

```
platformio.ini   environments: m5stick-c-plus (default) and test (2-minute session)
src/main.cpp     the whole firmware
PORTING.md       notes on running it on other M5Stick models
```
