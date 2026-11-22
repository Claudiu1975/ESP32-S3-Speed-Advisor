// Version: PS_3 (button update: long press Up = brightness, long press Down = GPS power save; larger fonts)
/*
  ESP32-S3 Super Mini Speed Advisor
  Supports SSD1306 (128x32 / 128x64), ST7735S, ST7789V
  GPS: GY-NEO6MV2 / NEO-7MV2 / NEO-8MV2 / GT-U7 / ATGM336H / DX-GP10 / Quectel LC86L
      (any module that outputs standard NMEA at 9600 baud)

  Disclaimer: Hobby project only. Not a certified speedometer.
  Use at your own risk.
*/

#include <math.h>
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

// ========== DISPLAY LAYOUT (all displays) ==========
// Top line and bottom line are 1.5x the size they had in PS_1. The speed fills the space between
// them. Text is drawn from the built-in 5x7 font, scaled by any multiple of 0.5.
#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  #define COL_BG      SSD1306_BLACK
  #define COL_TEXT    SSD1306_WHITE
  #define COL_DIM     SSD1306_WHITE
  #define COL_LIMIT   SSD1306_WHITE
  #define COL_GREEN   SSD1306_WHITE
  #define COL_ORANGE  SSD1306_WHITE
  #define COL_RED     SSD1306_WHITE
  #define COL_CYAN    SSD1306_WHITE
  #define DISPLAY_FLUSH() display.display()
#else
  #define COL_BG      ST77XX_BLACK
  #define COL_TEXT    ST77XX_WHITE
  #define COL_DIM     0x7BEF   // grey
  #define COL_LIMIT   ST77XX_YELLOW
  #define COL_GREEN   ST77XX_GREEN
  #define COL_ORANGE  ST77XX_ORANGE
  #define COL_RED     ST77XX_RED
  #define COL_CYAN    ST77XX_CYAN
  #define DISPLAY_FLUSH() do {} while (0)
#endif

bool hasTop = true;                 // false on the 128x32 OLED (no room for a top line)
float tS = 1.5f, bS = 1.5f;         // text scale of the top line / bottom line
int16_t gapPx = 2, divY = 0, speedTop = 0, speedH = 0, bottomY = 0;

static GFXcanvas1 glyphCanvas(6, 8);   // used to read the pixels of the built-in font

static int16_t rnd(float v) { return (int16_t)(v + 0.5f); }
static int16_t capH(float sc) { return rnd(7 * sc); }                    // height of capital letters / digits
static int16_t textW(int n, float sc) { return rnd(n * 6 * sc - sc); }  // width of n characters

void computeLayout() {
  int16_t H = display.height();
#if DISPLAY_TYPE == 0 || DISPLAY_TYPE == 1
  float prevTop = 1, prevBottom = 1;          // sizes used in PS_1
#else
  float prevTop = (H >= 200) ? 2 : 1;
  float prevBottom = (H >= 120) ? 2 : 1;
#endif
  hasTop = (H >= 48);
  tS = 1.5f * prevTop;
  bS = 1.5f * prevBottom;
  gapPx = (H <= 64) ? 2 : 4;
  divY = hasTop ? capH(tS) + 1 : 0;           // thin line under the top line
  speedTop = hasTop ? divY + 1 + gapPx : 0;
  bottomY = H - capH(bS);
  speedH = bottomY - gapPx - speedTop;        // everything between the two lines
}

// Largest scale (step 0.5) at which n characters fit in maxW x maxH
float fillScale(int n, int16_t maxW, int16_t maxH) {
  float s = floorf(fminf(maxH / 7.0f, maxW / (n * 6.0f - 1.0f)) * 2.0f) / 2.0f;
  return s < 1.0f ? 1.0f : s;
}

// Wanted scale, reduced in steps of 0.5 until n characters fit in maxW
float fitScale(int n, float wanted, int16_t maxW) {
  float s = wanted;
  while (s > 1.0f && textW(n, s) > maxW) s -= 0.5f;
  return s;
}

void drawGlyphScaled(int16_t x, int16_t y, char c, float sc, uint16_t fg) {
  glyphCanvas.fillScreen(0);
  glyphCanvas.drawChar(0, 0, c, 1, 0, 1);
  for (int j = 0; j < 8; j++) {
    int16_t y0 = y + rnd(j * sc), y1 = y + rnd((j + 1) * sc);
    int i = 0;
    while (i < 5) {
      if (!glyphCanvas.getPixel(i, j)) { i++; continue; }
      int k = i;
      while (k < 5 && glyphCanvas.getPixel(k, j)) k++;      // horizontal run of set pixels
      int16_t x0 = x + rnd(i * sc), x1 = x + rnd(k * sc);
      display.fillRect(x0, y0, x1 - x0, y1 - y0, fg);
      i = k;
    }
  }
}

void drawText(int16_t x, int16_t y, const char *s, float sc, uint16_t fg) {
  for (int k = 0; s[k]; k++) {
    if (s[k] != ' ') drawGlyphScaled(x + rnd(k * 6 * sc), y, s[k], sc, fg);
  }
}

void drawTextCentered(const char *s, int16_t y, float sc, uint16_t fg) {
  drawText((display.width() - textW(strlen(s), sc)) / 2, y, s, sc, fg);
}

void drawTextRight(const char *s, int16_t y, float sc, uint16_t fg) {
  drawText(display.width() - 1 - textW(strlen(s), sc), y, s, sc, fg);
}

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
  computeLayout();
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
  bool upNow = digitalRead(BTN_UP_PIN);
  bool downNow = digitalRead(BTN_DOWN_PIN);
  unsigned long now = millis();

  // UP: short = limit +STEP, long = cycle brightness
  if (upNow == LOW && upPrev == HIGH) {
    upDownMs = now;
    upLongDone = false;
  }
  if (upNow == LOW && !upLongDone && now - upDownMs >= LONG_PRESS_MS) {
    upLongDone = true;
    cycleBrightness();
  }
  if (upNow == HIGH && upPrev == LOW && !upLongDone) {
    thresholdKmh += STEP;
    if (thresholdKmh > 200.0f) thresholdKmh = 200.0f;
    lastButtonMs = now;
    currentMode = MODE_SET;
    showSetScreen();
  }

  // DOWN: short = limit -STEP, long = GPS power save on/off
  if (downNow == LOW && downPrev == HIGH) {
    downDownMs = now;
    downLongDone = false;
  }
  if (downNow == LOW && !downLongDone && now - downDownMs >= LONG_PRESS_MS) {
    downLongDone = true;
    toggleGpsPowerSave();
  }
  if (downNow == HIGH && downPrev == LOW && !downLongDone) {
    thresholdKmh -= STEP;
    if (thresholdKmh < 5.0f) thresholdKmh = 5.0f;
    lastButtonMs = now;
    currentMode = MODE_SET;
    showSetScreen();
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
  int16_t W = display.width(), H = display.height();
  display.fillScreen(COL_BG);
  float s1 = fitScale(strlen(toastLine1), hasTop ? tS : bS, W - 4);
  drawTextCentered(toastLine1, 0, s1, COL_DIM);
  int16_t y2 = capH(s1) + rnd(s1) + gapPx;         // room for descenders (p, y, g) and a gap
  int16_t h2 = H - y2;
  float s2 = fillScale(strlen(toastLine2), W - 4, h2);
  uint16_t c2 = COL_TEXT;
  if (strcmp(toastLine2, "ON") == 0) c2 = COL_GREEN;
  else if (strcmp(toastLine2, "FAILED") == 0) c2 = COL_RED;
  drawTextCentered(toastLine2, y2 + (h2 - capH(s2)) / 2, s2, c2);
  DISPLAY_FLUSH();
}

void cycleBrightness() {
  brightnessLevel = (brightnessLevel + 1) % 3;   // 0 -> 1 -> 2 -> 0 ...
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
  int16_t W = display.width();
  display.fillScreen(COL_BG);

  char v[8];
  snprintf(v, sizeof(v), "%.1f", thresholdKmh);
  float sc = fillScale(strlen(v), W - 4, speedH);
  drawTextCentered(v, speedTop + (speedH - capH(sc)) / 2, sc, COL_LIMIT);

  if (hasTop) {
    display.drawFastHLine(0, divY, W, COL_DIM);
    drawTextCentered("LIMIT (km/h)", 0, tS, COL_DIM);
    char b[12];
    snprintf(b, sizeof(b), "Bat %d%%", batteryPct);
    drawTextRight(b, bottomY, bS, COL_DIM);
  } else {
    drawTextCentered("LIMIT km/h", bottomY, bS, COL_DIM);
  }
  DISPLAY_FLUSH();
}

// Draws only the parts of the run screen that changed.
void drawRun(int speedInt, int sats, int bat, bool show, bool over) {
  static int lSpeed = -1, lN = -1, lSats = -1, lBat = -1;
  static float lLimit = -1;
  static bool lShow = false, lOver = false, lPs = false;
  int16_t W = display.width(), H = display.height();
  bool full = forceRedraw;
  bool changed = false;
  int n = (speedInt >= 100) ? 3 : 2;                      // digits shown, the speed is as large as possible
  bool statusChanged = sats != lSats || bat != lBat || gpsPowerSave != lPs;

  if (full) {
    forceRedraw = false;
    display.fillScreen(COL_BG);
    if (hasTop) display.drawFastHLine(0, divY, W, COL_DIM);
    changed = true;
  }

  // Top line: satellites (+ PS) on the left, battery on the right
  if (hasTop && (full || statusChanged)) {
    display.fillRect(0, 0, W, divY, COL_BG);
    char l[16];
    snprintf(l, sizeof(l), "SAT %d", sats);
    drawText(1, 0, l, tS, sats >= 4 ? COL_GREEN : COL_ORANGE);
    if (gpsPowerSave) drawText(1 + rnd(strlen(l) * 6 * tS), 0, " PS", tS, COL_CYAN);
    char b[8];
    snprintf(b, sizeof(b), "%d%%", bat);
    drawTextRight(b, 0, tS, bat <= 20 ? COL_RED : COL_TEXT);
    changed = true;
  }

  // Middle: the speed, red over the limit, blank during the flash-off phase
  if (full || speedInt != lSpeed || n != lN || show != lShow || over != lOver) {
    display.fillRect(0, speedTop, W, speedH, COL_BG);
    if (show) {
      char s[8];
      snprintf(s, sizeof(s), "%*d", n, speedInt > 999 ? 999 : speedInt);
      float sc = fillScale(n, W - 4, speedH);
      drawTextCentered(s, speedTop + (speedH - capH(sc)) / 2, sc, over ? COL_RED : COL_TEXT);
    }
    changed = true;
  }

  // Bottom line: the limit (on the 128x32 OLED also satellites and battery)
  if (full || thresholdKmh != lLimit || (!hasTop && statusChanged)) {
    display.fillRect(0, bottomY, W, H - bottomY, COL_BG);
    char opt[3][24];
    if (hasTop) {
      snprintf(opt[0], sizeof(opt[0]), "LIMIT %.1f", thresholdKmh);
      snprintf(opt[1], sizeof(opt[1]), "LIM %.1f", thresholdKmh);
      snprintf(opt[2], sizeof(opt[2]), "%.1f", thresholdKmh);
    } else {
      snprintf(opt[0], sizeof(opt[0]), "S%d L%.1f B%d%s", sats, thresholdKmh, bat, gpsPowerSave ? " P" : "");
      snprintf(opt[1], sizeof(opt[1]), "S%d L%.0f B%d%s", sats, thresholdKmh, bat, gpsPowerSave ? " P" : "");
      snprintf(opt[2], sizeof(opt[2]), "L%.0f", thresholdKmh);
    }
    int pick = 2;
    for (int i = 0; i < 3; i++) {                          // first text that fits at this scale
      if (textW(strlen(opt[i]), bS) <= W - 4) { pick = i; break; }
    }
    drawTextCentered(opt[pick], bottomY, bS, COL_LIMIT);
    changed = true;
  }

  lSpeed = speedInt; lN = n; lSats = sats; lBat = bat; lLimit = thresholdKmh;
  lShow = show; lOver = over; lPs = gpsPowerSave;
  if (changed) DISPLAY_FLUSH();
}

void updateDisplay() {
  if (currentMode == MODE_SET || toastUntilMs != 0) return;

  float speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0f;
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  int speedInt = (int)(speed + 0.5f);
  bool show = !alertActive || displayOn;
  drawRun(speedInt, sats, batteryPct, show, alertActive);
}
