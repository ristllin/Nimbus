// tftmin - smallest program that puts something on the panel, plus ONE variable.
//
// WHY THIS EXISTS
// ---------------
// The blank-screen fault was being chased inside the full firmware, which runs
// Wi-Fi, BLE, an orchestrator, a web server, SD, a 45-LED ring, touch polling
// and a watchdog all at once. Stripped to nothing, the panel is PERFECT: a
// single fill() held solid red for a full minute, then solid green, with no
// flicker (owner-observed 2026-07-30). So the panel and its supply are sound and
// something the firmware does is blanking it.
//
// STEP 2 - the paint PATH, not another subsystem.
// The obvious next variable is not Wi-Fi or the ring: it is that tftmin painted
// with fill() (a small solid write) while the real firmware pushes a 150 KB
// framebuffer through a render task on the other core. That is the nearest thing
// to the failing case that still has nothing else running.
//
//   'f'  paint with fill()      - the known-good path
//   'b'  paint with pushFrame() - the real firmware's path, 150 KB via the task
//   'l'  loop the chosen paint every 2 s (the firmware repaints periodically)
//   's'  stop looping
//
// If the screen goes white under 'b' but not 'f', the blit path is implicated
// and no other subsystem needs to be involved at all.
#include <Arduino.h>

#include <esp_heap_caps.h>
#include <solide/display_tft.h>

namespace {

constexpr uint16_t kRed   = 0xF800;
constexpr uint16_t kGreen = 0x07E0;

// The real firmware's framebuffer lives in PSRAM (150 KB is far too big for
// internal SRAM). Matching that here matters: a different memory region means a
// different DMA path, which would be a variable rather than a control.
uint16_t* g_fb = nullptr;
constexpr size_t kPixels = size_t(solide::display_tft::kW) * solide::display_tft::kH;

bool looping = false;
bool probing = true;   // read registers in the loop? (the variable under test)
uint16_t colour = kRed;

// Read MADCTL back out of RDDST's top byte - the one register this panel answers
// on (RDDPM reads 0x00 here, unimplemented). Says whether it is still CONFIGURED.
// ⚠ It does NOT say whether the panel is DISPLAYING: it stayed correct through
// every blank screen in this investigation, which is why the eye is the oracle
// below and this is only context.
uint8_t madctl() { return uint8_t(solide::display_tft::readReg(0x09, 4) >> 24); }

// 0 = fill() (internal source, known good)
// 1 = pushFrame() straight from PSRAM (the firmware's path - breaks the panel)
// 2 = pushFrame() from an INTERNAL bounce buffer
//
// ⚠ The variable is WHERE THE PIXELS COME FROM, not how many there are. fill()
// is safe and pushFrame() is not, and the standing difference between them is
// that the framebuffer lives in PSRAM: on the S3, DMA out of PSRAM shares the
// external-memory bus, and a 150 KB burst from there is a very different
// transaction from one out of internal SRAM. Mode 2 keeps the blit size and the
// path identical and changes ONLY the source memory, so a pass there indicts
// PSRAM-sourced DMA rather than the blit itself.
int paintMode = 0;
uint16_t* g_bounce = nullptr;
constexpr size_t kBounceRows = 24;   // 24 rows x 320 x 2B = 15 KB, internal

void paint() {
  if (paintMode == 1 && g_fb) {
    for (size_t i = 0; i < kPixels; i++) g_fb[i] = colour;
    solide::display_tft::pushFrame(g_fb);
  } else if (paintMode == 2 && g_fb && g_bounce) {
    // Same total bytes, same commands - only the DMA source changes.
    for (size_t i = 0; i < kPixels; i++) g_fb[i] = colour;
    solide::display_tft::pushFrameChunked(g_fb, g_bounce, kBounceRows);
  } else {
    solide::display_tft::fill(colour);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\ntftmin: panel-only test. Nothing else runs.");

  if (!solide::display_tft::begin()) {
    Serial.println("tftmin: display_tft::begin() FAILED");
    return;
  }
  g_fb = static_cast<uint16_t*>(heap_caps_malloc(kPixels * 2, MALLOC_CAP_SPIRAM));
  g_bounce = static_cast<uint16_t*>(
      heap_caps_malloc(kBounceRows * solide::display_tft::kW * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  Serial.printf("tftmin: panel %dx%d  bl_attached=%d  psram_fb=%s\n",
                int(solide::display_tft::kW), int(solide::display_tft::kH),
                int(solide::display_tft::backlightAttached()),
                g_fb ? "ok" : "FAILED");

  solide::display_tft::setBacklight(100);
  paint();
  Serial.println("tftmin: painted RED with fill(). Keys: f=fill b=blit l=loop s=stop");
  Serial.println("tftmin: THE EYE IS THE ORACLE - the registers stayed correct");
  Serial.println("tftmin: through every blank screen, so they cannot answer this.");
}

void loop() {
  static uint32_t t = 0;
  static uint32_t lastPaint = 0;

  while (Serial.available()) {
    switch (Serial.read()) {
      case 'f': paintMode = 0; colour = kRed;
                Serial.println("tftmin: PATH=fill() (internal) - RED"); paint(); break;
      case 'b': paintMode = 1; colour = kGreen;
                Serial.println("tftmin: PATH=pushFrame from PSRAM - GREEN"); paint(); break;
      case 'i': paintMode = 2; colour = 0x001F;
                Serial.println("tftmin: PATH=pushFrame via INTERNAL bounce - BLUE"); paint(); break;
      case 'l': looping = true;  Serial.println("tftmin: LOOP on (repaint every 2s)"); break;
      case 's': looping = false; Serial.println("tftmin: LOOP off"); break;
      case 'q': probing = false; Serial.println("tftmin: QUIET - blits only, no register reads"); break;
      case 'p': probing = true;  Serial.println("tftmin: PROBE - blits + a register read each second"); break;
      default: break;
    }
  }

  if (looping && millis() - lastPaint > 1000) { lastPaint = millis(); paint(); }

  // ⚠ THE VARIABLE UNDER TEST. readReg() runs HERE, on the main task, while the
  // render task blits on the other core. tfttouch never reads registers in its
  // loop and never goes white; tftmin did, every 2 s, and phase 3 reset the panel
  // (MADCTL 0x28 -> 0x00). So the suspect is the DIAGNOSTIC racing the blit -
  // which would mean the instrument has been causing the fault it measures, and
  // matches the full firmware exactly (its watchdog reads registers every 5 s).
  //   'q'  quiet  - blits only, NEVER read a register
  //   'p'  probe  - blits plus a register read each second (the old behaviour)
  if (millis() / 1000 != t) {
    t = millis() / 1000;
    if (probing)
      Serial.printf("tftmin t=%3lus loop=%d PROBE madctl=0x%02X  (eye: white yet?)\n",
                    (unsigned long)t, int(looping), madctl());
    else
      Serial.printf("tftmin t=%3lus loop=%d quiet (no register reads)  (eye: white yet?)\n",
                    (unsigned long)t, int(looping));
  }
}
