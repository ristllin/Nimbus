// Freenove ESP32-S3 CYD (FNK0104B) touch bring-up - FT6336U + ILI9341, isolated.
//
// No Nimbus, no serial console dependency: the SCREEN is the readout. It fills
// the whole panel with a colour chosen by WHICH QUADRANT you touch (using the
// mapped landscape coordinate), and black when you lift off. So:
//   - if the screen changes colour at all when you touch -> the FT6336U read
//     path works end to end;
//   - which colour appears for a given corner tells us the exact orientation,
//     i.e. the swapXY / invertX / invertY flags the driver calibration needs.
//
// Expected with the initial guess (swapXY=true, no inversion):
//   top-left = RED, top-right = GREEN, bottom-left = BLUE, bottom-right = YELLOW
//
//   pio run -e tfttouch-cyd -t upload --upload-port <freenove usbmodem>
#include <Arduino.h>

#include "solide/display_tft.h"
#include "solide/touch.h"

using namespace solide;

static constexpr uint16_t BLACK  = 0x0000, RED = 0xF800, GREEN = 0x07E0,
                          BLUE   = 0x001F, YELLOW = 0xFFE0, WHITE = 0xFFFF;

static uint16_t g_last = 0xABCD;   // force first paint

static uint16_t quadrantColour(int x, int y) {
  const int W = display_tft::kW, H = display_tft::kH;
  const bool right = x >= W / 2;
  const bool bottom = y >= H / 2;
  if (!right && !bottom) return RED;
  if (right  && !bottom) return GREEN;
  if (!right &&  bottom) return BLUE;
  return YELLOW;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[tfttouch-cyd] FT6336U + ILI9341 bring-up");

  const bool disp = display_tft::begin();
  display_tft::setBacklight(100);
  display_tft::fill(WHITE);            // proves the panel/backlight before any touch
  delay(400);
  display_tft::fill(BLACK);

  const bool tch = touch::begin();
  // Landscape mapping guess for a portrait-native FT6336U. Adjust from results.
  touch::Calibration cal{};
  cal.swapXY = true;
  cal.invertX = false;
  cal.invertY = false;
  touch::setCalibration(cal);

  Serial.printf("[tfttouch-cyd] display=%d touch_present=%d\n", int(disp), int(tch));
}

void loop() {
  const touch::Point p = touch::read();

  uint16_t want = BLACK;
  if (p.down) want = quadrantColour(p.x, p.y);

  if (want != g_last) {
    display_tft::fill(want);
    g_last = want;
  }

  // Raw native coords too, for anyone who does have the console.
  static uint32_t lastLog = 0;
  if (p.down && millis() - lastLog > 200) {
    lastLog = millis();
    uint16_t rx = 0, ry = 0, rz = 0;
    touch::readRaw(rx, ry, rz);
    Serial.printf("[touch] down x=%d y=%d  raw=(%u,%u)\n", p.x, p.y, rx, ry);
  }
  delay(10);
}
