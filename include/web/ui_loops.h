#pragma once
#include <Arduino.h>

// ui_loops - #pane-gov, host of the Assistant page's Routines and Safety subpanes
// (CUM-163). Routines (Phase 3 C1) are the governance surface: agent-created loops
// start pending your approval; hard token/fire caps auto-pause a runaway. Safety
// unifies the real controls only - the Downloads trust policy (#fetchpol) and Guest
// moderation (#modInbound/#modOutbound/#modInjection), both wired to /api/orch. The
// old safeIn/safeOut/safeMedia toggles were retired: they posted to /api/safety,
// which has no firmware handler, so they enforced nothing (CUM-163 investigation).
// Element IDs (loopList, lpName, lpPrompt, lpKind, fetchpol, modInbound, …) are
// unchanged so ui_js.h's wiring works. User copy says "routine"; the wire/commands
// stay loops (AGENTS.md style guide).

static const char UI_LOOPS[] PROGMEM = R"=====(<div class=pane id=pane-gov style="display:none">
<div class=subpane id=subpane-routines style="display:none">
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

<div class=subpane id=subpane-safety style="display:none">
<p class=plede>Download trust and guest screening. Your own messages and the web page are always exempt.</p>
<div class=sec>
<h2>Downloads <button class=qh type=button aria-expanded=false aria-label="About downloads">?</button></h2>
<p class="hint tip">How much trust the assistant gets when it wants to download a file from the web. Approve asks you for each link; Scan checks the file with AI before keeping it; Full trust downloads immediately.</p>
<div class=row style="gap:8px;flex-wrap:wrap">
<select id=fetchpol style="flex:1 1 160px"><option value=0>Off</option><option value=1 selected>Ask me per link</option><option value=2>Scan, then keep</option><option value=3>Full trust</option></select>
<button id=fetchpolsave type=button>Save</button>
</div>
<p class=hint id=fetchpolmsg></p>
<div id=fetchRows></div>
</div>
<div class=sec>
<h2>Guest moderation <button class=qh type=button aria-expanded=false aria-label="About guest moderation">?</button></h2>
<p class="hint tip">Screens guests, never you. Your own messages, the web page, and voice are always exempt. The message and reply checks each cost one moderation call per screened item (Cumulo moderation on a Cumulo key, otherwise Mistral on your key), so leave them off unless guests can reach the bot.</p>
<div class=row><label><input type=checkbox id=modInbound> Check guest messages before answering</label></div>
<p class="hint">If a message cannot be checked, it is not answered. Costs one call per guest message.</p>
<div class=row><label><input type=checkbox id=modOutbound> Check replies sent to guests</label></div>
<p class="hint">A flagged reply is held back. If a check cannot run, the reply still goes out. Costs one call per guest reply.</p>
<div class=row><label><input type=checkbox id=modInjection> Flag suspicious fetched content</label></div>
<p class="hint">Marks fetched web content that looks like a hidden instruction as untrusted, so it is treated as data. It marks, never blocks, and runs on the device at no extra cost.</p>
<div class=row><button id=modSave type=button>Save</button></div>
<p class=hint id=modmsg></p>
</div>
</div>
</div>

)=====";
