// tftbringup - minimal ILI9341 + XPT2046 bring-up, decoupled from the full
// firmware boot. Answers the only questions that matter on a freshly wired
// board, in order:
//
//   1. Does the panel initialise and light up at all?   (colour bars)
//   2. Are the colours right, or is the panel BGR-swapped? (named bars)
//   3. Does the backlight PWM work?                     (fade)
//   4. Does touch report sane coordinates?              (raw + mapped dump)
//
// Serial prints every touch sample, so the calibration constants can be read
// off a real finger instead of guessed. Run:
//   pio run -e tftbringup -t upload && <bounded serial read>
#include <Arduino.h>

#include "solide/display_tft.h"
#include "solide/touch.h"

namespace {

constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// The web UI palette - if these render as the right colours, the pixel format
// and the BGR bit are both correct.
struct Bar { const char* name; uint16_t colour; };
const Bar kBars[] = {
    {"RED    (should be RED)",   RGB(0xF0, 0x68, 0x7A)},  // --crit
    {"GREEN  (should be GREEN)", RGB(0x63, 0xD1, 0x9A)},  // --ok
    {"BLUE   (should be BLUE)",  RGB(0x6C, 0xB8, 0xFF)},  // --info
    {"TEAL   (should be TEAL)",  RGB(0x5A, 0xD6, 0xC4)},  // --teal
    {"AMBER  (should be AMBER)", RGB(0xF0, 0xB4, 0x5A)},  // --amber
};

// One static full-frame buffer. On the real firmware this lives in PSRAM;
// bring-up keeps it simple and static so a PSRAM fault cannot mask a panel
// fault - we are testing the panel here, nothing else.
uint16_t g_fb[solide::display_tft::kW * solide::display_tft::kH];

inline uint16_t be(uint16_t c) { return uint16_t((c << 8) | (c >> 8)); }

void fillRect(int x, int y, int w, int h, uint16_t colour) {
  const uint16_t v = be(colour);
  for (int yy = y; yy < y + h && yy < solide::display_tft::kH; yy++)
    for (int xx = x; xx < x + w && xx < solide::display_tft::kW; xx++)
      g_fb[yy * solide::display_tft::kW + xx] = v;
}

void drawBars() {
  const int n = sizeof(kBars) / sizeof(kBars[0]);
  const int barH = solide::display_tft::kH / (n + 1);
  fillRect(0, 0, solide::display_tft::kW, solide::display_tft::kH, RGB(0x14, 0x15, 0x18));
  for (int i = 0; i < n; i++)
    fillRect(8, i * barH + 4, solide::display_tft::kW - 16, barH - 8, kBars[i].colour);
  // A white 20px corner marker: proves the origin and that the address window
  // is not rotated or offset.
  fillRect(0, solide::display_tft::kH - 20, 20, 20, 0xFFFF);
}

void push() {
  while (solide::display_tft::busy()) delay(1);
  solide::display_tft::pushFrame(g_fb);
  while (solide::display_tft::busy()) delay(1);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);   // let native-USB CDC enumerate before the first print
  Serial.println("\n=== TFT BRINGUP ===");

  Serial.printf("display_tft::begin() ... ");
  const bool okDisp = solide::display_tft::begin();
  Serial.printf("%s (task=%d)\n", okDisp ? "OK" : "FAILED",
                int(solide::display_tft::taskAlive()));
  if (!okDisp) {
    Serial.println("PANEL DID NOT INIT - check wiring: SCK 42 MOSI 41 CS 38 DC 40 RST 39");
    return;
  }

  Serial.println("fill red / green / blue (each 400 ms) - watch the panel");
  solide::display_tft::fill(RGB(255, 0, 0));   delay(400);
  solide::display_tft::fill(RGB(0, 255, 0));   delay(400);
  solide::display_tft::fill(RGB(0, 0, 255));   delay(400);

  Serial.println("colour bars, top to bottom:");
  for (const auto& b : kBars) Serial.printf("  %s\n", b.name);
  Serial.println("  + a WHITE 20px square at the BOTTOM-LEFT (origin check)");
  drawBars();
  push();

  Serial.println("backlight fade 100 -> 0 -> 100");
  for (int p = 100; p >= 0; p -= 10) { solide::display_tft::setBacklight(p); delay(60); }
  for (int p = 0; p <= 100; p += 10) { solide::display_tft::setBacklight(p); delay(60); }

  Serial.printf("touch::begin() ... ");
  Serial.printf("%s\n", solide::touch::begin() ? "OK" : "FAILED (no T_CS pin?)");
  Serial.println("\nTouch the panel - raw counts + mapped px follow.");
  Serial.println("Note the raw values at each CORNER to set the calibration.");
}

void loop() {
  static uint32_t lastPrint = 0;
  static bool wasDown = false;

  uint16_t rx = 0, ry = 0, rz = 0;
  const bool rawHit = solide::touch::readRaw(rx, ry, rz);
  const solide::touch::Point p = solide::touch::read();

  // Print on every edge, and at 4 Hz while held - enough to read corner values
  // off the console without flooding it.
  if (p.down != wasDown || (p.down && millis() - lastPrint > 250)) {
    Serial.printf("touch down=%d raw=(%4u,%4u) z=%4u -> px=(%3d,%3d)\n",
                  int(p.down), rx, ry, rz, p.x, p.y);
    lastPrint = millis();
    wasDown = p.down;
  }
  (void)rawHit;

  // Live feedback on the panel itself: a teal dot under the finger proves the
  // whole chain (touch -> coords -> framebuffer -> panel) end to end.
  //
  // ⚠ Never touch g_fb while a frame is in flight - the driver blits from it on
  // its own task, so writing here would tear the image. On a bring-up sketch
  // that matters more than usual: the tearing would read as a PANEL fault and
  // send someone chasing the wiring.
  if (solide::display_tft::busy()) { delay(5); return; }

  static bool dirty = false;
  if (p.down) {
    fillRect(p.x - 6, p.y - 6, 12, 12, 0x5AD6 /* teal-ish */);
    dirty = true;
  } else if (dirty) {
    drawBars();
    dirty = false;
  }
  solide::display_tft::pushFrame(g_fb);
  delay(20);
}
