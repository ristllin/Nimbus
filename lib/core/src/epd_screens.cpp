#include "nimbus/epd_render/screens.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "nimbus/device_identity.h"   // wifiQrPayload - the setup screen's join QR
#include "nimbus/nsn_proto.h"   // nsn::harnessName (v2 session identity)
#include "nimbus/text_page.h"

// Every screen as pure framebuffer math - no clock, no panel, no Arduino.
// Shared conventions (see the header): a 12px header strip (mode name left,
// profile/posture middle, "NN%" right when battery telemetry is valid, plus a
// "!" marker when the network is degraded), a 1px separator on row 12, body
// below. IdleArt is the one exception: the real screen is 3-color and drawn
// device-side from solide's status_art, so here it renders as a labelled
// placeholder frame (renderScreen must never have an unhandled ScreenId).

namespace nimbus::epd {

namespace {

using attn::ScreenId;
using attn::VoiceStage;
using solide::ring::Status;

constexpr int kSepY = 24;  // TWO-LINE header (owner R4): rows 0..23, separator on 24

void drawHeader(Fb& fb, const ScreenCtx& ctx) {
  // TWO descriptive lines (owner R4): every token is a word:value pair anyone can
  // read cold - no bare "Desk"/"Balanced", no cryptic wi+/bt- glyphs.
  //   L1:  <device> <Mode>                      wifi:on bt:off [!] [+75%]
  //   L2:  light:Full   sound:med 50%   power:Desk
  auto shortMode = [](const char* m) -> const char* {
    if (m && std::strcmp(m, "orchestrator") == 0) return "Orch";
    if (m && std::strcmp(m, "notifier") == 0)     return "Notif";
    return m ? m : "";
  };
  auto radioWord = [](uint8_t st) -> const char* {
    return st >= 2 ? "on" : (st == 1 ? ".." : "off");   // ".." = searching/advertising
  };
  // L1 right cluster first (it owns its width; the name truncates around it).
  std::string right = std::string("wifi:") + radioWord(ctx.wifiState) +
                      " bt:" + radioWord(ctx.btState);
  if (!ctx.sdShort.empty()) right += " sd:" + ctx.sdShort;
  if (ctx.networkDegraded) right += " !";
  if (ctx.battery.valid) {
    // Voltage-TREND glyphs riding the % (owner 2026-07-16: never claim charging -
    // no charge-detect hardware exists): '^' = voltage rising, '=' = voltage
    // stable/high, none = draining. Same inference bits, honest semantics.
    const char* glyph = ctx.battery.charging ? "^"
                      : (ctx.battery.onExternalPower ? "=" : "");
    char pct[10];
    std::snprintf(pct, sizeof pct, " %s%u%%", glyph, unsigned(ctx.battery.percent));
    right += pct;
  }
  const int rightX = kW - 2 - Fb::textWidth(right);
  std::string name = ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName;
  std::string tag  = std::string(" ") + shortMode(ctx.modeName);
  const int nameBudget = rightX - 8 - Fb::textWidth(tag) - 2;
  while (name.size() > 1 && Fb::textWidth(name) > nameBudget) name.pop_back();
  fb.text(2, 2, name + tag);
  fb.text(rightX, 2, right);

  // L2: the device's sensory state, spelled out.
  // Battery-mode display names track the canonical Dark/Balanced/Full set
  // (AGENTS.md copy style guide) - machine keys arrive on the wire.
  auto friendlyProfile = [](const char* p) -> const char* {
    if (!p) return "";
    if (std::strcmp(p, "battery_saver") == 0) return "Dark";
    if (std::strcmp(p, "balanced") == 0)      return "Balanced";
    if (std::strcmp(p, "desk") == 0)          return "Full";
    return p;
  };
  const char* lvl = (ctx.posture == Posture::Full) ? "Full"
                  : (ctx.posture == Posture::Calm) ? "Calm" : "Dark";
  static const char* kSndWord[] = {"off", "low", "med", "high"};
  std::string snd = std::string("sound:") + kSndWord[ctx.sfxLevel > 3 ? 3 : ctx.sfxLevel];
  if (ctx.sfxLevel > 0) {
    char v[8];
    std::snprintf(v, sizeof v, " %u%%", unsigned(ctx.sfxVolume));
    snd += v;
  }
  std::string l2 = std::string("ring:") + lvl + "  " + snd + "  power:" +
                   friendlyProfile(ctx.profileName);
  fb.text(2, 13, l2);

  fb.hline(0, kSepY, kW);
}

// Outlined progress bar with a proportional fill. Never renders as a HOLLOW box:
// pct==0 draws nothing (callers gate on progress>0, belt-and-braces), and 1-2%
// gets a 1-px minimum sliver - the fill used to round to 0 px below 2%, leaving
// a small empty outline to the right of the job text (owner-reported artifact).
void drawBar(Fb& fb, int x, int y, int w, int h, uint8_t pct) {
  if (pct == 0) return;
  fb.rect(x, y, w, h);
  const int p = std::min<int>(pct, 100);
  int fill = (w - 2) * p / 100;
  if (fill < 1) fill = 1;
  fb.fillRect(x + 1, y + 1, fill, h - 2);
}

// StatusIdle body: up to 6 job rows (status label, optional job label,
// optional progress bar on the right), or a hint when the table is empty.
// Notifier with no jobs shows CONNECT GUIDANCE instead of a bare "(no jobs)"
// (owner P2.7): the device never appears in the Mac's Bluetooth list (custom
// peripheral, pair-on-access), so without on-screen instructions there is no
// way to know what to run or whether it's connected. btState: 1=advertising,
// 2=linked (a bonded broker is connected).
void drawStatusBody(Fb& fb, const ScreenCtx& ctx) {
  if (ctx.jobs.empty()) {
    const bool notifier =
        ctx.modeName && std::string(ctx.modeName) == "notifier";
    if (notifier) {
      const std::string name = ctx.deviceName.empty() ? "Nimbus" : ctx.deviceName;
      if (ctx.btState >= 2) {
        fb.text(4, 44, "Connected - waiting for coding sessions.");
        fb.text(4, 62, "Start an AI session with the");
        fb.text(4, 74, "nimbus-notify hooks installed.");
      } else if (ctx.btState == 1) {
        fb.text(4, 40, ("Ready to pair as \"" + name + "\"").substr(0, 48));
        fb.text(4, 54, "I won't show in Bluetooth settings.");
        fb.text(4, 68, "On your computer, run:");
        fb.text(4, 80, "  pip install nimbus-notify");
        fb.text(4, 92, "  nimbus-notify-broker");
        fb.text(4, 106, "It finds and pairs on its own.");
      } else {
        fb.text(4, 52, "Bluetooth is off.");
        fb.text(4, 66, "Turn it on in Settings > Connectivity.");
        fb.text(4, 80, "Double-click the knob to open Settings.");
      }
      return;
    }
    const char* hint = "(nothing running)";
    fb.text((kW - Fb::textWidth(hint)) / 2, 62, hint);
    return;
  }
  // Up to 6 rows fit. When there are more, show 5 jobs + a "+N more" row so the
  // count is honest - a silent drop read as "only 6 sessions" (audit). The extra
  // sessions are still on the ring (their own arcs) and reachable via the cursor.
  const int total = int(ctx.jobs.size());
  const bool overflow = total > 6;
  const int rows = overflow ? 5 : std::min<int>(total, 6);
  int y = 26;
  for (int i = 0; i < rows; ++i, y += 16) {
    const JobInfo& j = ctx.jobs[size_t(i)];
    fb.text(4, y, statusLabel(j.status));
    // nsn v2: name the session - "harness title" (either may be absent).
    std::string name = j.harness ? std::string(nsn::harnessName(j.harness)) : std::string();
    if (!j.label.empty()) name += (name.empty() ? "" : " ") + j.label;
    if (!name.empty()) fb.text(58, y, name.substr(0, 28));
    if (j.progress > 0) drawBar(fb, 232, y + 1, 56, 7, j.progress);
  }
  if (overflow) {
    char more[24];
    std::snprintf(more, sizeof more, "+%d more", total - rows);
    fb.text(4, y, more);
  }
}

// JobDetail: identity/status title, TextPager body (2 lines x 48 cols),
// "page i/N" footer only when there is more than one page.
void drawJobDetail(Fb& fb, const ScreenCtx& ctx) {
  if (ctx.cursorJob < 0 || size_t(ctx.cursorJob) >= ctx.jobs.size()) {
    fb.text(4, 30, "(no job selected)");
    return;
  }
  const JobInfo& j = ctx.jobs[size_t(ctx.cursorJob)];

  // nsn v2 identity: "harness title"; fall back to "job N" for a v1 broker.
  char who[44];
  if (j.harness || !j.label.empty())
    std::snprintf(who, sizeof who, "%s%s%s", nsn::harnessName(j.harness),
                  (j.harness && !j.label.empty()) ? " " : "", j.label.c_str());
  else
    std::snprintf(who, sizeof who, "job %lu", (unsigned long)j.key);
  char title[80];
  if (j.progress > 0)
    std::snprintf(title, sizeof title, "%s: %s %u%%", who, statusLabel(j.status), unsigned(j.progress));
  else
    std::snprintf(title, sizeof title, "%s: %s", who, statusLabel(j.status));
  fb.text(4, 26, title);
  fb.hline(4, 36, kW - 8);

  TextPager pager;
  pager.setText(j.label, 48, 2);  // 48 cols * 6px = 288px <= body width
  const size_t pages = pager.pageCount();
  size_t idx = ctx.detailPage < 0 ? 0 : size_t(ctx.detailPage);
  if (idx >= pages) idx = pages - 1;
  const std::vector<std::string> lines = pager.page(idx);
  fb.text(4, 44, lines[0]);
  fb.text(4, 56, lines[1]);

  if (pages > 1) {
    char foot[24];
    std::snprintf(foot, sizeof foot, "page %u/%u", unsigned(idx + 1),
                  unsigned(pages));
    fb.text(kW - 4 - Fb::textWidth(foot), kH - 12, foot);
  }
}

// SessionDetail: the encoder-cursor's focused sub-session (Orchestrator). Shows a
// "focus" header, the session id/state, and the (coarse) task/category wrapped.
// Long-press-to-talk here injects a turn to the orchestrator hinting at this
// session; the mic never talks to the sub-agent directly (fabric is fire-and-forget).
void drawSessionDetail(Fb& fb, const ScreenCtx& ctx) {
  if (ctx.sessionIsRoot) {
    // The Orchestrator itself - the head agent, ALWAYS present (focus index 0). This
    // is the "you <-> Nimbus" home; rotate to reach sub-agents, long-press to talk.
    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "Nimbus%s%s",
                  ctx.sessionProvider.empty() ? "" : " - ",
                  ctx.sessionProvider.c_str());
    fb.text(4, 26, hdr);
    fb.hline(4, 36, kW - 8);
    fb.text(4, 50, ctx.sessionState.empty() ? "ready" : ctx.sessionState.c_str());
    fb.text(4, kH - 12, "Hold to talk - turn for sessions");
    return;
  }
  if (ctx.sessionTitle.empty()) {
    // Defensive fallback only - the device always seeds the Orchestrator root, so an
    // Orchestrator-mode focus is never empty in practice.
    fb.text(4, 30, "no active session");
    fb.text(4, 44, "Hold the knob to talk to Nimbus");
    return;
  }
  char hdr[64];
  std::snprintf(hdr, sizeof hdr, "session: %s%s%s",
                ctx.sessionProvider.empty() ? "" : ctx.sessionProvider.c_str(),
                ctx.sessionState.empty() ? "" : " - ",
                ctx.sessionState.c_str());
  fb.text(4, 26, hdr);
  fb.hline(4, 36, kW - 8);
  const std::vector<std::string> lines = wrapText(ctx.sessionTitle, 48);
  fb.text(4, 48, lines.size() > 0 ? lines[0].c_str() : "");
  fb.text(4, 60, lines.size() > 1 ? lines[1].c_str() : "");
  fb.text(4, kH - 12, "Hold the knob to talk");
}

// Ask: title + the wrapped body, PAGINATED (P2.3 - long replies used to hard-cap
// at 9 lines with the rest silently dropped). ctx.detailPage selects the page
// (the device pages it with the knob while the reply is held); a footer shows
// "page i/N" when there is more, plus the dismiss hint.
void drawAsk(Fb& fb, const ScreenCtx& ctx) {
  fb.text(4, 26, "ask:");
  TextPager pager;
  pager.setText(ctx.askText, 48, 7);   // 7 body lines/page at 10px pitch (y 40..100)
  const size_t pages = pager.pageCount();
  size_t idx = ctx.detailPage < 0 ? 0 : size_t(ctx.detailPage);
  if (pages && idx >= pages) idx = pages - 1;
  const std::vector<std::string> lines = pager.page(idx);
  int y = 40;
  for (size_t i = 0; i < lines.size() && i < 7; ++i, y += 10) fb.text(4, y, lines[i]);
  fb.text(4, kH - 12, "Click to dismiss");
  if (pages > 1) {
    char foot[32];
    std::snprintf(foot, sizeof foot, "turn: page %u/%u", unsigned(idx + 1),
                  unsigned(pages));
    fb.text(kW - 4 - Fb::textWidth(foot), kH - 12, foot);
  }
}

// Battery telemetry: big percent, gauge, millivolts, power-state markers.
// Self-test results: a title + summary band, then the per-item rows in two
// columns (name left, verdict right-aligned in its column). FAIL verdicts are
// boxed so a failure is unmissable at a glance on the low-contrast panel.
void drawSelfTest(Fb& fb, const ScreenCtx& ctx) {
  fb.text(8, 28, "Self-test", 2);
  if (ctx.selfTestSummary.length())
    fb.text(kW - 8 - Fb::textWidth(ctx.selfTestSummary.c_str()), 32, ctx.selfTestSummary.c_str());

  const int n = (int)ctx.selfTest.size();
  const int perCol = 6;                 // 6 rows/col x 2 cols = 12 items
  const int colX[2] = {8, 156};
  const int colVX[2] = {138, kW - 8};   // right edge of each column's verdict
  const int y0 = 50, dy = 12;
  for (int i = 0; i < n && i < perCol * 2; i++) {
    const auto& row = ctx.selfTest[i];
    const int col = i / perCol, r = i % perCol;
    const int x = colX[col], y = y0 + r * dy;
    fb.text(x, y, row.name.c_str());
    const char* v = row.status == 0 ? "ok" : row.status == 1 ? "FAIL" : "skip";
    const int vw = Fb::textWidth(v);
    fb.text(colVX[col] - vw, y, v);
    if (row.status == 1) fb.rect(colVX[col] - vw - 2, y - 1, vw + 4, 10);  // box FAILs
  }
}

void drawBattery(Fb& fb, const ScreenCtx& ctx) {
  const power::Sample& b = ctx.battery;
  if (!b.valid) {
    const char* msg = "no battery telemetry";
    fb.text((kW - Fb::textWidth(msg)) / 2, 62, msg);
    return;
  }
  char buf[24];
  std::snprintf(buf, sizeof buf, "%u%%", unsigned(b.percent));
  fb.text(8, 26, buf, 2);

  fb.rect(120, 30, 100, 12);  // gauge body + nub
  const int fill = 98 * std::min<int>(b.percent, 100) / 100;
  if (fill > 0) fb.fillRect(121, 31, fill, 10);
  fb.fillRect(220, 33, 3, 6);

  std::snprintf(buf, sizeof buf, "%u mV", unsigned(b.millivolts));
  fb.text(8, 52, buf);

  // Charge state (top-right, next to the gauge) - the model's inference. "unknown"
  // just means too few samples yet (transient after boot / a telemetry gap), so show
  // it as "measuring..." rather than a bare, alarming "unknown" the owner can't parse.
  if (!ctx.battChargeState.empty()) {
    std::string cs = ctx.battChargeState == "unknown" ? "measuring..." : ctx.battChargeState;
    fb.text(kW - Fb::textWidth(cs.c_str()) - 6, 52, cs.c_str());
  }

  int y = 66;
  // Time-to-empty (only while discharging; -1 otherwise).
  if (ctx.battMinutesToEmpty >= 0) {
    const int hrs = ctx.battMinutesToEmpty / 60, mins = ctx.battMinutesToEmpty % 60;
    if (hrs > 0) std::snprintf(buf, sizeof buf, "~%dh %02dm left", hrs, mins);
    else         std::snprintf(buf, sizeof buf, "~%dm left", mins);
    fb.text(8, y, buf);
    y += 12;
  } else if (b.charging) {
    fb.text(8, y, "+ charging");
    y += 12;
  } else if (b.onExternalPower) {
    fb.text(8, y, "on external power");
    y += 12;
  }
  // Health / degradation (LiitoKala band-traversal heuristic).
  std::snprintf(buf, sizeof buf, "health %u%%", unsigned(ctx.battHealthPct));
  fb.text(8, y, buf);
}

// Menu: scroll window that keeps the selection visible, '>' cursor. A
// non-empty menuTitle adds a breadcrumb band (title line + separator) above
// the list; a non-empty menuHelp adds a help pane (separator + up to 3 wrapped
// lines) at the bottom. The list window shrinks around whichever bands are
// present; a titleless, helpless ctx keeps the original 12-row layout.
void drawMenu(Fb& fb, const ScreenCtx& ctx) {
  int y = 26;      // first list row (below the 2-line header)
  int rows = 10;   // scroll-window height (last row y=107, glyph bottom 114)

  if (!ctx.menuTitle.empty()) {
    std::string title = ctx.menuTitle;
    // Title budget at x=2 is 49 chars. Too long: left-elide keeping the tail -
    // the deepest breadcrumb level is the informative part.
    if (title.size() > 48) title = ".." + title.substr(title.size() - 46);
    fb.text(2, 26, title);
    fb.hline(0, 35, kW);
    y = 39;
    rows = 9;      // last row y=111, glyph bottom 118 < kH
  }

  if (!ctx.menuHelp.empty()) {
    constexpr int kHelpSepY = 94;
    rows = ctx.menuTitle.empty() ? 7 : 6;  // glyph bottoms stay above the pane
    fb.hline(0, kHelpSepY, kW);
    const std::vector<std::string> lines = wrapText(ctx.menuHelp, 48);
    int hy = 98;   // 3 lines at pitch 9: bottoms 105/114/123 < kH
    for (size_t i = 0; i < lines.size() && i < 3; ++i, hy += 9)
      fb.text(4, hy, lines[i]);
  }

  if (ctx.menuItems.empty()) {
    fb.text(4, y + 5, "(empty menu)");
    return;
  }
  const int count = int(ctx.menuItems.size());
  const int sel = std::max(0, std::min(ctx.menuSelected, count - 1));
  const int top = (sel >= rows) ? sel - (rows - 1) : 0;
  for (int i = top; i < count && i < top + rows; ++i, y += 9) {
    if (i == sel) fb.text(2, y, ">");
    fb.text(12, y, ctx.menuItems[size_t(i)]);
    // EDITING indicator (P2.2): while rotation is captured adjusting this row's
    // value, invert the whole row stripe - unmistakable on the low-contrast panel
    // (the "< val >" chevrons alone were easy to miss; owner: "no change" when
    // editing Volume). Same invert affordance as the attention badge.
    if (i == sel && ctx.menuAdjusting) fb.invertRect(0, y - 1, kW, 10);
  }
}

// VoiceGlyph: one compact centered region (small partial-refresh window) with
// a distinct simple glyph per pipeline stage.
void drawVoiceGlyph(Fb& fb, const ScreenCtx& ctx) {
  constexpr int gw = 40, gh = 40;
  const int gx = (kW - gw) / 2, gy = 44;
  fb.rect(gx, gy, gw, gh);
  const char* label = "-";
  switch (ctx.voice) {
    case VoiceStage::None:
      break;  // empty frame
    case VoiceStage::Recording:  // solid dot
      fb.fillRect(gx + 13, gy + 8, 14, 14);
      label = "rec";
      break;
    case VoiceStage::Processing:  // concentric frames
      fb.rect(gx + 8, gy + 6, 24, 18);
      fb.rect(gx + 14, gy + 10, 12, 10);
      label = "proc";
      break;
    case VoiceStage::Speaking: {  // waveform bars
      static const int kBarH[5] = {4, 10, 16, 10, 4};
      for (int i = 0; i < 5; ++i) {
        fb.fillRect(gx + 9 + i * 5, gy + 16 - kBarH[i] / 2, 3, kBarH[i]);
      }
      label = "talk";
      break;
    }
  }
  fb.text(gx + (gw - Fb::textWidth(label)) / 2, gy + gh - 11, label);
}

// Draw the setup/config URL as a scannable QR on the RIGHT, with a centered
// "scan me" caption directly beneath it - in the QR's OWN column so it can never
// be overwritten by the left-hand instruction lines (the field bug). Returns the
// x of the QR block's left edge so the caller clips its text column before it.
static int drawScannableQr(Fb& fb, const std::string& url, int topY) {
  nimbus::qr::QrCode qr;
  if (url.empty() || !nimbus::qr::encode(url, qr)) {
    fb.text(kW - 70, topY + 18, "no url");
    return kW - 76;
  }
  const int scale = 2;
  const int foot = Fb::qrFootprint(qr, scale);   // e.g. v3 URL -> (29+8)*2 = 74px
  const int qx = kW - foot - 4;
  fb.drawQr(qx, topY, qr, scale);
  const std::string cap = "scan me";
  int cx = qx + (foot - Fb::textWidth(cap)) / 2;
  if (cx < qx) cx = qx;
  fb.text(cx, topY + foot + 3, cap);
  return qx - 4;
}

// SetupInfo: the first-run "get this device onto your Wi-Fi" screen. Numbered
// steps on the left, a scan-to-configure QR on the right. Ordered the way you
// actually do it: the device isn't on your Wi-Fi yet, so step 1 is to JOIN ITS
// OWN setup network (dynamic name - never hardcoded), THEN scan, THEN provision
// your real Wi-Fi from the page. The setup URL is ALWAYS the SoftAP address (a
// LAN IP is unroutable from the AP subnet - audit P1.2).
void drawSetup(Fb& fb, const ScreenCtx& ctx) {
  const std::string name = ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName;
  // Notifier connects over Bluetooth (the nsn broker), not Wi-Fi - it runs no
  // radio and has no setup network. Point at the broker instead of telling the
  // owner to join a Wi-Fi that isn't there.
  if (ctx.modeName && std::string(ctx.modeName) == "notifier") {
    fb.text(4, 28, "Waiting for a Bluetooth connection");
    fb.text(4, 50, "On your computer, run the");
    fb.text(4, 62, "nimbus-notify broker - it finds");
    fb.text(4, 74, "this device automatically.");
    std::string dev = "Device: " + name;               // clip a long name to the panel
    while (!dev.empty() && 4 + Fb::textWidth(dev) > kW - 4) dev.pop_back();
    fb.text(4, 98, dev);
    return;
  }
  // The QR is a Wi-Fi JOIN code (WIFI:S:..;T:WPA;P:..;;) - a phone camera scans
  // it and joins the setup network directly, password included; the captive
  // portal then opens the setup page. The passphrase is per-device (NVS) and
  // this screen is the only place the owner can learn it, so it is also printed
  // for anyone joining by hand. URL QR only as a fallback when no AP is named.
  const std::string url = !ctx.apName.empty()
      ? nimbus::identity::wifiQrPayload(ctx.apName, ctx.apPass)
      : (!ctx.setupUrl.empty() ? ctx.setupUrl
                               : (ctx.configUrl.empty() ? ctx.portalUrl : ctx.configUrl));
  const int clipX = drawScannableQr(fb, url, 28);
  auto line = [&](int y, const std::string& s) {
    std::string t = s;                              // clip before the QR, never under it
    while (!t.empty() && 4 + Fb::textWidth(t) > clipX) t.pop_back();
    fb.text(4, y, t);
  };
  line(28, "Connect " + name + " to Wi-Fi");
  line(44, "1  Scan to join its network:");
  if (!ctx.apName.empty()) {
    line(56, "   " + ctx.apName);
    if (!ctx.apPass.empty()) line(68, "   Password: " + ctx.apPass);
  }
  line(84, "2  Setup opens on your phone");
  line(118, "No password or code to type");
}

// ConfigQr: the "connect to this device" screen. A scannable QR on the right,
// and on the left the one action that applies to the live link. The QR is the
// credential: scanning it signs the browser in, so a token is never a separate
// setup step.
void drawConfigQr(Fb& fb, const ScreenCtx& ctx) {
  // ConfigQr is not first-run SetupInfo: it opens the best CURRENTLY reachable
  // address. On LAN that is configUrl; while offline configUrl falls back to the
  // setup AP. Preferring setupUrl here made a healthy TFT advertise a hotspot it
  // had intentionally shut down.
  // While the setup AP is up (always on e-ink; on a TFT board until STA joins) a
  // phone is most likely ON that AP, from which the LAN address is unroutable
  // (audit P1.2) - so advertise the AP address + its credentials. Only once the AP
  // is gone (TFT, post-join) is the LAN address the reachable one. Before this gate,
  // a joined e-ink board (AP never dropped) showed an unroutable LAN QR and hid the
  // AP credentials, stranding a phone that was on the AP.
  const bool onLan = ctx.apUp ? false
                              : (!ctx.configUrl.empty() &&
                                 (ctx.setupUrl.empty() || ctx.configUrl != ctx.setupUrl));
  const std::string url = onLan ? ctx.configUrl
                         : (ctx.setupUrl.empty() ? (ctx.configUrl.empty() ? ctx.portalUrl : ctx.configUrl)
                                                 : ctx.setupUrl);
  const int clipX = drawScannableQr(fb, url, 28);
  auto line = [&](int y, const std::string& s) {
    std::string t = s;                              // clip before the QR, never under it
    while (!t.empty() && 4 + Fb::textWidth(t) > clipX) t.pop_back();
    fb.text(4, y, t);
  };
  const std::string name = ctx.deviceName.empty() ? std::string("Nimbus") : ctx.deviceName;
  line(28, "Open " + name + " settings");
  if (onLan) {
    line(44, "Home Wi-Fi is connected");
    line(60, "Scan - already signed in");
    line(84, "No token or code to type");
  } else {
    line(44, "1  Join setup hotspot:");
    if (!ctx.apName.empty()) line(56, "   " + ctx.apName);
    if (!ctx.apPass.empty()) line(68, "   Password: " + ctx.apPass);
    line(84, "2  Scan - signed in automatically");
  }
  // The live network state. This field was computed on EVERY render and then thrown
  // away - the comment above this function has always promised it ran here - so the
  // screen stayed silent in the one situation you reach it for: the device
  // unreachable and no clue why. Pre-clipped and ASCII by nimbus::wifi::netStatusLine.
  // y=118: glyph bottom 125 < kH 128, matching drawSetup's last row.
  //
  // FULL WIDTH, not line(): the QR ends around y=105, so clipping this row at the QR
  // column would chop the message mid-name - "use Nimbus-3" instead of
  // "use Nimbus-3-setup", naming a network that does not exist. That is the exact
  // failure this line was added to prevent, and the golden caught it.
  if (!ctx.netStatus.empty()) {
    std::string t = ctx.netStatus;
    while (!t.empty() && 4 + Fb::textWidth(t) > kW - 2) t.pop_back();
    fb.text(4, 118, t);
  }
}

// Full recovery token. The menu row is intentionally only a destination label:
// squeezing a credential into it clipped the tail while the TFT still drew a
// chevron, then activating that chevron did nothing. This screen shows the exact
// value and makes clear that the QR is the normal sign-in path.
void drawTokenDetail(Fb& fb, const ScreenCtx& ctx) {
  const char* title = "Recovery sign-in code";
  fb.text((kW - Fb::textWidth(title, 2)) / 2, 32, title, 2);
  std::string token = ctx.webToken.empty() ? std::string("-") : ctx.webToken;
  // 24 characters fit at scale 1 with ample margins. If the format grows,
  // split it rather than eliding even one credential character.
  constexpr size_t kLine = 32;
  int y = 62;
  for (size_t at = 0; at < token.size(); at += kLine, y += 16) {
    const std::string part = token.substr(at, kLine);
    fb.text((kW - Fb::textWidth(part)) / 2, y, part);
  }
  const char* hint = "Normal sign-in: scan the QR";
  fb.text((kW - Fb::textWidth(hint)) / 2, 102, hint);
}

// Pairing: big "Pair Nimbus" + the 6-digit passkey the Mac must enter, shown
// while a central is mid-pairing (main.cpp renders it on net::ble::pairingActive()).
// On a production silent-serial unit this is the ONLY channel for the code, so the
// secured link can actually be paired (no companion app; the OS's own sheet asks
// for what's shown here). Big + centered so it's legible across the room; the
// common header (mode/profile) is kept by the renderScreen wrapper.
void drawPairing(Fb& fb, const ScreenCtx& ctx) {
  const std::string title = "Pair Nimbus";
  fb.text((kW - Fb::textWidth(title, 2)) / 2, 28, title, 2);
  // The passkey, big + centered. Empty guard keeps the layout stable if a render
  // races ahead of the code being stashed.
  const std::string code = ctx.pairingCode.empty() ? "------" : ctx.pairingCode;
  fb.text((kW - Fb::textWidth(code, 3)) / 2, 54, code, 3);
  // Honest copy (P2.7): the old "System Settings > Bluetooth" instruction was
  // WRONG for this device - a custom peripheral never appears in that list. This
  // screen only fires if MITM/passkey pairing is ever re-enabled; the code would
  // be confirmed in whatever app is connecting (the broker), not macOS settings.
  const std::string l1 = "Enter this code in the";
  const std::string l2 = "connecting app (nimbus-notify).";
  fb.text((kW - Fb::textWidth(l1)) / 2, 100, l1);
  fb.text((kW - Fb::textWidth(l2)) / 2, 112, l2);
}

// Screensaver (owner 2026-07-16): after 1h+ of no interaction/state edges the
// panel shows the Nimbus dotted-ring mark + the device name instead of stale
// status. The ring mirrors the SVG logo (tools/logo/gen_logo.py, approved
// variant I - crisp concentric rows, per-dot shade variance); on the 1-bit
// panel "shade" becomes three dot tones: solid black / checker-dithered grey /
// small. Layout + tones come from a FIXED-seed LCG so the frame is
// deterministic (golden-tested); e-ink has no burn-in, so a static image is
// fine and re-randomizing would only cost refresh flashes at night.
void fillDot(Fb& fb, int cx, int cy, int r, int tone) {
  if (tone == 2) r -= 1;                       // "small" reads as a lighter shade
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy > r * r) continue;
      if (tone == 1 && ((dx + dy) & 1)) continue;   // checker dither = grey
      fb.set(cx + dx, cy + dy);
    }
}

void drawScreensaver(Fb& fb, const ScreenCtx& ctx) {
  uint32_t seed = 2026;
  auto rnd = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return int((seed >> 16) & 0x7FFF);
  };
  const int cx = 66, cy = 64;
  const int radii[4] = {28, 36, 44, 52};
  for (int row = 0; row < 4; row++) {
    const int r = radii[row];
    const int n = int(2.0 * 3.14159265 * r / 7.5);   // ~7.5 px centre spacing
    const double phase = (rnd() % 628) / 100.0;
    for (int k = 0; k < n; k++) {
      const double a = phase + k * 2.0 * 3.14159265 / n;
      const int x = cx + int(r * std::cos(a) + 0.5);
      const int y = cy + int(r * std::sin(a) + 0.5);
      const int w = rnd() % 12;                       // tone weights 5/4/3
      const int tone = w < 5 ? 0 : (w < 9 ? 1 : 2);
      fillDot(fb, x, y, 3 + (rnd() % 2), tone);
    }
  }
  // Device name to the right of the ring, vertically centred; drop to scale 2
  // if a long custom name would overflow the panel.
  std::string name = ctx.deviceName.empty() ? "Nimbus" : ctx.deviceName;
  int scale = 3;
  if (Fb::textWidth(name, scale) > kW - 140 - 4) scale = 2;
  if (Fb::textWidth(name, scale) > kW - 140 - 4)
    name = name.substr(0, size_t((kW - 140 - 4) / (6 * scale)));
  const int tw = Fb::textWidth(name, scale);
  const int tx = 140 + (kW - 140 - tw) / 2;
  fb.text(tx, cy - (7 * scale) / 2 - 6, name, scale);
  const char* hint = "turn to wake";
  fb.text(140 + (kW - 140 - Fb::textWidth(hint)) / 2, cy + (7 * scale) / 2 + 4,
          hint);
}

// IdleArt placeholder: the real screen is 3-color and rendered device-side.
void drawIdleArt(Fb& fb) {
  fb.rect(0, 0, kW, kH);
  fb.rect(3, 3, kW - 6, kH - 6);
  const char* t1 = "idle art";
  fb.text((kW - Fb::textWidth(t1, 2)) / 2, 48, t1, 2);
  const char* t2 = "(3-color, rendered device-side)";
  fb.text((kW - Fb::textWidth(t2)) / 2, 72, t2);
}

}  // namespace

// Real page count drawAsk produces for this text (same TextPager params). The
// device clamps knob-paging with THIS - a byte-length estimate under-counted
// wrap-expanded text and made the tail pages unreachable (review on P2.3).
// EXTERNAL linkage (outside the anon namespace): main.cpp calls it.
int askPageCount(const std::string& text) {
  TextPager pager;
  pager.setText(text, 48, 7);
  return int(pager.pageCount());
}

const char* statusLabel(uint8_t status) {
  switch (Status(status)) {
    case Status::Idle:             return "idle";
    case Status::Running:          return "running";
    case Status::WaitingInput:     return "input?";
    case Status::AwaitingApproval: return "approve?";
    case Status::Done:             return "done";
    case Status::Error:            return "error";
    case Status::Offline:          return "offline";
  }
  return "?";
}

Region renderBadgeRegion(Fb& fb, const ScreenCtx& ctx) {
  const Region r{200, 44, 90, 40};
  fb.fillRect(r.x, r.y, r.w, r.h, false);      // clear whatever is underneath
  fb.rect(r.x, r.y, r.w, r.h);                 // double border
  fb.rect(r.x + 2, r.y + 2, r.w - 4, r.h - 4);
  const std::string t = ctx.badgeText.substr(0, 13);  // keep inside the box
  fb.text(r.x + (r.w - Fb::textWidth(t)) / 2, r.y + (r.h - 7) / 2, t);
  // Active attention inverts the whole region: a filled box survives the
  // low-contrast grey partial-refresh path (plan 3.4).
  if (ctx.badgeActive) fb.invertRect(r.x, r.y, r.w, r.h);
  return r;
}

void renderScreen(Fb& fb, attn::ScreenId screen, const ScreenCtx& ctx) {
  fb.clear();
  if (screen == ScreenId::IdleArt) {  // placeholder frame, no header
    drawIdleArt(fb);
    return;
  }
  if (screen == ScreenId::Screensaver) {  // logo frame, no header
    drawScreensaver(fb, ctx);
    return;
  }
  drawHeader(fb, ctx);
  switch (screen) {
    case ScreenId::StatusIdle: drawStatusBody(fb, ctx); break;
    case ScreenId::JobDetail:  drawJobDetail(fb, ctx); break;
    case ScreenId::Badge:      // ambient status with the badge overlaid
      drawStatusBody(fb, ctx);
      renderBadgeRegion(fb, ctx);
      break;
    case ScreenId::Menu:       drawMenu(fb, ctx); break;
    case ScreenId::SelfTest:   drawSelfTest(fb, ctx); break;
    case ScreenId::Battery:    drawBattery(fb, ctx); break;
    case ScreenId::Ask:        drawAsk(fb, ctx); break;
    case ScreenId::VoiceGlyph: drawVoiceGlyph(fb, ctx); break;
    case ScreenId::SetupInfo:  drawSetup(fb, ctx); break;
    case ScreenId::ConfigQr:   drawConfigQr(fb, ctx); break;
    case ScreenId::TokenDetail: drawTokenDetail(fb, ctx); break;
    case ScreenId::SessionDetail: drawSessionDetail(fb, ctx); break;
    case ScreenId::Pairing:    drawPairing(fb, ctx); break;
    case ScreenId::IdleArt:    break;  // handled above
    case ScreenId::Screensaver: break; // handled above
  }
}

}  // namespace nimbus::epd
