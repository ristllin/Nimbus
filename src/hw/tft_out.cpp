#include "tft_out.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>   // esp_ptr_external_ram

#include <cstring>

#include "nimbus/fault.h"
#include "solide/display_tft.h"
#include "solide/touch.h"

namespace nimbus::hw::tft {

namespace {

using namespace nimbus::tft;

// TWO framebuffers in PSRAM, alternated. The driver blits asynchronously off a
// task, so composing into the buffer it is still reading would tear the frame;
// the renderer always writes the one NOT in flight.
Fb565* g_fb[2] = {nullptr, nullptr};
uint8_t g_slot = 0;
Rendered g_taps;             // regions of the frame currently on the panel
bool     g_ready = false;
bool     g_haveLast = false;
uint8_t* g_last = nullptr;   // last frame actually pushed (dirty-gate), PSRAM

// How long an UNCHANGED frame is trusted before the panel is re-armed and
// repainted anyway. Bounds how long a silently-reset panel can stay blank.
constexpr uint32_t kHealMs = 5000;
uint32_t g_lastPushMs = 0;   // millis() of the last real push
uint32_t g_heals = 0;        // forced repaints (panel watchdog, not errors)
uint32_t g_lastHealthMs = 0; // last panel health probe
bool g_probeEnabled = false; // read panel registers at all? (suspect - see below)
uint32_t g_contentLost = 0;  // times the panel's own pixels disagreed with ours
uint32_t g_repaints = 0;     // unconditional watchdog repaints (covers the
                             // undetectable pixel-loss mode)

}  // namespace

bool begin() {
  if (g_ready) return true;

  if (!solide::display_tft::begin()) {
    log_e("tft_out: panel init failed");
    return false;
  }
  // Touch borrows the panel's SPI bus, so it MUST be brought up after it.
  if (!solide::touch::begin())
    log_w("tft_out: touch init failed - display works, input will not");

  // 150 KB each. PSRAM ONLY: two of these in internal SRAM would consume the
  // whole ~327 KB internal heap and the device would not survive a TLS
  // handshake (the same lesson as the earlier framebuffers and the SFX queue).
  //
  // ⚠ Fb565 holds a std::vector, so placement-new into PSRAM puts only the
  // 12-byte header there - the PIXELS come from the default allocator. That
  // lands in PSRAM today because CONFIG_SPIRAM_USE_MALLOC=y with
  // ALWAYSINTERNAL=4096, but that is an sdkconfig value, not a guarantee: with
  // PSRAM absent or exhausted, malloc falls back to internal and quietly eats
  // the heap. So VERIFY where the pixels actually landed and refuse rather than
  // limp - a headless device with a working web UI beats an OOM reboot loop.
  // Every failure path unwinds what it already took: a half-allocated pair
  // would strand 150 KB with no owner, and Fb565's ctor can throw bad_alloc
  // (exceptions are on) which would reach std::terminate -> panic reboot, i.e.
  // exactly the OOM loop this guard exists to avoid.
  auto unwind = [] {
    for (int j = 0; j < 2; j++) {
      if (!g_fb[j]) continue;
      g_fb[j]->~Fb565();
      heap_caps_free(g_fb[j]);
      g_fb[j] = nullptr;
    }
  };
  for (int i = 0; i < 2; i++) {
    void* p = heap_caps_malloc(sizeof(Fb565), MALLOC_CAP_SPIRAM);
    if (!p) { log_e("tft_out: no PSRAM for framebuffer %d", i); unwind(); return false; }
    try {
      g_fb[i] = new (p) Fb565();
    } catch (...) {
      heap_caps_free(p);
      log_e("tft_out: framebuffer %d allocation threw", i);
      unwind();
      return false;
    }
    if (!esp_ptr_external_ram(g_fb[i]->data())) {
      log_e("tft_out: framebuffer %d pixels landed in INTERNAL RAM (%u B) - refusing",
            i, unsigned(kFbBytes));
      unwind();
      return false;
    }
  }
  g_last = static_cast<uint8_t*>(heap_caps_malloc(size_t(kFbBytes), MALLOC_CAP_SPIRAM));

  g_ready = true;
  return true;
}

bool ready() { return g_ready; }

Push renderAndPush(nimbus::attn::ScreenId screen, const nimbus::render::ScreenCtx& ctx) {
  if (!g_ready) return Push::Dropped;
  // Resilience: FAULT screen drops the panel update while the compose pipeline
  // upstream keeps running - the device is exercised as if the display failed.
  if (nimbus::fault::active(nimbus::fault::SCREEN)) return Push::Dropped;

  // Never compose into the buffer the driver is still reading.
  if (solide::display_tft::busy()) return Push::Dropped;

  Fb565* fb = g_fb[g_slot];
  Rendered r = renderScreen(*fb, screen, ctx);

  // Dirty-gate. The panel is fast, but several producers re-emit an IDENTICAL
  // frame (battery repaint tick, the broker's ~30 s snapshot heartbeat), and
  // every skipped push leaves the shared SPI bus free for touch.
  //
  // ⚠ The gate EXPIRES. Skipping a push asserts "the panel already shows this",
  // and that assumption can be FALSE: the ILI9341 can lose its state without the
  // ESP32 noticing (a brownout on its rail, ESD, a glitch on RESET), reverting to
  // sleeping / 18-bit format. The panel then sits WHITE while the firmware is
  // certain it is painted - and because the composed frame never changes on an
  // idle device, it would NEVER repaint. Observed on hardware 2026-07-29: a
  // correct frame had been pushed, /api/screenshot showed the real UI, and the
  // glass was blank white until a restart.
  //
  // So an unchanged frame is only trusted for kHealMs. After that the panel is
  // re-armed and repainted unconditionally. Cost when nothing is wrong: one
  // ~31 ms blit every 5 s (~0.6% of the bus), and no visible change since the
  // pixels are identical.
  const uint32_t now = millis();
  if (g_haveLast && g_last &&
      std::memcmp(fb->data(), g_last, size_t(kFbBytes)) == 0) {
    if (uint32_t(now - g_lastPushMs) < kHealMs)
      return Push::Unchanged;   // the panel is ALREADY correct - not a drop
    // Past the window, re-assert the panel mode state, then let the register read
    // only decide whether to ALSO repaint. healthy() compares its MADCTL readback
    // against what we wrote - but MADCTL survives SLPIN and DISPOFF (only a reset
    // clears it), so a panel the setup-mode AP beacon has knocked to sleep reads
    // HEALTHY while the glass is white. So rearm() runs UNCONDITIONALLY: it re-sends
    // SLPOUT + DISPON (reset-free, microseconds, invisible on a live panel) and
    // wakes exactly that slept-but-configured panel. Before CUM-231 this branch did
    // NOTHING when healthy() was true; that was masked only because the flip=1
    // watchdog thrash (driver <=v0.7.1 on a flipped panel) made healthy() read false
    // every cycle, reasserting the panel each time. Once the driver stopped the
    // thrash, a beacon-slept panel stayed white here until the 5 s tickHealth. The
    // check now classifies (config lost -> also repaint), it never gates the reassert.
    const bool configOk = solide::display_tft::healthy();
    solide::display_tft::rearm();
    if (configOk) {
      g_lastPushMs = now;       // configured and awake now - no repaint needed
      return Push::Unchanged;
    }
    // Confirmed config loss: fall through and repaint on top of the reassert.
    g_heals++;
  }

  if (!solide::display_tft::pushFrame(fb->pixels())) return Push::Dropped;

  g_lastPushMs = now;
  if (g_last) {
    std::memcpy(g_last, fb->data(), size_t(kFbBytes));
    g_haveLast = true;
  }
  g_taps = std::move(r);       // hit-testing follows what is ON the panel
  g_slot ^= 1;                 // next compose goes to the other buffer
  return Push::Pushed;
}

bool repaintRingRegion(const nimbus::render::ScreenCtx& ctx) {
  if (!g_ready) return false;
  if (nimbus::fault::active(nimbus::fault::SCREEN)) return false;
  if (solide::display_tft::busy()) return false;   // never collide with an async full blit

  // Compose the whole StatusIdle frame in RAM (cheap - no SPI), but push ONLY the
  // ring square. The static parts (header) are already on the panel from the last
  // full renderAndPush; the animated ring + its inner legend are inside the rect.
  Fb565* fb = g_fb[g_slot];
  Rendered r = renderScreen(*fb, nimbus::attn::ScreenId::StatusIdle, ctx);
  if (r.ringW <= 0 || r.ringH <= 0) return false;   // this frame has no on-screen ring

  const uint16_t* win = fb->pixels() + size_t(r.ringY) * size_t(kW) + size_t(r.ringX);
  if (!solide::display_tft::pushRegion(r.ringX, r.ringY, r.ringW, r.ringH, win, kW))
    return false;

  // Keep the dirty-gate snapshot + hit-testing coherent with the ring we just drew.
  if (g_last)
    for (int rr = 0; rr < r.ringH; ++rr) {
      const size_t off = (size_t(r.ringY + rr) * size_t(kW) + size_t(r.ringX)) * 2;
      std::memcpy(g_last + off, fb->data() + off, size_t(r.ringW) * 2);
    }
  g_taps = std::move(r);
  return true;
}

const uint8_t* lastFrame() { return (g_ready && g_haveLast) ? g_last : nullptr; }
size_t lastFrameBytes() { return size_t(kFbBytes); }

void forceRepaint() { g_haveLast = false; }  // next push always lands (post-flip)

// Compare what the panel HOLDS against what we last PUSHED.
//
// This is the health check MADCTL cannot be: it verifies CONTENT, not
// configuration. The panel can hold a perfect register set and still have lost
// or corrupted its pixels, and that case is invisible to every other signal -
// which is exactly the gap the blank-screen investigation kept running into.
//
// Samples rather than sweeps: readPixel runs at 4 MHz and costs ~9 bytes each,
// so a scattered handful every watchdog window is cheap while a full compare
// would not be. Points are spread across the surface so a partial-area fault
// cannot hide between them.
//
// ⚠ Returns true when there is nothing to compare against (no frame pushed yet),
// because "unknown" must not be reported as "broken" - a false alarm here would
// drive a repaint loop.
bool panelContentOk(int samples) {
  if (!g_ready || !g_haveLast || !g_last) return true;
  if (samples < 1) samples = 1;
  const int pts[8][2] = {
    {4, 4}, {kW / 2, 4}, {kW - 5, 4}, {4, kH / 2},
    {kW - 5, kH / 2}, {4, kH - 5}, {kW / 2, kH - 5}, {kW - 5, kH - 5},
  };
  const int n = samples > 8 ? 8 : samples;
  for (int i = 0; i < n; i++) {
    const int x = pts[i][0], y = pts[i][1];
    const size_t off = (size_t(y) * size_t(kW) + size_t(x)) * 2;
    const uint16_t want = uint16_t((g_last[off] << 8) | g_last[off + 1]);
    const uint16_t got = solide::display_tft::readPixel(x, y);
    // The panel returns 18-bit 6-6-6; comparing the top 5 bits per channel is
    // the most precision a 565 round-trip can carry.
    if ((got & 0xF7DE) != (want & 0xF7DE)) return false;
  }
  return true;
}

uint32_t healCount() { return g_heals; }
uint32_t repaintCount() { return g_repaints; }
uint32_t contentLostCount() { return g_contentLost; }
void setProbeEnabled(bool on) { g_probeEnabled = on; }
bool probeEnabled() { return g_probeEnabled; }
bool panelConfigOk() { return g_ready && solide::display_tft::healthy(); }

bool tickHealth(uint32_t now) {
  if (!g_ready || nimbus::fault::active(nimbus::fault::SCREEN)) return false;
  if (uint32_t(now - g_lastHealthMs) < kHealMs) return false;
  g_lastHealthMs = now;

  // ⚠ TWO failure modes, and only one of them is detectable.
  //
  // (a) The panel loses its CONFIGURATION - MADCTL reverts to the power-on
  //     default. healthy() sees this, and a rearm() fixes it.
  // (b) The panel keeps its configuration but loses its PIXELS. Nothing in the
  //     register space reveals that: healthy() returns true while the glass is
  //     blank white.
  //
  // An earlier version of this checked healthy() and returned early when it was
  // true, to avoid a "needless" 150 KB blit. That made (b) UNRECOVERABLE - the
  // composed frame is byte-identical forever on an idle device, so the dirty
  // gate skipped it and the screen stayed white until a restart. Observed in the
  // field with the framebuffer holding correct UI content and hal.display true.
  //
  // So the repaint is now UNCONDITIONAL. It costs one ~31 ms blit per window
  // (~0.6% of the bus) and is invisible when nothing is wrong, because the
  // pixels are identical. That is the right price for covering a fault we cannot
  // observe. The register check still runs - it upgrades the response from
  // "repaint" to "reconfigure, then repaint" - but it is no longer a GATE on
  // whether we act at all.
  // ⚠ rearm() runs UNCONDITIONALLY too, not just when the registers look wrong.
  //
  // Field symptom: the UI shows for about a second after boot and then goes
  // white, every time, while MADCTL still reads back correctly. MADCTL survives
  // SLPIN and DISPOFF - only a reset clears it - so a panel that has gone to
  // sleep or switched its display off is INVISIBLE to healthy(), and a repaint
  // alone cannot rescue it either: RAMWR into a sleeping panel shows nothing.
  //
  // rearm() re-sends SLPOUT + DISPON (plus the pixel format and access order).
  // It is reset-free, takes microseconds, and is a no-op on a healthy panel - so
  // there is no reason to gate it on a check that cannot see the failure it
  // would fix. The register check now only classifies what happened, for the
  // counters; it never decides whether to act.
  // ⚠ REGISTER PROBING IS THE SUSPECT, so it is gated off by default.
  //
  // healthy() and panelContentOk() both READ the panel (readReg / readPixel) from
  // the MAIN task while the render task blits on the other core. Measured in the
  // minimal test app: with blits running, those reads return NOISE (MADCTL walked
  // 0x29/0x20/0x01/0x00 at random where an idle bus gives a rock-solid 0x28). An
  // instrument that is itself disturbed by the traffic it observes can just as
  // easily be disturbing it - and the one app that never reads registers
  // (tfttouch) is the one that never goes white.
  //
  // The repaint below does NOT depend on these, so switching them off costs
  // nothing but the counters. g_probeEnabled flips at runtime (PANELPROBE) so the
  // hypothesis is tested on the device instead of argued.
  const bool configLost  = g_probeEnabled && !solide::display_tft::healthy();
  const bool contentLost = g_probeEnabled && !panelContentOk(4);
  solide::display_tft::rearm();
  if (configLost)  g_heals++;
  if (contentLost) g_contentLost++;
  g_repaints++;
  g_haveLast = false;   // force the push past the dirty gate
  return true;
}

const Rendered* current() { return g_ready ? &g_taps : nullptr; }

const TapRegion* hitTest(int x, int y) {
  if (!g_ready) return nullptr;
  return g_taps.hit(x, y);
}

void setBacklight(uint8_t pct) { solide::display_tft::setBacklight(pct); }
uint8_t backlight() { return solide::display_tft::backlight(); }

}  // namespace nimbus::hw::tft
