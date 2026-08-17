#pragma once
#include <Arduino.h>

// web_pages - embedded PROGMEM HTML for the Nimbus config web UI.
//
// Adapted from Nuage-Solide include/web_pages.h (SETUP_HTML + a gutted
// SETTINGS_HTML): kept the dark-theme skeleton, the `.sec`/`.info`/`.badge` CSS,
// the `$=id=>...` helper, and the WiFi scan/save block. Dropped every provider /
// model / Telegram / voice / session section and all the auth/WebSocket JS. The
// page is fully static (no %SID%/%WARN% templating) - live data comes purely
// from GET /api/state, polled on an interval; mutations POST /api/config.
//
// One page (CONFIG_HTML) serves the config controls, the live stats header, and
// the WiFi setup section. Served zero-copy via sendStaticP.

#include "web/ui_shell.h"
#include "web/ui_device.h"
#include "web/ui_agent.h"
#include "web/ui_memory.h"
#include "web/ui_wifi.h"
#include "web/ui_loops.h"
#include "web/ui_onboard.h"
#include "web/ui_js.h"

// The config page, as an ordered list of PROGMEM fragments (P4 commit 1: a
// MECHANICAL split of the old single CONFIG_HTML blob - byte-identical when
// concatenated; tools/webui_concat_check.py asserts it). webui.cpp streams
// them in order with a chunked response, so no RAM copy of the page exists.
static const char* const CONFIG_HTML_PARTS[] = {
  UI_SHELL, UI_DEVICE, UI_AGENT, UI_MEMORY, UI_WIFI, UI_LOOPS, UI_ONBOARD, UI_JS,
};
static constexpr size_t CONFIG_HTML_PART_COUNT =
    sizeof(CONFIG_HTML_PARTS) / sizeof(CONFIG_HTML_PARTS[0]);
