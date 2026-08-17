#pragma once
#include <cstdint>

// ============================================================================
// tft_render/theme - the colour + metric tokens for the touch UI.
//
// THESE ARE THE WEB UI'S OWN TOKENS. The device screen and the web interface
// are one product and must read as one, so every value here is copied from the
// CSS custom properties in include/web/ui_shell.h and carries its variable name
// in the comment beside it. Change one, change the other - that pairing is the
// only thing keeping the two surfaces from drifting apart.
//
// ⚠ Colour ownership, where the two systems overlap:
//   - CHROME (background, cards, borders, text) uses THIS palette, always.
//   - PER-SESSION STATUS accents use the ring's statusStyle()/themePalette()
//     roles instead, so the LED ring and the screen never disagree about what
//     "waiting" or "error" looks like. tintFor() below is how a role colour
//     gets rendered as a card accent in the web's soft-tint style.
// ============================================================================

namespace nimbus::tft {

// RGB565, the panel's native format. Built from the same 8-bit hex the CSS uses
// so the two are trivially checkable against each other.
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t hex(uint32_t v) {
  return rgb(uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v));
}

// ---- surfaces ---------------------------------------------------------------
constexpr uint16_t kBg      = hex(0x141518);  // --bg      page background
constexpr uint16_t kRaise   = hex(0x1c1e23);  // --raise   card fill (web .sec)
constexpr uint16_t kRaise2  = hex(0x232630);  // --raise2  pressed / nested
constexpr uint16_t kRaise3  = hex(0x2a2e37);  // --raise3
constexpr uint16_t kLine    = hex(0x2a2d36);  // --line    card border, divider
constexpr uint16_t kLine2   = hex(0x363b46);  // --line2   stronger divider

// ---- text -------------------------------------------------------------------
constexpr uint16_t kInk     = hex(0xeceef2);  // --ink     primary
constexpr uint16_t kInk2    = hex(0xa7adba);  // --ink2    secondary
constexpr uint16_t kInk3    = hex(0x6f7684);  // --ink3    labels/captions (web .k)

// ---- accents ----------------------------------------------------------------
constexpr uint16_t kTeal    = hex(0x5ad6c4);  // --teal    selection, primary action
constexpr uint16_t kTealD   = hex(0x2ea394);  // --teal-d  pressed accent
constexpr uint16_t kAmber   = hex(0xf0b45a);  // --amber   approval / attention
constexpr uint16_t kWarm    = hex(0xf0947a);  // --warm

// ---- semantic status --------------------------------------------------------
constexpr uint16_t kOk      = hex(0x63d19a);  // --ok      done / healthy
constexpr uint16_t kWarn    = hex(0xeab54a);  // --warn    caution
constexpr uint16_t kCrit    = hex(0xf0687a);  // --crit    error
constexpr uint16_t kInfo    = hex(0x6cb8ff);  // --info    informational

// ---- metrics (web .sec / .badge / .tabs, in panel px) -----------------------
// ⚠ LANDSCAPE. The panel is mounted with its long edge horizontal (owner,
// 2026-07-29: the portrait build was "on the wrong side" and had to turn 90
// degrees counter-clockwise), so the logical surface is 320x240, not the module's
// native 240x320 portrait. The driver's MADCTL carries the matching rotation and
// the touch calibration flags carry the matching axis swap - all three must agree
// or taps land somewhere other than where the pixels are.
// The pixel COUNT is identical either way, so the 150 KB framebuffers and every
// allocation guard are unaffected by the rotation.
constexpr int kScreenW    = 320;
constexpr int kScreenH    = 240;
constexpr int kPad        = 12;  // outer page gutter
constexpr int kCardPad    = 12;  // web .sec uses 16; tightened for a small panel
constexpr int kCardRadius = 12;  // web .sec 16px, scaled to the smaller cards
constexpr int kPillRadius = 8;   // web .badge 11px
// The header carries the Back/Home and gear targets, so it is at least kMinTap
// tall - at 40px those controls were below the touch minimum (caught by
// test_tft_render's structural gate before any golden was blessed).
constexpr int kHeaderH    = 44;
constexpr int kTabBarH    = 46;  // web mobile .tabs
constexpr int kRowH       = 46;  // menu row: >= the 44px tap minimum
constexpr int kMinTap     = 44;  // a11y floor; tftpreview.py regions enforces it

// ---- soft tint --------------------------------------------------------------
// The web fills status chips with the status colour at ~12% alpha over the card
// (--teal-soft is rgba(90,214,196,.12)). There is no alpha in RGB565, so blend
// against the surface at build time and get the identical result.
constexpr uint8_t kTintPct = 12;  // matches --*-soft

constexpr uint16_t blend(uint16_t fg, uint16_t bg, uint8_t pct) {
  // Unpack both to 5/6/5, mix, repack. constexpr so tints cost nothing at runtime.
  const int fr = (fg >> 11) & 0x1F, fgn = (fg >> 5) & 0x3F, fb = fg & 0x1F;
  const int br = (bg >> 11) & 0x1F, bgn = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  const int r = (fr * pct + br * (100 - pct)) / 100;
  const int g = (fgn * pct + bgn * (100 - pct)) / 100;
  const int b = (fb * pct + bb * (100 - pct)) / 100;
  return uint16_t((r << 11) | (g << 5) | b);
}

// A status colour as a chip background over a card (the web .badge treatment).
constexpr uint16_t tintFor(uint16_t statusColour, uint16_t surface = kRaise) {
  return blend(statusColour, surface, kTintPct);
}

// ---- ring-role bridge -------------------------------------------------------
// The ring speaks in HUE (statusStyle roles). The screen needs a concrete
// colour that (a) means the same thing and (b) does not clash with the web
// chrome. These are the web's semantic colours chosen to match each ring role,
// so a red ring and a red card are the same red.
enum class StatusTone : uint8_t { Neutral, Working, Waiting, Approval, Done, Error };

constexpr uint16_t colourFor(StatusTone t) {
  switch (t) {
    case StatusTone::Working:  return kInfo;   // moving, no action needed
    case StatusTone::Waiting:  return kTeal;   // needs you - the accent
    case StatusTone::Approval: return kAmber;  // needs a decision
    case StatusTone::Done:     return kOk;
    case StatusTone::Error:    return kCrit;
    case StatusTone::Neutral:
    default:                   return kInk3;
  }
}

// Short label for a tone - the chip text (sentence case per the style guide).
constexpr const char* labelFor(StatusTone t) {
  switch (t) {
    case StatusTone::Working:  return "working";
    case StatusTone::Waiting:  return "waiting";
    case StatusTone::Approval: return "approve";
    case StatusTone::Done:     return "done";
    case StatusTone::Error:    return "error";
    case StatusTone::Neutral:
    default:                   return "idle";
  }
}

}  // namespace nimbus::tft
