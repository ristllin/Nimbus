// P2 bring-up: drives the ENTIRE portable Nimbus core on real hardware.
//
//   encoder detent -> cursor moves on the ring INSTANTLY (ring_plan -> leds)
//                  -> dwell timer (epd_sched) -> after ~300 ms of knob silence,
//                     the e-ink renders the cursor's job detail (epd_render ->
//                     framebuffer -> solide::display)
//   click          -> back to the status screen
//
// This is the "ring is instant, e-ink is dwell" division of labor from the
// brief, running live. Flash with:  pio run -e p2bringup -t upload
#include <Arduino.h>
#include <solide/solide.h>

#include "hw/epd_out.h"
#include "hw/ring_out.h"
#include "nimbus/attention.h"
#include "nimbus/epd_render/screens.h"
#include "nimbus/epd_sched.h"
#include "nimbus/profile.h"
#include "nimbus/ring_plan.h"
#include "nimbus_config.h"

using namespace nimbus;
using solide::ring::Status;

// ---- a realistic scene (labels: nsn v1 has none, so we supply them here) ----
struct Job { uint32_t key; Status status; uint8_t progress; uint8_t hue; const char* label; };
static const Job kScene[] = {
    {1, Status::Running, 40, 170, "build firmware image for the s3 target board"},
    {2, Status::AwaitingApproval, 0, 32, "approve deploy to production cluster"},
    {3, Status::Running, 75, 85, "run host unit tests"},
};
static constexpr int kJobs = int(sizeof(kScene) / sizeof(kScene[0]));

static attn::Router g_router;
static Config g_cfg;
static ring::Cursor g_cursor;
static epd::Scheduler g_sched;
static epd::Fb g_fb;  // static: requestBitmap keeps the pointer until rendered

// Panel-busy approximation: solide::display renders async without a completion
// callback, so we treat the panel as busy for the measured refresh time and
// feed epd_sched::onRenderDone when it elapses. (A real completion signal is a
// small solide::display enhancement - noted for later.)
static uint32_t g_panelDoneAt = 0;
static bool     g_panelBusy = false;

static uint8_t curId(attn::ScreenId s) { return uint8_t(s); }

static epd::ScreenCtx buildCtx(int cursorJob) {
  epd::ScreenCtx c;
  c.modeName = "notifier";
  c.posture = g_cfg.posture();
  c.profileName = profileName(g_cfg.profile());
  c.cursorJob = cursorJob;
  for (const Job& j : kScene)
    c.jobs.push_back({j.key, uint8_t(j.status), j.progress, j.hue, j.label});
  return c;
}

static void renderScreen(attn::ScreenId screen, int cursorJob) {
  epd::ScreenCtx ctx = buildCtx(cursorJob);
  epd::renderScreen(g_fb, screen, ctx);
  hw::pushFramebuffer(g_fb, epd::Kind::FastBW);
  g_panelBusy = true;
  g_panelDoneAt = millis() + NIMBUS_EPD_FASTBW_MS;
  Serial.printf("EPD render screen=%u cursor=%d (busy ~%d ms)\n",
                curId(screen), cursorJob, NIMBUS_EPD_FASTBW_MS);
}

static void refreshRing() {
  ring::Plan p = ring::compose(g_router, g_cfg, g_cursor, g_panelBusy, millis());
  hw::applyRingPlan(p);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\nnimbus P2 bring-up - encoder drives cursor(ring)+dwell(e-ink)");

  solide::BeginResult b = solide::begin();
  Serial.printf("begin: display=%d leds=%d input=%d\n", b.display, b.leds, b.input);

  // Desk profile => Full ring level: the ring shows per-job segments.
  g_cfg.setProfile(ProfileId::Desk);

  // Seed the scene into the attention router (drives the ring segments).
  for (const Job& j : kScene) {
    attn::Event e;
    e.type = attn::Event::Type::JobState;
    e.key = j.key; e.status = uint8_t(j.status);
    e.hasAccent = true; e.accentHue = j.hue;
    g_router.route(e, millis());
    attn::Event p; p.type = attn::Event::Type::JobProgress;
    p.key = j.key; p.value = j.progress;
    g_router.route(p, millis());
  }

  g_sched.configure({NIMBUS_DWELL_MS, NIMBUS_EPD_COALESCE_MS,
                     NIMBUS_FULL_REFRESH_EVERY_N});
  refreshRing();
  renderScreen(attn::ScreenId::StatusIdle, -1);
  Serial.println("ready - turn the knob to move the ring cursor; pause to dwell; click for status.");
}

void loop() {
  const uint32_t now = millis();

  // Async panel "done" approximation.
  if (g_panelBusy && int32_t(now - g_panelDoneAt) >= 0) {
    g_panelBusy = false;
    g_sched.onRenderDone(now);
    refreshRing();  // drop the "syncing" cursor shimmer now the panel is free
  }

  // Encoder: cursor is INSTANT on the ring; e-ink only follows after dwell.
  solide::input::Event e;
  while (solide::input::pop(e)) {
    if (e == solide::input::Event::RotateCW || e == solide::input::Event::RotateCCW) {
      const int dir = (e == solide::input::Event::RotateCW) ? +1 : -1;
      g_cursor.onDetent(dir, kJobs, now);          // cursor over the job list
      g_sched.onDetent(curId(attn::ScreenId::JobDetail), now);
      refreshRing();                                // instant ring update
      Serial.printf("ENC %s -> cursor job %u\n",
                    dir > 0 ? "CW" : "CCW", g_cursor.index());
    } else if (e == solide::input::Event::Click) {
      g_sched.onIntent(curId(attn::ScreenId::StatusIdle), /*attention=*/false, now);
      Serial.println("ENC CLICK -> status");
    }
  }

  // Scheduler: issues the dwell/coalesced render when due and the panel is free.
  epd::RenderCommand cmd = g_sched.tick(now);
  if (cmd.render) {
    const attn::ScreenId screen = attn::ScreenId(cmd.screenId);
    const int cursorJob =
        (screen == attn::ScreenId::JobDetail) ? int(g_cursor.index()) : -1;
    renderScreen(screen, cursorJob);
  }

  delay(3);
}
