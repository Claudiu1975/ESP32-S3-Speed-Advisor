/*
  ESP32-S3 Super Mini Speed Advisor
  Supports SSD1306 (128x32 / 128x64), ST7735S, ST7789V
  GPS: GY-NEO6MV2 / NEO-7MV2 / NEO-8MV2 / GT-U7 / ATGM336H / DX-GP10 / Quectel LC86L
      (any module that outputs standard NMEA at 9600 baud)

  Disclaimer: Hobby project only. Not a certified speedometer.
  Use at your own risk.
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
#define DISPLAY_TYPE 3

#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#elif DISPLAY_TYPE == 2
  #include <SPI.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_ST7735.h>
#elif DISPLAY_TYPE == 3
  #include <SPI.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_ST7789.h>
#endif

// ========== GPS CONFIG ==========
#define GPS_BAUD        9600
#define GPS_RX_PIN      44   // Board RX label (GPIO44)
#define GPS_TX_PIN      43   // Board TX label (GPIO43)
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
  #define TFT_CS        10   // -1 for modules without a CS pin (see ST7789_SPI_MODE)
  #define TFT_DC        7
  #define TFT_RST       15

  // 1 = landscape, 3 = landscape rotated 180 degrees (0 / 2 = portrait)
  #define TFT_ROTATION  1
#endif

#if DISPLAY_TYPE == 2
  // ST7735S panel variant. If colours are wrong (red/blue swapped) or there is
  // a garbage stripe at one edge, try another value:
  //   INITR_BLACKTAB      1.8" 128x160 (most common)
  //   INITR_GREENTAB      1.8" 128x160 (green tab on the screen protector)
  //   INITR_REDTAB        1.8" 128x160 (red tab)
  //   INITR_MINI160x80    0.96" 80x160
  //   INITR_MINI160x80_PLUGIN  0.96" 80x160 IPS (inverted colours)
  #define ST7735_TAB      INITR_BLACKTAB
  #define TFT_SPI_HZ      27000000
#elif DISPLAY_TYPE == 3
  // ST7789V panel size (native portrait resolution):
  //   240, 240  -> 1.3" / 1.54"
  //   240, 320  -> 2.0" / 2.4"
  //   135, 240  -> 1.14"
  //   170, 320  -> 1.9"
  #define ST7789_WIDTH    240
  #define ST7789_HEIGHT   240
  // Most modules: SPI_MODE0. 240x240 modules WITHOUT a CS pin: set TFT_CS to -1
  // and use SPI_MODE3.
  #define ST7789_SPI_MODE SPI_MODE0
  #define TFT_SPI_HZ      40000000
#endif

// ========== POWER SAVING / BRIGHTNESS ==========
#define CPU_FREQ_MHZ       80
#define LONG_PRESS_MS      800
#define BATTERY_READ_MS    1000

// ========== SERIAL DEBUG ==========
// 1 = print GPS / speed / alert status to the Serial Monitor once per second
#define DEBUG_SERIAL       1
#define DEBUG_INTERVAL_MS  1000

// ========== GPS POWER SAVE ==========
#define GPS_PS_LPMODE        1
#define TRIPLE_CLICK_GAP_MS  500
#define UBX_ACK_TIMEOUT_MS   1000

#if DISPLAY_TYPE == 2 || DISPLAY_TYPE == 3
  #define TFT_BL_PIN           13  // -1 if BL is wired to 3V3
  #define TFT_BL_ACTIVE_HIGH   1
  #define TFT_BL_PWM_HZ        5000
#endif

// ========== BATTERY CONFIG ==========
#define BATTERY_DIVIDER_RATIO  2.0f
#define BATTERY_MIN_V          3.0f
#define BATTERY_MAX_V          4.2f

// ========== THRESHOLD & TIMING ==========
float thresholdKmh = 90.0f;
const float STEP = 0.5f;

const uint32_t ALERT_PHASE1_MS = 45000;
const uint32_t ALERT_PHASE2_MS = 55000;

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
enum UbxResult { UBX_ACK, UBX_NAK, UBX_TIMEOUT };
Mode currentMode = MODE_SET;
unsigned long lastButtonMs = 0;
unsigned long alertStartMs = 0;
bool alertActive = false;
bool displayOn = true;
unsigned long lastBeepToggle = 0;
bool buzzerState = false;

int brightnessLevel = 2;
int batteryPct = 0;
unsigned long lastBatteryMs = 0;
bool forceRedraw = true;

bool gpsPowerSave = false;
bool gpsApplyPending = false;
unsigned long toastUntilMs = 0;
char toastLine1[24] = "";
char toastLine2[24] = "";

// ========== TFT LAYOUT ==========
#if DISPLAY_TYPE == 2 || DISPLAY_TYPE == 3
  #define COL_BG    ST77XX_BLACK
  #define COL_TEXT  ST77XX_WHITE
  #define COL_DIM   0x7BEF   // grey
  #define COL_LIMIT ST77XX_YELLOW

  uint8_t szSmall, szMid, szBig;   // text sizes chosen for the panel
  int16_t topH, bottomY, speedY, unitY, speedX;

  void computeLayout() {
    int16_t W = display.width();
    int16_t H = display.height();
    szSmall = (H >= 200) ? 2 : 1;
    szMid   = (H >= 120) ? 2 : 1;
    szBig   = (H >= 200) ? 9 : (H >= 120 ? 6 : 4);
    while (szBig > 2 && 3 * 6 * szBig > W - 4) szBig--;   // "200" must fit

    topH = 8 * szSmall + 6;
    int16_t bottomH = 8 * szMid + 6;
    bottomY = H - bottomH + 3;
    int16_t block = 8 * szBig + 4 + 8 * szMid;            // speed + "km/h"
    speedY = topH + (H - topH - bottomH - block) / 2;
    unitY  = speedY + 8 * szBig + 4;
    speedX = (W - (3 * 6 * szBig - szBig)) / 2;            // 3-digit field
  }

  uint8_t fitSize(const char *s, int16_t maxW, uint8_t maxSize) {
    uint8_t sz = maxSize;
    while (sz > 1 && (int16_t)(strlen(s) * 6 * sz) > maxW) sz--;
    return sz;
  }

  void printCentered(const char *s, int16_t y, uint8_t size, uint16_t fg) {
    display.setTextSize(size);
    display.setTextColor(fg);
    int16_t w = strlen(s) * 6 * size - size;
    display.setCursor((display.width() - w) / 2, y);
    display.print(s);
  }
#endif

void setup() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  Serial.begin(115200);
  prefs.begin("speedadv", false);
  gpsPowerSave = prefs.getBool("gpsps", false);
  gpsApplyPending = gpsPowerSave;
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(BTN_UP_PIN, INPUT_PULLUP);
  pinMode(BTN_DOWN_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 allocation failed"));
  }
  display.setTextColor(SSD1306_WHITE);
#else
  // Start SPI on our pins first, without MISO. Otherwise the default ESP32-S3
  // SPI pins are used and MISO lands on GPIO13, which is the backlight pin.
  SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);
  #if DISPLAY_TYPE == 2
    display.initR(ST7735_TAB);
  #else
    display.init(ST7789_WIDTH, ST7789_HEIGHT, ST7789_SPI_MODE);
  #endif
  display.setSPISpeed(TFT_SPI_HZ);
  display.setRotation(TFT_ROTATION);
  display.fillScreen(COL_BG);
  display.setTextColor(COL_TEXT);
  display.setTextWrap(false);
  computeLayout();
  Serial.printf("TFT %dx%d ready\n", display.width(), display.height());
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
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  handleButtons();

  if (currentMode == MODE_SET) {
    if (millis() - lastButtonMs > 5000) {
      currentMode = MODE_RUN;
      forceRedraw = true;
    }
  }

  if (gpsApplyPending && millis() > 3000) {
    gpsApplyPending = false;
    if (!applyGpsPowerMode(true)) {
      gpsPowerSave = false;
      prefs.putBool("gpsps", false);
      showToast("GPS power save", "FAILED");
    }
  }

  if (toastUntilMs != 0 && millis() > toastUntilMs) {
    toastUntilMs = 0;
    forceRedraw = true;
    if (currentMode == MODE_SET) showSetScreen();
  }

  if (millis() - lastBatteryMs >= BATTERY_READ_MS) {
    lastBatteryMs = millis();
    batteryPct = (int)(readBatteryPercent() + 0.5f);
  }

  updateAlert();
  updateDisplay();
  printDebug();

  delay(50);
}

void printDebug() {
#if DEBUG_SERIAL
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg < DEBUG_INTERVAL_MS) return;
  lastDbg = millis();
  Serial.printf("chars:%lu  fix:%s  sats:%d  speed:%.1f km/h  limit:%.1f  bat:%d%%  alert:%s\n",
                (unsigned long)gps.charsProcessed(),
                gps.location.isValid() ? "yes" : "no",
                gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                gps.speed.isValid() ? gps.speed.kmph() : 0.0,
                thresholdKmh, batteryPct, alertActive ? "ON" : "off");
#endif
}

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
    if (downClicks == 0 || now - lastDownClickMs > TRIPLE_CLICK_GAP_MS) {
      downClicks = 0;
      limitBeforeClicks = thresholdKmh;
    }
    downClicks++;
    lastDownClickMs = now;
    lastButtonMs = now;
    if (downClicks >= 3) {
      downClicks = 0;
      thresholdKmh = limitBeforeClicks;
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
  for (uint8_t i = 2; i < n; i++) { a += frame[i]; b += a; }
  frame[n++] = a;
  frame[n++] = b;
  GPSSerial.write(frame, n);
  GPSSerial.flush();
}

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

bool applyGpsPowerMode(bool saveOn) {
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  uint8_t payload[2] = {0x08, (uint8_t)(saveOn ? GPS_PS_LPMODE : 0)};
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
  int16_t W = display.width(), H = display.height();
  display.fillScreen(COL_BG);
  uint8_t s1 = fitSize(toastLine1, W - 8, 3);
  uint8_t s2 = fitSize(toastLine2, W - 8, H >= 200 ? 6 : 4);
  uint16_t c2 = COL_TEXT;
  if (strcmp(toastLine2, "ON") == 0) c2 = ST77XX_GREEN;
  else if (strcmp(toastLine2, "FAILED") == 0) c2 = ST77XX_RED;
  printCentered(toastLine1, H / 2 - 8 * s1 - 6, s1, COL_DIM);
  printCentered(toastLine2, H / 2 + 6, s2, c2);
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
    display.dim(false);
  } else if (brightnessLevel == 1) {
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0x40);
  } else {
    display.dim(true);
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
    interval = 1000;
  } else if (elapsed < ALERT_PHASE2_MS) {
    float t = (elapsed - ALERT_PHASE1_MS) / 10000.0f;
    interval = 1000 - (int)(t * 900);
    if (interval < 100) interval = 100;
  } else {
    interval = 0;
  }

  if (interval == 0) {
    digitalWrite(BUZZER_PIN, HIGH);
    displayOn = (millis() / 200) % 2 == 0;
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
  int16_t W = display.width(), H = display.height();
  display.fillScreen(COL_BG);

  uint8_t vs = fitSize("200.0", W - 8, szBig);
  int16_t vy = (H - 8 * vs) / 2;
  printCentered("SPEED LIMIT", vy - 8 * szMid - 10, szMid, COL_DIM);

  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", thresholdKmh);
  printCentered(buf, vy, vs, COL_LIMIT);
  printCentered("km/h", vy + 8 * vs + 6, szMid, COL_DIM);

  char batBuf[12];
  snprintf(batBuf, sizeof(batBuf), "Bat %d%%", batteryPct);
  display.setTextSize(szSmall);
  display.setTextColor(COL_DIM);
  display.setCursor(W - 2 - (int16_t)strlen(batBuf) * 6 * szSmall + szSmall, H - 8 * szSmall - 2);
  display.print(batBuf);
#endif
}

#if DISPLAY_TYPE == 2 || DISPLAY_TYPE == 3
// Draws only the parts of the run screen that changed, so the digits do not
// flicker. Text is drawn with a background colour over a fixed-width field,
// which overwrites the old value without clearing the screen first.
void tftDrawRun(int speedInt, int sats, int bat, bool show, bool over) {
  static int lSpeed = -1, lSats = -1, lBat = -1;
  static float lLimit = -1;
  static bool lShow = false, lOver = false, lPs = false;
  int16_t W = display.width();
  bool full = forceRedraw;

  if (full) {
    forceRedraw = false;
    display.fillScreen(COL_BG);
    display.drawFastHLine(0, topH - 3, W, COL_DIM);
    printCentered("km/h", unitY, szMid, COL_DIM);
  }

  // Top bar: satellites (+ PS) on the left, battery on the right
  if (full || sats != lSats || bat != lBat || gpsPowerSave != lPs) {
    display.fillRect(0, 0, W, topH - 4, COL_BG);
    display.setTextSize(szSmall);
    display.setTextColor(sats >= 4 ? ST77XX_GREEN : ST77XX_ORANGE);
    display.setCursor(2, 2);
    display.print(F("SAT "));
    display.print(sats);
    if (gpsPowerSave) {
      display.setTextColor(ST77XX_CYAN);
      display.print(F(" PS"));
    }
    char b[8];
    snprintf(b, sizeof(b), "%d%%", bat);
    display.setTextColor(bat <= 20 ? ST77XX_RED : COL_TEXT);
    display.setCursor(W - 2 - (int16_t)strlen(b) * 6 * szSmall + szSmall, 2);
    display.print(b);
  }

  // Speed: white normally, red over the limit, blank during the flash-off phase
  if (full || speedInt != lSpeed || show != lShow || over != lOver) {
    if (show) {
      char s[8];
      snprintf(s, sizeof(s), "%3d", speedInt > 999 ? 999 : speedInt);
      display.setTextSize(szBig);
      display.setTextColor(over ? ST77XX_RED : COL_TEXT, COL_BG);
      display.setCursor(speedX, speedY);
      display.print(s);
    } else {
      display.fillRect(speedX, speedY, 3 * 6 * szBig, 8 * szBig, COL_BG);
    }
  }

  // Bottom: current limit
  if (full || thresholdKmh != lLimit) {
    char l[16];
    snprintf(l, sizeof(l), "LIMIT %5.1f", thresholdKmh);
    display.setTextSize(szMid);
    display.setTextColor(COL_LIMIT, COL_BG);
    int16_t w = strlen(l) * 6 * szMid - szMid;
    display.setCursor((W - w) / 2, bottomY);
    display.print(l);
  }

  lSpeed = speedInt; lSats = sats; lBat = bat; lLimit = thresholdKmh;
  lShow = show; lOver = over; lPs = gpsPowerSave;
}
#endif

void updateDisplay() {
  if (currentMode == MODE_SET || toastUntilMs != 0) return;

  float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  int bat = batteryPct;

  int speedInt = (int)(speed + 0.5f);
  bool show = !alertActive || displayOn;

#if DISPLAY_TYPE == 2 || DISPLAY_TYPE == 3
  tftDrawRun(speedInt, sats, bat, show, alertActive);
#else
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
    display.print(thresholdKmh, 0);
  }
  display.display();
#endif
#endif
}
