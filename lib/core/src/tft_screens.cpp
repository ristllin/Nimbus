#include "nimbus/tft_render/screens.h"

#include <algorithm>

#include "nimbus/device_identity.h"   // wifiQrPayload - the setup screen's join QR
#include "nimbus/qr.h"
#include "nimbus/text_page.h"         // TextPager - Ask-screen pagination

// The colour touch UI. Layout language is lifted from the web interface so the
// two surfaces read as one product: cards are the web's .sec (raised fill, 1px
// line border, rounded), captions are .k (uppercase, letter-spaced, --ink3),
// status chips are .badge (soft-tinted pill, coloured text).
//
// Every interactive element registers a TapRegion as it is drawn, so the hit
// map and the pixels cannot disagree.

namespace nimbus::tft {

namespace {

using render::ScreenCtx;

// The page gutter (L.gut(), = the outer pad) and body top (L.bodyTop(), just
// below the header) come from the Layout the renderer builds from the
// framebuffer's size, so a screen draws to whatever panel it is given.

void push(Rendered& r, int x, int y, int w, int h, TapRegion::Action a,
          int index = -1) {
  TapRegion t;
  t.x = int16_t(x); t.y = int16_t(y); t.w = int16_t(w); t.h = int16_t(h);
  t.action = a; t.index = int16_t(index);
  r.taps.push_back(t);
}

// ---- small icons ------------------------------------------------------------
// Drawn as primitives: no emoji, no font dependency, and they tint with the
// palette like everything else.

void iconGear(Fb565& fb, int cx, int cy, uint16_t c) {
  // ⚠ EIGHT teeth, not four. With only the four orthogonal ones this read as a
  // cross or an asterisk rather than a cog (owner, 2026-07-30) - four spokes on
  // a round body is the shape of a plus sign, and the eye resolves it that way
  // long before it considers "gear". The diagonals are what make it a cog.
  //
  // Offsets are a precomputed octagon at r=9 (no trig, no float): the axis teeth
  // sit at 9, the diagonals at 6,6 - which is 8.5, close enough that the ring of
  // teeth reads as circular at this size.
  static const int8_t kTeeth[8][2] = {
      {0, -9}, {6, -6}, {9, 0}, {6, 6}, {0, 9}, {-6, 6}, {-9, 0}, {-6, -6},
  };
  for (const auto& t : kTeeth) fb.fillRect(cx + t[0] - 2, cy + t[1] - 2, 4, 4, c);
  // Body: a round rect at full corner radius is a circle at this size.
  fb.fillRoundRect(cx - 7, cy - 7, 14, 14, 7, c);
  // Hub punched back out to the background, which is what separates a gear from
  // a blob - a solid disc with bumps still does not read as a cog.
  fb.fillRoundRect(cx - 4, cy - 4, 8, 8, 4, kBg);
}

void iconMic(Fb565& fb, int cx, int cy, uint16_t c) {
  fb.fillRoundRect(cx - 4, cy - 9, 8, 12, 4, c);    // capsule
  fb.hline(cx - 7, cy + 5, 15, c);                  // cradle
  fb.vline(cx - 7, cy + 1, 5, c);
  fb.vline(cx + 7, cy + 1, 5, c);
  fb.vline(cx, cy + 5, 4, c);                       // stem
  fb.hline(cx - 4, cy + 9, 9, c);                   // base
}

void iconChevronLeft(Fb565& fb, int cx, int cy, uint16_t c) {
  for (int i = 0; i < 6; i++) {
    fb.set(cx + i - 2, cy - i, c);  fb.set(cx + i - 1, cy - i, c);
    fb.set(cx + i - 2, cy + i, c);  fb.set(cx + i - 1, cy + i, c);
  }
}

void iconChevronRight(Fb565& fb, int cx, int cy, uint16_t c) {
  for (int i = 0; i < 6; i++) {
    fb.set(cx - i + 1, cy - i, c);  fb.set(cx - i + 2, cy - i, c);
    fb.set(cx - i + 1, cy + i, c);  fb.set(cx - i + 2, cy + i, c);
  }
}

void iconChevronUp(Fb565& fb, int cx, int cy, uint16_t c) {
  for (int i = 0; i < 6; i++) {
    fb.set(cx - i, cy + i - 2, c);  fb.set(cx - i, cy + i - 1, c);
    fb.set(cx + i, cy + i - 2, c);  fb.set(cx + i, cy + i - 1, c);
  }
}

void iconChevronDown(Fb565& fb, int cx, int cy, uint16_t c) {
  for (int i = 0; i < 6; i++) {
    fb.set(cx - i, cy - i + 1, c);  fb.set(cx - i, cy - i + 2, c);
    fb.set(cx + i, cy - i + 1, c);  fb.set(cx + i, cy - i + 2, c);
  }
}

// Battery pill with a fill proportional to charge.
void iconBattery(Fb565& fb, int x, int y, uint8_t pct, uint16_t c) {
  constexpr int w = 22, h = 11;
  fb.roundRect(x, y, w, h, 3, c);
  fb.fillRect(x + w, y + 3, 2, 5, c);                       // terminal
  const int inner = ((w - 4) * std::min<int>(pct, 100)) / 100;
  if (inner > 0) fb.fillRect(x + 2, y + 2, inner, h - 4, c);
}

// ---- shared chrome ----------------------------------------------------------

// Top bar: device name, optional back chevron, gear, battery. Present on every
// screen so navigation never depends on remembering a gesture.
void drawHeader(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx,
                const std::string& title, bool backable) {
  fb.fillRect(0, 0, L.w, L.headerH, kRaise);
  fb.hline(0, L.headerH - 1, L.w, kLine);

  int tx = L.gut();
  if (backable) {
    iconChevronLeft(fb, L.gut() + 6, L.headerH / 2, kTeal);
    // The tap target is deliberately MUCH larger than the glyph - the whole left
    // third of the bar goes Back (capacitive edge taps are easy to miss on a small
    // target). 96px wide comfortably clears the title, which starts at tx below.
    push(r, 0, 0, 96, L.headerH, TapRegion::Action::Back);
    tx = L.gut() + 22;
  } else {
    push(r, 0, 0, 110, L.headerH, TapRegion::Action::Home);
  }

  std::string name = title.empty()
      ? (ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName)
      : title;
  // Drop a trailing "  vX.Y.Z": the menu breadcrumb carries it for the wider
  // the earlier layout, and here it only pushed the actual title into an ellipsis
  // ("Settings .."). The version is on the Software update row and in /api/state.
  {
    const size_t v = name.rfind("  v");
    if (v != std::string::npos && v + 3 < name.size() &&
        name[v + 3] >= '0' && name[v + 3] <= '9')
      name.erase(v);
  }
  // Clip before the right cluster (battery + gear) rather than at a fixed width,
  // so a long title can never collide with them.
  const int titleMax = (L.w - L.minTap - (ctx.battery.valid ? 34 : 4)) - tx;
  // ⚠ A breadcrumb is MOST specific at its tail. Clipping head-first turned
  // "Settings > Customize > Brightness" into "Settings ..", hiding the one word
  // that says what is being edited. Drop leading segments until it fits, so the
  // deepest name always survives.
  while (fb.textWidth(name, 2) > titleMax) {
    const size_t gt = name.find(" > ");
    if (gt == std::string::npos) break;
    name = name.substr(gt + 3);
  }
  fb.textClipped(tx, (L.headerH - fb.textHeight(2)) / 2, name, kInk, titleMax, 2);

  // Right cluster: the gear is flush to the right edge so its tap target fits
  // on-panel exactly; the battery sits to its left.
  const int gearX = L.w - L.minTap;                 // target spans gearX..L.w
  iconGear(fb, gearX + L.minTap / 2, L.headerH / 2, kInk2);
  push(r, gearX, 0, L.minTap, L.headerH, TapRegion::Action::OpenMenu);

  if (ctx.battery.valid)
    iconBattery(fb, gearX - 30, (L.headerH - 11) / 2, ctx.battery.percent, kInk2);
}

// ---- status home ------------------------------------------------------------

// RGB (0-255 per channel) -> logical RGB565 (Fb565 swaps to big-endian on store).
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Q15 unit-circle offsets for the 45-LED ring, dot 0 at top going clockwise.
// Integer table (not runtime sin/cos), so the on-screen ring is bit-deterministic
// across hosts - a golden could safely capture it later without libm variance.
static const int16_t kRingCos[45] = {
  0, 4560, 9032, 13328, 17364, 21062, 24351, 27165, 29451,
  31163, 32269, 32747, 32587, 31794, 30381, 28377, 25821, 22762,
  19260, 15383, 11207, 6813, 2286, -2286, -6813, -11207, -15383,
  -19260, -22762, -25821, -28377, -30381, -31794, -32587, -32747, -32269,
  -31163, -29451, -27165, -24351, -21062, -17364, -13328, -9032, -4560,
};
static const int16_t kRingSin[45] = {
  -32767, -32448, -31498, -29934, -27788, -25101, -21925, -18323, -14364,
  -10126, -5690, -1144, 3425, 7927, 12275, 16383, 20173, 23571,
  26509, 28932, 30791, 32051, 32687, 32687, 32051, 30791, 28932,
  26509, 23571, 20173, 16384, 12275, 7927, 3425, -1144, -5690,
  -10126, -14364, -18323, -21925, -25101, -27788, -29934, -31498, -32448,
};

// The on-screen ring: draw the composited LED frame as a circle of small discs
// inside the square [x,y,size]. Boards with no physical ring show this in the
// notifier so the panel says exactly what the LEDs would have. Only ever called
// when ctx.ringLeds is non-empty, so the existing (empty-ringLeds) goldens are untouched.
static void drawRingWidget(Fb565& fb, int x, int y, int size,
                           const std::vector<ScreenCtx::RingLed>& leds) {
  const int n = int(leds.size());
  if (n <= 0) return;
  const int cx = x + size / 2;
  const int cy = y + size / 2;
  const int dotD = size >= 160 ? 11 : size >= 96 ? 8 : 6;
  const int R = size / 2 - dotD / 2 - 1;
  for (int i = 0; i < n; ++i) {
    const int t = (i * 45) / n;                       // map onto the 45-entry table
    const int px = cx + (R * kRingCos[t]) / 32767;
    const int py = cy + (R * kRingSin[t]) / 32767;
    const auto& L = leds[size_t(i)];
    const bool lit = (L.r | L.g | L.b) != 0;
    fb.fillRoundRect(px - dotD / 2, py - dotD / 2, dotD, dotD, dotD / 2,
                     lit ? rgb565(L.r, L.g, L.b) : kRaise2);
  }
}

// Center a single line inside the ring, clipped to the inner circle so text never
// spills onto the dots.
static void ringCenterLine(Fb565& fb, int cx, int y, int inner, const std::string& s,
                           uint16_t colour) {
  const int w = std::min(fb.textWidth(s, 1), inner);
  fb.textClipped(cx - w / 2, y, s, colour, inner, 1);
}

// Ring-dominant Notifier screen for boards with NO physical LED ring: the ring is
// the whole display. Sessions are the arcs the ring already draws; a small legend
// sits inside it (clipped to the inner circle), and orchestrator's hold-to-talk is
// a tall button on the RIGHT so the ring keeps the full screen height. The ring
// square is exposed on Rendered so the device pushes ONLY it at animation cadence.
static void drawRingHome(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  const bool showMic = ctx.modeName && std::string(ctx.modeName) == "orchestrator";
  const int micW = showMic ? 84 : 0;                  // right-side strip for the mic
  const int bodyTop = L.bodyTop();
  const int bodyH = (L.h - L.gut()) - bodyTop;            // ring uses the FULL body height
  const int ringAreaW = L.w - micW - 2 * L.gut();         // width left of the mic strip

  const int rs = std::min(ringAreaW, bodyH);
  const int rx = L.gut() + (ringAreaW - rs) / 2;
  const int ry = bodyTop + (bodyH - rs) / 2;
  drawRingWidget(fb, rx, ry, rs, ctx.ringLeds);
  r.ringX = int16_t(rx); r.ringY = int16_t(ry);
  r.ringW = int16_t(rs); r.ringH = int16_t(rs);

  const int cx = rx + rs / 2, cy = ry + rs / 2;
  const int inner = rs - 72;                           // text stays well inside the dots
  if (ctx.jobs.empty()) {
    ringCenterLine(fb, cx, cy - 8, inner, "Nothing running", kInk);
    ringCenterLine(fb, cx, cy + 8, inner, "no sessions yet", kInk3);
  } else {
    const int fi = (ctx.cursorJob >= 0 && ctx.cursorJob < int(ctx.jobs.size()))
                     ? ctx.cursorJob : 0;
    const auto& j = ctx.jobs[size_t(fi)];
    const StatusTone tone = toneFor(j.status);
    const std::string name = j.label.empty() ? std::string("session") : j.label;
    ringCenterLine(fb, cx, cy - 14, inner, name, kInk);
    fb.label(cx - std::min(fb.labelWidth(labelFor(tone)), inner) / 2, cy + 2,
             labelFor(tone), colourFor(tone));
    if (ctx.jobs.size() > 1) {
      char cnt[24];
      std::snprintf(cnt, sizeof cnt, "%d active", int(ctx.jobs.size()));
      ringCenterLine(fb, cx, cy + 18, inner, cnt, kInk3);
    }
  }

  // Hold-to-talk: a tall button on the RIGHT (orchestrator only). Pressed state
  // fills brighter so a touch is felt instantly (ctx.micHeld set by the device).
  if (showMic) {
    const int bw = micW - L.gut(), bh = std::min(bodyH, 132);
    const int bx = L.w - L.gut() - bw, by = bodyTop + (bodyH - bh) / 2;
    fb.fillRoundRect(bx, by, bw, bh, L.cardRadius, ctx.micHeld ? kInk : kTeal);
    iconMic(fb, bx + bw / 2, by + bh / 2 - 9, kBg);
    fb.text(bx + (bw - fb.textWidth("hold", 1)) / 2, by + bh / 2 + 7, "hold", kBg, 1);
    push(r, bx, by, bw, bh, TapRegion::Action::Mic);
  }
}

void drawStatusHome(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, "", false);

  // Ringless board: the ring IS the display, so use the dedicated ring-dominant
  // layout. buildCtx fills ringLeds only on a board with no physical ring, so a
  // ring board falls straight through to the original card layout (goldens hold).
  if (!ctx.ringLeds.empty()) { drawRingHome(fb, L, r, ctx); return; }
  const int bodyRight = L.w;

  // ⚠ The mic is ORCHESTRATOR-ONLY. Hold-to-talk records, transcribes and sends a
  // TURN; in Notifier mode there is no orchestrator running to send it to, so the
  // control cannot work - and a button that does nothing is worse than no button,
  // because the owner cannot tell it apart from a broken one.
  // Notifier mode gets the space back for session cards, which is what that mode
  // is actually for.
  const bool showMic = ctx.modeName && std::string(ctx.modeName) == "orchestrator";
  const int micH = showMic ? 44 : 0;   // >= L.minTap when shown
  const int gridTop = L.bodyTop();
  const int gridBot = L.h - L.gut() - (showMic ? micH + 8 : 0);

  if (ctx.jobs.empty()) {
    // Empty state: say what the device is doing, not "no data".
    fb.card(L.gut(), gridTop, bodyRight - 2 * L.gut(), 84);
    const std::string l1 = "Nothing running";
    const std::string l2 = ctx.modeName && std::string(ctx.modeName) == "notifier"
                               ? "Waiting for a session"
                               : "Ready when you are";
    fb.text((bodyRight - fb.textWidth(l1, 2)) / 2, gridTop + 26, l1, kInk, 2);
    fb.text((bodyRight - fb.textWidth(l2, 1)) / 2, gridTop + 50, l2, kInk3, 1);
  } else {
    // TWO-UP grid. A single column is right on a 240px-wide portrait panel, but
    // this panel is mounted landscape: 320 wide gives each card ~144px (enough
    // for a real title) while 240 tall fits only ONE full-width row, which is not
    // a session list at all. Two columns x two rows shows four sessions.
    // (The earlier rejection of a 2-up grid was measured at 240px WIDE, where it
    // left ~7 characters of title. It is a different trade at 320.)
    constexpr int cols = 2;
    constexpr int gap = 8;
    const int cardW = (bodyRight - 2 * L.gut() - gap * (cols - 1)) / cols;
    constexpr int cardH = 56;
    const int maxRows = std::max(1, (gridBot - gridTop + gap) / (cardH + gap));
    const int shown = std::min<int>(int(ctx.jobs.size()), maxRows * cols);

    for (int i = 0; i < shown; i++) {
      const int cx = L.gut() + (i % cols) * (cardW + gap);
      const int cy = gridTop + (i / cols) * (cardH + gap);
      const auto& j = ctx.jobs[size_t(i)];
      const StatusTone tone = toneFor(j.status);
      const uint16_t tc = colourFor(tone);
      const bool focused = (i == ctx.cursorJob);
      const uint16_t surface = focused ? kRaise2 : kRaise;

      // The focused card takes the accent border - the touch equivalent of the
      // knob cursor, so the ring's highlighted session is identifiable here.
      fb.card(cx, cy, cardW, cardH, surface, focused ? kTeal : kLine);

      // A left accent stripe in the status tone carries the state AT A GLANCE,
      // using the same role colours as the ring - so the screen and the LEDs say
      // the same thing without the title paying for it.
      fb.fillRect(cx + 1, cy + 8, 3, cardH - 16, tc);

      // ⚠ The title gets the FULL card width. A right-aligned status chip was
      // tried first and left ~4 characters at this card size ("bui..", "publ.."),
      // which is worse than the portrait layout it replaced - a session list whose
      // titles are unreadable is not a session list. State moves to line two.
      // AUTO-FIT, same rule as the menu rows. A 144px card caps a scale-2 title
      // at about ten characters, so real session names arrived as "build fi..",
      // "review d..", "publish ..". Knowing WHICH session needs you is the whole
      // job of this screen, and a truncated name does not carry that - dropping
      // a size to show the name in full is the better trade. Short names keep
      // the larger size, so nothing legible is made smaller for nothing.
      const std::string title = j.label.empty() ? std::string("session") : j.label;
      const int titleMaxW = cardW - 22;
      const int titleScale = fb.textWidth(title, 2) <= titleMaxW ? 2 : 1;
      fb.textClipped(cx + 12, cy + (titleScale == 2 ? 9 : 12), title, kInk,
                     titleMaxW, titleScale);

      // Line two: state in its tone, then the harness. Both small - they are
      // qualifiers, and the title is the thing being scanned for.
      const std::string state = labelFor(tone);
      fb.label(cx + 12, cy + 32, state, tc);
      const char* harness = j.harness == 1 ? "claude"
                          : j.harness == 2 ? "codex"
                          : j.harness == 3 ? "vibe" : "";
      if (harness[0])
        // labelWidth, NOT textWidth: label() letter-spaces (the web .k caption
        // treatment), so measuring with textWidth under-reads it and the two words
        // ran together ("WORKINGCODEX") for every state longer than four letters.
        fb.label(cx + 12 + fb.labelWidth(state) + 8, cy + 32, harness, kInk3);

      // Progress hairline for a running job - ambient, no numbers to read.
      // Flush to the card's bottom edge: at cardH-12 it struck THROUGH the
      // harness label on the shorter landscape card.
      if (tone == StatusTone::Working && j.progress > 0) {
        const int pw = ((cardW - 24) * std::min<int>(j.progress, 100)) / 100;
        fb.fillRect(cx + 12, cy + cardH - 6, cardW - 24, 2, kRaise3);
        fb.fillRect(cx + 12, cy + cardH - 6, pw, 2, tc);
      }

      push(r, cx, cy, cardW, cardH, TapRegion::Action::SessionCard, i);
    }
  }

  // Hold-to-talk bar. Full width, unmissable, and the only accent-filled
  // control on the screen - in Orchestrator mode. See showMic above.
  if (!showMic) return;
  const int my = L.h - L.gut() - micH;
  fb.fillRoundRect(L.gut(), my, L.w - 2 * L.gut(), micH, L.cardRadius, kTeal);
  iconMic(fb, L.gut() + 30, my + micH / 2, kBg);
  fb.text(L.gut() + 52, my + (micH - fb.textHeight(2)) / 2, "Hold to talk", kBg, 2);
  push(r, L.gut(), my, L.w - 2 * L.gut(), micH, TapRegion::Action::Mic);
}

// ---- menu / settings --------------------------------------------------------

void drawMenu(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, ctx.menuTitle.empty() ? "Settings" : ctx.menuTitle, true);

  const int n = int(ctx.menuItems.size());
  if (n == 0) return;

  // When a value is being adjusted the selected row becomes a stepper - the
  // the touch cursor step.
  if (ctx.menuAdjusting && ctx.menuSelected >= 0 && ctx.menuSelected < n) {
    // The FSM wraps the captured value in "< ... >" as the cursor's
    // adjust affordance. Here there are real [-] and [+] buttons directly
    // beneath it, so the arrows are noise that competes with them.
    std::string row = ctx.menuItems[size_t(ctx.menuSelected)];
    if (row.size() > 4 && row.front() == '<' && row.back() == '>') {
      row = row.substr(1, row.size() - 2);
      while (!row.empty() && row.front() == ' ') row.erase(0, 1);
      while (!row.empty() && row.back() == ' ') row.pop_back();
    }
    const int cy = L.bodyTop() + 20;
    // Landscape is 80px shorter than portrait, so this screen is laid out from
    // BOTH ends: the value card hangs off the top, Save is anchored to the bottom
    // (the same idiom as the status screen's mic bar). Offsetting Save from the
    // card is what ran it off the panel twice.
    fb.card(L.gut(), cy, L.w - 2 * L.gut(), 96);
    // Centre it: the value is the subject of this screen, and the buttons that
    // change it are centred under it.
    //
    // ⚠ AUTO-FIT the scale. A fixed scale 3 is wider than the panel for a long
    // row like "Screensaver  60 min", and centring then computes a NEGATIVE x -
    // so the text was clipped off BOTH edges and unreadable. Row text comes from
    // the menu FSM and can be any length, so pick the largest scale that fits
    // rather than assuming one.
    const int rowMaxW = L.w - 2 * L.gut() - 24;
    int rowScale = 3;
    while (rowScale > 1 && fb.textWidth(row, rowScale) > rowMaxW) rowScale--;
    const int rowW = fb.textWidth(row, rowScale);
    const int rowX = rowW <= rowMaxW ? (L.w - rowW) / 2 : L.gut() + 12;
    fb.textClipped(rowX, cy + 10, row, kInk, rowMaxW, rowScale);

    constexpr int btn = 56;
    const int by = cy + 32;
    // minus
    fb.fillRoundRect(L.gut() + 12, by, btn, btn, 10, kRaise2);
    fb.roundRect(L.gut() + 12, by, btn, btn, 10, kLine2);
    fb.fillRect(L.gut() + 12 + btn / 2 - 10, by + btn / 2 - 1, 20, 3, kInk);
    push(r, L.gut() + 12, by, btn, btn, TapRegion::Action::ValueDown);
    // plus
    const int px = L.w - L.gut() - 12 - btn;
    fb.fillRoundRect(px, by, btn, btn, 10, kRaise2);
    fb.roundRect(px, by, btn, btn, 10, kLine2);
    fb.fillRect(px + btn / 2 - 10, by + btn / 2 - 1, 20, 3, kInk);
    fb.fillRect(px + btn / 2 - 1, by + btn / 2 - 10, 3, 20, kInk);
    push(r, px, by, btn, btn, TapRegion::Action::ValueUp);

    if (!ctx.menuHelp.empty())
      fb.textClipped(L.gut() + 12, cy + 100, ctx.menuHelp, kInk3, L.w - 2 * L.gut() - 24, 1);

    // Save
    const int sy = L.h - L.gut() - L.minTap;   // bottom-anchored, never off-panel
    fb.fillRoundRect(L.gut(), sy, L.w - 2 * L.gut(), L.minTap, L.cardRadius, kTeal);
    fb.text((L.w - fb.textWidth("Save", 2)) / 2, sy + (L.minTap - fb.textHeight(2)) / 2,
            "Save", kBg, 2);
    push(r, L.gut(), sy, L.w - 2 * L.gut(), L.minTap, TapRegion::Action::Commit);
    return;
  }

  // Row list. Scrolls around the selection so the cursor is always on screen.
  const int listTop = L.bodyTop();
  const int listBot = L.h - L.gut();
  // TWO COLUMNS on the landscape panel. Measured before changing anything: the
  // single-column layout drew just 2 rows per page, so the 13-row Settings menu
  // took SEVEN pages to walk - for a screen whose whole job is to be the control
  // surface when the web UI is unreachable. Two columns of three is one page in
  // three, and 320px of width is exactly what makes it possible.
  constexpr int cols = 2;
  constexpr int colGap = 8;
  const int colW = (L.w - 2 * L.gut() - colGap * (cols - 1)) / cols;
  const int rowsPerCol = std::max(1, (listBot - listTop) / (L.rowH + 4));
  int perPage = rowsPerCol * cols;
  int first = 0;
  if (n > perPage) {
    // ⚠ PAGE-ALIGNED, not centred on the selection.
    //
    // Centring is right for a knob: the cursor moves and the view follows it.
    // On touch it is actively wrong - you TAP a row, which sets the selection,
    // so the list immediately re-centres and the row you just pressed slides out
    // from under your finger. The owner saw this as the menu "changing order"
    // (2026-07-30), which is exactly what a list jumping by half a page looks
    // like when you did not ask it to move.
    //
    // Page alignment keeps the view completely still while the selection stays
    // on the visible page, so tapping never moves anything. Swiping steps five
    // rows, which crosses a page boundary and turns the page - a deliberate
    // change rather than a surprising one.
    first = (ctx.menuSelected / perPage) * perPage;
    if (first > n - perPage) first = n - perPage;
    if (first < 0) first = 0;
  }

  for (int i = first; i < std::min(n, first + perPage); i++) {
    const int slot = i - first;
    // COLUMN-MAJOR: reading order runs down the left column, then down the
    // right, so a menu the owner already knows keeps its order.
    const int x = L.gut() + (slot / rowsPerCol) * (colW + colGap);
    const int y = listTop + (slot % rowsPerCol) * (L.rowH + 4);
    const bool sel = (i == ctx.menuSelected);
    fb.card(x, y, colW, L.rowH, sel ? kRaise2 : kRaise,
            sel ? kTeal : kLine, 10);

    // Rows arrive as either "Label: Value" or "Label >". Render them the way
    // the web UI does - label left, value right and dimmer - rather than as one
    // long string:
    //   * the trailing " >" is DROPPED; a chevron is already drawn, and showing
    //     both read as "Customize > >" on the panel.
    //   * a value on the right survives; as one string "Battery mode: Balanced"
    //     clipped to "Battery mode:.." and lost the part that matters.
    std::string label = ctx.menuItems[size_t(i)];
    std::string value;
    const bool forward = label.size() > 2 &&
                         label.compare(label.size() - 2, 2, " >") == 0;
    if (forward)
      label.erase(label.size() - 2);
    // The FSM decorates rows for the cursor: "[ 30 ]" marks the value row
    // and "< Back" the way out. On a touch panel the brackets are meaningless
    // and the "<" fought the right-chevron drawn beside it. Strip both and let
    // the chevron carry the direction.
    bool back = false;
    if (label.rfind("< ", 0) == 0) { back = true; label.erase(0, 2); }
    if (label.size() >= 4 && label.front() == '[' && label.back() == ']') {
      label = label.substr(1, label.size() - 2);
      while (!label.empty() && label.front() == ' ') label.erase(0, 1);
      while (!label.empty() && label.back() == ' ') label.pop_back();
    }
    const size_t sep = label.find(": ");
    if (sep != std::string::npos) {
      value = label.substr(sep + 2);
      label = label.substr(0, sep);
    }

    const int chevX = x + colW - 14;
    const int contentRight = (back || forward) ? chevX - 8 : x + colW - 10;
    const int avail = contentRight - (x + 12);
    // AUTO-FIT the label, per row. A 144px column caps a scale-2 label at about
    // nine characters, so "Battery mode", "Connectivity" and "Screensaver" all
    // came out truncated ("Battery..", "Connect..") - and a settings list you
    // cannot read is not navigable. Shrinking only the labels that need it keeps
    // "Sound", "Theme" and "Done" at full size instead of shrinking everything.
    const int labelScale = fb.textWidth(label, 2) <= avail ? 2 : 1;
    if (value.empty()) {
      fb.textClipped(x + 12, y + (L.rowH - fb.textHeight(labelScale)) / 2, label,
                     sel ? kInk : kInk2, avail, labelScale);
    } else {
      // STACKED, not side by side. Sharing one line cost the label half the row
      // and clipped "Battery mode" to "Battery ..", which is the half a person
      // navigates by. Stacked, the label gets the full width and the value sits
      // under it in the web's caption style.
      fb.textClipped(x + 12, y + 8, label, sel ? kInk : kInk2, avail, labelScale);
      fb.textClipped(x + 12, y + 28, value, sel ? kTeal : kInk3, avail, 1);
    }
    // A chevron is a promise of navigation, not generic decoration. Rows that
    // toggle/cycle/run an action remain fully tappable cards but show no arrow.
    // The menu model marks actual descendants with a trailing " >" and Back
    // with "< ", so the pixels now tell the same truth as the FSM.
    if (back)         iconChevronLeft(fb, chevX, y + L.rowH / 2, kInk3);
    else if (forward) iconChevronRight(fb, chevX, y + L.rowH / 2, kInk3);
    push(r, x, y, colW, L.rowH, TapRegion::Action::MenuRow, i);
  }

  // Paging affordances, only when the list actually overflows.
  //
  // ⚠ These must NOT sit on top of a row. hit() is last-wins, so an invisible
  // target overlapping the first/last row would silently scroll instead of
  // opening it - over a chevron that says "tap me to descend". They therefore
  // live in the gutter BELOW the last drawn row, are drawn as visible arrows,
  // and only exist when there is somewhere to scroll to.
  // Scroll indicator, in the right-hand gutter. Swiping without one is
  // disorienting: there is no way to tell how far down you are, or even whether
  // there is anything more to see (owner, 2026-07-30). The bar answers both -
  // its LENGTH is how much of the list fits on screen, its POSITION is where you
  // are, and its ABSENCE means everything already fits.
  //
  // Sits at x = L.w - L.gut() + 2, entirely inside the gutter, so it cannot clip the
  // right-hand column's chevron. No tap region: it is an indicator, not a
  // control - the pager and swipe both already scroll, and a third target here
  // would be a 4px-wide one.
  if (n > perPage) {
    const int trackX = L.w - L.gut() + 2;
    const int trackW = 4;
    const int trackY = listTop;
    const int trackH = listBot - listTop;
    fb.fillRoundRect(trackX, trackY, trackW, trackH, 2, kLine);
    // Floor the thumb so a very long list still leaves something grabbable to
    // look at rather than a single pixel.
    int thumbH = trackH * perPage / n;
    if (thumbH < 18) thumbH = 18;
    const int span = n - perPage;
    const int thumbY = trackY + (span > 0 ? (trackH - thumbH) * first / span : 0);
    fb.fillRoundRect(trackX, thumbY, trackW, thumbH, 2, kTeal);
  }

  // The pager lives in the HEADER, in the spare space between the Back target and
  // the gear. Below the rows it cost a whole row of list - the reason the menu
  // only showed two entries at a time - and the header had 220px doing nothing.
  //
  // ⚠ Still must not overlap anything: hit() is last-wins, so a target sitting on
  // another would silently scroll instead of activating what it looks like. These
  // sit strictly between Back (ends at 56) and the gear (starts at L.w-44), are
  // drawn as visible arrows, and exist only when there is somewhere to scroll to.
  if (n > perPage) {
    const int pagerW = L.minTap;
    const int upX = L.w - L.minTap - 2 * pagerW - 8;
    const int dnX = L.w - L.minTap - pagerW - 4;
    if (first > 0) {
      iconChevronUp(fb, upX + pagerW / 2, L.headerH / 2, kInk2);
      push(r, upX, 0, pagerW, L.headerH, TapRegion::Action::ScrollUp);
    }
    if (first + perPage < n) {
      iconChevronDown(fb, dnX + pagerW / 2, L.headerH / 2, kInk2);
      push(r, dnX, 0, pagerW, L.headerH, TapRegion::Action::ScrollDown);
    }
  }
}

// ---- text-ish screens -------------------------------------------------------

// The text style for a card body: glyph scale + colour. Bundled so drawTextCard
// stays within the argument budget now that it also carries the panel Layout.
struct TextStyle { int scale = 2; uint16_t colour = kInk; };

// Word-wrapped body text inside a card. Used by Ask, SessionDetail, JobDetail,
// SetupInfo and Pairing, which differ only in what they say.
int drawTextCard(Fb565& fb, const Layout& L, int y, const std::string& body,
                 TextStyle st = {}, int width = 0) {
  const int scale = st.scale;
  const uint16_t colour = st.colour;
  const int cardW = width > 0 ? width : L.w - 2 * L.gut();
  const int innerW = cardW - 2 * 12;
  const int lineH = fb.textHeight(scale) + 6;
  const int perLine = std::max(1, innerW / (6 * scale));

  // Greedy word wrap.
  std::vector<std::string> lines;
  std::string cur;
  std::string word;
  auto flushWord = [&]() {
    if (word.empty()) return;
    // ⚠ Break an OVER-LONG single word on character boundaries. Greedy word wrap
    // alone cannot split a token with no spaces in it, and the sign-in address is
    // exactly that ("http://.../?t=<24 hex>") - it ran straight off its card and
    // underneath the QR beside it.
    while (int(word.size()) > perLine) {
      if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
      lines.push_back(word.substr(0, size_t(perLine)));
      word.erase(0, size_t(perLine));
    }
    if (word.empty()) return;
    if (cur.empty()) cur = word;
    else if (int(cur.size() + 1 + word.size()) <= perLine) cur += " " + word;
    else { lines.push_back(cur); cur = word; }
    word.clear();
  };
  for (char c : asciiSanitize(body)) {
    if (c == '\n') { flushWord(); lines.push_back(cur); cur.clear(); }
    else if (c == ' ') flushWord();
    else word.push_back(c);
  }
  flushWord();
  if (!cur.empty()) lines.push_back(cur);

  const int maxLines = std::max(1, (L.h - L.gut() - y - 24) / lineH);
  const int shown = std::min<int>(int(lines.size()), maxLines);
  const int cardH = shown * lineH + 24;
  fb.card(L.gut(), y, cardW, cardH);
  for (int i = 0; i < shown; i++)
    fb.text(L.gut() + 12, y + 12 + i * lineH, lines[size_t(i)], colour, scale);
  return y + cardH;
}

void drawSessionDetail(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, ctx.sessionIsRoot ? "Nimbus" : "Session", true);
  int y = L.bodyTop();

  const std::string title = ctx.sessionTitle.empty()
      ? (ctx.sessionIsRoot ? std::string("Orchestrator") : std::string("session"))
      : ctx.sessionTitle;
  fb.card(L.gut(), y, L.w - 2 * L.gut(), 76);
  fb.textClipped(L.gut() + 12, y + 12, title, kInk, L.w - 2 * L.gut() - 24, 2);
  if (!ctx.sessionProvider.empty())
    fb.label(L.gut() + 12, y + 34, ctx.sessionProvider, kInk3);
  if (!ctx.sessionState.empty()) {
    const StatusTone tone = ctx.sessionState == "done"    ? StatusTone::Done
                          : ctx.sessionState == "error"   ? StatusTone::Error
                          : ctx.sessionState == "running" ? StatusTone::Working
                                                          : StatusTone::Neutral;
    fb.chip(L.gut() + 12, y + 48, ctx.sessionState, colourFor(tone));
  }
  y += 84;

  if (!ctx.askText.empty()) y = drawTextCard(fb, L, y, ctx.askText, {1, kInk2}) + 8;

  const int my = L.h - L.gut() - L.minTap;
  fb.fillRoundRect(L.gut(), my, L.w - 2 * L.gut(), L.minTap, L.cardRadius, kTeal);
  iconMic(fb, L.gut() + 26, my + L.minTap / 2, kBg);
  fb.text(L.gut() + 48, my + (L.minTap - fb.textHeight(2)) / 2, "Hold to talk", kBg, 2);
  push(r, L.gut(), my, L.w - 2 * L.gut(), L.minTap, TapRegion::Action::Mic);
}

// Ask-screen text-card geometry (perLine chars, maxLines rows, lineH px), shared
// between drawAsk (which needs it to actually draw) and the standalone
// askPageCount() below (which needs it to report a page count a touch board's
// swipe-to-page can clamp against) - so the two can never disagree about how
// much text fits. Fixed geometry (L.bodyTop(), scale 2, full width): Ask has one
// call site. Always reserves room for the "page N/M" indicator line + a gap
// above the Close button, whether or not this particular reply needs it, so the
// card height (and the button's position) stay stable across single- and
// multi-page replies.
struct AskGeom { int perLine; int maxLines; int lineH; };
AskGeom askGeometry(const Layout& L) {
  constexpr int scale = 2;
  const int y = L.bodyTop();
  const int cardW = L.w - 2 * L.gut();
  const int innerW = cardW - 2 * 12;
  const int lineH = Fb565::textHeight(scale) + 6;
  const int perLine = std::max(1, innerW / (6 * scale));
  constexpr int kFooterGap = 6, kIndicatorH = 14;
  const int textBottom = (L.h - L.gut() - L.minTap) - kFooterGap - kIndicatorH;
  const int maxLines = std::max(1, (textBottom - y - 24) / lineH);
  return {perLine, maxLines, lineH};
}

// Ask: title + the wrapped body, PAGINATED - a long reply used to silently
// truncate to whatever fit one card, with no indication there was more (the
// "message is cut off, no way to see the rest" report). ctx.detailPage selects
// the page; a footer shows "page i/N" when there is more. On a knob board the
// encoder pages it (main.cpp); on a touch board a swipe does (also main.cpp) -
// this renderer only draws whatever page it's asked for.
void drawAsk(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, "Message", true);
  const std::string body = ctx.askText.empty() ? "(no message)" : ctx.askText;
  const AskGeom g = askGeometry(L);
  TextPager pager;
  pager.setText(body, size_t(g.perLine), size_t(g.maxLines));
  const size_t pages = pager.pageCount();
  size_t idx = ctx.detailPage < 0 ? 0 : size_t(ctx.detailPage);
  if (pages && idx >= pages) idx = pages - 1;
  const std::vector<std::string> lines = pager.page(idx);

  const int cardH = g.maxLines * g.lineH + 24;
  fb.card(L.gut(), L.bodyTop(), L.w - 2 * L.gut(), cardH);
  for (size_t i = 0; i < lines.size(); ++i)
    fb.text(L.gut() + 12, L.bodyTop() + 12 + int(i) * g.lineH, lines[i], kInk, 2);

  if (pages > 1) {
    char foot[32];
    std::snprintf(foot, sizeof foot, "page %u/%u", unsigned(idx + 1), unsigned(pages));
    fb.text((L.w - fb.textWidth(foot, 1)) / 2, L.bodyTop() + cardH + 6, foot, kInk3, 1);
  }

  // A transient pre-reboot notice (mode switch) restarts the device on its own,
  // so it draws NO Close button: the control could never be used before the reset
  // and read as dead (owner: "switching to notifier mode always shows a button
  // saying close, it does nothing"). A real reply keeps Close as its dismiss exit.
  if (!ctx.askClosable) return;
  const int by = L.h - L.gut() - L.minTap;
  fb.fillRoundRect(L.gut(), by, L.w - 2 * L.gut(), L.minTap, L.cardRadius, kRaise2);
  fb.roundRect(L.gut(), by, L.w - 2 * L.gut(), L.minTap, L.cardRadius, kLine2);
  fb.text((L.w - fb.textWidth("Close", 2)) / 2, by + (L.minTap - fb.textHeight(2)) / 2,
          "Close", kInk, 2);
  push(r, L.gut(), by, L.w - 2 * L.gut(), L.minTap, TapRegion::Action::Home);
}

void drawVoice(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, "Voice", false);
  const char* what = ctx.voice == attn::VoiceStage::Recording  ? "Listening"
                   : ctx.voice == attn::VoiceStage::Processing ? "Transcribing"
                   : ctx.voice == attn::VoiceStage::Speaking   ? "Speaking"
                                                               : "Ready";
  // Recording breathes red on the ring; the screen uses the same colour so the
  // two channels agree about what is happening.
  const uint16_t tone = ctx.voice == attn::VoiceStage::Recording ? kCrit : kTeal;
  const int cy = L.h / 2 - 40;
  fb.fillRoundRect(L.w / 2 - 44, cy, 88, 88, 44, tintFor(tone, kBg));
  iconMic(fb, L.w / 2, cy + 44, tone);
  fb.text((L.w - fb.textWidth(what, 2)) / 2, cy + 108, what, kInk, 2);
}

void drawSelfTest(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, "Self-test", true);
  int y = L.bodyTop();
  if (!ctx.selfTestSummary.empty()) {
    fb.label(L.gut(), y, ctx.selfTestSummary, kInk3);
    y += 16;
  }
  const int rowH = 22;
  const int maxRows = std::max(1, (L.h - L.gut() - y) / rowH);
  const int shown = std::min<int>(int(ctx.selfTest.size()), maxRows);
  for (int i = 0; i < shown; i++) {
    const auto& row = ctx.selfTest[size_t(i)];
    const uint16_t c = row.status == 0 ? kOk : row.status == 1 ? kCrit : kInk3;
    const char* mark = row.status == 0 ? "ok" : row.status == 1 ? "fail" : "skip";
    fb.textClipped(L.gut(), y + 4, row.name, kInk2, L.w - 2 * L.gut() - 46, 1);
    fb.text(L.w - L.gut() - fb.textWidth(mark, 1), y + 4, mark, c, 1);
    fb.hline(L.gut(), y + rowH - 3, L.w - 2 * L.gut(), kLine);
    y += rowH;
  }
}

void drawBattery(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, "Battery", true);
  const int y = L.bodyTop();
  fb.card(L.gut(), y, L.w - 2 * L.gut(), 120);

  const uint8_t pct = ctx.battery.valid ? ctx.battery.percent : 0;
  const uint16_t c = pct <= 10 ? kCrit : pct <= 25 ? kWarn : kOk;
  const std::string big = std::to_string(pct) + "%";
  fb.text((L.w - fb.textWidth(big, 3)) / 2, y + 18, big, c, 3);

  // Level bar
  const int bx = L.gut() + 16, bw = L.w - 2 * L.gut() - 32;
  fb.fillRoundRect(bx, y + 58, bw, 12, 6, kRaise3);
  if (pct > 0) fb.fillRoundRect(bx, y + 58, (bw * pct) / 100, 12, 6, c);

  if (!ctx.battChargeState.empty())
    fb.chip(bx, y + 80, ctx.battChargeState, kInk2);
  if (ctx.battMinutesToEmpty >= 0) {
    const std::string left = std::to_string(ctx.battMinutesToEmpty / 60) + "h " +
                             std::to_string(ctx.battMinutesToEmpty % 60) + "m left";
    fb.text(L.w - L.gut() - 16 - fb.textWidth(left, 1), y + 84, left, kInk3, 1);
  }
}

// The QR itself: a WHITE card with dark modules and a real quiet zone, because a
// scanner needs both the contrast and the margin. Returns the width consumed (0
// if nothing was drawn), so the caller can lay the text out beside it.
int drawQr(Fb565& fb, const Layout& L, const std::string& url, int y, int maxBox) {
  if (url.empty()) return 0;
  nimbus::qr::QrCode q;
  if (!nimbus::qr::encode(url, q) || q.size <= 0) return 0;   // too long for v6-M

  // ISO/IEC 18004 requires a four-module quiet zone. Three decoded from a
  // framebuffer in ideal conditions but was unreliable through a phone camera
  // on the actual glossy panel; keep the full scanner margin.
  constexpr int quiet = 4;
  const int mod = std::max(1, (maxBox - 4) / (q.size + 2 * quiet));
  const int box = mod * (q.size + 2 * quiet);
  const int bx = L.w - L.gut() - box;
  fb.fillRoundRect(bx, y, box, box, 6, 0xFFFF);  // white, not kInk: needs max contrast
  for (int my = 0; my < q.size; my++)
    for (int mx = 0; mx < q.size; mx++)
      if (q.module(mx, my))
        fb.fillRect(bx + (mx + quiet) * mod, y + (my + quiet) * mod, mod, mod, 0x0000);
  return box;
}

std::string displayUrl(const std::string& url) {
  const size_t query = url.find('?');
  return query == std::string::npos ? url : url.substr(0, query);
}

void drawSetup(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx, bool config) {
  // Notifier connects over Bluetooth (the nsn broker), not Wi-Fi - it never runs
  // the radio. So its "not connected" screen must NOT tell the owner to join a
  // setup Wi-Fi network that does not exist; it points at the broker instead.
  if (!config && ctx.modeName && std::string(ctx.modeName) == "notifier") {
    drawHeader(fb, L, r, ctx, "Setup", true);
    int y = L.bodyTop();
    y = drawTextCard(fb, L, y,
        "Waiting for a Bluetooth connection.\n\nOn your computer, run the "
        "nimbus-notify broker - it finds this device automatically.",
        {1, kInk2}, 0) + 10;
    fb.label(L.gut(), y, "device", kInk3);
    y += 14;
    drawTextCard(fb, L, y, ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName,
                 {1, kTeal}, 0);
    if (!ctx.fwVersion.empty())
      fb.text(L.gut(), L.h - fb.textHeight(1) - 1, ctx.fwVersion, kInk3, 1);
    return;
  }
  drawHeader(fb, L, r, ctx, config ? "Sign in" : "Setup", true);
  int y = L.bodyTop();
  const std::string body = config
      ? (ctx.netStatus.empty() ? std::string("Scan to open settings. You are signed in automatically.")
                               : ctx.netStatus)
      : ("Scan to join " + (ctx.apName.empty() ? std::string("the setup hotspot")
                                               : ctx.apName) +
         ". Setup then opens on your phone; nothing to type.");

  // Setup (pre-join) encodes a Wi-Fi JOIN code - a phone camera joins the setup
  // network directly, per-device password included, and the captive portal takes
  // over. Sign-in (config) keeps the token-bearing settings URL. The passphrase
  // is also printed below for anyone joining by hand: this screen is the only
  // place the owner can learn it.
  const std::string url = config
      ? ctx.configUrl
      : (!ctx.apName.empty() ? nimbus::identity::wifiQrPayload(ctx.apName, ctx.apPass)
                             : (ctx.setupUrl.empty() ? ctx.configUrl : ctx.setupUrl));

  // ⚠ The copy says "Scan to open the settings page" - so there had better be
  // something to scan. There was NOT: this screen drew the URL as text only, on
  // both panels' worth of empty space, which made the instruction a lie and left
  // the owner to type a 24-character token by hand. Landscape has the room for
  // the QR beside the text, which is the whole point of the wider surface.
  //
  // The QR is bounded by the body HEIGHT and, so a readable text column always
  // survives beside it, by a WIDTH cap too. On a near-square panel (freenove-40)
  // the height bound alone let the QR eat nearly the full width and starved the
  // text to a few pixels; the width cap keeps the reflow legible. On 320x240 the
  // height bound is the binding one, so this cap leaves that panel byte-identical.
  constexpr int kQrTextMin = 108;   // min px kept for the text column beside the QR
  const int qrBox = std::min(L.h - L.bodyTop() - L.gut(),
                             L.w - 2 * L.gut() - 10 - kQrTextMin);
  const int qrW = drawQr(fb, L, url, y, qrBox);
  const int textW = qrW ? (L.w - 2 * L.gut() - qrW - 10) : 0;

  y = drawTextCard(fb, L, y, body, {1, kInk2}, textW) + 8;
  if (!url.empty()) {
    const bool showPass = !config && !ctx.apPass.empty();
    fb.label(L.gut(), y, config ? "QR includes sign-in"
                             : (showPass ? "network password" : "setup address"),
             kInk3);
    y += 14;
    y = drawTextCard(fb, L, y,
                     config ? std::string("Nothing to type.")
                            : (showPass ? ctx.apPass : displayUrl(ctx.setupUrl)),
                     {1, kTeal}, textW);
  }
  // "Show code" affordance (Sign-in QR only): the escape hatch for a camera
  // that cannot scan. Tapping it opens the full device sign-in code (TokenDetail)
  // to read and type by hand. It lives in the text column so it never collides
  // with the QR on the right, is bottom-anchored above the fw-version line, and
  // is at least minTap tall for the a11y tap floor. (CUM-48 #3)
  if (config) {
    const int bh = L.minTap;
    const int bw = textW ? textW : (L.w - 2 * L.gut());
    const int bx = L.gut();
    int by = L.h - L.gut() - bh - (fb.textHeight(1) + 3);   // clear the fw-version line
    if (by < y + 6) by = y + 6;                             // never overlap the caption above
    fb.card(bx, by, bw, bh);
    fb.text(bx + 10, by + (bh - fb.textHeight(1)) / 2, "Show code", kTeal, 1);
    push(r, bx, by, bw, bh, TapRegion::Action::ShowCode);
  }
  // Firmware version, small in the bottom-left. Guarded so the golden fixture
  // (empty fwVersion) draws nothing and stays byte-identical.
  if (!ctx.fwVersion.empty())
    fb.text(L.gut(), L.h - fb.textHeight(1) - 1, ctx.fwVersion, kInk3, 1);
}

void drawTokenDetail(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, "Device sign-in code", true);   // canonical (CUM-45)
  const int y = L.bodyTop();
  fb.text(L.gut(), y, "Only if you can't scan the Sign-in QR.", kInk3, 1);

  const int cardY = y + 24;
  const int cardH = 116;
  fb.card(L.gut(), cardY, L.w - 2 * L.gut(), cardH);
  fb.label(L.gut() + 14, cardY + 12, "sign-in code", kInk3);

  std::string token = asciiSanitize(ctx.webToken.empty() ? std::string("-")
                                                          : ctx.webToken);
  // A real token is 24 hex characters. Two exact 12-character lines are large
  // enough to transcribe and cannot clip. Keep the loop general so a future
  // token-length change still shows every character (up to the card's 3 lines).
  constexpr size_t kCharsPerLine = 12;
  int line = 0;
  for (size_t at = 0; at < token.size() && line < 3; at += kCharsPerLine, ++line) {
    const std::string part = token.substr(at, kCharsPerLine);
    const int tx = (L.w - fb.textWidth(part, 2)) / 2;
    fb.text(tx, cardY + 38 + line * 24, part, kTeal, 2);
  }
  fb.text(L.gut() + 14, cardY + cardH - 18, "Join the lines exactly.", kInk3, 1);
}

void drawPairing(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, L, r, ctx, "Pairing", false);
  const int y = L.bodyTop() + 20;
  fb.card(L.gut(), y, L.w - 2 * L.gut(), 110);
  fb.label(L.gut() + 14, y + 14, "pairing code", kInk3);
  const std::string code = ctx.pairingCode.empty() ? "------" : ctx.pairingCode;
  fb.text((L.w - fb.textWidth(code, 3)) / 2, y + 44, code, kTeal, 3);
  fb.text((L.w - fb.textWidth("Enter this on your computer", 1)) / 2, y + 84,
          "Enter this on your computer", kInk3, 1);
}

void drawScreensaver(Fb565& fb, const Layout& L, Rendered& r, const ScreenCtx& ctx) {
  // The backlight is blanked separately (that is the real power saving); this
  // is what shows if it is still lit. Any tap wakes.
  fb.clear(kBg);
  const std::string name = ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName;
  const int cy = L.h / 2;
  fb.roundRect(L.w / 2 - 34, cy - 58, 68, 68, 34, kLine2);
  fb.fillRoundRect(L.w / 2 - 8, cy - 32, 16, 16, 8, kTeal);
  fb.text((L.w - fb.textWidth(name, 2)) / 2, cy + 22, name, kInk3, 2);
  push(r, 0, 0, L.w, L.h, TapRegion::Action::Home);
}

}  // namespace

// Real page count drawAsk will produce for this text (declared in
// tft_render/screens.h). Deliberately its own count - the old 48-col grid was
// hardcoded to the old 48-col/7-line grid, which does not match this
// renderer's pixel geometry/font metrics. Needs external linkage (unlike every
// draw* helper above, which is only ever reached via renderScreen's switch) so
// a touch board's swipe-to-page (main.cpp) can clamp against exactly what this
// renderer will draw.
int askPageCount(const std::string& text) {
  // main.cpp drives a single, compiled panel, so the default-size geometry is the
  // one it will actually render (Ask never changes panel mid-run).
  const Layout L = defaultLayout();
  const AskGeom g = askGeometry(L);
  TextPager pager;
  pager.setText(text, size_t(g.perLine), size_t(g.maxLines));
  return int(pager.pageCount());
}

StatusTone toneFor(uint8_t status) {
  // solide::ring::Status - kept in lockstep with statusStyle()'s roles so the
  // ring hue and the card tint always mean the same thing.
  switch (status) {
    case 1: return StatusTone::Working;    // Running
    case 2: return StatusTone::Waiting;    // WaitingInput
    case 3: return StatusTone::Approval;   // AwaitingApproval
    case 4: return StatusTone::Done;
    case 5: return StatusTone::Error;
    case 0:                                // Idle
    case 6:                                // Offline
    default: return StatusTone::Neutral;
  }
}

Rendered renderScreen(Fb565& fb, attn::ScreenId screen, const ScreenCtx& ctx) {
  fb.clear(kBg);
  Rendered r;
  // The layout follows the framebuffer's size, so the SAME renderer draws every
  // supported panel (freenove-28/35/40). At the default size every metric equals
  // the pre-parameterization constant, so the goldens stay byte-identical.
  const Layout L = Layout::forSize(fb.width(), fb.height());

  switch (screen) {
    case attn::ScreenId::Menu:          drawMenu(fb, L, r, ctx); break;
    case attn::ScreenId::SessionDetail: drawSessionDetail(fb, L, r, ctx); break;
    case attn::ScreenId::JobDetail:     drawSessionDetail(fb, L, r, ctx); break;
    case attn::ScreenId::Ask:           drawAsk(fb, L, r, ctx); break;
    case attn::ScreenId::VoiceGlyph:    drawVoice(fb, L, r, ctx); break;
    case attn::ScreenId::SelfTest:      drawSelfTest(fb, L, r, ctx); break;
    case attn::ScreenId::Battery:       drawBattery(fb, L, r, ctx); break;
    case attn::ScreenId::SetupInfo:     drawSetup(fb, L, r, ctx, false); break;
    case attn::ScreenId::ConfigQr:      drawSetup(fb, L, r, ctx, true); break;
    case attn::ScreenId::TokenDetail:   drawTokenDetail(fb, L, r, ctx); break;
    case attn::ScreenId::Pairing:       drawPairing(fb, L, r, ctx); break;
    case attn::ScreenId::Screensaver:   drawScreensaver(fb, L, r, ctx); break;
    case attn::ScreenId::StatusIdle:
    case attn::ScreenId::Badge:
    case attn::ScreenId::IdleArt:
    default:                            drawStatusHome(fb, L, r, ctx); break;
  }
  return r;
}

}  // namespace nimbus::tft
