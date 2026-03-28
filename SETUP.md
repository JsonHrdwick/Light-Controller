# Cabinet Lighting Controller — Setup Guide

ESP32-based PWM controller for dual-channel (warm white + cool white) LED strips, with a web portal, time scheduling, and ambient light sensing.

---

## Table of Contents

1. [Parts List](#parts-list)
2. [Circuit Design](#circuit-design)
3. [Wiring Diagram](#wiring-diagram)
4. [Light Sensor Wiring](#light-sensor-wiring)
5. [Software Setup](#software-setup)
6. [Flashing the ESP32](#flashing-the-esp32)
7. [First Boot](#first-boot)
8. [Using the Web Portal](#using-the-web-portal)
9. [Troubleshooting](#troubleshooting)

---

## Parts List

| Qty | Part | Notes |
|-----|------|-------|
| 1 | ESP32 development board | Any standard 38-pin devkit works (WROOM, WROVER) |
| 2 | N-channel MOSFET (logic-level) | IRLZ44N (through-hole). Must be **logic-level** — turns fully on at 3.3V gate. Standard IRFZ44N will NOT work |
| 2 | 100Ω resistor | Gate resistors, one per MOSFET — limits current spike when gate capacitance charges |
| 2 | 10kΩ resistor | Gate pull-downs to GND (one per MOSFET); one also used for LDR voltage divider |
| 1 | LDR (photoresistor) | Standard GL5528 or equivalent |
| 1 | 24V DC power supply | Sized for your strip — typical strips draw ~1A/meter per channel |
| 1 | LM2596 DC-DC buck converter module | Steps 24V down to 5V for the ESP32. Adjust the onboard trimmer pot to 5.0V output **before** connecting the ESP32 |
| — | Dual-channel LED strip | 3-lead: WW−, CW−, 24V+ (common anode) |
| — | Prototype board or PCB | Perfboard works fine for this circuit |
| — | Screw terminals or JST connectors | For strip and power connections |

> **MOSFET note:** The IRLZ44N is the logic-level version of the IRFZ44N — they look identical but the IRLZ44N turns fully on at 3.3V. Using the non-logic-level IRFZ44N will result in the MOSFET running hot and the strip staying dim.

---

## Circuit Design

### Overview

Your strip has three leads: **WW−**, **CW−**, and a shared **24V+**. Each channel has its own negative lead, which means each one can be independently switched at the bottom of the circuit using a simple N-channel MOSFET. The 24V+ wire connects directly to the positive rail — no switching needed there.

```
24V ──────────────────────── 24V+ lead (strip) ──────────────── Buck converter
                                     │                          (→ 5V → ESP32)
                                  [strip]
                          WW− lead       CW− lead
                              │               │
                           Drain           Drain
                        IRLZ44N (Q1)   IRLZ44N (Q2)
                           Source          Source
                              │               │
GND ──────────────────────────┴───────────────┘
```

The ESP32 is powered separately from the 24V rail via the LM2596 buck converter stepped down to 5V.

### Why low-side switching?

Each MOSFET sits between the strip's negative lead and GND. When the gate is driven high by the ESP32, the MOSFET turns on and completes the circuit — current flows from 24V through the strip LEDs and out through the MOSFET to GND. When the gate is low, the MOSFET is off and the circuit is broken.

This is the simplest possible topology. The ESP32 GPIO drives the gate directly — no driver transistor, no Zener clamp needed.

### Gate resistor purpose

The 100Ω resistor between the ESP32 GPIO and the MOSFET gate limits the inrush current spike when the gate capacitance charges on each PWM cycle. Without it, repeated spikes can stress the GPIO pin over time.

### Gate pull-down resistor purpose

The 10kΩ resistor from each gate to GND ensures the MOSFET is held off when the ESP32 is booting, in reset, or if the GPIO is floating. Without it, the strip can flash on unpredictably at startup.

---

## Wiring Diagram

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 SECTION 1 — POWER
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  24V PSU (+) ──────────────────────────────── 24V rail
  24V PSU (−) ──────────────────────────────── GND rail

  24V rail ──── LM2596 IN+
  GND rail ──── LM2596 IN−
                LM2596 OUT+ (set to 5.0V) ──── ESP32 VIN
                LM2596 OUT−               ──── ESP32 GND

  ⚠ Adjust LM2596 trimmer pot to 5.0V BEFORE connecting ESP32


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 SECTION 2 — WARM WHITE CHANNEL
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  GPIO 25 ──── [100Ω] ──┬──── IRLZ44N (Q1) Gate
                         │
                       [10kΩ]
                         │
                       GND rail

                         IRLZ44N (Q1) Drain  ──── WW− lead (strip)
                         IRLZ44N (Q1) Source ──── GND rail


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 SECTION 3 — COOL WHITE CHANNEL  (identical to Section 2)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  GPIO 26 ──── [100Ω] ──┬──── IRLZ44N (Q2) Gate
                         │
                       [10kΩ]
                         │
                       GND rail

                         IRLZ44N (Q2) Drain  ──── CW− lead (strip)
                         IRLZ44N (Q2) Source ──── GND rail


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 SECTION 4 — LED STRIP
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  24V+ lead ──── 24V rail  (no MOSFET — direct connection)
  WW−  lead ──── Q1 Drain  (Section 2)
  CW−  lead ──── Q2 Drain  (Section 3)


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 SECTION 5 — LIGHT SENSOR (LDR)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  ESP32 3.3V ──── [LDR] ──┬──── GPIO 34
                           │
                         [10kΩ]
                           │
                         GND rail


━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 GROUND — ALL OF THESE CONNECT TO THE SAME GND RAIL
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  24V PSU (−)
  LM2596 OUT−
  ESP32 GND
  IRLZ44N Q1 Source
  IRLZ44N Q2 Source
```

**How each channel works:**
- GPIO HIGH → IRLZ44N gate goes high → MOSFET turns on → current flows from 24V through strip → out WW− → through MOSFET to GND → strip lights up
- GPIO LOW → gate pulled to GND by 10kΩ → MOSFET off → circuit broken → strip dark

---

## Light Sensor Wiring

The LDR (light-dependent resistor) is wired as a voltage divider with a 10kΩ fixed resistor. As ambient light decreases, the LDR resistance increases, pulling the voltage at the junction lower — this is what the ESP32 reads as the ADC value.

```
  3.3V ──────┐
             │
           [LDR]          ← resistance drops in bright light (~1kΩ)
             │                resistance rises in darkness (~1MΩ)
             ├──────── GPIO 34 (ADC input)
             │
          [10kΩ]
             │
  GND  ──────┘
```

**ADC behavior:**
- Bright room → LDR resistance low → voltage at GPIO34 high → ADC reads ~3000–4095
- Dark room → LDR resistance high → voltage at GPIO34 low → ADC reads ~50–400

The default threshold in the code is `500`. Set it in the web UI after installation using the live ADC reading displayed on the sensor card — take a reading in normal light conditions, take one in the dark, then set the threshold between the two values.

> **Important:** GPIO 34 on the ESP32 is input-only and has no internal pull-up/pull-down. Do not attempt to configure it as an output. Do not connect the LDR directly to a 5V source — the ESP32 ADC is 3.3V max.

---

## Software Setup

### Option A — Arduino IDE

1. Install **Arduino IDE 2.x** from arduino.cc

2. Add the ESP32 board package:
   - Open **File → Preferences**
   - Add this URL to "Additional boards manager URLs":
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Open **Tools → Board → Boards Manager**, search `esp32`, install **esp32 by Espressif Systems**

3. Install libraries via **Tools → Manage Libraries**:

   | Library name to search | Author |
   |------------------------|--------|
   | `ESPAsyncWebServer` | me-no-dev |
   | `AsyncTCP` | me-no-dev |
   | `ArduinoJson` | Benoit Blanchon |

   > ESPAsyncWebServer and AsyncTCP may not appear in the library manager. If so, download the ZIPs from GitHub and install via **Sketch → Include Library → Add .ZIP Library**:
   > - github.com/me-no-dev/ESPAsyncWebServer
   > - github.com/me-no-dev/AsyncTCP

4. Open `cabinet_lights.ino` in Arduino IDE

5. Edit the configuration block at the top of the file:
   ```cpp
   const char* WIFI_SSID     = "your_network_name";
   const char* WIFI_PASSWORD = "your_password";
   const long  GMT_OFFSET_SEC = -18000;  // see timezone table in file
   ```

### Option B — PlatformIO (VS Code)

1. Install the PlatformIO extension in VS Code

2. Create a new project targeting `Espressif ESP32 Dev Module`

3. Replace the generated `platformio.ini` with:
   ```ini
   [env:esp32dev]
   platform = espressif32
   board = esp32dev
   framework = arduino
   monitor_speed = 115200
   lib_deps =
       me-no-dev/AsyncTCP @ ^1.1.1
       me-no-dev/ESPAsyncWebServer @ ^1.2.3
       bblanchon/ArduinoJson @ ^6.21.3
   board_build.filesystem = spiffs
   ```

4. Place `cabinet_lights.ino` (rename to `main.cpp`) in the `src/` folder

5. Edit credentials and timezone at the top of the file

---

## Flashing the ESP32

1. Connect the ESP32 to your computer via USB

2. Select the correct port:
   - **Arduino IDE:** Tools → Port → (select the COM port or /dev/ttyUSB0 that appeared)
   - **PlatformIO:** auto-detected, or set `upload_port` in `platformio.ini`

3. Select the board:
   - **Arduino IDE:** Tools → Board → ESP32 Arduino → **ESP32 Dev Module**

4. Set partition scheme (important for SPIFFS storage):
   - **Arduino IDE:** Tools → Partition Scheme → **Default 4MB with spiffs**

5. Flash:
   - **Arduino IDE:** Click the Upload button (→)
   - **PlatformIO:** Click the Upload button or run `pio run --target upload`

6. If the upload fails or times out:
   - Hold the **BOOT** button on the ESP32 (may be labelled **IO0** on some boards), click Upload, release once "Connecting…" appears
   - Some boards require a 10µF capacitor between EN and GND to auto-reset into flash mode

---

## First Boot

1. Open **Tools → Serial Monitor** (baud: 115200)

2. You should see:
   ```
   [WiFi] Connecting......
   [WiFi] Connected. IP: 192.168.1.xxx
   [NTP] Syncing…
   [HTTP] Server started on port 80
   ```

3. Navigate to that IP address in any browser on the same network

4. The web portal will load. On first boot all settings default to:
   - Lights: OFF
   - Brightness: 80%, Color Temp: 30% (warm-biased)
   - No schedules
   - Light sensor: disabled

5. Calibrate the light sensor threshold:
   - Enable the sensor card in the UI
   - Note the live ADC reading in normal ambient light
   - Cover/shade the LDR and note the reading in darkness
   - Set the threshold to a value halfway between the two readings

---

## Using the Web Portal

### Control card
- **Power toggle** — turns both channels on or off (PWM goes to zero; state is saved)
- **Brightness** — master level 1–100%
- **Color Temp** — 0% is full warm white, 100% is full cool white; the mix is calculated per-channel

### Schedules card
- Add a schedule with a time (24-hour), brightness, color temp, and which days of the week it should fire
- Toggle the switch on a schedule to enable/disable it without deleting it
- The ESP32 checks schedules every 30 seconds against NTP time, deduplicating within the same minute

### Light Sensor card
- **Enable** — activates ambient light monitoring (polled every 2 seconds)
- **Dark Threshold** — ADC value below which the "when dark" action fires; live reading shown top-right
- **When Dark** options:
  - *Dim to level* — reduces brightness to the configured dim level, restores previous state when light returns
  - *Turn off* — switches off the lights; restores when light returns
  - *Turn on (full)* — turns on at 100% brightness; useful as a motion-adjacent night light trigger
- The sensor overrides manual state temporarily; when ambient light is restored, the previous state is recovered

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Upload fails / "Connecting…" hangs | ESP32 not entering flash mode | Hold **BOOT** (may be labelled **IO0**) then click Upload, release when "Connecting…" appears |
| Lights on at startup / won't turn off | Gate pull-down missing | Add 10kΩ from each MOSFET gate to GND |
| One channel always dim | Wrong MOSFET type (non-logic-level) | Replace with IRLZ44N — standard IRFZ44N needs 10V gate and won't fully saturate at 3.3V |
| MOSFETs get hot | FET not fully saturating | Confirm part is IRLZ44N (logic-level), not IRFZ44N |
| Channel stays permanently off | Gate resistor wrong | Check 100Ω is between GPIO and gate, not between gate and GND |
| Web page doesn't load | ESP32 not on network | Check SSID/password; check Serial Monitor for IP |
| Schedules don't fire | NTP not synced | Ensure ESP32 has internet access; check timezone offset |
| LDR threshold never triggers | Divider resistor wrong way | 3.3V → LDR → GPIO34 → 10kΩ → GND (not reversed) |
| ADC reading stuck at 0 | Using GPIO 34 as output | GPIO 34 is input-only; do not reassign it |
| All settings lost on reboot | SPIFFS not formatted | Set partition scheme to "Default 4MB with spiffs" and reflash |
