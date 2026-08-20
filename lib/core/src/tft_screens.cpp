#include "nimbus/tft_render/screens.h"

#include <algorithm>

#include "nimbus/device_identity.h"   // wifiQrPayload - the setup screen's join QR
#include "nimbus/qr.h"

// The colour touch UI. Layout language is lifted from the web interface so the
// two surfaces read as one product: cards are the web's .sec (raised fill, 1px
// line border, rounded), captions are .k (uppercase, letter-spaced, --ink3),
// status chips are .badge (soft-tinted pill, coloured text).
//
// Every interactive element registers a TapRegion as it is drawn, so the hit
// map and the pixels cannot disagree.

namespace nimbus::tft {

namespace {

using epd::ScreenCtx;

constexpr int kGut = kPad;                 // 12px page gutter
constexpr int kBodyTop = kHeaderH + 8;

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
void drawHeader(Fb565& fb, Rendered& r, const ScreenCtx& ctx,
                const std::string& title, bool backable) {
  fb.fillRect(0, 0, kW, kHeaderH, kRaise);
  fb.hline(0, kHeaderH - 1, kW, kLine);

  int tx = kGut;
  if (backable) {
    iconChevronLeft(fb, kGut + 6, kHeaderH / 2, kTeal);
    // The tap target is deliberately much larger than the glyph - the whole
    // left end of the bar goes Back, which is the easiest target to hit.
    push(r, 0, 0, 56, kHeaderH, TapRegion::Action::Back);
    tx = kGut + 22;
  } else {
    push(r, 0, 0, 100, kHeaderH, TapRegion::Action::Home);
  }

  std::string name = title.empty()
      ? (ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName)
      : title;
  // Drop a trailing "  vX.Y.Z": the menu breadcrumb carries it for the wider
  // e-ink layout, and here it only pushed the actual title into an ellipsis
  // ("Settings .."). The version is on the Software update row and in /api/state.
  {
    const size_t v = name.rfind("  v");
    if (v != std::string::npos && v + 3 < name.size() &&
        name[v + 3] >= '0' && name[v + 3] <= '9')
      name.erase(v);
  }
  // Clip before the right cluster (battery + gear) rather than at a fixed width,
  // so a long title can never collide with them.
  const int titleMax = (kW - kMinTap - (ctx.battery.valid ? 34 : 4)) - tx;
  // ⚠ A breadcrumb is MOST specific at its tail. Clipping head-first turned
  // "Settings > Customize > Brightness" into "Settings ..", hiding the one word
  // that says what is being edited. Drop leading segments until it fits, so the
  // deepest name always survives.
  while (fb.textWidth(name, 2) > titleMax) {
    const size_t gt = name.find(" > ");
    if (gt == std::string::npos) break;
    name = name.substr(gt + 3);
  }
  fb.textClipped(tx, (kHeaderH - fb.textHeight(2)) / 2, name, kInk, titleMax, 2);

  // Right cluster: the gear is flush to the right edge so its tap target fits
  // on-panel exactly; the battery sits to its left.
  const int gearX = kW - kMinTap;                 // target spans gearX..kW
  iconGear(fb, gearX + kMinTap / 2, kHeaderH / 2, kInk2);
  push(r, gearX, 0, kMinTap, kHeaderH, TapRegion::Action::OpenMenu);

  if (ctx.battery.valid)
    iconBattery(fb, gearX - 30, (kHeaderH - 11) / 2, ctx.battery.percent, kInk2);
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

// Ring-dominant Notifier screen for boards with NO physical LED ring: the ring is
// the whole display. Sessions are the arcs the ring already draws; a small legend
// sits inside it, and orchestrator's hold-to-talk shrinks to a compact bottom pill
// so the ring keeps the space. The ring square is exposed on Rendered so the
// device can push ONLY that rectangle at animation cadence.
static void drawRingHome(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  const bool showMic = ctx.modeName && std::string(ctx.modeName) == "orchestrator";
  const int micH = showMic ? kMinTap : 0;             // >= tap minimum when shown
  const int bodyTop = kBodyTop;
  const int bodyBot = kH - kGut - (showMic ? micH + kGut : 0);
  const int bodyH = bodyBot - bodyTop;

  const int rs = std::min(kW - 2 * kGut, bodyH);      // big square ring
  const int rx = (kW - rs) / 2;
  const int ry = bodyTop + (bodyH - rs) / 2;
  drawRingWidget(fb, rx, ry, rs, ctx.ringLeds);
  r.ringX = int16_t(rx); r.ringY = int16_t(ry);
  r.ringW = int16_t(rs); r.ringH = int16_t(rs);

  const int cx = rx + rs / 2, cy = ry + rs / 2;
  const int inner = rs - 64;                          // keep text inside the dots
  if (ctx.jobs.empty()) {
    const std::string l1 = "Nothing running";
    const std::string l2 = "Waiting for a session";
    fb.text(cx - fb.textWidth(l1, 1) / 2, cy - 8, l1, kInk, 1);
    fb.text(cx - fb.textWidth(l2, 1) / 2, cy + 6, l2, kInk3, 1);
  } else {
    const int fi = (ctx.cursorJob >= 0 && ctx.cursorJob < int(ctx.jobs.size()))
                     ? ctx.cursorJob : 0;
    const auto& j = ctx.jobs[size_t(fi)];
    const StatusTone tone = toneFor(j.status);
    const std::string name = j.label.empty() ? std::string("session") : j.label;
    const std::string state = labelFor(tone);
    const int nameW = std::min(fb.textWidth(name, 1), inner);
    fb.textClipped(cx - nameW / 2, cy - 12, name, kInk, inner, 1);
    fb.label(cx - fb.labelWidth(state) / 2, cy + 2, state, colourFor(tone));
    if (ctx.jobs.size() > 1) {
      char cnt[24];
      std::snprintf(cnt, sizeof cnt, "%d active", int(ctx.jobs.size()));
      fb.text(cx - fb.textWidth(cnt, 1) / 2, cy + 16, cnt, kInk3, 1);
    }
  }

  if (showMic) {
    const int pw = 168, ph = micH;
    const int px = (kW - pw) / 2, py = kH - kGut - ph;
    fb.fillRoundRect(px, py, pw, ph, ph / 2, kTeal);
    iconMic(fb, px + 26, py + ph / 2, kBg);
    fb.text(px + 46, py + (ph - fb.textHeight(1)) / 2, "Hold to talk", kBg, 1);
    push(r, px, py, pw, ph, TapRegion::Action::Mic);
  }
}

void drawStatusHome(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, "", false);

  // Ringless board: the ring IS the display, so use the dedicated ring-dominant
  // layout. buildCtx fills ringLeds only on a board with no physical ring, so a
  // ring board falls straight through to the original card layout (goldens hold).
  if (!ctx.ringLeds.empty()) { drawRingHome(fb, r, ctx); return; }
  const int bodyRight = kW;

  // ⚠ The mic is ORCHESTRATOR-ONLY. Hold-to-talk records, transcribes and sends a
  // TURN; in Notifier mode there is no orchestrator running to send it to, so the
  // control cannot work - and a button that does nothing is worse than no button,
  // because the owner cannot tell it apart from a broken one.
  // Notifier mode gets the space back for session cards, which is what that mode
  // is actually for.
  const bool showMic = ctx.modeName && std::string(ctx.modeName) == "orchestrator";
  const int micH = showMic ? 44 : 0;   // >= kMinTap when shown
  const int gridTop = kBodyTop;
  const int gridBot = kH - kGut - (showMic ? micH + 8 : 0);

  if (ctx.jobs.empty()) {
    // Empty state: say what the device is doing, not "no data".
    fb.card(kGut, gridTop, bodyRight - 2 * kGut, 84);
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
    const int cardW = (bodyRight - 2 * kGut - gap * (cols - 1)) / cols;
    constexpr int cardH = 56;
    const int maxRows = std::max(1, (gridBot - gridTop + gap) / (cardH + gap));
    const int shown = std::min<int>(int(ctx.jobs.size()), maxRows * cols);

    for (int i = 0; i < shown; i++) {
      const int cx = kGut + (i % cols) * (cardW + gap);
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
  const int my = kH - kGut - micH;
  fb.fillRoundRect(kGut, my, kW - 2 * kGut, micH, kCardRadius, kTeal);
  iconMic(fb, kGut + 30, my + micH / 2, kBg);
  fb.text(kGut + 52, my + (micH - fb.textHeight(2)) / 2, "Hold to talk", kBg, 2);
  push(r, kGut, my, kW - 2 * kGut, micH, TapRegion::Action::Mic);
}

// ---- menu / settings --------------------------------------------------------

void drawMenu(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, ctx.menuTitle.empty() ? "Settings" : ctx.menuTitle, true);

  const int n = int(ctx.menuItems.size());
  if (n == 0) return;

  // When a value is being adjusted the selected row becomes a stepper - the
  // touch replacement for turning the knob.
  if (ctx.menuAdjusting && ctx.menuSelected >= 0 && ctx.menuSelected < n) {
    // The FSM wraps the captured value in "< ... >" as the E-INK cursor's
    // adjust affordance. Here there are real [-] and [+] buttons directly
    // beneath it, so the arrows are noise that competes with them.
    std::string row = ctx.menuItems[size_t(ctx.menuSelected)];
    if (row.size() > 4 && row.front() == '<' && row.back() == '>') {
      row = row.substr(1, row.size() - 2);
      while (!row.empty() && row.front() == ' ') row.erase(0, 1);
      while (!row.empty() && row.back() == ' ') row.pop_back();
    }
    const int cy = kBodyTop + 20;
    // Landscape is 80px shorter than portrait, so this screen is laid out from
    // BOTH ends: the value card hangs off the top, Save is anchored to the bottom
    // (the same idiom as the status screen's mic bar). Offsetting Save from the
    // card is what ran it off the panel twice.
    fb.card(kGut, cy, kW - 2 * kGut, 96);
    // Centre it: the value is the subject of this screen, and the buttons that
    // change it are centred under it.
    //
    // ⚠ AUTO-FIT the scale. A fixed scale 3 is wider than the panel for a long
    // row like "Screensaver  60 min", and centring then computes a NEGATIVE x -
    // so the text was clipped off BOTH edges and unreadable. Row text comes from
    // the menu FSM and can be any length, so pick the largest scale that fits
    // rather than assuming one.
    const int rowMaxW = kW - 2 * kGut - 24;
    int rowScale = 3;
    while (rowScale > 1 && fb.textWidth(row, rowScale) > rowMaxW) rowScale--;
    const int rowW = fb.textWidth(row, rowScale);
    const int rowX = rowW <= rowMaxW ? (kW - rowW) / 2 : kGut + 12;
    fb.textClipped(rowX, cy + 10, row, kInk, rowMaxW, rowScale);

    constexpr int btn = 56;
    const int by = cy + 32;
    // minus
    fb.fillRoundRect(kGut + 12, by, btn, btn, 10, kRaise2);
    fb.roundRect(kGut + 12, by, btn, btn, 10, kLine2);
    fb.fillRect(kGut + 12 + btn / 2 - 10, by + btn / 2 - 1, 20, 3, kInk);
    push(r, kGut + 12, by, btn, btn, TapRegion::Action::ValueDown);
    // plus
    const int px = kW - kGut - 12 - btn;
    fb.fillRoundRect(px, by, btn, btn, 10, kRaise2);
    fb.roundRect(px, by, btn, btn, 10, kLine2);
    fb.fillRect(px + btn / 2 - 10, by + btn / 2 - 1, 20, 3, kInk);
    fb.fillRect(px + btn / 2 - 1, by + btn / 2 - 10, 3, 20, kInk);
    push(r, px, by, btn, btn, TapRegion::Action::ValueUp);

    if (!ctx.menuHelp.empty())
      fb.textClipped(kGut + 12, cy + 100, ctx.menuHelp, kInk3, kW - 2 * kGut - 24, 1);

    // Save
    const int sy = kH - kGut - kMinTap;   // bottom-anchored, never off-panel
    fb.fillRoundRect(kGut, sy, kW - 2 * kGut, kMinTap, kCardRadius, kTeal);
    fb.text((kW - fb.textWidth("Save", 2)) / 2, sy + (kMinTap - fb.textHeight(2)) / 2,
            "Save", kBg, 2);
    push(r, kGut, sy, kW - 2 * kGut, kMinTap, TapRegion::Action::Commit);
    return;
  }

  // Row list. Scrolls around the selection so the cursor is always on screen.
  const int listTop = kBodyTop;
  const int listBot = kH - kGut;
  // TWO COLUMNS on the landscape panel. Measured before changing anything: the
  // single-column layout drew just 2 rows per page, so the 13-row Settings menu
  // took SEVEN pages to walk - for a screen whose whole job is to be the control
  // surface when the web UI is unreachable. Two columns of three is one page in
  // three, and 320px of width is exactly what makes it possible.
  constexpr int cols = 2;
  constexpr int colGap = 8;
  const int colW = (kW - 2 * kGut - colGap * (cols - 1)) / cols;
  const int rowsPerCol = std::max(1, (listBot - listTop) / (kRowH + 4));
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
    const int x = kGut + (slot / rowsPerCol) * (colW + colGap);
    const int y = listTop + (slot % rowsPerCol) * (kRowH + 4);
    const bool sel = (i == ctx.menuSelected);
    fb.card(x, y, colW, kRowH, sel ? kRaise2 : kRaise,
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
    // The FSM decorates rows for the E-INK cursor: "[ 30 ]" marks the value row
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
      fb.textClipped(x + 12, y + (kRowH - fb.textHeight(labelScale)) / 2, label,
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
    if (back)         iconChevronLeft(fb, chevX, y + kRowH / 2, kInk3);
    else if (forward) iconChevronRight(fb, chevX, y + kRowH / 2, kInk3);
    push(r, x, y, colW, kRowH, TapRegion::Action::MenuRow, i);
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
  // Sits at x = kW - kGut + 2, entirely inside the gutter, so it cannot clip the
  // right-hand column's chevron. No tap region: it is an indicator, not a
  // control - the pager and swipe both already scroll, and a third target here
  // would be a 4px-wide one.
  if (n > perPage) {
    const int trackX = kW - kGut + 2;
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
  // sit strictly between Back (ends at 56) and the gear (starts at kW-44), are
  // drawn as visible arrows, and exist only when there is somewhere to scroll to.
  if (n > perPage) {
    const int pagerW = kMinTap;
    const int upX = kW - kMinTap - 2 * pagerW - 8;
    const int dnX = kW - kMinTap - pagerW - 4;
    if (first > 0) {
      iconChevronUp(fb, upX + pagerW / 2, kHeaderH / 2, kInk2);
      push(r, upX, 0, pagerW, kHeaderH, TapRegion::Action::ScrollUp);
    }
    if (first + perPage < n) {
      iconChevronDown(fb, dnX + pagerW / 2, kHeaderH / 2, kInk2);
      push(r, dnX, 0, pagerW, kHeaderH, TapRegion::Action::ScrollDown);
    }
  }
}

// ---- text-ish screens -------------------------------------------------------

// Word-wrapped body text inside a card. Used by Ask, SessionDetail, JobDetail,
// SetupInfo and Pairing, which differ only in what they say.
int drawTextCard(Fb565& fb, int y, const std::string& body, int scale = 2,
                 uint16_t colour = kInk, int width = 0) {
  const int cardW = width > 0 ? width : kW - 2 * kGut;
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

  const int maxLines = std::max(1, (kH - kGut - y - 24) / lineH);
  const int shown = std::min<int>(int(lines.size()), maxLines);
  const int cardH = shown * lineH + 24;
  fb.card(kGut, y, cardW, cardH);
  for (int i = 0; i < shown; i++)
    fb.text(kGut + 12, y + 12 + i * lineH, lines[size_t(i)], colour, scale);
  return y + cardH;
}

void drawSessionDetail(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, ctx.sessionIsRoot ? "Nimbus" : "Session", true);
  int y = kBodyTop;

  const std::string title = ctx.sessionTitle.empty()
      ? (ctx.sessionIsRoot ? std::string("Orchestrator") : std::string("session"))
      : ctx.sessionTitle;
  fb.card(kGut, y, kW - 2 * kGut, 76);
  fb.textClipped(kGut + 12, y + 12, title, kInk, kW - 2 * kGut - 24, 2);
  if (!ctx.sessionProvider.empty())
    fb.label(kGut + 12, y + 34, ctx.sessionProvider, kInk3);
  if (!ctx.sessionState.empty()) {
    const StatusTone tone = ctx.sessionState == "done"    ? StatusTone::Done
                          : ctx.sessionState == "error"   ? StatusTone::Error
                          : ctx.sessionState == "running" ? StatusTone::Working
                                                          : StatusTone::Neutral;
    fb.chip(kGut + 12, y + 48, ctx.sessionState, colourFor(tone));
  }
  y += 84;

  if (!ctx.askText.empty()) y = drawTextCard(fb, y, ctx.askText, 1, kInk2) + 8;

  const int my = kH - kGut - kMinTap;
  fb.fillRoundRect(kGut, my, kW - 2 * kGut, kMinTap, kCardRadius, kTeal);
  iconMic(fb, kGut + 26, my + kMinTap / 2, kBg);
  fb.text(kGut + 48, my + (kMinTap - fb.textHeight(2)) / 2, "Hold to talk", kBg, 2);
  push(r, kGut, my, kW - 2 * kGut, kMinTap, TapRegion::Action::Mic);
}

void drawAsk(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, "Message", true);
  drawTextCard(fb, kBodyTop, ctx.askText.empty() ? "(no message)" : ctx.askText, 2);
  const int by = kH - kGut - kMinTap;
  fb.fillRoundRect(kGut, by, kW - 2 * kGut, kMinTap, kCardRadius, kRaise2);
  fb.roundRect(kGut, by, kW - 2 * kGut, kMinTap, kCardRadius, kLine2);
  fb.text((kW - fb.textWidth("Close", 2)) / 2, by + (kMinTap - fb.textHeight(2)) / 2,
          "Close", kInk, 2);
  push(r, kGut, by, kW - 2 * kGut, kMinTap, TapRegion::Action::Home);
}

void drawVoice(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, "Voice", false);
  const char* what = ctx.voice == attn::VoiceStage::Recording  ? "Listening"
                   : ctx.voice == attn::VoiceStage::Processing ? "Transcribing"
                   : ctx.voice == attn::VoiceStage::Speaking   ? "Speaking"
                                                               : "Ready";
  // Recording breathes red on the ring; the screen uses the same colour so the
  // two channels agree about what is happening.
  const uint16_t tone = ctx.voice == attn::VoiceStage::Recording ? kCrit : kTeal;
  const int cy = kH / 2 - 40;
  fb.fillRoundRect(kW / 2 - 44, cy, 88, 88, 44, tintFor(tone, kBg));
  iconMic(fb, kW / 2, cy + 44, tone);
  fb.text((kW - fb.textWidth(what, 2)) / 2, cy + 108, what, kInk, 2);
}

void drawSelfTest(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, "Self-test", true);
  int y = kBodyTop;
  if (!ctx.selfTestSummary.empty()) {
    fb.label(kGut, y, ctx.selfTestSummary, kInk3);
    y += 16;
  }
  const int rowH = 22;
  const int maxRows = std::max(1, (kH - kGut - y) / rowH);
  const int shown = std::min<int>(int(ctx.selfTest.size()), maxRows);
  for (int i = 0; i < shown; i++) {
    const auto& row = ctx.selfTest[size_t(i)];
    const uint16_t c = row.status == 0 ? kOk : row.status == 1 ? kCrit : kInk3;
    const char* mark = row.status == 0 ? "ok" : row.status == 1 ? "fail" : "skip";
    fb.textClipped(kGut, y + 4, row.name, kInk2, kW - 2 * kGut - 46, 1);
    fb.text(kW - kGut - fb.textWidth(mark, 1), y + 4, mark, c, 1);
    fb.hline(kGut, y + rowH - 3, kW - 2 * kGut, kLine);
    y += rowH;
  }
}

void drawBattery(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, "Battery", true);
  const int y = kBodyTop;
  fb.card(kGut, y, kW - 2 * kGut, 120);

  const uint8_t pct = ctx.battery.valid ? ctx.battery.percent : 0;
  const uint16_t c = pct <= 10 ? kCrit : pct <= 25 ? kWarn : kOk;
  const std::string big = std::to_string(pct) + "%";
  fb.text((kW - fb.textWidth(big, 3)) / 2, y + 18, big, c, 3);

  // Level bar
  const int bx = kGut + 16, bw = kW - 2 * kGut - 32;
  fb.fillRoundRect(bx, y + 58, bw, 12, 6, kRaise3);
  if (pct > 0) fb.fillRoundRect(bx, y + 58, (bw * pct) / 100, 12, 6, c);

  if (!ctx.battChargeState.empty())
    fb.chip(bx, y + 80, ctx.battChargeState, kInk2);
  if (ctx.battMinutesToEmpty >= 0) {
    const std::string left = std::to_string(ctx.battMinutesToEmpty / 60) + "h " +
                             std::to_string(ctx.battMinutesToEmpty % 60) + "m left";
    fb.text(kW - kGut - 16 - fb.textWidth(left, 1), y + 84, left, kInk3, 1);
  }
}

// The QR itself: a WHITE card with dark modules and a real quiet zone, because a
// scanner needs both the contrast and the margin. Returns the width consumed (0
// if nothing was drawn), so the caller can lay the text out beside it.
int drawQr(Fb565& fb, const std::string& url, int y, int maxBox) {
  if (url.empty()) return 0;
  nimbus::qr::QrCode q;
  if (!nimbus::qr::encode(url, q) || q.size <= 0) return 0;   // too long for v6-M

  // ISO/IEC 18004 requires a four-module quiet zone. Three decoded from a
  // framebuffer in ideal conditions but was unreliable through a phone camera
  // on the actual glossy panel; keep the full scanner margin.
  constexpr int quiet = 4;
  const int mod = std::max(1, (maxBox - 4) / (q.size + 2 * quiet));
  const int box = mod * (q.size + 2 * quiet);
  const int bx = kW - kGut - box;
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

void drawSetup(Fb565& fb, Rendered& r, const ScreenCtx& ctx, bool config) {
  // Notifier connects over Bluetooth (the nsn broker), not Wi-Fi - it never runs
  // the radio. So its "not connected" screen must NOT tell the owner to join a
  // setup Wi-Fi network that does not exist; it points at the broker instead.
  if (!config && ctx.modeName && std::string(ctx.modeName) == "notifier") {
    drawHeader(fb, r, ctx, "Setup", true);
    int y = kBodyTop;
    y = drawTextCard(fb, y,
        "Waiting for a Bluetooth connection.\n\nOn your computer, run the "
        "nimbus-notify broker \xE2\x80\x94 it finds this device automatically.",
        1, kInk2, 0) + 10;
    fb.label(kGut, y, "device", kInk3);
    y += 14;
    drawTextCard(fb, y, ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName,
                 1, kTeal, 0);
    return;
  }
  drawHeader(fb, r, ctx, config ? "Sign in" : "Setup", true);
  int y = kBodyTop;
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
  const int qrW = drawQr(fb, url, y, kH - kBodyTop - kGut);
  const int textW = qrW ? (kW - 2 * kGut - qrW - 10) : 0;

  y = drawTextCard(fb, y, body, 1, kInk2, textW) + 8;
  if (!url.empty()) {
    const bool showPass = !config && !ctx.apPass.empty();
    fb.label(kGut, y, config ? "QR includes sign-in"
                             : (showPass ? "network password" : "setup address"),
             kInk3);
    y += 14;
    y = drawTextCard(fb, y,
                     config ? std::string("No token to type.")
                            : (showPass ? ctx.apPass : displayUrl(ctx.setupUrl)),
                     1, kTeal, textW);
  }
}

void drawTokenDetail(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, "Sign-in code", true);
  const int y = kBodyTop;
  fb.text(kGut, y, "Recovery only - scan the QR normally.", kInk3, 1);

  const int cardY = y + 24;
  const int cardH = 116;
  fb.card(kGut, cardY, kW - 2 * kGut, cardH);
  fb.label(kGut + 14, cardY + 12, "full recovery code", kInk3);

  std::string token = asciiSanitize(ctx.webToken.empty() ? std::string("-")
                                                          : ctx.webToken);
  // A real token is 24 hex characters. Two exact 12-character lines are large
  // enough to transcribe and cannot clip. Keep the loop general so a future
  // token-length change still shows every character (up to the card's 3 lines).
  constexpr size_t kCharsPerLine = 12;
  int line = 0;
  for (size_t at = 0; at < token.size() && line < 3; at += kCharsPerLine, ++line) {
    const std::string part = token.substr(at, kCharsPerLine);
    const int tx = (kW - fb.textWidth(part, 2)) / 2;
    fb.text(tx, cardY + 38 + line * 24, part, kTeal, 2);
  }
  fb.text(kGut + 14, cardY + cardH - 18, "Join the lines exactly.", kInk3, 1);
}

void drawPairing(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  drawHeader(fb, r, ctx, "Pairing", false);
  const int y = kBodyTop + 20;
  fb.card(kGut, y, kW - 2 * kGut, 110);
  fb.label(kGut + 14, y + 14, "pairing code", kInk3);
  const std::string code = ctx.pairingCode.empty() ? "------" : ctx.pairingCode;
  fb.text((kW - fb.textWidth(code, 3)) / 2, y + 44, code, kTeal, 3);
  fb.text((kW - fb.textWidth("Enter this on your computer", 1)) / 2, y + 84,
          "Enter this on your computer", kInk3, 1);
}

void drawScreensaver(Fb565& fb, Rendered& r, const ScreenCtx& ctx) {
  // The backlight is blanked separately (that is the real power saving); this
  // is what shows if it is still lit. Any tap wakes.
  fb.clear(kBg);
  const std::string name = ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName;
  const int cy = kH / 2;
  fb.roundRect(kW / 2 - 34, cy - 58, 68, 68, 34, kLine2);
  fb.fillRoundRect(kW / 2 - 8, cy - 32, 16, 16, 8, kTeal);
  fb.text((kW - fb.textWidth(name, 2)) / 2, cy + 22, name, kInk3, 2);
  push(r, 0, 0, kW, kH, TapRegion::Action::Home);
}

}  // namespace

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

  switch (screen) {
    case attn::ScreenId::Menu:          drawMenu(fb, r, ctx); break;
    case attn::ScreenId::SessionDetail: drawSessionDetail(fb, r, ctx); break;
    case attn::ScreenId::JobDetail:     drawSessionDetail(fb, r, ctx); break;
    case attn::ScreenId::Ask:           drawAsk(fb, r, ctx); break;
    case attn::ScreenId::VoiceGlyph:    drawVoice(fb, r, ctx); break;
    case attn::ScreenId::SelfTest:      drawSelfTest(fb, r, ctx); break;
    case attn::ScreenId::Battery:       drawBattery(fb, r, ctx); break;
    case attn::ScreenId::SetupInfo:     drawSetup(fb, r, ctx, false); break;
    case attn::ScreenId::ConfigQr:      drawSetup(fb, r, ctx, true); break;
    case attn::ScreenId::TokenDetail:   drawTokenDetail(fb, r, ctx); break;
    case attn::ScreenId::Pairing:       drawPairing(fb, r, ctx); break;
    case attn::ScreenId::Screensaver:   drawScreensaver(fb, r, ctx); break;
    case attn::ScreenId::StatusIdle:
    case attn::ScreenId::Badge:
    case attn::ScreenId::IdleArt:
    default:                            drawStatusHome(fb, r, ctx); break;
  }
  return r;
}

}  // namespace nimbus::tft
