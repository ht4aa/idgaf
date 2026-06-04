/* Mini Radar Kit
 *
 * Hardware:
 *   - Arduino Uno / Nano (ATmega328)
 *   - HC-SR04 ultrasonic sensor   Trig->D6, Echo->D5
 *   - SG90 micro servo            Signal->D3
 *   - ST7735 1.8" TFT 128x160     CD->D9, CS->D10, RST->D8 (HW SPI: SCK->D13, MOSI->D11)
 *
 * Features:
 *   - Single parameterized sweep (no copy/paste between forward and reverse)
 *   - pulseIn() timeout so a missing echo never freezes the sweep
 *   - Geometry derived from one RADAR_RADIUS so nothing is clipped off-screen
 *   - Calibration system (servo offset, temp-compensated sound speed,
 *     distance scale/offset) with an interactive serial menu + EEPROM storage
 */

#include <Servo.h>
#include <SPI.h>
#include <EEPROM.h>
#include "Ucglib.h"

// ---------- Pins ----------
#define TRIG_PIN   6   // Ultrasonic Trig
#define ECHO_PIN   5   // Ultrasonic Echo
#define SERVO_PIN  3   // Servo signal

// ---------- Debug ----------
// Comment out to silence the per-reading serial spam (faster sweep).
// The calibration menu still works regardless of this setting.
#define DEBUG 1

#if DEBUG
  #define DBG_DEG(deg, dist)               \
    do {                                    \
      Serial.print(F("Degree: "));          \
      Serial.print(deg);                    \
      Serial.print(F("  ,Distance: "));     \
      Serial.println(dist);                 \
    } while (0)
#else
  #define DBG_DEG(deg, dist)
#endif

// ---------- Screen geometry ----------
const int Xmax  = 160;            // horizontal pixels
const int Ymax  = 128;            // vertical pixels
const int Xcent = Xmax / 2;       // horizontal center (80)
const int base  = 118;            // baseline (radar origin) y

// Outer radius of the radar. Must fit on screen: <= Xcent and <= base.
// 78 leaves a 2 px margin on each side so the full semicircle is visible.
const int RADAR_RADIUS = 78;
const int scanline     = RADAR_RADIUS;       // sweep line length
const int RIM_RADIUS   = RADAR_RADIUS - 1;   // out-of-range blips sit on the rim

// ---------- Range mapping ----------
const int   MAX_RANGE_CM = 100;                          // beyond this -> rim
const float BLIP_SCALE   = (float)RADAR_RADIUS / MAX_RANGE_CM; // px per cm
const unsigned long ECHO_TIMEOUT_US = 25000;             // ~4 m; avoids long blocking

// ---------- Servo sweep range ----------
const int SWEEP_MIN  = 4;
const int SWEEP_MAX  = 180;
const int SWEEP_STEP = 2;

// ============================================================================
//  Calibration
// ============================================================================
// Stored in EEPROM so it survives power cycles. Adjust live via the serial
// menu (send 'c' within 3 s of boot), or edit the defaults below.
struct Calib {
  uint16_t magic;        // validity marker
  int8_t   servoOffset;  // degrees added to every servo angle to center it
  int8_t   distOffsetCm; // added to measured distance (sensor mount offset)
  float    distScale;    // multiplies measured distance (gain correction)
  float    ambientTempC; // for temperature-compensated speed of sound
};

const uint16_t CALIB_MAGIC = 0x5244;  // 'RD'
const int      CALIB_ADDR  = 0;

Calib cal = {
  CALIB_MAGIC,
  0,        // servoOffset
  0,        // distOffsetCm
  1.00f,    // distScale
  20.0f     // ambientTempC
};

// Speed of sound, out-and-back, in cm per microsecond. Recomputed from temp.
float gSoundCmPerUs = 0.01717f;

Servo baseServo;
Ucglib_ST7735_18x128x160_HWSPI ucg(/*cd=*/ 9, /*cs=*/ 10, /*reset=*/ 8);

// ----------------------------------------------------------------------------
void setup(void)
{
  ucg.begin(UCG_FONT_MODE_SOLID);
  ucg.setRotate90();  // landscape; use setRotate270 if the display is flipped

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  Serial.begin(115200);
  baseServo.attach(SERVO_PIN);

  loadCalib();              // pull saved calibration from EEPROM (if any)
  updateSoundSpeed();
  maybeRunCalibration();    // offer the serial menu for 3 s

  splashScreen();

  ucg.setFont(ucg_font_orgv01_hr);
  ucg.setFontMode(UCG_FONT_MODE_SOLID);
}

// ----------------------------------------------------------------------------
void loop(void)
{
  sweep(SWEEP_MAX, SWEEP_MIN, -SWEEP_STEP);
  delay(200);
  sweep(SWEEP_MIN + 1, SWEEP_MAX - 4, SWEEP_STEP);
  delay(100);
}

// ============================================================================
//  Calibration: storage
// ============================================================================
void loadCalib(void)
{
  Calib stored;
  EEPROM.get(CALIB_ADDR, stored);
  if (stored.magic == CALIB_MAGIC) cal = stored;  // else keep defaults
}

void saveCalib(void)
{
  cal.magic = CALIB_MAGIC;
  EEPROM.put(CALIB_ADDR, cal);
}

void updateSoundSpeed(void)
{
  // v(m/s) = 331.4 + 0.6*T ; convert to cm/us and halve for out-and-back.
  gSoundCmPerUs = (331.4f + 0.6f * cal.ambientTempC) / 10000.0f / 2.0f;
}

void printCalib(void)
{
  Serial.println(F("--- Calibration ---"));
  Serial.print(F("servoOffset  : ")); Serial.println(cal.servoOffset);
  Serial.print(F("distOffsetCm : ")); Serial.println(cal.distOffsetCm);
  Serial.print(F("distScale    : ")); Serial.println(cal.distScale, 3);
  Serial.print(F("ambientTempC : ")); Serial.println(cal.ambientTempC, 1);
}

// ============================================================================
//  Calibration: interactive serial menu
// ============================================================================
// Send 'c' within 3 seconds of boot to enter. Commands:
//   t<value>  ambient temperature in C   (e.g. t22)
//   o<value>  servo offset in degrees    (e.g. o-3)
//   z<value>  distance offset in cm      (e.g. z2)
//   d<value>  calibrate scale: place an object at <value> cm, then send (e.g. d30)
//   p         print current calibration
//   s         save to EEPROM and exit
//   x         exit without saving
void maybeRunCalibration(void)
{
  Serial.println(F("Send 'c' within 3s for calibration menu..."));
  unsigned long t0 = millis();
  while (millis() - t0 < 3000) {
    if (Serial.available() && Serial.read() == 'c') {
      runCalibrationMenu();
      return;
    }
  }
}

void runCalibrationMenu(void)
{
  Serial.println(F("=== Calibration menu ==="));
  Serial.println(F("t<C> o<deg> z<cm> d<cm> | p print  s save  x exit"));
  printCalib();

  for (;;) {
    if (!Serial.available()) continue;
    char cmd = Serial.read();

    switch (cmd) {
      case 't':
        cal.ambientTempC = Serial.parseFloat();
        updateSoundSpeed();
        Serial.print(F("temp -> ")); Serial.println(cal.ambientTempC, 1);
        break;

      case 'o':
        cal.servoOffset = (int8_t)Serial.parseInt();
        baseServo.write(constrain(90 + cal.servoOffset, 0, 180));
        Serial.print(F("servoOffset -> ")); Serial.println(cal.servoOffset);
        break;

      case 'z':
        cal.distOffsetCm = (int8_t)Serial.parseInt();
        Serial.print(F("distOffsetCm -> ")); Serial.println(cal.distOffsetCm);
        break;

      case 'd': {
        float known = Serial.parseFloat();
        if (known > 0) {
          int raw = rawDistanceCm();   // uncorrected median reading
          if (raw > 0) {
            cal.distScale = known / raw;
            Serial.print(F("raw=")); Serial.print(raw);
            Serial.print(F(" known=")); Serial.print(known);
            Serial.print(F(" -> distScale=")); Serial.println(cal.distScale, 3);
          } else {
            Serial.println(F("no echo, try again"));
          }
        }
        break;
      }

      case 'p': printCalib(); break;

      case 's':
        saveCalib();
        Serial.println(F("saved. exiting."));
        return;

      case 'x':
        Serial.println(F("exit (not saved)."));
        loadCalib();          // revert in-RAM changes to last saved values
        updateSoundSpeed();
        return;

      default: break;         // ignore whitespace / unknown
    }
  }
}

// ============================================================================
//  Measurement
// ============================================================================
// Raw, uncorrected distance in cm (median of 3). -1 on timeout.
int rawDistanceCm(void)
{
  int a = pingCm(), b = pingCm(), c = pingCm();
  if (a > b) { int t = a; a = b; b = t; }
  if (b > c) { int t = b; b = c; c = t; }
  if (a > b) { int t = a; a = b; b = t; }
  return b;
}

// Single ping -> cm using current sound speed. -1 on timeout.
int pingCm(void)
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (duration == 0) return -1;
  return (int)(duration * gSoundCmPerUs);
}

// Calibrated distance in cm. -1 when no echo.
int calculateDistance(void)
{
  int raw = rawDistanceCm();
  if (raw < 0) return -1;
  int d = (int)(raw * cal.distScale) + cal.distOffsetCm;
  return (d < 0) ? 0 : d;
}

// ============================================================================
//  Servo
// ============================================================================
void writeServo(int angle)
{
  baseServo.write(constrain(angle + cal.servoOffset, 0, 180));
}

// ============================================================================
//  Display
// ============================================================================
void splashScreen(void)
{
  ucg.setFontMode(UCG_FONT_MODE_TRANSPARENT);
  ucg.setColor(0, 0, 100, 0);
  ucg.setColor(1, 0, 100, 0);
  ucg.setColor(2, 20, 20, 20);
  ucg.setColor(3, 20, 20, 20);
  ucg.drawGradientBox(0, 0, Xmax, Ymax);

  ucg.setPrintDir(0);

  ucg.setFont(ucg_font_logisoso18_tf);
  ucg.setColor(0, 5, 0);
  ucg.setPrintPos(27, 42); ucg.print("Mini Radar");
  ucg.setColor(0, 255, 0);
  ucg.setPrintPos(25, 40); ucg.print("Mini Radar");

  ucg.setFont(ucg_font_helvB08_tf);
  ucg.setColor(0, 255, 0);
  ucg.setPrintPos(40, 100); ucg.print("Testing...");

  // Exercise the servo to check for binding / wire snag.
  writeServo(90);
  for (int x = 0; x < 180; x += 5) { writeServo(x); delay(50); }

  ucg.setColor(0, 255, 0);
  ucg.print("OK!");
  delay(500);

  cls();
}

// Clear screen by tiling black boxes.
void cls(void)
{
  ucg.setColor(0, 0, 0, 0);
  for (int s = 0; s < Ymax; s += 8)
    for (int t = 0; t < Xmax; t += 16)
      ucg.drawBox(t, s, 16, 8);
}

// Static background: rings, baseline, angle scale, decorations.
void fix(void)
{
  ucg.setColor(0, 40, 0);
  ucg.drawDisc(Xcent, base + 1, 3, UCG_DRAW_ALL);

  // Four range rings at 25/50/75/100 cm, scaled to fit the screen.
  for (uint8_t i = 1; i <= 4; i++) {
    int r = RADAR_RADIUS * i / 4;
    ucg.drawCircle(Xcent, base + 1, r, UCG_DRAW_UPPER_LEFT);
    ucg.drawCircle(Xcent, base + 1, r, UCG_DRAW_UPPER_RIGHT);
  }
  ucg.drawLine(0, base + 1, Xmax, base + 1);

  // Angle scale ticks (long every 10 degrees).
  ucg.setColor(0, 120, 0);
  int outer = RADAR_RADIUS - 2;
  for (int i = 40; i < 140; i += 2) {
    int inner = (i % 10 == 0) ? RADAR_RADIUS - 10 : RADAR_RADIUS - 5;
    ucg.drawLine(inner * cos(radians(i)) + Xcent, base - inner * sin(radians(i)),
                 outer * cos(radians(i)) + Xcent, base - outer * sin(radians(i)));
  }

  // Decorative corner blocks.
  ucg.setColor(0, 200, 0);
  ucg.drawLine(0, 0, 0, 18);
  for (int i = 0; i < 5; i++) {
    ucg.setColor(random(255), random(255), random(255));
    ucg.drawBox(2, i * 4, random(14) + 2, 3);
  }

  ucg.setColor(0, 0, 180);
  ucg.drawFrame(146, 0, 14, 14);
  ucg.setColor(0, 0, 60);
  ucg.drawHLine(148, 0, 10);
  ucg.drawVLine(146, 2, 10);
  ucg.drawHLine(148, 13, 10);
  ucg.drawVLine(159, 2, 10);

  ucg.setColor(random(255), random(255), random(255));
  ucg.drawBox(148, 2, 4, 4);
  ucg.setColor(0, 220, 0);
  ucg.drawBox(148, 8, 4, 4);
  ucg.setColor(random(255), random(255), random(255));
  ucg.drawBox(154, 8, 4, 4);
  ucg.setColor(random(255), random(255), random(255));
  ucg.drawBox(154, 2, 4, 4);

  ucg.setColor(0, 0, 90);
  ucg.drawTetragon(62, 123, 58, 127, 98, 127, 102, 123);
  ucg.setColor(0, 0, 160);
  ucg.drawTetragon(67, 123, 63, 127, 93, 127, 97, 123);
  ucg.setColor(0, 255, 0);
  ucg.drawTetragon(72, 123, 68, 127, 88, 127, 92, 123);

  fix_font();
}

// Range labels placed next to each ring along the vertical axis.
void fix_font(void)
{
  ucg.setColor(0, 180, 0);
  for (uint8_t i = 1; i <= 4; i++) {
    int r = RADAR_RADIUS * i / 4;
    ucg.setPrintPos(66, base - r + 6);
    ucg.print(i * 25);
    ucg.print("cm");
  }
}

// Three-segment fading sweep line at leading angle `lead`, sweep direction `dir`.
void drawSweepLine(int lead, int dir)
{
  const uint8_t greens[3] = {255, 128, 0};   // bright -> dim -> erase
  int a = lead;
  for (int i = 0; i < 3; i++) {
    ucg.setColor(0, greens[i], 0);
    ucg.drawLine(Xcent, base,
                 scanline * cos(radians(a)) + Xcent,
                 base - scanline * sin(radians(a)));
    a -= dir * 2;
  }
}

// Plot a target blip for angle/distance. Skips drawing when there is no echo.
void drawBlip(int deg, int distance)
{
  if (distance < 0) return;

  if (distance < MAX_RANGE_CM) {
    ucg.setColor(255, 0, 0);
    ucg.drawDisc(BLIP_SCALE * distance * cos(radians(deg)) + Xcent,
                 -(BLIP_SCALE * distance * sin(radians(deg))) + base,
                 1, UCG_DRAW_ALL);
  } else {
    ucg.setColor(255, 255, 0);
    ucg.drawDisc(RIM_RADIUS * cos(radians(deg)) + Xcent,
                 -RIM_RADIUS * sin(radians(deg)) + base,
                 1, UCG_DRAW_ALL);
  }
}

// Bottom status line.
void drawReadout(int deg, int distance)
{
  ucg.setColor(0, 0, 155, 0);
  ucg.setPrintPos(0, 126);   ucg.print("DEG: ");
  ucg.setPrintPos(24, 126);  ucg.print(deg);  ucg.print("   ");
  ucg.setPrintPos(125, 126); ucg.print("   ");
  if (distance < 0) ucg.print("--");
  else              ucg.print(distance);
  ucg.print("cm   ");
}

// One full sweep from `start` to `end` stepping by `step` (sign = direction).
void sweep(int start, int end, int step)
{
  int dir = (step > 0) ? 1 : -1;

  cls();
  fix();

  for (int x = start; (dir > 0) ? (x < end) : (x > end); x += step) {
    writeServo(x);

    int lead = x - dir * 4;
    drawSweepLine(lead, dir);

    int distance = calculateDistance();
    delay(20);

    drawBlip(x, distance);
    DBG_DEG(x, distance);

    if (x > 70 && x < 110) fix_font();  // redraw labels the sweep passes over

    drawReadout(x, distance);
  }
}
