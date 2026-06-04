/*
 * STM32 Auto-Detecting Multimeter (DC, Ohm, AC)
 * PA2: DC Voltmeter | PA1: Ohmmeter | PA7: AC Voltmeter
 * LCD 16x2  : Graphics/drawings (RS=PB12, EN=PB13, D4=PB14, D5=PB15, D6=PB4, D7=PB5)
 * OLED 0.91in: Numbers (SDA=PB7, SCL=PB6)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LiquidCrystal.h>

// --- OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledReady = false;  // tracks if OLED init succeeded

// --- LCD (safe Black Pill pins) ---
LiquidCrystal lcd(PB12, PB13, PB14, PB15, PB4, PB5);

// --- MULTIMETER CONFIG ---
#define FACTOR 125
const int   PIN_VOLTMETER = PA2;
const int   PIN_OHMMETER  = PA1;
const int   PIN_AC        = PA7;
const float R_KNOWN       = 9900.0;
const int   THRESHOLD     = 20;
float       AC_CAL        = .71867;

// ============================================================
// CUSTOM LCD CHARACTERS
// ============================================================

// DC bar: 6 fill levels (slots 0-5)
uint8_t blk0[8] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
uint8_t blk1[8] = {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10};
uint8_t blk2[8] = {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18};
uint8_t blk3[8] = {0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C};
uint8_t blk4[8] = {0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E};
uint8_t blk5[8] = {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}; // full block

// AC wave chars (slots 0-3): bottom-up fill, 1/3/5/7 rows
uint8_t ac0[8] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; // 1 row
uint8_t ac1[8] = {0x00,0x00,0x00,0x00,0x00,0x1F,0x1F,0x1F}; // 3 rows
uint8_t ac2[8] = {0x00,0x00,0x00,0x1F,0x1F,0x1F,0x1F,0x1F}; // 5 rows
uint8_t ac3[8] = {0x00,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}; // 7 rows

// Ohm needle chars
// slot 5: left arc
uint8_t arcL[8] = {0x03,0x0C,0x10,0x10,0x10,0x08,0x07,0x00};
// slot 6: right arc
uint8_t arcR[8] = {0x18,0x06,0x01,0x01,0x01,0x02,0x1C,0x00};
// slot 7: needle (reused per position via redefinition)
uint8_t needleUp[8]   = {0x04,0x04,0x0E,0x0E,0x1F,0x00,0x00,0x00}; // pointing up
uint8_t needleMid[8]  = {0x00,0x1F,0x1F,0x0E,0x04,0x04,0x00,0x00}; // mid
uint8_t needleDown[8] = {0x00,0x00,0x00,0x1F,0x0E,0x0E,0x04,0x04}; // low

void loadDCChars() {
  lcd.createChar(0, blk0);
  lcd.createChar(1, blk1);
  lcd.createChar(2, blk2);
  lcd.createChar(3, blk3);
  lcd.createChar(4, blk4);
  lcd.createChar(5, blk5);
}

void loadACChars() {
  lcd.createChar(0, ac0);
  lcd.createChar(1, ac1);
  lcd.createChar(2, ac2);
  lcd.createChar(3, ac3);
}

void loadOhmChars(uint8_t* needle) {
  lcd.createChar(5, arcL);
  lcd.createChar(6, arcR);
  lcd.createChar(7, needle);
}

// ============================================================
// LCD DRAWING FUNCTIONS
// ============================================================

// DC: smooth bar 0.0-1.0
void drawDCBar(float norm) {
  // Row 0: label
  lcd.setCursor(0, 0);
  lcd.print("DC Voltage      ");

  // Row 1: 14-char bar + brackets
  lcd.setCursor(0, 1);
  lcd.print("[");

  int totalUnits = 14 * 5; // 70 sub-pixel units
  int filled = (int)(norm * totalUnits + 0.5);
  if (filled > totalUnits) filled = totalUnits;

  for (int c = 0; c < 14; c++) {
    int px = filled - c * 5;
    uint8_t ch;
    if      (px >= 5) ch = 5;
    else if (px == 4) ch = 4;
    else if (px == 3) ch = 3;
    else if (px == 2) ch = 2;
    else if (px == 1) ch = 1;
    else              ch = 0;
    lcd.write(ch);
  }
  lcd.print("]");
}

// AC: scrolling sine wave across 16 chars
int acPhase = 0;
// Heights per column in one full cycle (0,1,2,3 = 4 fill levels), length 12
const uint8_t sineSeq[12] = {0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1};

void drawACWave() {
  lcd.setCursor(0, 0);
  lcd.print("AC Voltage      ");

  lcd.setCursor(0, 1);
  for (int c = 0; c < 16; c++) {
    uint8_t h = sineSeq[(c + acPhase) % 12];
    lcd.write(h); // chars 0-3
  }
  acPhase = (acPhase + 1) % 12;
}

// OHM: arc gauge with moving needle
// needle position 0-12 maps across cols 2-13
// cols 0-1 = arcL, cols 14-15 = arcR, col = needle pos
void drawOhmGauge(float normR) {
  // Pick needle char based on position (left=up arc, mid=mid, right=down)
  uint8_t* needleChar;
  if      (normR < 0.33) needleChar = needleUp;
  else if (normR < 0.66) needleChar = needleMid;
  else                   needleChar = needleDown;
  loadOhmChars(needleChar);

  int needleCol = 2 + (int)(normR * 11.0); // cols 2-13
  if (needleCol > 13) needleCol = 13;

  // Row 0: label + scale hints
  lcd.setCursor(0, 0);
  lcd.print("OHM  1");
  lcd.setCursor(10, 0); lcd.print("100k  M");

  // Row 1: arc + needle
  lcd.setCursor(0, 1);  lcd.write((uint8_t)5); // arcL
  lcd.setCursor(1, 1);  lcd.write((uint8_t)5);
  for (int c = 2; c <= 13; c++) {
    lcd.setCursor(c, 1);
    if (c == needleCol) lcd.write((uint8_t)7); // needle
    else                lcd.print("-");
  }
  lcd.setCursor(14, 1); lcd.write((uint8_t)6); // arcR
  lcd.setCursor(15, 1); lcd.write((uint8_t)6);
}

// ============================================================
// OLED NUMBER DISPLAY
// ============================================================

void oledShowDC(float v) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("DC Voltage");

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(v, 2);
  display.print(" V");
  display.display();
}

void oledShowAC(float v) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("AC Voltage");

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(v, 1);
  display.print(" V");
  display.display();
}

void oledShowOhm(float r) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Resistance");

  display.setTextSize(2);
  display.setCursor(0, 16);
  if (r < 0) {
    display.print("OPEN");
  } else if (r >= 1000000.0) {
    display.print(r / 1000000.0, 2);
    display.setTextSize(1);
    display.print(" MOhm");
  } else if (r >= 1000.0) {
    display.print(r / 1000.0, 2);
    display.setTextSize(1);
    display.print(" kOhm");
  } else {
    display.print(r, 1);
    display.setTextSize(1);
    display.print(" Ohm");
  }
  display.display();
}

void oledShowStandby() {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(28, 5);
  display.print("STM32 Meter");
  display.setCursor(22, 20);
  display.print("Select a mode");
  display.display();
}

void oledShowConflict() {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 5);
  display.print("!! CONFLICT !!");
  display.setCursor(8, 20);
  display.print("One mode at a time");
  display.display();
}

// ============================================================
// SETUP
// ============================================================

int lastMode = -1;

void setup() {
  Serial.begin(115200);
  delay(100);

  analogReadResolution(12);
  pinMode(PIN_VOLTMETER, INPUT_ANALOG);
  pinMode(PIN_OHMMETER,  INPUT_ANALOG);
  pinMode(PIN_AC,        INPUT_ANALOG);

  // LCD init
  lcd.begin(16, 2);
  loadDCChars(); // default load
  lcd.clear();
  lcd.setCursor(2, 0); lcd.print("STM32 Meter");
  lcd.setCursor(4, 1); lcd.print("Starting...");

  // --- OLED init (with retry + slower I2C clock, from code 2) ---
  Wire.begin();
  Wire.setClock(100000); // 100 kHz - more reliable than default 400 kHz

  int retries = 3;
  while (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C) && retries > 0) {
    Serial.println(F("Retrying display..."));
    delay(1000);
    Wire.begin();
    retries--;
  }

  if (retries == 0) {
    Serial.println(F("Display failed - running in Serial-only mode"));
    oledReady = false;
  } else {
    oledReady = true;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(16, 5);  display.print("STM32 Multimeter");
    display.setCursor(40, 20); display.print("Ready!");
    display.display();
  }

  delay(1500);
  lcd.clear();
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  int detect_DC  = analogRead(PIN_VOLTMETER); delay(2);
  int detect_Ohm = analogRead(PIN_OHMMETER);  delay(2);
  int detect_AC  = analogRead(PIN_AC);

  int active = (detect_DC  > THRESHOLD ? 1 : 0)
             + (detect_Ohm > THRESHOLD ? 1 : 0)
             + (detect_AC  > THRESHOLD ? 1 : 0);

  // --- CONFLICT ---
  if (active > 1) {
    if (lastMode != 99) { lcd.clear(); lastMode = 99; }
    bool flash = (millis() / 400) % 2;
    lcd.setCursor(0, 0); lcd.print(flash ? "!!! CONFLICT !!!" : "                ");
    lcd.setCursor(0, 1); lcd.print("  one mode only ");
    oledShowConflict();
    delay(400); return;
  }

  // --- DC ---
  if (detect_DC > THRESHOLD) {
    if (lastMode != 1) { lcd.clear(); loadDCChars(); lastMode = 1; }

    long sum = 0;
    analogRead(PIN_VOLTMETER); delay(2);
    for (int i = 0; i < 50; i++) { sum += analogRead(PIN_VOLTMETER); delay(1); }
    float avgRaw = sum / 50.0;
    float v_real = (avgRaw * (3.3f / 4095.0f)) * FACTOR;

    float norm = constrain(v_real / 250.0, 0.0, 1.0);
    drawDCBar(norm);
    oledShowDC(v_real);

    Serial.print("[DC] "); Serial.print(v_real, 2); Serial.println(" V");
  }

  // --- OHM ---
  else if (detect_Ohm > THRESHOLD) {
    if (lastMode != 2) { lcd.clear(); lastMode = 2; }

    long sum = 0;
    delay(20);
    analogRead(PIN_OHMMETER); delay(2);
    for (int i = 0; i < 50; i++) { sum += analogRead(PIN_OHMMETER); delay(1); }
    float avgRaw = sum / 50.0;

    if (avgRaw <= 10) {
      drawOhmGauge(1.0); // peg needle to max = open
      oledShowOhm(-1);
      Serial.println("[OHM] OPEN");
    } else {
      float r = R_KNOWN * ((4095.0f - avgRaw) / avgRaw);
      float logR = log10(max(r, 1.0f));
      float normR = constrain(logR / 6.0f, 0.0f, 1.0f);
      drawOhmGauge(normR);
      oledShowOhm(r);
      Serial.print("[OHM] "); Serial.print(r, 1); Serial.println(" Ohm");
    }
  }

// --- AC ---
  else if (detect_AC > THRESHOLD) {
    if (lastMode != 3) { lcd.clear(); loadACChars(); lastMode = 3; }

    uint64_t sum_sq = 0; // 64-bit to prevent STM32 overflow
    uint64_t sum = 0;    
    uint32_t samples = 0;
    
    uint32_t t0 = millis();
    while (millis() - t0 < 100) {
      uint32_t raw = analogRead(PIN_AC);
      sum += raw;
      sum_sq += (uint64_t)raw * raw; // Cast to 64-bit before multiplying
      samples++;
    }

    // Calculate True RMS dynamically (removes any DC offset automatically)
    float mean = (float)sum / samples;
    float mean_sq = (float)sum_sq / samples;
    float variance = mean_sq - (mean * mean);
    
    // Catch floating point rounding errors near zero
    float rms_raw = variance > 0 ? sqrt(variance) : 0; 
    
    float final_AC = rms_raw * AC_CAL;

    // Software Noise Gate: If the reading is trivially small, force it to 0
    if (final_AC < 5.0) { 
        final_AC = 0.0;
    }

    drawACWave();
    oledShowAC(final_AC);

    Serial.print("[AC] Raw RMS: "); Serial.print(rms_raw);
    Serial.print(" | Final: "); Serial.print(final_AC, 2); Serial.println(" V");
  }

  // --- STANDBY ---
  else {
    if (lastMode != 0) { lcd.clear(); lastMode = 0; }

    int pos = (millis() / 120) % 32;
    if (pos >= 16) pos = 31 - pos;
    lcd.setCursor(0, 0); lcd.print("  -- STANDBY -- ");
    lcd.setCursor(0, 1);
    for (int c = 0; c < 16; c++) lcd.print(c == pos ? "o" : ".");

    oledShowStandby();
    Serial.println("[STANDBY]");
  }

  delay(400);
}