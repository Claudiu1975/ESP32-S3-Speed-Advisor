// Version: PS_6 (Bluetooth LE: the Android web app shows the speed, warns and changes the settings)
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

// ========== WEB PORTAL (Wi-Fi captive portal) ==========
// The board opens its own Wi-Fi network. Connect a phone to it and the portal page
// opens by itself (or browse to http://192.168.4.1). The page shows the live speed,
// warns when the limit is exceeded and has the same settings as the buttons.
#define PORTAL_ENABLE        1
#define PORTAL_SSID          "SpeedAdvisor"
#define PORTAL_PASS          "speedadv"          // 8+ characters, or "" for an open network
#define PORTAL_CHANNEL       6
#define PORTAL_TX_POWER      WIFI_POWER_8_5dBm   // low power: enough inside a car, saves battery
#define PORTAL_IDLE_OFF_MIN  10   // Wi-Fi off after this many minutes with no phone connected
                                  // (0 = never). Power-cycle the board to get it back.

#if PORTAL_ENABLE
  #include <WiFi.h>
  #include <WebServer.h>
  #include <DNSServer.h>
  // Declared here because the Arduino IDE does not find functions that come after
  // the long HTML text (raw string) further down
  void portalStart();
  void portalStop();
  void portalLoop();
  void portalSendStatus();
  void portalRedirect();
#endif

// ========== BLUETOOTH LE ==========
// The Android web app (app/ folder, opened in Chrome) connects over Bluetooth LE and
// does the same as the Wi-Fi page. Needs the NimBLE-Arduino library (1.4.x or 2.x).
// With Wi-Fi and BLE both on, choose Tools -> Partition Scheme -> "Huge APP (3MB No OTA)".
#define BLE_ENABLE           1
#define BLE_NAME             "SpeedAdvisor"
#define BLE_NOTIFY_MS        300   // how often the app gets the speed

#if BLE_ENABLE
  #include <NimBLEDevice.h>
  // Declared here for the same reason as the portal functions above
  void bleStart();
  void bleLoop();
  void bleUpdateStatus(bool notify);
  void bleRunCommand(const uint8_t *b);
#endif

// ========== DISPLAY SELECTION ==========
// 0 = SSD1306 128x32 I2C
// 1 = SSD1306 128x64 I2C (GM009605 style)
// 2 = ST7735S SPI
// 3 = ST7789V SPI
// All four drivers are built in. The type is chosen at run time (hold Down and
// short press Up, or from the web page) and remembered after power-off.
// DISPLAY_TYPE is only the type used on the very first start.
#define DISPLAY_TYPE 3
#define DISPLAY_TYPE_COUNT 4

#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_ST7789.h>

// ========== GPS CONFIG ==========
#define GPS_BAUD        9600
#define GPS_RX_PIN      44   // Board RX label (GPIO44)
#define GPS_TX_PIN      43   // Board TX label (GPIO43)
#define BUZZER_PIN      4
#define BTN_UP_PIN      5
#define BTN_DOWN_PIN    6
#define BAT_ADC_PIN     1

// OLED (I2C)
#define I2C_SDA       8
#define I2C_SCL       9
#define OLED_ADDR     0x3C

// TFT (SPI)
#define TFT_MOSI      11
#define TFT_SCK       12
#define TFT_CS        10   // -1 for modules without a CS pin (see ST7789_SPI_MODE)
#define TFT_DC        7
#define TFT_RST       15

// 1 = landscape, 3 = landscape rotated 180 degrees (0 / 2 = portrait)
#define TFT_ROTATION  1

// ST7735S
  // ST7735S panel variant. If colours are wrong (red/blue swapped) or there is
  // a garbage stripe at one edge, try another value:
  //   INITR_BLACKTAB      1.8" 128x160 (most common)
  //   INITR_GREENTAB      1.8" 128x160 (green tab on the screen protector)
  //   INITR_REDTAB        1.8" 128x160 (red tab)
  //   INITR_MINI160x80    0.96" 80x160
  //   INITR_MINI160x80_PLUGIN  0.96" 80x160 IPS (inverted colours)
  #define ST7735_TAB      INITR_BLACKTAB
  #define ST7735_SPI_HZ   27000000

// ST7789V
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
  #define ST7789_SPI_HZ   40000000

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

#define TFT_BL_PIN           13  // TFT backlight. -1 if BL is wired to 3V3
#define TFT_BL_ACTIVE_HIGH   1
#define TFT_BL_PWM_HZ        5000

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

// Only the driver of the selected display type exists; gfx points to it.
Adafruit_GFX *gfx = nullptr;
Adafruit_SSD1306 *oled = nullptr;
Adafruit_ST7735 *st7735 = nullptr;
Adafruit_ST7789 *st7789 = nullptr;
int displayType = DISPLAY_TYPE;
const char *const DISPLAY_NAMES[DISPLAY_TYPE_COUNT] = {"OLED 128x32", "OLED 128x64", "ST7735", "ST7789"};

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
unsigned long alertIntervalMs = 0;   // current beep interval (0 = continuous tone), for the web page

// ========== DISPLAY LAYOUT (all displays) ==========
// Top line and bottom line are 1.5x the size they had in PS_1. The speed fills the space between
// them. Text is drawn from the built-in 5x7 font, scaled by any multiple of 0.5.
// Colours are set in initDisplay(): all white on the OLEDs, real colours on the TFTs
uint16_t COL_BG, COL_TEXT, COL_DIM, COL_LIMIT, COL_GREEN, COL_ORANGE, COL_RED, COL_CYAN;

void DISPLAY_FLUSH() {
  if (oled) oled->display();   // the OLED draws into a buffer; the TFTs draw directly
}

bool hasTop = true;                 // false on the 128x32 OLED (no room for a top line)
float tS = 1.5f, bS = 1.5f;         // text scale of the top line / bottom line
int16_t gapPx = 2, divY = 0, speedTop = 0, speedH = 0, bottomY = 0;

static GFXcanvas1 glyphCanvas(6, 8);   // used to read the pixels of the built-in font

static int16_t rnd(float v) { return (int16_t)(v + 0.5f); }
static int16_t capH(float sc) { return rnd(7 * sc); }                    // height of capital letters / digits
static int16_t textW(int n, float sc) { return rnd(n * 6 * sc - sc); }  // width of n characters

void computeLayout() {
  int16_t H = gfx->height();
  float prevTop = 1, prevBottom = 1;          // sizes used in PS_1 (OLED)
  if (!oled) {
    prevTop = (H >= 200) ? 2 : 1;
    prevBottom = (H >= 120) ? 2 : 1;
  }
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
      gfx->fillRect(x0, y0, x1 - x0, y1 - y0, fg);
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
  drawText((gfx->width() - textW(strlen(s), sc)) / 2, y, s, sc, fg);
}

void drawTextRight(const char *s, int16_t y, float sc, uint16_t fg) {
  drawText(gfx->width() - 1 - textW(strlen(s), sc), y, s, sc, fg);
}

// Starts the driver of displayType (and drops the previous one, when switching)
void initDisplay() {
  delete oled;   oled = nullptr;
  delete st7735; st7735 = nullptr;
  delete st7789; st7789 = nullptr;

  if (displayType <= 1) {
    static bool wireStarted = false;
    if (!wireStarted) { Wire.begin(I2C_SDA, I2C_SCL); wireStarted = true; }
    oled = new Adafruit_SSD1306(128, displayType == 0 ? 32 : 64, &Wire, -1);
    if (!oled->begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
      Serial.println(F("SSD1306 allocation failed"));
    }
    gfx = oled;
    COL_BG = SSD1306_BLACK;
    COL_TEXT = COL_DIM = COL_LIMIT = COL_GREEN = COL_ORANGE = COL_RED = COL_CYAN = SSD1306_WHITE;
  } else {
    // Start SPI on our pins first, without MISO. Otherwise the default ESP32-S3
    // SPI pins are used and MISO lands on GPIO13, which is the backlight pin.
    static bool spiStarted = false;
    if (!spiStarted) { SPI.begin(TFT_SCK, -1, TFT_MOSI, -1); spiStarted = true; }
    if (displayType == 2) {
      st7735 = new Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
      st7735->initR(ST7735_TAB);
      st7735->setSPISpeed(ST7735_SPI_HZ);
      gfx = st7735;
    } else {
      st7789 = new Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
      st7789->init(ST7789_WIDTH, ST7789_HEIGHT, ST7789_SPI_MODE);
      st7789->setSPISpeed(ST7789_SPI_HZ);
      gfx = st7789;
    }
    gfx->setRotation(TFT_ROTATION);
    COL_BG = ST77XX_BLACK;   COL_TEXT = ST77XX_WHITE;  COL_DIM = 0x7BEF;  // grey
    COL_LIMIT = ST77XX_YELLOW; COL_GREEN = ST77XX_GREEN; COL_ORANGE = ST77XX_ORANGE;
    COL_RED = ST77XX_RED;    COL_CYAN = ST77XX_CYAN;
  }
  gfx->fillScreen(COL_BG);
  gfx->setTextColor(COL_TEXT);
  gfx->setTextWrap(false);
  gfx->setTextSize(1);
  computeLayout();
  Serial.printf("Display: %s %dx%d\n", DISPLAY_NAMES[displayType], gfx->width(), gfx->height());
}

// Shared by the buttons and the web portal. Saved, and confirmed on the new gfx->
void setDisplayType(int t) {
  if (t < 0 || t >= DISPLAY_TYPE_COUNT) return;
  displayType = t;
  prefs.putUChar("disp", displayType);
  initDisplay();
  applyBrightness();
  forceRedraw = true;
  showToast("Display", DISPLAY_NAMES[displayType]);
}

void cycleDisplayType() {
  setDisplayType((displayType + 1) % DISPLAY_TYPE_COUNT);
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

  displayType = prefs.getUChar("disp", DISPLAY_TYPE);
  if (displayType >= DISPLAY_TYPE_COUNT) displayType = DISPLAY_TYPE;
  initDisplay();
  initBacklight();
  applyBrightness();
  batteryPct = (int)(readBatteryPercent() + 0.5f);
  lastBatteryMs = millis();
  lastButtonMs = millis();
  showSetScreen();
#if PORTAL_ENABLE
  portalStart();
#endif
#if BLE_ENABLE
  bleStart();
#endif
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
#if PORTAL_ENABLE
  portalLoop();
#endif
#if BLE_ENABLE
  bleLoop();
#endif

  delay(50);
}

void printDebug() {
#if DEBUG_SERIAL
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg < DEBUG_INTERVAL_MS) return;
  lastDbg = millis();
  Serial.printf("chars:%lu  fix:%s  sats:%d  speed:%.1f km/h  limit:%.1f  bat:%d%%  alert:%s  display:%s\n",
                (unsigned long)gps.charsProcessed(),
                gps.location.isValid() ? "yes" : "no",
                gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                gps.speed.isValid() ? gps.speed.kmph() : 0.0,
                thresholdKmh, batteryPct, alertActive ? "ON" : "off", DISPLAY_NAMES[displayType]);
#endif
}

void handleButtons() {
  static bool upPrev = HIGH, downPrev = HIGH;
  static unsigned long upDownMs = 0, downDownMs = 0;
  static bool upLongDone = false, upCombo = false, downCombo = false;
  bool upNow = digitalRead(BTN_UP_PIN);
  bool downNow = digitalRead(BTN_DOWN_PIN);
  unsigned long now = millis();

  // DOWN pressed (checked first, so a press of both in the same loop counts as a combo)
  if (downNow == LOW && downPrev == HIGH) {
    downDownMs = now;
    downCombo = false;
  }

  // UP: short = limit +STEP, long = cycle brightness.
  //     Short press while Down is held = next display type.
  if (upNow == LOW && upPrev == HIGH) {
    upDownMs = now;
    upLongDone = false;
    upCombo = (downNow == LOW);
    if (upCombo) downCombo = true;          // releasing Down later does nothing
  }
  if (upNow == LOW && !upCombo && !upLongDone && now - upDownMs >= LONG_PRESS_MS) {
    upLongDone = true;
    cycleBrightness();
  }
  if (upNow == HIGH && upPrev == LOW) {
    if (upCombo) cycleDisplayType();
    else if (!upLongDone) setLimit(thresholdKmh + STEP);
    upCombo = false;
  }

  // DOWN released: short = limit -STEP, long = GPS power save on/off.
  // Acted on at release, so holding Down for the display combo never switches power save.
  if (downNow == HIGH && downPrev == LOW) {
    if (!downCombo) {
      if (now - downDownMs >= LONG_PRESS_MS) toggleGpsPowerSave();
      else setLimit(thresholdKmh - STEP);
    }
    downCombo = false;
  }

  upPrev = upNow;
  downPrev = downNow;
}

// Shared by the buttons and the web portal: new limit (rounded to STEP, 5..200 km/h),
// shown on the set screen for 5 s like a button press.
void setLimit(float v) {
  v = roundf(v / STEP) * STEP;
  if (v > 200.0f) v = 200.0f;
  if (v < 5.0f) v = 5.0f;
  thresholdKmh = v;
  lastButtonMs = millis();
  currentMode = MODE_SET;
  showSetScreen();
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
  setGpsPowerSave(!gpsPowerSave);
}

void setGpsPowerSave(bool target) {
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
  int16_t W = gfx->width(), H = gfx->height();
  gfx->fillScreen(COL_BG);
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
  setBrightness((brightnessLevel + 1) % 3);      // 0 -> 1 -> 2 -> 0 ...
}

void setBrightness(int level) {
  if (level < 0) level = 0;
  if (level > 2) level = 2;
  brightnessLevel = level;
  applyBrightness();
  forceRedraw = true;
  if (currentMode == MODE_SET) showSetScreen();
}

void initBacklight() {
#if TFT_BL_PIN >= 0
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL_PIN, TFT_BL_PWM_HZ, 8);
  #else
    ledcSetup(0, TFT_BL_PWM_HZ, 8);
    ledcAttachPin(TFT_BL_PIN, 0);
  #endif
#endif
}

void applyBrightness() {
  if (oled) {
    if (brightnessLevel == 2) {
      oled->dim(false);
    } else if (brightnessLevel == 1) {
      oled->ssd1306_command(SSD1306_SETCONTRAST);
      oled->ssd1306_command(0x40);
    } else {
      oled->dim(true);
    }
    return;
  }
#if TFT_BL_PIN >= 0
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

  if (!alertActive) { alertIntervalMs = 0; return; }

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
  alertIntervalMs = interval;

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
  int16_t W = gfx->width();
  gfx->fillScreen(COL_BG);

  char v[8];
  snprintf(v, sizeof(v), "%.1f", thresholdKmh);
  float sc = fillScale(strlen(v), W - 4, speedH);
  drawTextCentered(v, speedTop + (speedH - capH(sc)) / 2, sc, COL_LIMIT);

  if (hasTop) {
    gfx->drawFastHLine(0, divY, W, COL_DIM);
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
  int16_t W = gfx->width(), H = gfx->height();
  bool full = forceRedraw;
  bool changed = false;
  int n = (speedInt >= 100) ? 3 : 2;                      // digits shown, the speed is as large as possible
  bool statusChanged = sats != lSats || bat != lBat || gpsPowerSave != lPs;

  if (full) {
    forceRedraw = false;
    gfx->fillScreen(COL_BG);
    if (hasTop) gfx->drawFastHLine(0, divY, W, COL_DIM);
    changed = true;
  }

  // Top line: satellites (+ PS) on the left, battery on the right
  if (hasTop && (full || statusChanged)) {
    gfx->fillRect(0, 0, W, divY, COL_BG);
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
    gfx->fillRect(0, speedTop, W, speedH, COL_BG);
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
    gfx->fillRect(0, bottomY, W, H - bottomY, COL_BG);
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

// ========== WEB PORTAL ==========
#if PORTAL_ENABLE

WebServer server(80);
DNSServer dnsServer;
bool portalOn = false;
unsigned long portalLastClientMs = 0;

static const char PORTAL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Speed Advisor</title>
<style>
body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:#000;color:#fff;text-align:center;-webkit-user-select:none;user-select:none}
.top{display:flex;justify-content:space-between;padding:10px 14px;font-size:18px;color:#aaa}
#spd{font-size:42vw;font-weight:700;line-height:1;margin:2px 0;font-variant-numeric:tabular-nums}
#unit{color:#888;margin-top:-6px;font-size:18px}
#warn{visibility:hidden;background:#e00;color:#fff;font-size:26px;font-weight:700;padding:10px;margin:6px 10px;border-radius:10px}
body.over #spd{color:#f22}
body.over #warn{visibility:visible}
body.flash{background:#600}
.card{background:#161616;border-radius:14px;margin:10px;padding:12px}
.lbl{color:#888;font-size:13px;letter-spacing:1px;margin-bottom:6px}
.row{display:flex;justify-content:center;align-items:center;flex-wrap:wrap}
.lim{font-size:44px;color:#fd0;font-weight:700;min-width:150px;display:inline-block}
button{background:#333;color:#fff;border:0;border-radius:10px;font-size:22px;padding:12px 18px;margin:4px;min-width:64px}
#d0,#d1,#d2,#d3{font-size:17px;padding:12px 10px}
button:active{background:#555}
button.on{background:#0a7}
input{font-size:22px;width:100px;padding:10px;border-radius:10px;border:0;text-align:center;margin:4px}
#link{color:#f80}
</style></head><body>
<div class="top"><span id="sat">SAT -</span><span id="link"></span><span id="bat">--%</span></div>
<div id="warn">SLOW DOWN</div>
<div id="spd">--</div><div id="unit">km/h</div>
<div class="card"><div class="lbl">LIMIT (km/h)</div>
<div class="row"><button onclick="api('limit?d=-1')">&minus;</button><span class="lim" id="lim">--</span><button onclick="api('limit?d=1')">+</button></div>
<div class="row"><input id="limIn" type="number" step="0.5" min="5" max="200" inputmode="decimal" placeholder="km/h"><button onclick="setLim()">Set</button></div></div>
<div class="card"><div class="lbl">BRIGHTNESS</div><div class="row">
<button id="b0" onclick="api('bright?l=0')">Low</button><button id="b1" onclick="api('bright?l=1')">Mid</button><button id="b2" onclick="api('bright?l=2')">High</button></div></div>
<div class="card"><div class="lbl">DISPLAY TYPE</div><div class="row">
<button id="d0" onclick="api('display?t=0')">OLED 128x32</button><button id="d1" onclick="api('display?t=1')">OLED 128x64</button>
<button id="d2" onclick="api('display?t=2')">ST7735</button><button id="d3" onclick="api('display?t=3')">ST7789</button></div></div>
<div class="card"><div class="lbl">GPS POWER SAVE</div><div class="row"><button id="ps" onclick="api('gpsps?on='+(S.ps?0:1))">--</button></div></div>
<div class="card"><div class="lbl">PHONE ALERT</div><div class="row"><button id="snd" onclick="togSnd()">Sound OFF</button></div></div>
<script>
var S={},fails=0,snd=false,ac=null,gain=null,beepOn=false;
function $(i){return document.getElementById(i)}
function api(p){return fetch('/api/'+p).then(function(r){return r.json()}).then(show).catch(function(){})}
function setLim(){var v=parseFloat($('limIn').value);if(!isNaN(v))api('limit?v='+v);$('limIn').value='';$('limIn').blur()}
function show(s){
 S=s;fails=0;$('link').textContent='';
 $('spd').textContent=s.fix?Math.round(s.spd):'--';
 $('lim').textContent=s.lim.toFixed(1);
 $('sat').textContent='SAT '+s.sats+(s.ps?' PS':'');
 $('sat').style.color=s.sats>=4?'#0c0':'#f80';
 $('bat').textContent=s.bat+'%';$('bat').style.color=s.bat<=20?'#f22':'#aaa';
 for(var i=0;i<3;i++)$('b'+i).className=(s.br==i)?'on':'';
 for(var i=0;i<4;i++)$('d'+i).className=(s.disp==i)?'on':'';
 $('ps').textContent=s.ps?'ON':'OFF';$('ps').className=s.ps?'on':'';
 document.body.classList.toggle('over',!!s.over);
}
function poll(){
 fetch('/api/status').then(function(r){return r.json()}).then(show)
 .catch(function(){if(++fails>2){$('link').textContent='NO LINK';S.over=0;document.body.classList.remove('over')}})
 .then(function(){setTimeout(poll,500)});
}
function togSnd(){
 snd=!snd;
 if(snd&&!ac){
  var C=window.AudioContext||window.webkitAudioContext;
  if(C){ac=new C();var o=ac.createOscillator();gain=ac.createGain();o.type='square';o.frequency.value=2400;
  gain.gain.value=0;o.connect(gain);gain.connect(ac.destination);o.start();}
 }
 if(ac&&ac.resume)ac.resume();
 if(!snd&&gain)gain.gain.value=0;
 $('snd').textContent=snd?'Sound ON':'Sound OFF';$('snd').className=snd?'on':'';
}
// Same rhythm as the buzzer: on/off every "iv" ms, continuous tone when iv is 0
setInterval(function(){
 var on=false;
 if(S.over)on=(S.iv==0)||(Math.floor(Date.now()/S.iv)%2==0);
 document.body.classList.toggle('flash',on);
 if(gain)gain.gain.value=(snd&&on)?0.25:0;
 if(navigator.vibrate){if(on&&!beepOn)navigator.vibrate(S.iv==0?60000:S.iv);else if(!on&&beepOn)navigator.vibrate(0);}
 beepOn=on;
},50);
poll();
</script></body></html>)HTML";

void portalSendStatus() {
  bool fix = gps.speed.isValid() && gps.speed.age() < 2000;
  int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"spd\":%.1f,\"fix\":%d,\"lim\":%.1f,\"sats\":%d,\"bat\":%d,\"over\":%d,\"iv\":%lu,\"br\":%d,\"ps\":%d,\"disp\":%d}",
           fix ? gps.speed.kmph() : 0.0, fix ? 1 : 0, thresholdKmh, sats, batteryPct,
           alertActive ? 1 : 0, alertIntervalMs, brightnessLevel, gpsPowerSave ? 1 : 0, displayType);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

// Every unknown address (the phone's captive-portal checks included) is sent to the portal page
void portalRedirect() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

void portalStart() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(PORTAL_SSID, strlen(PORTAL_PASS) >= 8 ? PORTAL_PASS : NULL, PORTAL_CHANNEL);
  WiFi.setTxPower(PORTAL_TX_POWER);
  delay(100);

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());          // answer every name with our own address

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", PORTAL_HTML);
  });
  server.on("/api/status", HTTP_GET, portalSendStatus);
  server.on("/api/limit", HTTP_GET, []() {             // ?d=1 / ?d=-1 one step, ?v=95.5 exact value
    if (server.hasArg("v")) setLimit(server.arg("v").toFloat());
    else if (server.hasArg("d")) setLimit(thresholdKmh + (server.arg("d").toInt() < 0 ? -STEP : STEP));
    portalSendStatus();
  });
  server.on("/api/bright", HTTP_GET, []() {            // ?l=0..2, no argument = next level
    if (server.hasArg("l")) setBrightness(server.arg("l").toInt());
    else cycleBrightness();
    portalSendStatus();
  });
  server.on("/api/gpsps", HTTP_GET, []() {             // ?on=1 / ?on=0, no argument = toggle
    bool target = server.hasArg("on") ? server.arg("on").toInt() != 0 : !gpsPowerSave;
    if (target != gpsPowerSave) setGpsPowerSave(target);
    portalSendStatus();
  });
  server.on("/api/display", HTTP_GET, []() {           // ?t=0..3, no argument = next type
    if (server.hasArg("t")) setDisplayType(server.arg("t").toInt());
    else cycleDisplayType();
    portalSendStatus();
  });
  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  server.onNotFound(portalRedirect);
  server.begin();

  portalOn = true;
  portalLastClientMs = millis();
  Serial.printf("Portal: Wi-Fi \"%s\"  http://%s/\n", PORTAL_SSID, WiFi.softAPIP().toString().c_str());
}

void portalStop() {
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  portalOn = false;
  Serial.println("Portal: Wi-Fi off (no phone connected)");
  showToast("Wi-Fi portal", "OFF");
}

void portalLoop() {
  if (!portalOn) return;
  dnsServer.processNextRequest();
  server.handleClient();
#if PORTAL_IDLE_OFF_MIN > 0
  if (WiFi.softAPgetStationNum() > 0) {
    portalLastClientMs = millis();
  } else if (millis() - portalLastClientMs > PORTAL_IDLE_OFF_MIN * 60000UL) {
    portalStop();
  }
#endif
}

#endif  // PORTAL_ENABLE

// ========== BLUETOOTH LE ==========
// One service with two characteristics:
//
// STATUS  (read + notify, 12 bytes, little-endian), sent every BLE_NOTIFY_MS:
//   0-1  speed x10 (km/h)        2-3  limit x10 (km/h)
//   4    satellites              5    battery %
//   6    flags: bit0 GPS fix, bit1 over the limit, bit2 GPS power save, bit3 Wi-Fi portal on
//   7    brightness 0..2         8    display type 0..3
//   9    reserved               10-11 beep interval ms (0 = continuous tone)
//
// COMMAND (write):
//   01 lo hi   set the limit, value x10 (e.g. 95.5 km/h = 955)
//   02 s       one step: 01 = +0.5, FF = -0.5
//   03 l       brightness 0..2 (FF = next)
//   04 v       GPS power save 0 / 1 (FF = toggle)
//   05 t       display type 0..3 (FF = next)
#if BLE_ENABLE

#define BLE_SERVICE_UUID  "5a1e0001-7b3c-4d2a-9f10-53504545444f"
#define BLE_STATUS_UUID   "5a1e0002-7b3c-4d2a-9f10-53504545444f"
#define BLE_COMMAND_UUID  "5a1e0003-7b3c-4d2a-9f10-53504545444f"

NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *bleStatusChr = nullptr;
QueueHandle_t bleCmdQueue = nullptr;   // commands arrive on the BLE task; loop() carries them out

struct BleCmd { uint8_t b[4]; };

class BleCommandCallbacks : public NimBLECharacteristicCallbacks {
  void queueCmd(NimBLECharacteristic *c) {
    auto v = c->getValue();
    BleCmd cmd = {{0, 0, 0, 0}};
    size_t n = v.length();
    if (n == 0) return;
    if (n > sizeof(cmd.b)) n = sizeof(cmd.b);
    memcpy(cmd.b, v.data(), n);
    xQueueSend(bleCmdQueue, &cmd, 0);
  }
 public:
  // NimBLE-Arduino 1.x and 2.x call different versions of onWrite; both are here
  void onWrite(NimBLECharacteristic *c) { queueCmd(c); }
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) { queueCmd(c); }
};

void bleStart() {
  bleCmdQueue = xQueueCreate(8, sizeof(BleCmd));
  NimBLEDevice::init(BLE_NAME);
  bleServer = NimBLEDevice::createServer();
  NimBLEService *svc = bleServer->createService(BLE_SERVICE_UUID);
  bleStatusChr = svc->createCharacteristic(BLE_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *cmdChr =
      svc->createCharacteristic(BLE_COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cmdChr->setCallbacks(new BleCommandCallbacks());
  svc->start();

  // Advertising: the service in the advertisement, the name in the scan response
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData, scanData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(BLE_SERVICE_UUID));
  scanData.setName(BLE_NAME);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->start();
  bleUpdateStatus(false);
  Serial.println("BLE: advertising as \"" BLE_NAME "\"");
}

void bleUpdateStatus(bool notify) {
  bool fix = gps.speed.isValid() && gps.speed.age() < 2000;
  uint16_t spd = fix ? (uint16_t)(gps.speed.kmph() * 10.0 + 0.5) : 0;
  uint16_t lim = (uint16_t)(thresholdKmh * 10.0f + 0.5f);
  uint16_t iv = (uint16_t)(alertIntervalMs > 65535 ? 65535 : alertIntervalMs);
  bool wifi = false;
#if PORTAL_ENABLE
  wifi = portalOn;
#endif
  uint8_t d[12];
  d[0] = spd & 0xFF; d[1] = spd >> 8;
  d[2] = lim & 0xFF; d[3] = lim >> 8;
  d[4] = gps.satellites.isValid() ? (uint8_t)gps.satellites.value() : 0;
  d[5] = (uint8_t)batteryPct;
  d[6] = (fix ? 1 : 0) | (alertActive ? 2 : 0) | (gpsPowerSave ? 4 : 0) | (wifi ? 8 : 0);
  d[7] = (uint8_t)brightnessLevel;
  d[8] = (uint8_t)displayType;
  d[9] = 0;
  d[10] = iv & 0xFF; d[11] = iv >> 8;
  bleStatusChr->setValue(d, sizeof(d));
  if (notify) bleStatusChr->notify();
}

// (plain byte pointer: Arduino puts function prototypes above the struct definitions)
void bleRunCommand(const uint8_t *b) {
  switch (b[0]) {
    case 0x01: setLimit((b[1] | (b[2] << 8)) / 10.0f); break;
    case 0x02: setLimit(thresholdKmh + ((int8_t)b[1] < 0 ? -STEP : STEP)); break;
    case 0x03: if (b[1] == 0xFF) cycleBrightness(); else setBrightness(b[1]); break;
    case 0x04: {
      bool target = (b[1] == 0xFF) ? !gpsPowerSave : b[1] != 0;
      if (target != gpsPowerSave) setGpsPowerSave(target);
      break;
    }
    case 0x05: if (b[1] == 0xFF) cycleDisplayType(); else setDisplayType(b[1]); break;
  }
}

void bleLoop() {
  static unsigned long lastNotifyMs = 0;
  BleCmd c;
  bool changed = false;
  while (xQueueReceive(bleCmdQueue, &c, 0) == pdTRUE) {
    bleRunCommand(c.b);
    changed = true;
  }
  if (bleServer->getConnectedCount() > 0) {
    if (changed || millis() - lastNotifyMs >= BLE_NOTIFY_MS) {
      lastNotifyMs = millis();
      bleUpdateStatus(true);
    }
  } else {
    bleUpdateStatus(false);
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) NimBLEDevice::getAdvertising()->start();
  }
}

#endif  // BLE_ENABLE
