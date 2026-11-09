/*
  ESP32-S3 Super Mini Speed Advisor
  Supports SSD1306 (128x32 / 128x64), ST7735S, ST7789V
  GPS: GY-NEO6MV2 / NEO-7MV2 / NEO-8MV2 (u-blox NEO-6M / NEO-7M / NEO-8M)

  Disclaimer: Hobby project only. Not a certified speedometer.
  Use at your own risk. See README.md for full disclaimer and credits.
*/

#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <stdio.h>
#include <Preferences.h>

// ========== DISPLAY SELECTION ==========
// 0 = SSD1306 128x32 I2C
// 1 = SSD1306 128x64 I2C (GM009605 style)
// 2 = ST7735S SPI
// 3 = ST7789V SPI
#define DISPLAY_TYPE 0

#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#elif DISPLAY_TYPE == 2
  #include <Adafruit_GFX.h>
  #include <Adafruit_ST7735.h>
#elif DISPLAY_TYPE == 3
  #include <Adafruit_GFX.h>
  #include <Adafruit_ST7789.h>
#endif

// ========== GPS CONFIG ==========
// Compatible modules: GY-NEO6MV2, NEO-7MV2, NEO-8MV2 (and similar u-blox UART GPS)
// Default baud is 9600 for most breakout boards. Change if your module was reconfigured.
#define GPS_BAUD        9600
#define GPS_RX_PIN      16
#define GPS_TX_PIN      17
#define BUZZER_PIN      4
#define BTN_UP_PIN      5
#define BTN_DOWN_PIN    6
#define BAT_ADC_PIN     1

#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  #define I2C_SDA       8
  #define I2C_SCL       9
  #define OLED_ADDR     0x3C
#else
  #define TFT_MOSI      11
  #define TFT_SCK       12
  #define TFT_CS        10
  #define TFT_DC        7
  #define TFT_RST       15
#endif

// ========== POWER SAVING / BRIGHTNESS ==========
#define CPU_FREQ_MHZ       80     // 80 MHz is plenty for this sketch (default would be 240)
#define LONG_PRESS_MS      800    // hold a button this long to change the brightness
#define BATTERY_READ_MS    1000   // battery voltage is sampled once per second

// ========== GPS POWER SAVE ==========
// Triple click of the Down button switches the GPS module between full performance and
// power save. Uses the u-blox UBX-CFG-RXM command (lpMode). The setting is remembered.
#define GPS_PS_LPMODE        1      // lpMode when power save is ON: 1 = Power Save Mode, 4 = Eco mode
#define TRIPLE_CLICK_GAP_MS  500    // max time between two clicks of a triple click
#define UBX_ACK_TIMEOUT_MS   1000

// TFT backlight control. Wire the module's BL pin to this GPIO to enable dimming.
// Set to -1 if BL is tied to 3V3 (the backlight then stays at full brightness).
#if DISPLAY_TYPE == 2 || DISPLAY_TYPE == 3
  #define TFT_BL_PIN           13
  #define TFT_BL_ACTIVE_HIGH   1   // 0 if your module's backlight is active-low
  #define TFT_BL_PWM_HZ        5000
#endif

// ========== BATTERY CONFIG ==========
// Voltage divider ratio (R1+R2)/R2. Example: two equal 100k resistors → 2.0
#define BATTERY_DIVIDER_RATIO  2.0f
#define BATTERY_MIN_V          3.0f
#define BATTERY_MAX_V          4.2f

// ========== THRESHOLD & TIMING ==========
float thresholdKmh = 90.0f;
const float STEP = 0.5f;

const uint32_t ALERT_PHASE1_MS = 45000;   // 45 s → start increasing frequency
const uint32_t ALERT_PHASE2_MS = 55000;   // 55 s → continuous

// ========== OBJECTS ==========
Preferences prefs;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);

#if DISPLAY_TYPE == 0
  Adafruit_SSD1306 display(128, 32, &Wire, -1);
#elif DISPLAY_TYPE == 1
  Adafruit_SSD1306 display(128, 64, &Wire, -1);
#elif DISPLAY_TYPE == 2
  Adafruit_ST7735 display(TFT_CS, TFT_DC, TFT_RST);
#elif DISPLAY_TYPE == 3
  Adafruit_ST7789 display(TFT_CS, TFT_DC, TFT_RST);
#endif

// ========== STATE ==========
enum Mode { MODE_SET, MODE_RUN };
Mode currentMode = MODE_SET;
unsigned long lastButtonMs = 0;
unsigned long alertStartMs = 0;
bool alertActive = false;
bool displayOn = true;
unsigned long lastBeepToggle = 0;
bool buzzerState = false;

// Brightness: 0 = low, 1 = medium, 2 = full (long press Up = brighter, long press Down = dimmer)
int brightnessLevel = 2;
int batteryPct = 0;
unsigned long lastBatteryMs = 0;
bool forceRedraw = true;

bool gpsPowerSave = false;
bool gpsApplyPending = false;
unsigned long toastUntilMs = 0;
char toastLine1[24] = "";
char toastLine2[24] = "";

void setup() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  Serial.begin(115200);
  prefs.begin("speedadv", false);
  gpsPowerSave = prefs.getBool("gpsps", false);
  gpsApplyPending = gpsPowerSave;   // re-apply the saved setting once the GPS has booted
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(BTN_UP_PIN, INPUT_PULLUP);
  pinMode(BTN_DOWN_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // ~0-3.3 V range

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 allocation failed"));
  }
  display.setTextColor(SSD1306_WHITE);
#else
  // SPI init – adjust SPI frequency / orientation as needed for your module
  #if DISPLAY_TYPE == 2
    display.initR(INITR_BLACKTAB);  // common for many ST7735S; try other INITR_ if colors wrong
  #else
    display.init(240, 320);         // adjust resolution for your ST7789V module
  #endif
  display.setRotation(1);
  display.fillScreen(ST77XX_BLACK);
  display.setTextColor(ST77XX_WHITE);
#endif

  display.setTextSize(1);
  initBacklight();
  applyBrightness();
  batteryPct = (int)(readBatteryPercent() + 0.5f);
  lastBatteryMs = millis();
  lastButtonMs = millis();
  showSetScreen();
}

void loop() {
  // Feed GPS
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  handleButtons();

  if (currentMode == MODE_SET) {
    if (millis() - lastButtonMs > 5000) {  // 5 s idle → switch to run mode
      currentMode = MODE_RUN;
      forceRedraw = true;
    }
  }

  // Re-apply a saved GPS power-save setting a few seconds after boot
  if (gpsApplyPending && millis() > 3000) {
    gpsApplyPending = false;
    if (!applyGpsPowerMode(true)) {
      gpsPowerSave = false;
      prefs.putBool("gpsps", false);
      showToast("GPS power save", "FAILED");
    }
  }

  // End of an on-screen message
  if (toastUntilMs != 0 && millis() > toastUntilMs) {
    toastUntilMs = 0;
    forceRedraw = true;
    if (currentMode == MODE_SET) showSetScreen();
  }

  // Sample the battery once per second instead of on every redraw
  if (millis() - lastBatteryMs >= BATTERY_READ_MS) {
    lastBatteryMs = millis();
    batteryPct = (int)(readBatteryPercent() + 0.5f);
  }

  updateAlert();
  updateDisplay();

  delay(50);  // light loop
}

// Short press: change the limit by one step (acts on release).
// Long press (hold >= LONG_PRESS_MS): Up = brighter, Down = dimmer.
void handleButtons() {
  static bool upPrev = HIGH, downPrev = HIGH;
  static unsigned long upDownMs = 0, downDownMs = 0;
  static bool upLongDone = false, downLongDone = false;
  static uint8_t downClicks = 0;
  static unsigned long lastDownClickMs = 0;
  static float limitBeforeClicks = 0;
  bool upNow = digitalRead(BTN_UP_PIN);
  bool downNow = digitalRead(BTN_DOWN_PIN);
  unsigned long now = millis();

  // --- Up button ---
  if (upNow == LOW && upPrev == HIGH) {
    upDownMs = now;
    upLongDone = false;
  }
  if (upNow == LOW && !upLongDone && now - upDownMs >= LONG_PRESS_MS) {
    upLongDone = true;
    downClicks = 0;
    changeBrightness(+1);
  }
  if (upNow == HIGH && upPrev == LOW && !upLongDone) {
    downClicks = 0;
    thresholdKmh += STEP;
    if (thresholdKmh > 200.0f) thresholdKmh = 200.0f;
    lastButtonMs = now;
    currentMode = MODE_SET;
    showSetScreen();
  }

  // --- Down button ---
  if (downNow == LOW && downPrev == HIGH) {
    downDownMs = now;
    downLongDone = false;
  }
  if (downNow == LOW && !downLongDone && now - downDownMs >= LONG_PRESS_MS) {
    downLongDone = true;
    downClicks = 0;
    changeBrightness(-1);
  }
  if (downNow == HIGH && downPrev == LOW && !downLongDone) {
    // Count clicks; three quick clicks toggle the GPS power save instead of changing the limit
    if (downClicks == 0 || now - lastDownClickMs > TRIPLE_CLICK_GAP_MS) {
      downClicks = 0;
      limitBeforeClicks = thresholdKmh;
    }
    downClicks++;
    lastDownClickMs = now;
    lastButtonMs = now;
    if (downClicks >= 3) {
      downClicks = 0;
      thresholdKmh = limitBeforeClicks;   // undo the two limit steps made by this triple click
      toggleGpsPowerSave();
    } else {
      thresholdKmh -= STEP;
      if (thresholdKmh < 5.0f) thresholdKmh = 5.0f;
      currentMode = MODE_SET;
      showSetScreen();
    }
  }

  upPrev = upNow;
  downPrev = downNow;
}

// ---------------------------------------------------------------------------
// GPS power save (u-blox UBX-CFG-RXM)
// ---------------------------------------------------------------------------
enum UbxResult { UBX_ACK, UBX_NAK, UBX_TIMEOUT };

void ubxSend(uint8_t cls, uint8_t id, const uint8_t *payload, uint8_t len) {
  uint8_t frame[16];
  uint8_t n = 0;
  frame[n++] = 0xB5;
  frame[n++] = 0x62;
  frame[n++] = cls;
  frame[n++] = id;
  frame[n++] = len;
  frame[n++] = 0;
  for (uint8_t i = 0; i < len; i++) frame[n++] = payload[i];
  uint8_t a = 0, b = 0;
  for (uint8_t i = 2; i < n; i++) { a += frame[i]; b += a; }   // UBX checksum
  frame[n++] = a;
  frame[n++] = b;
  GPSSerial.write(frame, n);
  GPSSerial.flush();
}

// Waits for UBX-ACK-ACK / UBX-ACK-NAK for the given message. NMEA bytes keep feeding the parser.
UbxResult ubxWaitAck(uint8_t cls, uint8_t id, uint32_t timeoutMs) {
  uint8_t win[10] = {0};
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (GPSSerial.available()) {
      uint8_t c = GPSSerial.read();
      gps.encode(c);
      memmove(win, win + 1, 9);
      win[9] = c;
      if (win[0] == 0xB5 && win[1] == 0x62 && win[2] == 0x05 && (win[3] == 0x01 || win[3] == 0x00) &&
          win[4] == 0x02 && win[5] == 0x00 && win[6] == cls && win[7] == id) {
        uint8_t a = 0, b = 0;
        for (uint8_t i = 2; i < 8; i++) { a += win[i]; b += a; }
        if (a == win[8] && b == win[9]) return win[3] == 0x01 ? UBX_ACK : UBX_NAK;
      }
    }
    delay(2);
  }
  return UBX_TIMEOUT;
}

// Returns true if the module acknowledged the new power mode.
bool applyGpsPowerMode(bool saveOn) {
  while (GPSSerial.available()) gps.encode(GPSSerial.read());   // drain old data
  uint8_t payload[2] = {0x08, (uint8_t)(saveOn ? GPS_PS_LPMODE : 0)};   // UBX-CFG-RXM: reserved1 = 8, lpMode
  ubxSend(0x06, 0x11, payload, 2);
  UbxResult r = ubxWaitAck(0x06, 0x11, UBX_ACK_TIMEOUT_MS);
  Serial.printf("GPS power save %s: %s\n", saveOn ? "ON" : "OFF",
                r == UBX_ACK ? "acknowledged" : (r == UBX_NAK ? "rejected (NAK)" : "no answer"));
  return r == UBX_ACK;
}

void toggleGpsPowerSave() {
  bool target = !gpsPowerSave;
  if (applyGpsPowerMode(target)) {
    gpsPowerSave = target;
    prefs.putBool("gpsps", gpsPowerSave);
    showToast("GPS power save", gpsPowerSave ? "ON" : "OFF");
  } else {
    showToast("GPS power save", "FAILED");
  }
}

void showToast(const char *l1, const char *l2) {
  strncpy(toastLine1, l1, sizeof(toastLine1) - 1);
  strncpy(toastLine2, l2, sizeof(toastLine2) - 1);
  toastUntilMs = millis() + 1500;
  drawToast();
}

void drawToast() {
#if DISPLAY_TYPE == 0
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 2);
  display.print(toastLine1);
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(toastLine2);
  display.display();
#elif DISPLAY_TYPE == 1
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 12);
  display.print(toastLine1);
  display.setTextSize(3);
  display.setCursor(0, 30);
  display.print(toastLine2);
  display.display();
#else
  display.fillScreen(ST77XX_BLACK);
  display.setTextSize(2);
  display.setCursor(10, 30);
  display.print(toastLine1);
  display.setTextSize(4);
  display.setCursor(10, 70);
  display.print(toastLine2);
#endif
}

void changeBrightness(int delta) {
  int level = brightnessLevel + delta;
  if (level < 0) level = 0;
  if (level > 2) level = 2;
  if (level == brightnessLevel) return;
  brightnessLevel = level;
  applyBrightness();
  forceRedraw = true;
  if (currentMode == MODE_SET) showSetScreen();
}

void initBacklight() {
#if (DISPLAY_TYPE == 2 || DISPLAY_TYPE == 3) && TFT_BL_PIN >= 0
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL_PIN, TFT_BL_PWM_HZ, 8);
  #else
    ledcSetup(0, TFT_BL_PWM_HZ, 8);
    ledcAttachPin(TFT_BL_PIN, 0);
  #endif
#endif
}

void applyBrightness() {
#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  if (brightnessLevel == 2) {
    display.dim(false);                       // default contrast
  } else if (brightnessLevel == 1) {
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0x40);
  } else {
    display.dim(true);                        // lowest contrast
  }
#elif TFT_BL_PIN >= 0
  static const uint8_t duty[3] = {25, 100, 255};
  uint8_t d = duty[brightnessLevel];
  #if !TFT_BL_ACTIVE_HIGH
    d = 255 - d;
  #endif
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL_PIN, d);
  #else
    ledcWrite(0, d);
  #endif
#endif
}

void updateAlert() {
  float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  bool over = speed > thresholdKmh && gps.speed.age() < 2000;

  if (over && !alertActive) {
    alertActive = true;
    alertStartMs = millis();
  } else if (!over) {
    alertActive = false;
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  }

  if (!alertActive) return;

  unsigned long elapsed = millis() - alertStartMs;
  unsigned long interval;

  if (elapsed < ALERT_PHASE1_MS) {
    interval = 1000;          // 1 Hz short beeps
  } else if (elapsed < ALERT_PHASE2_MS) {
    // linear increase from 1000 ms down to ~100 ms
    float t = (elapsed - ALERT_PHASE1_MS) / 10000.0f;
    interval = 1000 - (int)(t * 900);
    if (interval < 100) interval = 100;
  } else {
    interval = 0;             // continuous
  }

  if (interval == 0) {
    digitalWrite(BUZZER_PIN, HIGH);
    displayOn = (millis() / 200) % 2 == 0;  // fast flash
  } else {
    if (millis() - lastBeepToggle >= interval) {
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      lastBeepToggle = millis();
      displayOn = buzzerState;
    }
  }
}

float readBatteryPercent() {
  int raw = analogRead(BAT_ADC_PIN);
  float voltage = (raw / 4095.0f) * 3.3f * BATTERY_DIVIDER_RATIO;
  float percent = (voltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100.0f;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return percent;
}

void showSetScreen() {
#if DISPLAY_TYPE == 0
  // Compact but cleaner 128x32
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(20, 0);
  display.print(F("LIMIT"));
  display.setTextSize(2);
  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", thresholdKmh);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 12);
  display.print(buf);
  display.display();
#elif DISPLAY_TYPE == 1
  // Fancy 128x64 (GM009605)
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(28, 4);
  display.print(F("SPEED LIMIT"));
  display.drawLine(10, 16, 118, 16, SSD1306_WHITE);

  display.setTextSize(3);
  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", thresholdKmh);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, 22);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(46, 50);
  display.print(F("km/h"));
  display.setCursor(4, 56);
  display.print(F("Bat "));
  display.print(batteryPct);
  display.print(F("%"));
  display.display();
#else
  // TFT (ST7735S / ST7789V) – larger centered value
  display.fillScreen(ST77XX_BLACK);
  display.setTextSize(2);
  display.setCursor(30, 10);
  display.print(F("LIMIT"));
  display.setTextSize(4);
  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", thresholdKmh);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((display.width() - w) / 2, 50);
  display.print(buf);
  display.setTextSize(2);
  display.setCursor(70, 100);
  display.print(F("km/h"));
  display.setCursor(10, display.height() - 20);
  display.print(F("Bat "));
  display.print(batteryPct);
  display.print(F("%"));
#endif
}

void updateDisplay() {
  if (currentMode == MODE_SET || toastUntilMs != 0) return;

  float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  int bat = batteryPct;

  // Redraw only when something visible changed (saves SPI/I2C traffic and CPU time)
  int speedInt = (int)(speed + 0.5f);
  bool show = !alertActive || displayOn;
  static int lastSpeed = -1, lastSats = -1, lastBat = -1;
  static float lastLimit = -1;
  static bool lastShow = false, lastPs = false;
  if (!forceRedraw && speedInt == lastSpeed && sats == lastSats && bat == lastBat &&
      thresholdKmh == lastLimit && show == lastShow && gpsPowerSave == lastPs) {
    return;
  }
  forceRedraw = false;
  lastSpeed = speedInt; lastSats = sats; lastBat = bat; lastLimit = thresholdKmh; lastShow = show; lastPs = gpsPowerSave;

#if DISPLAY_TYPE == 0
  // 128x32 – larger speed
  display.clearDisplay();
  if (!alertActive || displayOn) {
    display.setTextSize(2);
    char spdBuf[8];
    snprintf(spdBuf, sizeof(spdBuf), "%.0f", speed);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(spdBuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 4);
    display.print(spdBuf);
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.print(F("S"));
    display.print(sats);
    display.print(F(" L"));
    display.print(thresholdKmh, 0);
    display.print(F(" B"));
    display.print((int)bat);
    if (gpsPowerSave) display.print(F(" P"));
  }
  display.display();
#elif DISPLAY_TYPE == 1
  // Large centered speed on 128x64
  display.clearDisplay();
  if (!alertActive || displayOn) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("SAT "));
    display.print(sats);
    if (gpsPowerSave) display.print(F(" PS"));
    char batBuf[8];
    snprintf(batBuf, sizeof(batBuf), "%d%%", (int)bat);
    display.setCursor(128 - 6 * strlen(batBuf), 0);
    display.print(batBuf);
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    display.setTextSize(3);
    char spdBuf[8];
    snprintf(spdBuf, sizeof(spdBuf), "%.0f", speed);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(spdBuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 18);
    display.print(spdBuf);

    display.setTextSize(1);
    display.setCursor(50, 46);
    display.print(F("km/h"));
    display.setCursor(0, 56);
    display.print(F("Limit "));
    display.print(thresholdKmh, 1);
    int barW = map((int)bat, 0, 100, 0, 40);
    display.drawRect(86, 55, 42, 8, SSD1306_WHITE);
    display.fillRect(87, 56, barW, 6, SSD1306_WHITE);
  }
  display.display();
#else
  // TFT – large centered speed
  if (!alertActive || displayOn) {
    display.fillScreen(ST77XX_BLACK);
    display.setTextSize(1);
    display.setCursor(5, 5);
    display.print(F("SAT "));
    display.print(sats);
    if (gpsPowerSave) display.print(F(" PS"));
    display.setCursor(display.width() - 50, 5);
    display.print((int)bat);
    display.print(F("%"));

    display.setTextSize(6);
    char spdBuf[8];
    snprintf(spdBuf, sizeof(spdBuf), "%.0f", speed);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(spdBuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((display.width() - w) / 2, 40);
    display.print(spdBuf);

    display.setTextSize(2);
    display.setCursor((display.width() - 60) / 2, 100);
    display.print(F("km/h"));
    display.setCursor(10, display.height() - 30);
    display.print(F("Limit "));
    display.print(thresholdKmh, 1);
  } else {
    display.fillScreen(ST77XX_BLACK);
  }
#endif
}
