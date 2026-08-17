#pragma once
#include <Arduino.h>

// ui_onboard - the first-run setup wizard overlay (#onbov). A full-screen overlay
// shown by ui_js.h when GET /api/state reports needsOnboarding:true (a fresh or
// factory-reset device). HTML only (the single <script> lives in ui_js.h); the
// step machine renders each step's body into #onbBody. Two hard-gated steps
// (Wi-Fi, a verified provider) reuse /scan, /savewifi, /api/orch + /api/verify;
// the rest are skippable. Finishing POSTs /api/onboard/complete.

static const char UI_ONBOARD[] PROGMEM = R"=====(<div id=onbov style="display:none;position:fixed;inset:0;z-index:200;background:var(--bg);overflow-y:auto">
<div style="max-width:540px;margin:0 auto;padding:28px 18px calc(28px + env(safe-area-inset-bottom));display:flex;flex-direction:column;gap:16px;min-height:100%;box-sizing:border-box">
<div style="display:flex;align-items:center;gap:12px">
<img src=/logo.svg alt="" style="width:42px;height:42px">
<div><div class=eyebrow>First-time setup</div><div class=ptitle id=onbTitle>Welcome</div></div>
</div>
<div id=onbDots style="display:flex;gap:6px;flex-wrap:wrap"></div>
<div class=sec id=onbBody style="margin:0"></div>
<div style="display:flex;justify-content:space-between;align-items:center;gap:8px;margin-top:auto">
<button type=button id=onbBack style="background:#333;color:#cde">Back</button>
<div style="display:flex;gap:8px">
<button type=button id=onbSkip style="background:#333;color:#cde">Skip</button>
<button type=button id=onbNext>Next</button>
</div>
</div>
<p class=hint id=onbMsg></p>
</div>
</div>

)=====";
