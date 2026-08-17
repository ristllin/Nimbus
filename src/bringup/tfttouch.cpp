// tfttouch - five buttons you press to prove display + touch + calibration.
//
// WHAT IT IS FOR
// --------------
// Three things need proving on this panel and they are easiest to prove together,
// because a wrong answer in any one of them looks the same from a distance:
//   display      the buttons draw at all
//   touch        a press is detected at all
//   calibration  pressing button 3 registers as button 3 - not 1, not 5
// The last one is the reason for five separate targets rather than one: a
// mirrored or swapped axis is INVISIBLE with a single button, and obvious the
// moment you press left and the right-hand one lights.
//
// ⚠ IT REPAINTS ONLY ON A PRESS, never on a timer. Repeated framebuffer blits
// were measured to reset this panel outright (MADCTL 0x28 -> 0x00) with nothing
// else running, so a redraw loop would corrupt the very thing being tested. Each
// paint is also preceded by a re-arm, so a panel that did get knocked over comes
// back rather than leaving the test dead in the water.
//
// Serial prints RAW ADC and MAPPED pixel for every press, which is what turns a
// failed press into a calibration you can compute rather than guess.
//
//   pio run -e tfttouch-uart -t upload
#include <Arduino.h>

#include <esp_heap_caps.h>
#include <solide/display_tft.h>
#include <solide/touch.h>

namespace {

constexpr int kW = solide::display_tft::kW;   // 320 (landscape)
constexpr int kH = solide::display_tft::kH;   // 240
constexpr size_t kPixels = size_t(kW) * kH;

constexpr int kButtons  = 5;
constexpr int kBandH    = 56;                 // result band across the top
constexpr int kBtnTop   = kBandH + 8;
constexpr int kBtnH     = kH - kBtnTop - 8;
constexpr int kBtnW     = kW / kButtons;

// Deliberately far apart in hue so a mis-mapped press is unmistakable, and
// ordered left-to-right so "I pressed the 2nd, the 4th lit" is easy to report.
constexpr uint16_t kCol[kButtons] = {
    0xF800,  // 1 red
    0xFD20,  // 2 orange
    0x07E0,  // 3 green
    0x001F,  // 4 blue
    0xF81F,  // 5 magenta
};
constexpr uint16_t kInk  = 0xFFFF;
constexpr uint16_t kDark = 0x18E3;

uint16_t* g_fb = nullptr;
int g_last = -1;          // last button pressed (-1 = none yet)

void rect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > kW) w = kW - x;
  if (y + h > kH) h = kH - y;
  for (int yy = y; yy < y + h; yy++) {
    uint16_t* row = g_fb + size_t(yy) * kW;
    for (int xx = x; xx < x + w; xx++) row[xx] = c;
  }
}

// A blocky digit 1-5, drawn from segments - enough to label a button without
// dragging a font in. Each button must be identifiable at a glance or the test
// cannot tell you WHICH one lit.
void digit(int n, int cx, int cy, uint16_t c) {
  const int t = 6, w = 26, h = 40;              // stroke, width, height
  const int x = cx - w / 2, y = cy - h / 2;
  auto seg = [&](bool on, int sx, int sy, int sw, int sh) {
    if (on) rect(sx, sy, sw, sh, c);
  };
  //        top        tl          tr          mid         bl          br         bot
  const bool S[5][7] = {
      {1,0,1,1,1,0,1},  // 2-ish shapes; index 0 used for '1'
      {1,0,1,1,1,0,1},
      {1,0,1,1,1,0,1},
      {0,1,1,1,0,1,0},
      {1,1,0,1,0,1,1},
  };
  if (n == 1) { rect(cx - t / 2, y, t, h, c); return; }
  const bool* s = S[n - 1];
  seg(s[0], x, y, w, t);                        // top
  seg(s[1], x, y, t, h / 2);                    // top-left
  seg(s[2], x + w - t, y, t, h / 2);            // top-right
  seg(s[3], x, y + h / 2 - t / 2, w, t);        // middle
  seg(s[4], x, y + h / 2, t, h / 2);            // bottom-left
  seg(s[5], x + w - t, y + h / 2, t, h / 2);    // bottom-right
  seg(s[6], x, y + h - t, w, t);                // bottom
}

void compose() {
  // Result band: shows the colour of whatever was last pressed, so the answer is
  // readable from across the room without looking at a serial log.
  rect(0, 0, kW, kH, kDark);
  rect(0, 0, kW, kBandH, g_last >= 0 ? kCol[g_last] : kDark);
  if (g_last >= 0) digit(g_last + 1, kW / 2, kBandH / 2, 0x0000);

  for (int i = 0; i < kButtons; i++) {
    const int x = i * kBtnW;
    rect(x + 3, kBtnTop, kBtnW - 6, kBtnH, kCol[i]);
    digit(i + 1, x + kBtnW / 2, kBtnTop + kBtnH / 2, kInk);
  }
}

void paint() {
  // Re-arm first: a panel knocked over by an earlier blit would otherwise stay
  // blank and make a working press look like a dead one.
  solide::display_tft::rearm();
  compose();
  solide::display_tft::pushFrame(g_fb);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\ntfttouch: 5-button display + touch + calibration test");

  if (!solide::display_tft::begin()) { Serial.println("tfttouch: panel init FAILED"); return; }
  if (!solide::touch::begin())       Serial.println("tfttouch: touch init failed (display still works)");

  g_fb = static_cast<uint16_t*>(heap_caps_malloc(kPixels * 2, MALLOC_CAP_SPIRAM));
  if (!g_fb) { Serial.println("tfttouch: no PSRAM for framebuffer"); return; }

  // ⚠ 180-degree flip. Owner-observed: the image renders upside down AND the
  // buttons answer in reverse (press 1, get 5). Those are ONE fault, not two -
  // touch is reporting correctly, but the picture is rotated under it, so the
  // button you SEE on the left is the framebuffer's right-hand button. Rotating
  // the display fixes the image and the mapping together; the touch calibration
  // needs no change at all.
  solide::display_tft::setFlip(true);
  solide::display_tft::setBacklight(100);
  paint();

  const solide::touch::Calibration c = solide::touch::calibration();
  Serial.printf("tfttouch: panel %dx%d, 5 buttons of %dpx\n", kW, kH, kBtnW);
  Serial.printf("tfttouch: calibration x=%u..%u y=%u..%u swap=%d invX=%d invY=%d\n",
                c.minX, c.maxX, c.minY, c.maxY, int(c.swapXY), int(c.invertX), int(c.invertY));
  Serial.println("tfttouch: PRESS THE BUTTONS LEFT TO RIGHT (1..5).");
  Serial.println("tfttouch: if the wrong one lights, the raw values below give the fix.");
}

void loop() {
  static bool down = false;
  static uint32_t lastMs = 0;

  // 'x' toggles the flip live, so the correct orientation is confirmed by eye
  // rather than guessed and reflashed.
  while (Serial.available()) {
    if (Serial.read() == 'x') {
      const bool f = !solide::display_tft::flipped();
      solide::display_tft::setFlip(f);
      paint();
      Serial.printf("tfttouch: FLIP=%d - is it upright now, and does 1 light 1?\n", int(f));
    }
  }

  uint16_t rx = 0, ry = 0, rz = 0;
  const bool hit = solide::touch::readRaw(rx, ry, rz);

  if (hit && !down && millis() - lastMs > 250) {   // debounce; act on the edge only
    down = true;
    lastMs = millis();
    const solide::touch::Point p = solide::touch::read();
    const int btn = (p.x >= 0 && p.x < kW) ? (p.x / kBtnW) : -1;

    Serial.printf("PRESS raw=%4u,%-4u z=%4u  ->  pixel=%4d,%-4d  ->  button=%s\n",
                  rx, ry, rz, p.x, p.y,
                  btn >= 0 ? String(btn + 1).c_str() : "off-panel");

    if (btn >= 0 && btn < kButtons) { g_last = btn; paint(); }
  } else if (!hit) {
    down = false;
  }
  delay(20);
}
