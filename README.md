# Smart Study Desk

ESP32-based desk system that tracks real study time, only counting it when you're
actually seated with your phone parked on its pad, nags you if you drift off,
reminds you to take breaks, and gives a local (offline) web dashboard.

## Hardware
See project brief — ESP32, OLED, DHT11, HC-SR04, PIR, Hall sensor, RGB status LED,
2x ambient RGB LEDs, buzzer, relay (charger socket), IR receiver + remote, touch pin,
toggle switch, push button.

## Setup
1. Install libraries via Arduino Library Manager: IRremote (v4.x), Adafruit_GFX,
   Adafruit_SSD1306, DHT sensor library (Adafruit). Preferences is built-in.
2. Edit `hotspotSSID` / `hotspotPassword` near the top of the .ino to your phone's
   hotspot — falls back to hosting its own `StudyDesk` network if not found.
3. Flash, open Serial Monitor at 115200 baud, enter the current time (HH:MM) when
   prompted.
4. Dashboard URL is printed in Serial Monitor after WiFi connects.

## Calibration needed after flashing
- `HALL_THRESHOLD` — use the commented-out test sketch at the bottom of the file
- `PRESENCE_DISTANCE_CM` — adjust to your desk depth
- `TOUCH_THRESHOLD` — calibrate with a standalone `touchRead()` print

## Remote button map
| Button | Action |
|---|---|
| Power | Start/stop session |
| Play | Pause/resume |
| Mute | DND toggle |
| Mode | Toggle ambient LED color mode |
| Prev/Next | Browse OLED screens (or color presets in LED mode) |
| 1-4 | Select subject (ACC / BS LAW / QA / ECO) |
| 1-9 (LED mode) | Pick ambient color preset |
| EQ | Relay (charger) on/off |

## Known limitations
- Exam countdown decrements once per detected midnight — if powered off overnight,
  that day won't auto-decrement. Fix via Bluetooth: send `DAYS <n>`.
- No RTC — time is set manually via Serial each boot.
