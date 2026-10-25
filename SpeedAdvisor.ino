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

void setup() {
  Serial.begin(115200);
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
    }
  }

  updateAlert();
  updateDisplay();

  delay(50);  // light loop
}

void handleButtons() {
  static bool upPrev = HIGH, downPrev = HIGH;
  bool upNow = digitalRead(BTN_UP_PIN);
  bool downNow = digitalRead(BTN_DOWN_PIN);

  if (upNow == LOW && upPrev == HIGH) {
    thresholdKmh += STEP;
    if (thresholdKmh > 200.0f) thresholdKmh = 200.0f;
    lastButtonMs = millis();
    currentMode = MODE_SET;
    showSetScreen();
  }
  if (downNow == LOW && downPrev == HIGH) {
    thresholdKmh -= STEP;
    if (thresholdKmh < 5.0f) thresholdKmh = 5.0f;
    lastButtonMs = millis();
    currentMode = MODE_SET;
    showSetScreen();
  }
  upPrev = upNow;
  downPrev = downNow;
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
  display.print(readBatteryPercent(), 0);
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
  display.print(readBatteryPercent(), 0);
  display.print(F("%"));
#endif
}

void updateDisplay() {
  if (currentMode == MODE_SET) return;

  float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  float bat = readBatteryPercent();

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
