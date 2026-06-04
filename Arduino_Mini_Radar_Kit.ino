/* Mini Radar Kit
 *
 * Hardware:
 *   - Arduino Uno / Nano (ATmega328)
 *   - HC-SR04 ultrasonic sensor   Trig->D6, Echo->D5
 *   - SG90 micro servo            Signal->D3
 *   - ST7735 1.8" TFT 128x160     CD->D9, CS->D10, RST->D8 (HW SPI: SCK->D13, MOSI->D11)
 *
 * Rewritten for clarity:
 *   - single parameterized sweep (no copy/paste between forward and reverse)
 *   - pulseIn() timeout so a missing echo never freezes the sweep
 *   - named constants instead of magic numbers
 *   - optional median filtering and toggleable serial debug
 */

#include <Servo.h>
#include <SPI.h>
#include "Ucglib.h"

// ---------- Pins ----------
#define TRIG_PIN   6   // Ultrasonic Trig
#define ECHO_PIN   5   // Ultrasonic Echo
#define SERVO_PIN  3   // Servo signal

// ---------- Debug ----------
// Comment this line out to disable serial output (faster sweep).
#define DEBUG 1

#if DEBUG
  #define DBG_BEGIN(baud) Serial.begin(baud)
  #define DBG_DEG(deg, dist)               \
    do {                                    \
      Serial.print(F("Degree: "));          \
      Serial.print(deg);                    \
      Serial.print(F("  ,Distance: "));     \
      Serial.println(dist);                 \
    } while (0)
#else
  #define DBG_BEGIN(baud)
  #define DBG_DEG(deg, dist)
#endif

// ---------- Screen geometry ----------
const int Xmax     = 160;          // horizontal pixels
const int Ymax     = 128;          // vertical pixels
const int Xcent    = Xmax / 2;     // horizontal center
const int base     = 118;          // baseline (radar origin) y
const int scanline = 105;          // sweep line length

// ---------- Measurement constants ----------
const float SOUND_CM_PER_US = 0.0343f / 2.0f; // speed of sound, out-and-back
const int   MAX_RANGE_CM    = 100;            // beyond this, plot on the rim
const float BLIP_SCALE      = 1.15f;          // px per cm when plotting blips
const int   RIM_RADIUS      = 116;            // rim radius for out-of-range blips
const unsigned long ECHO_TIMEOUT_US = 25000;  // ~4 m max; avoids long blocking

// ---------- Servo sweep range ----------
const int SWEEP_MIN = 4;
const int SWEEP_MAX = 180;
const int SWEEP_STEP = 2;

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

  DBG_BEGIN(115200);
  baseServo.attach(SERVO_PIN);

  splashScreen();

  ucg.setFont(ucg_font_orgv01_hr);
  ucg.setFontMode(UCG_FONT_MODE_SOLID);
}

// ----------------------------------------------------------------------------
void loop(void)
{
  // Sweep one way, then back. Same drawing code, opposite direction.
  sweep(SWEEP_MAX, SWEEP_MIN, -SWEEP_STEP);
  delay(200);

  sweep(SWEEP_MIN + 1, SWEEP_MAX - 4, SWEEP_STEP);
  delay(100);
}

// ----------------------------------------------------------------------------
// Splash / self-test screen shown once at boot.
void splashScreen(void)
{
  ucg.setFontMode(UCG_FONT_MODE_TRANSPARENT);
  ucg.setColor(0, 0, 100, 0);
  ucg.setColor(1, 0, 100, 0);
  ucg.setColor(2, 20, 20, 20);
  ucg.setColor(3, 20, 20, 20);
  ucg.drawGradientBox(0, 0, Xmax, Ymax);

  ucg.setPrintDir(0);

  // Drop-shadowed title.
  ucg.setFont(ucg_font_logisoso18_tf);
  ucg.setColor(0, 5, 0);
  ucg.setPrintPos(27, 42);
  ucg.print("Mini Radar");
  ucg.setColor(0, 255, 0);
  ucg.setPrintPos(25, 40);
  ucg.print("Mini Radar");

  ucg.setFont(ucg_font_helvB08_tf);
  ucg.setColor(0, 255, 0);
  ucg.setPrintPos(40, 100);
  ucg.print("Testing...");

  // Exercise the servo across its range to check for binding / wire snag.
  baseServo.write(90);
  for (int x = 0; x < 180; x += 5) {
    baseServo.write(x);
    delay(50);
  }

  ucg.setColor(0, 255, 0);
  ucg.print("OK!");
  delay(500);

  cls();
}

// ----------------------------------------------------------------------------
// Clear the screen by tiling black boxes (cheaper than clearScreen here).
void cls(void)
{
  ucg.setColor(0, 0, 0, 0);
  for (int s = 0; s < Ymax; s += 8)
    for (int t = 0; t < Xmax; t += 16)
      ucg.drawBox(t, s, 16, 8);
}

// ----------------------------------------------------------------------------
// Measure distance in cm. Returns -1 when no echo is received in time.
int calculateDistance(void)
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (duration == 0) return -1;            // timeout / out of range

  return (int)(duration * SOUND_CM_PER_US);
}

// ----------------------------------------------------------------------------
// Median of 3 readings to suppress HC-SR04 jitter. Ignores timeouts when it can.
int measureMedian(void)
{
  int a = calculateDistance();
  int b = calculateDistance();
  int c = calculateDistance();

  // Simple median-of-three.
  if (a > b) { int t = a; a = b; b = t; }
  if (b > c) { int t = b; b = c; c = t; }
  if (a > b) { int t = a; a = b; b = t; }
  return b;
}

// ----------------------------------------------------------------------------
// Static background: range rings, baseline, angle scale, and the decorations.
void fix(void)
{
  ucg.setColor(0, 40, 0);
  ucg.drawDisc(Xcent, base + 1, 3, UCG_DRAW_ALL);
  const int ringRadii[] = {29, 58, 86, 115};
  for (uint8_t i = 0; i < 4; i++) {
    ucg.drawCircle(Xcent, base + 1, ringRadii[i], UCG_DRAW_UPPER_LEFT);
    ucg.drawCircle(Xcent, base + 1, ringRadii[i], UCG_DRAW_UPPER_RIGHT);
  }
  ucg.drawLine(0, base + 1, Xmax, base + 1);

  // Angle scale ticks (long every 10 degrees).
  ucg.setColor(0, 120, 0);
  for (int i = 40; i < 140; i += 2) {
    int inner = (i % 10 == 0) ? 105 : 110;
    ucg.drawLine(inner * cos(radians(i)) + Xcent, base - inner * sin(radians(i)),
                 113 * cos(radians(i)) + Xcent, base - 113 * sin(radians(i)));
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

// ----------------------------------------------------------------------------
// Range labels along the vertical axis.
void fix_font(void)
{
  ucg.setColor(0, 180, 0);
  ucg.setPrintPos(70, 128 - 120 + 7); ucg.print("100cm");
  ucg.setPrintPos(70, 128 - 85 - 11); ucg.print("75cm");
  ucg.setPrintPos(70, 128 - 60 - 8);  ucg.print("50cm");
  ucg.setPrintPos(70, 128 - 35 - 4);  ucg.print("25cm");
}

// ----------------------------------------------------------------------------
// Draw the three-segment fading sweep line at angle `deg`.
// `lead` is the leading edge angle (a few degrees ahead in the sweep direction).
void drawSweepLine(int lead, int dir)
{
  const uint8_t greens[3] = {255, 128, 0};   // bright -> dim -> erase
  int a = lead;
  for (int i = 0; i < 3; i++) {
    ucg.setColor(0, greens[i], 0);
    ucg.drawLine(Xcent, base,
                 scanline * cos(radians(a)) + Xcent,
                 base - scanline * sin(radians(a)));
    a -= dir * 2;   // step the erase tail behind the bright head
  }
}

// ----------------------------------------------------------------------------
// Plot a target blip for the given angle/distance.
void drawBlip(int deg, int distance)
{
  if (distance < 0) return;  // no echo: draw nothing

  if (distance < MAX_RANGE_CM) {
    ucg.setColor(255, 0, 0);
    ucg.drawDisc(BLIP_SCALE * distance * cos(radians(deg)) + Xcent,
                 -(BLIP_SCALE * distance * sin(radians(deg))) + base,
                 1, UCG_DRAW_ALL);
  } else {
    // Out of range: mark the rim in yellow.
    ucg.setColor(255, 255, 0);
    ucg.drawDisc(RIM_RADIUS * cos(radians(deg)) + Xcent,
                 -RIM_RADIUS * sin(radians(deg)) + base,
                 1, UCG_DRAW_ALL);
  }
}

// ----------------------------------------------------------------------------
// Bottom status line: current angle and distance readout.
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

// ----------------------------------------------------------------------------
// One full sweep from `start` to `end` stepping by `step` (sign = direction).
void sweep(int start, int end, int step)
{
  int dir = (step > 0) ? 1 : -1;

  cls();
  fix();

  for (int x = start; (dir > 0) ? (x < end) : (x > end); x += step) {
    baseServo.write(x);

    // Leading edge of the sweep line sits a few degrees ahead of the servo.
    int lead = x - dir * 4;
    drawSweepLine(lead, dir);

    int distance = measureMedian();
    delay(20);

    drawBlip(x, distance);
    DBG_DEG(x, distance);

    // Redraw labels when the sweep line passes over them.
    if (x > 70 && x < 110) fix_font();

    drawReadout(x, distance);
  }
}
