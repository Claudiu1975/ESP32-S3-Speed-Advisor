# ESP32-S3 Super Mini Speed Advisor

A compact GPS-based speed advisor that alerts you when a configurable speed threshold is exceeded.  
Designed for the ESP32-S3 Super Mini (or Zero-style boards), GY-NEO6MV2 GPS, active buzzer, and multiple display options.

## Features

- Configurable speed threshold (default 90.0 km/h, adjustable in 0.5 km/h steps with two buttons)
- Visual + audible alert when threshold is exceeded:
  - Short beep every second
  - After 45 s: increasing beep frequency
  - At 55 s: continuous tone
  - Display flashes in sync with the buzzer
- Live speed + number of GPS satellites on the display
- Battery level indication (requires external voltage divider on an ADC pin)
- Support for multiple displays:
  - SSD1306 128×32 I2C
  - SSD1306 128×64 I2C (including GM009605-style modules)
  - ST7735S SPI
  - ST7789V SPI

## Disclaimer

**This is an unofficial hobby project.**  
It is provided “as is” without warranty of any kind. Use at your own risk.  
The author(s) are not responsible for any damage to equipment, vehicles, or persons resulting from the use or misuse of this software or hardware design.  
This device is **not** a certified speedometer, speed limiter, or safety system. Do not rely on it for legal speed compliance, navigation, or any safety-critical application.  
GPS accuracy depends on satellite visibility, antenna placement, and environmental conditions. Always obey traffic laws and drive responsibly.

## Credits

- GPS parsing: [TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus) by Mikal Hart
- OLED graphics: Adafruit SSD1306 & Adafruit GFX libraries
- TFT graphics: Adafruit ST7735 / ST7789 or TFT_eSPI (depending on configuration)
- ESP32 Arduino core by Espressif Systems
- Community pinout references for ESP32-S3 Super Mini / Zero boards

## Hardware Requirements

- ESP32-S3 Super Mini (or compatible Super Mini / Zero-style board)
- GY-NEO6MV2 (u-blox NEO-6M) GPS module + antenna
- TMB12A03 active buzzer (3 V rated) or equivalent active buzzer
- One of the supported displays (see below)
- Two momentary push buttons
- Optional: Li-ion / LiPo battery + voltage divider for battery monitoring
- Wiring, breadboard or PCB, USB-C cable

### Recommended Pin Assignment (ESP32-S3 Super Mini)

| Function       | GPIO | Notes                                      |
|----------------|------|--------------------------------------------|
| GPS RX         | 16   | GPS TX → ESP RX                            |
| GPS TX         | 17   | ESP TX → GPS RX                            |
| Buzzer         | 4    | Active buzzer + pin (other pin to GND)     |
| Button Up      | 5    | Internal pull-up, other side to GND        |
| Button Down    | 6    | Internal pull-up, other side to GND        |
| I2C SDA        | 8    | For OLED                                   |
| I2C SCL        | 9    | For OLED                                   |
| Battery ADC    | 1    | Via voltage divider (see below)            |
| TFT MOSI       | 11   | SPI                                        |
| TFT SCK        | 12   | SPI                                        |
| TFT CS         | 10   | SPI                                        |
| TFT DC         | 7    | SPI                                        |
| TFT RST        | 15   | SPI                                        |

**Always verify the silkscreen on your specific Super Mini board** — pin labels and available GPIOs can vary slightly between sellers.

### Battery Voltage Monitoring

The Super Mini does not have a built-in battery sense pin. Connect a voltage divider from the battery positive to GPIO1 (and GND):

```
Battery+ ----[100kΩ]----+----[100kΩ]---- GND
                        |
                       GPIO1
```

(Or use 220kΩ / 220kΩ for lower current draw.)  
Adjust the `BATTERY_DIVIDER_RATIO` constant in the sketch if you use different resistors.  
Typical Li-ion range used: 3.0 V (0 %) – 4.2 V (100 %).

### Display Wiring Summary

- **SSD1306 I2C (128×32 or 128×64 / GM009605)**: VCC→3.3 V, GND→GND, SDA→GPIO8, SCL→GPIO9
- **ST7735S / ST7789V SPI**: VCC→3.3 V, GND→GND, MOSI→11, SCK→12, CS→10, DC→7, RST→15 (and BL if present to 3.3 V or a GPIO)

## Software Setup

### Arduino IDE Board Settings

1. Install the **ESP32** board package:
   - Tools → Board → Boards Manager
   - Search for **esp32**
   - Install **esp32 by Espressif Systems** (2.0.14 or newer, preferably latest 3.x)

2. Select the board:
   - **Tools → Board → esp32 → ESP32S3 Dev Module**
   - (There is no dedicated Super Mini entry — ESP32S3 Dev Module is correct)

3. Recommended Tools settings:

| Setting | Value | Notes |
|---------|-------|-------|
| USB CDC On Boot | Enabled | Serial works over USB-C |
| USB Mode | Hardware CDC and JTAG | Default is fine |
| CPU Frequency | 240 MHz | Default |
| Flash Size | 4MB (32Mb) | Most Super Minis |
| Partition Scheme | Default 4MB with spiffs | Or any 4 MB scheme |
| PSRAM | OPI PSRAM | Enable if your board has PSRAM |
| Upload Speed | 921600 | Lower to 460800 if upload fails |
| Core Debug Level | None | |

4. Connect the board and select the correct port under **Tools → Port**.

5. Install libraries via **Tools → Manage Libraries**:
   - TinyGPSPlus
   - Adafruit SSD1306
   - Adafruit GFX Library
   - Adafruit ST7735 and ST7789 Library

6. Open `SpeedAdvisor.ino` and set the display type at the top of the sketch:
   ```cpp
   #define DISPLAY_TYPE 0   // 0 = SSD1306 128x32
                            // 1 = SSD1306 128x64 (GM009605)
                            // 2 = ST7735S
                            // 3 = ST7789V
   ```

7. Upload.

**Upload tip:** If the board does not enter download mode automatically, hold the **BOOT** button, press and release **RESET** (or unplug/replug), then release **BOOT** and click Upload.

After upload, open the Serial Monitor at **115200 baud** (with USB CDC On Boot enabled) for any debug output.

## Wiring Diagram (Text)

```
ESP32-S3 Super Mini
┌─────────────────────────┐
│ 5V  3V3  GND            │
│                         │
│ GPIO16 ← GPS TX         │
│ GPIO17 → GPS RX         │
│ GPIO4  → Buzzer +       │
│ GPIO5  ← Button Up      │
│ GPIO6  ← Button Down    │
│ GPIO8  ↔ OLED SDA       │
│ GPIO9  ↔ OLED SCL       │
│ GPIO1  ← Battery divider│
│                         │
│ (SPI pins for TFT)      │
└─────────────────────────┘

GPS GY-NEO6MV2
VCC → 3.3V or 5V (module has regulator)
GND → GND
TX  → ESP GPIO16
RX  → ESP GPIO17

Buzzer TMB12A03 (active)
+ → GPIO4
- → GND

Buttons (momentary)
One side → GPIO5 / GPIO6
Other side → GND
(internal pull-ups enabled in code)

Battery divider (optional)
Bat+ --[100k]--+--[100k]-- GND
               |
              GPIO1
```

Adjust pins in the sketch if your Super Mini silkscreen differs.

## Usage

1. Power on. The display shows the threshold speed (starts at 90.0 km/h).
2. Use the two buttons to increase/decrease the threshold in 0.5 km/h steps.
3. After a short idle period (or button confirmation), the device switches to normal mode.
4. Normal mode shows current GPS speed and satellite count (plus battery percentage).
5. When speed exceeds the threshold, the buzzer and display flash according to the timing rules above.
6. Speed must drop below the threshold to reset the alert timer.

## License

MIT License – see LICENSE file.

## Contributing

Issues and pull requests are welcome. Please test on real hardware before submitting display or pin changes.
