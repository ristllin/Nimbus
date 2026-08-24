#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "nimbus/tft_render/theme.h"

// ============================================================================
// tft_render/fb565 - a portable RGB565 framebuffer for the 240x320 touch panel.
//
// A color 1-word-per-pixel framebuffer, deliberately a SEPARATE type from the
// the e-ink renderer is frozen and must keep producing byte-identical goldens,
// so nothing here may perturb it.
//
// Coordinates: (0,0) top-left, x right, y down, PORTRAIT 240x320.
// Storage: BIG-ENDIAN RGB565, which is the ILI9341's own wire order - the
// device glue blits data() straight out with no per-pixel conversion, and
// tools/tftpreview.py reads the same bytes.
//
// The buffer is 150 KB, far too large for the stack or a static in internal
// SRAM, so Fb565 owns a heap vector and the device places it in PSRAM.
// ============================================================================

namespace nimbus::tft {

constexpr int kW = kScreenW;              // 320 (landscape)
constexpr int kH = kScreenH;              // 240 (landscape)
constexpr int kPixels = kW * kH;
constexpr int kFbBytes = kPixels * 2;     // 153600

// A tap target: a rectangle plus what activating it means. The renderer emits
// these alongside the pixels so the device can hit-test without duplicating any
// layout maths - the single source of truth for "where is that button" is the
// code that drew it.
struct TapRegion {
  int16_t x = 0, y = 0, w = 0, h = 0;
  // What a tap here does. Kept abstract so the input layer maps it onto the
  // SAME calls the encoder makes (see hw touch pump): the menu FSM never learns
  // that touch exists.
  enum class Action : uint8_t {
    None,
    MenuRow,     // select row `index` (set cursor + click)
    Back,        // one level up (menu onLongPress)
    Home,        // back to the status screen
    OpenMenu,    // the gear
    Mic,         // press-and-hold to talk
    ValueUp,     // stepper +   (menu onRotate(+1))
    ValueDown,   // stepper -   (menu onRotate(-1))
    Commit,      // Save/confirm (menu onClick)
    SessionCard, // focus session `index`
    ScrollUp,
    ScrollDown,
  };
  Action  action = Action::None;
  int16_t index = -1;   // row / session index where the action needs one

  bool contains(int px, int py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};

class Fb565 {
 public:
  Fb565() : buf_(size_t(kPixels), 0) { clear(); }

  // ---- raw access (device blit + goldens) ----------------------------------
  const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(buf_.data()); }
  const uint16_t* pixels() const { return buf_.data(); }
  uint16_t* pixels() { return buf_.data(); }
  static constexpr size_t byteSize() { return size_t(kFbBytes); }

  // ---- drawing (all colours are LOGICAL RGB565; storage swaps to BE) --------
  void clear(uint16_t colour = kBg);
  void set(int x, int y, uint16_t colour);
  uint16_t get(int x, int y) const;

  void hline(int x, int y, int w, uint16_t colour);
  void vline(int x, int y, int h, uint16_t colour);
  void fillRect(int x, int y, int w, int h, uint16_t colour);
  void rect(int x, int y, int w, int h, uint16_t colour);          // 1px outline
  // Rounded variants - the web's card (.sec) and chip (.badge) shapes.
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t colour);
  void roundRect(int x, int y, int w, int h, int r, uint16_t colour);

  // ---- text ----------------------------------------------------------------
  // A 5x7 base glyph set scaled by integer factors, matching the web's size
  // hierarchy closely enough at this panel size: scale 1 = caption (web .k),
  // 2 = body (13.5px), 3 = title. Non-ASCII is dropped (the atlas is ASCII).
  void text(int x, int y, const std::string& s, uint16_t colour, int scale = 2);
  static int textWidth(const std::string& s, int scale = 2);
  static int textHeight(int scale = 2) { return 7 * scale; }
  // Uppercased, letter-spaced caption - the web .k label treatment.
  void label(int x, int y, const std::string& s, uint16_t colour = kInk3);
  static int labelWidth(const std::string& s);
  // Draw `s` clipped to `maxW`, appending ".." when it does not fit.
  void textClipped(int x, int y, const std::string& s, uint16_t colour,
                   int maxW, int scale = 2);

  // ---- composites (the shared visual language) -----------------------------
  // A card in the web .sec style: raised fill, 1px line border, rounded.
  void card(int x, int y, int w, int h, uint16_t fill = kRaise,
            uint16_t border = kLine, int radius = kCardRadius);
  // A status chip in the web .badge style: soft-tinted pill, coloured text.
  void chip(int x, int y, const std::string& s, uint16_t colour,
            uint16_t surface = kRaise);
  static int chipWidth(const std::string& s);

 private:
  // Stored big-endian so data() is blit-ready; every accessor swaps.
  static constexpr uint16_t swap(uint16_t v) { return uint16_t((v << 8) | (v >> 8)); }
  std::vector<uint16_t> buf_;
};

// Sanitize to the printable ASCII the glyph atlas covers (mirrors the e-ink
// renderer's rule; the panel can show more, but one atlas keeps goldens honest).
std::string asciiSanitize(const std::string& s);

}  // namespace nimbus::tft
