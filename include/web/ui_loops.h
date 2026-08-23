#pragma once
#include <Arduino.h>

// ui_loops - the Routines pane (#pane-gov, Phase 3 C1). Scheduled/recurring
// tasks are the real governance surface: agent-created loops start pending your
// approval; hard token/fire caps auto-pause a runaway. Element IDs (loopList,
// lpName, lpPrompt, lpKind, …) are unchanged so ui_js.h's loops wiring works.
// User copy says "routine"; the wire/commands stay loops (AGENTS.md style guide).

static const char UI_LOOPS[] PROGMEM = R"=====(<div class=pane id=pane-gov style="display:none">
<div class=eyebrow>Automation</div>
<div class=ptitle>Routines</div>
<p class=plede>Tasks that run on a schedule. Routines the assistant creates for itself wait for your approval, and spending limits pause anything that misbehaves.</p>
<div class=sec>
<h2>Routines <button class=qh type=button aria-expanded=false aria-label="About routines">?</button></h2>
<p class="hint tip">The assistant runs a turn on a schedule - a morning digest, a reminder, nightly upkeep. Spend counts against per-routine and daily limits.</p>
<div id=govClock style="display:none"></div>
<div id=loopList>loading&hellip;</div>
</div>
<div class=sec>
<h2>New routine</h2>
<div class=row><input id=lpName placeholder="Name (e.g. Morning digest)" maxlength=24></div>
<div class=row><input id=lpPrompt placeholder="What should the assistant do?"></div>
<label>Schedule</label>
<select id=lpKind>
<option value=interval>On an interval</option>
<option value=daily>Daily</option>
<option value=weekly>Weekly</option>
</select>
<div class=row id=lpEveryRow><input id=lpEvery type=number min=5 value=360 style="width:90px"> minutes (minimum 5)</div>
<div class=row id=lpAtRow style="display:none">at <input id=lpAt type=time value="08:30"></div>
<div class=row id=lpDaysRow style="display:none">
<label><input type=checkbox class=lpday value=mon checked>Mon</label>
<label><input type=checkbox class=lpday value=tue>Tue</label>
<label><input type=checkbox class=lpday value=wed checked>Wed</label>
<label><input type=checkbox class=lpday value=thu>Thu</label>
<label><input type=checkbox class=lpday value=fri checked>Fri</label>
<label><input type=checkbox class=lpday value=sat>Sat</label>
<label><input type=checkbox class=lpday value=sun>Sun</label>
</div>
<div class=row><input id=lpChat placeholder="Telegram chat ID (blank = you)"></div>
<button id=lpCreate type=button>Create Routine</button>
<p id=lpMsg class=hint></p>
</div>
</div>

)=====";
