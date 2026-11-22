# Battery life estimate (1800 mAh)

Estimated runtime of the GPS version on a single 1800 mAh Li-ion/LiPo cell, with and without **GPS power save** (long press Down) and with the display at three **brightness levels** (long press Up).

These are **estimates from typical current figures, not measurements**. The display and GPS currents are the least certain numbers. Measure your own build with a USB power meter or a multimeter in series with the battery if you need real values.

## Assumptions

| Item | Current | Note |
|------|---------|------|
| ESP32-S3 at 80 MHz | 27 mA | No Wi-Fi or Bluetooth (the sketch sets 80 MHz) |
| GPS module, continuous | 45 mA | Typical GY-NEO6MV2 board, tracking |
| GPS module, power save on | 25 mA | u-blox power save mode at 1 Hz; datasheet shows about 50 % saving on the chip alone, less on a full board |
| Buzzer (TMB12A03) | 2.3 mA average | Assumed 30 mA when sounding, active 5 minutes every hour |
| Usable capacity | 85 % of 1800 mAh = 1530 mAh | The regulator and battery protection stop before the cell is empty |

Display current (whole display, including backlight on the TFTs). The backlight is assumed to be about 80 % of the display current on both TFTs and to scale with the PWM duty of the sketch (100 %, 39 %, 10 %). The OLED values are rough guesses for the three contrast settings.

| Display | Full (level 3) | Medium (level 2) | Lowest (level 1) |
|---------|----------------|------------------|------------------|
| SSD1306 128x32 | 8 mA | 6.5 mA | 5 mA |
| SSD1306 128x64 | 15 mA | 12 mA | 8 mA |
| ST7735S | 25 mA | 12.8 mA | 7 mA |
| ST7789V | 50 mA | 25.7 mA | 14 mA |

Formula: runtime (h) = usable capacity (mAh) / (ESP32 + GPS + display + buzzer, in mA).

Example, ST7789V, GPS power save off, full brightness: 27 + 45 + 50 + 2.3 = 124.3 mA, so 1530 / 124.3 = 12.3 h.

## Results (realistic, 85 % usable)

| Display | GPS PS off, full | GPS PS off, medium | GPS PS off, lowest | GPS PS on, full | GPS PS on, medium | GPS PS on, lowest |
|---------|------|------|------|------|------|------|
| SSD1306 128x32 | 19 h | 19 h | 19 h | 25 h | 25 h | 26 h |
| SSD1306 128x64 | 17 h | 18 h | 19 h | 22 h | 23 h | 25 h |
| ST7735S | 15 h | 18 h | 19 h | 19 h | 23 h | 25 h |
| ST7789V | 12 h | 15 h | 17 h | 15 h | 19 h | 22 h |

## What this means

- **GPS power save** is the biggest single saving, because the GPS module is the largest consumer. It only helps if your module accepts the power-save command (some clone modules do not) and with a good signal; with a weak signal the chip stays awake longer.
- **Dimming** matters most on the TFT versions, where the backlight is the largest display load. On the OLEDs the gain is small.
- The **buzzer** costs only a few percent of the runtime.
- Using both (power save on, lowest brightness) gives the longest runtime, but a dim display is harder to read in daylight. Medium is a practical compromise.
- Real cells deliver less than their label, especially old or cheap ones, and cold weather reduces capacity further. Treat these numbers as an upper bound.
- Charging through the board's 100 mA charger takes about 18 hours for an empty 1800 mAh cell.
