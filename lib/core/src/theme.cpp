#include "nimbus/theme.h"

namespace nimbus {

namespace {
struct Palette { const char* name; ThemeColor c[kThemeMaxColors]; int n; };

// Per-theme colour palettes (per brand/concept). First colour is the primary. Ordered
// to read as a gradient/sweep on the ring. Keep in sync with themeList()'s order.
const Palette kPalettes[] = {
  // RE-AUTHORED (owner R3, 2026-07-13). The old palettes were single-hue lightness
  // ramps, so Running/Waiting/Approval/Done (statusStyle roles 0-3 = indices 0-3
  // here) rendered as nearly the SAME colour. Each theme now anchors role 0
  // (running) on the family primary and gives roles 1-3 genuinely distinct,
  // family-tinted hues: 1 = needs-you (cool analogous), 2 = done (green-leaning),
  // 3 = approval (amber-leaning). OWNER RULE: red is reserved for ERRORS -
  // no role stop may sit in the red band (test_theme's red-reserved guard pins
  // this + a pairwise hue-distance floor, so a palette edit can't regress it).
  {"teal",      {{64,200,190},{70,120,235},{60,210,110},{235,190,70}},               4},
  {"ocean",     {{30,110,225},{140,110,235},{45,200,150},{235,185,80}},              4},
  {"ember",     {{255,115,10},{200,80,220},{120,205,80},{255,225,70}},               4},  // orange primary - red lives in the alert slot only
  {"forest",    {{45,180,75},{65,150,225},{50,200,180},{230,180,60}},                4},
  {"openai",    {{16,163,127},{95,125,240},{80,215,120},{240,190,70}},               4},
  {"anthropic", {{235,140,80},{150,110,220},{110,200,130},{248,215,110}},            4},
  // Mistral's brand is the yellow->orange flag band - orange primary, yellow approval.
  {"mistral",   {{255,120,5},{170,100,230},{150,215,60},{255,216,0}},                4},
  // Rainbow: roles 0-3 are blue/violet/green/orange; yellow+red close the sweep as
  // stops 4-5 (never a role - red stays error-only).
  {"rainbow",   {{40,120,230},{150,60,220},{40,190,70},{255,140,0},{240,215,30},{230,30,30}}, 6},
  {"gemini",    {{66,133,244},{155,114,203},{80,190,120},{242,184,75}},              4},
  {"perplexity",{{35,150,165},{110,120,235},{70,200,120},{230,185,80}},              4},
};
const int kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);

const Palette& paletteFor(const std::string& name) {
  for (int i = 0; i < kPaletteCount; ++i)
    if (name == kPalettes[i].name) return kPalettes[i];
  return kPalettes[0];  // "teal" default (never dark)
}
}  // namespace

int themePalette(const std::string& name, ThemeColor out[], int maxN) {
  const Palette& p = paletteFor(name);
  int n = p.n < maxN ? p.n : maxN;
  for (int i = 0; i < n; ++i) out[i] = p.c[i];
  return n;
}

ThemeColor themeAccent(const std::string& name) { return paletteFor(name).c[0]; }

const char* themeList() {
  return "teal,ocean,ember,forest,openai,anthropic,mistral,rainbow,gemini,perplexity";
}

int themeCount() {
  int n = 1;
  for (const char* s = themeList(); *s; ++s)
    if (*s == ',') ++n;
  return n;
}

std::string themeAt(int idx) {
  if (idx < 0) return "";
  const char* s = themeList();
  int i = 0;
  std::string cur;
  for (;; ++s) {
    if (*s == ',' || *s == 0) {
      if (i == idx) return cur;
      cur.clear();
      ++i;
      if (*s == 0) break;
    } else {
      cur += *s;
    }
  }
  return "";
}

int themeIndexOf(const std::string& slug) {
  const int n = themeCount();
  for (int i = 0; i < n; ++i)
    if (themeAt(i) == slug) return i;
  return 0;
}

// RGB -> HSV hue in the ring's 0-254 space (solide::ring::hsv). Near-grey (tiny
// chroma) has no meaningful hue -> fall back to a teal-ish hue (127) so a themed
// cue never renders colourless.
static uint8_t rgbToHue(ThemeColor c) {
  int r = c.r, g = c.g, b = c.b;
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  int d = mx - mn;
  if (d < 12) return 127;
  float h;
  if (mx == r)      h = (float)(g - b) / d + (g < b ? 6.0f : 0.0f);
  else if (mx == g) h = (float)(b - r) / d + 2.0f;
  else              h = (float)(r - g) / d + 4.0f;
  h /= 6.0f;                          // 0..1
  int hue = (int)(h * 255.0f);
  if (hue < 0) hue = 0;
  if (hue > 254) hue = 254;
  return (uint8_t)hue;
}

uint8_t themeHue(const std::string& name) { return rgbToHue(themeAccent(name)); }

uint8_t themeRoleHue(const std::string& name, int roleIdx) {
  ThemeColor pal[kThemeMaxColors];
  const int n = themePalette(name, pal, kThemeMaxColors);
  if (n <= 0) return 127;
  if (roleIdx < 0) roleIdx = 0;
  if (roleIdx >= n) roleIdx = n - 1;   // clamp into the family
  return rgbToHue(pal[roleIdx]);
}

uint8_t themeAlertHue(const std::string& name) {
  // Per-theme Error/alert hue: alarming yet in-family. Warm themes -> red (hue ~0-4);
  // cool themes (ocean/perplexity/teal) -> a warm amber/coral contrast so error reads
  // as danger without a jarring pure-red clash ("full red on ocean makes no sense").
  struct A { const char* name; uint8_t hue; };
  static const A k[] = {
      {"teal", 10}, {"ocean", 14}, {"ember", 2},  {"forest", 6},      {"openai", 4},
      {"anthropic", 4}, {"mistral", 2}, {"rainbow", 0}, {"gemini", 6}, {"perplexity", 12},
  };
  for (const auto& a : k)
    if (name == a.name) return a.hue;
  return 0;  // default: red
}

}  // namespace nimbus
