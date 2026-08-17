#pragma once
#include <cstdint>
#include <string>

// theme - the device's colour personality. A theme resolves to an accent RGB that
// skins ALL of Nimbus's own LED feedback cues (mode-switch breathe, hold-to-talk
// recording/transcribing, action acknowledgements). Per-agent / per-provider
// segment colours stay SEMANTIC (each running agent keeps its own hue) - the theme
// is the device chrome, not the data. Portable + Arduino-free so the name->RGB map
// is host-tested and shared by the device LED glue + the web layer.
namespace nimbus {

struct ThemeColor { uint8_t r, g, b; };

// A theme is a PALETTE of colours (per brand/concept), not a single accent - e.g.
// ocean is deep-blue -> aqua, ember is red -> amber, rainbow is ROYGBIV. Max colours
// per theme.
constexpr int kThemeMaxColors = 6;

// Fill out[] with a theme's palette (up to maxN), return the count. The palette drives
// the multi-colour ring cues + the web swatch picker. Unknown/empty -> the default
// (teal) palette so a bad value never yields a dark ring.
int themePalette(const std::string& name, ThemeColor out[], int maxN);

// The theme's PRIMARY colour (palette[0]) - for single-colour cues (the idle cursor
// glow). Unknown/empty -> default (teal), never dark.
ThemeColor themeAccent(const std::string& name);

// The known theme slugs, comma-separated (drives the web dropdown). Stable order.
const char* themeList();

// Indexed access to themeList() for the on-device menu picker. themeCount() = number
// of themes; themeAt(i) = the i-th slug ("" if out of range); themeIndexOf(slug) =
// its index (0 if unknown, so the picker never lands out of range).
int         themeCount();
std::string themeAt(int idx);
int         themeIndexOf(const std::string& slug);

// The theme accent as a ring HUE (0-254, the solide::ring HSV hue space) so the
// LED compositor can tint hue-based cues (the Calm "working" breathe, the cursor
// glow) in the theme colour. Derived from themeAccent()'s RGB. Near-greys map to
// the default teal's hue so the ring never looks colourless.
uint8_t themeHue(const std::string& name);

// The hue (0-254) for a status "role" within a theme's palette family: roleIdx
// indexes the palette, clamped to its size. Lets the ring paint each status a
// distinct hue from the ACTIVE theme's family (owner: themes must drive the ring,
// keyed by status). See nimbus::statusStyle() for the Status->role map.
uint8_t themeRoleHue(const std::string& name, int roleIdx);

// The theme's ALERT hue (Error / attention) - alarming but in-family: cool themes
// get a warm amber/coral contrast rather than a jarring pure red.
uint8_t themeAlertHue(const std::string& name);

}  // namespace nimbus
