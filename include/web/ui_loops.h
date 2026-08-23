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
<h2>Wake-ups <button class=qh type=button aria-expanded=false aria-label="About wake-ups">?</button></h2>
<p class="hint tip">A wake-up is a turn the assistant schedules for itself to follow up later. Allow silently lets them run; "Ask me first" holds each new one for a single yes/no, never a repeating prompt.</p>
<label>When the assistant schedules a wake-up</label>
<select id=wkPolicy>
<option value=silent-allow>Allow silently</option>
<option value=ask>Ask me first</option>
</select>
<div class=hint id=wkMsg></div>
<div id=wkPending style="display:none;margin-top:10px;padding:12px;border:1px solid var(--teal);border-radius:12px;background:var(--raise2)">
<b id=wkPendLabel></b>
<p class=hint id=wkPendWhen style="margin:4px 0"></p>
<div class=row><button id=wkApprove type=button>Approve</button><button id=wkDeny type=button style="background:var(--raise3);color:var(--ink2)">Deny</button></div>
<div class=hint id=wkPendMsg></div>
</div>
<div id=wkList class=hint style="margin-top:8px"></div>
</div>
<div class=sec>
<h2>Safety <button class=qh type=button aria-expanded=false aria-label="About safety">?</button></h2>
<p class="hint tip">Moderation gates screen content with your provider before it is acted on. Each gate that is on adds a small provider call per item, so it costs a little more.</p>
<label class=pr><input type=checkbox id=safeIn> Screen incoming messages</label>
<label class=pr><input type=checkbox id=safeOut> Screen the assistant's replies</label>
<label class=pr><input type=checkbox id=safeMedia> Screen images and files</label>
<p class=hint id=safeCost></p>
<div class=hint id=safeMsg></div>
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
