#pragma once
#include <Arduino.h>

// ui_memory - the Chat, Memory, and Usage panes (CUM-25 five-destination IA).
// Every element ID is unchanged from the old Orchestrator sub-panes so ui_js.h's
// wiring (memory dashboard, files browser, #usageTiles) works as-is. Active
// sessions live on Home (pane-dash #dashJobs); the old standalone Sessions pane
// was folded there. The pane-usage block is shown under the Assistant destination.
// Copy follows the AGENTS.md copy style guide.

static const char UI_MEMORY[] PROGMEM = R"=====(<div class=pane id=pane-chat style="display:none">
<div class=eyebrow>Conversation</div>
<div class=ptitle>Chat</div>
<p class=plede>Message the assistant and see its replies.</p>
<div class=sec id=chatDrop>
<div id=chatLog style="min-height:120px;max-height:52vh;overflow-y:auto;display:flex;flex-direction:column;gap:10px"></div>
<div class=row style="margin-top:10px;align-items:flex-end">
<textarea id=chatInput rows=2 placeholder="Message Nimbus&hellip;" style="flex:1"></textarea>
<input id=chatFile type=file multiple style="display:none">
<button id=chatAttach type=button title="Attach a file" aria-label="Attach a file" style="background:var(--raise3);color:var(--ink2)">Attach</button>
<button id=chatSend type=button>Send</button>
</div>
<p class=hint>Drop a file here or use Attach. Supported: images (png, jpg, gif, webp), text, md, csv, log, json, pdf.</p>
<div class=hint id=chatUpMsg></div>
<p class=hint id=chatMsg>Replies also go to your Telegram, if connected.</p>
<p class="hint tip" id=chatTrace style="display:none"></p>
</div>
</div>

<div class=pane id=pane-mem style="display:none">
<div class=eyebrow>On-device storage</div>
<div class=ptitle>Memory</div>
<p class=plede>What the device knows and the files it has made, stored on its SD card, not in a cloud.</p>

<details class=setgroup open><summary>Memory<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<label>Directive <button class=qh type=button aria-expanded=false aria-label="About the directive">?</button></label>
<p class="hint tip">Your standing instructions to the assistant. Only you can change them. Up to 600 characters.</p>
<textarea id=directive rows=4></textarea>
<div class=row><button id=savedir type=button>Save</button></div>
<label>Assistant memory <button class=qh type=button aria-expanded=false aria-label="About assistant memory">?</button></label>
<p class="hint tip">Working notes the assistant keeps per conversation. Shown here: the most recently active conversation&rsquo;s notes. Clear erases every conversation&rsquo;s notes.</p>
<pre id=memview class=memv>(empty)</pre>
<div class=row><button id=clearmem type=button>Clear</button></div>
<label>This conversation <button class=qh type=button aria-expanded=false aria-label="About clearing the conversation">?</button></label>
<p class="hint tip">Forget the current conversation and its active task, and start fresh. Long-term memory and files are kept.</p>
<div class=row><button id=clearconv type=button>Clear conversation</button></div>
</div>
</details>

<details class=setgroup><summary>Long-term memory <span class="badge" id=memstat></span><span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<div id=tierbanner style="display:none;margin:6px 0;padding:8px;border-radius:8px;background:#3a2a15;color:#f0c890;font-size:13px;line-height:1.35"></div>
<b style="display:block">Recall <button class=qh type=button aria-expanded=false aria-label="About recall">?</button></b>
<p class="hint tip">Memories the assistant searches by meaning. The list shows the top matches; search to find specific ones.</p>
<div class=row>
<input id=memq placeholder="Search memories">
<button id=memsearch type=button>Search</button>
</div>
<p class=hint id=memvmsg></p>
<div id=memlist></div>
<div class=row style="margin-top:6px;align-items:center">
<button id=memprev type=button>&lsaquo; Prev</button>
<span class=hint id=mempage></span>
<button id=memnext type=button>Next &rsaquo;</button>
</div>
<div class=row style="margin-top:6px">
<button id=memdedupe type=button>Deduplicate</button>
<button id=memflushnp type=button>Delete Temporary</button>
<button id=memflushall type=button style="color:#f0687a">Delete All</button>
</div>
<b style="display:block;margin-top:14px">Scratchpad <button class=qh type=button aria-expanded=false aria-label="About the scratchpad">?</button></b>
<p class="hint tip">The assistant's working notes for its current goals.</p>
<pre id=scratchview class=memv>(empty)</pre>
<b style="display:block;margin-top:14px">Recall tuning</b>
<label>Memories per turn <span class=hint>(1&ndash;100)</span></label>
<input id=cfg_rc type=number min=1 max=100>
<label>Relevance threshold <span class=hint>(0&ndash;1; 0 keeps all)</span></label>
<input id=cfg_rt type=number step=0.05 min=0 max=1>
<label>Storage limit <span class=hint>(0 = unlimited)</span> <button class=qh type=button aria-expanded=false aria-label="About the storage limit">?</button></label>
<p class="hint tip">At the limit, the least valuable memory is dropped - lowest importance, closest to expiry. Recall boosts what you use, so it survives.</p>
<input id=cfg_mv type=number min=0 max=20000>
<div class=row><button id=cfgsave type=button>Save</button></div>
<b style="display:block;margin-top:14px">Embedding model <span class="badge" id=emblock style="display:none">locked</span> <button class=qh type=button aria-expanded=false aria-label="About the embedding model">?</button></b>
<p class="hint tip">How memories become vectors for semantic search. Saving runs a real embedding call to verify the model works before the choice is stored.</p>
<label for=emb_provider>Provider</label>
<select id=emb_provider><option value=openai>OpenAI</option><option value=mistral>Mistral</option></select>
<label for=emb_model>Model</label>
<select id=emb_model></select>
<label for=emb_dims>Dimensions <span class=hint>(0 = provider default)</span></label>
<input id=emb_dims type=number min=0 placeholder="0" style="max-width:130px">
<div id=embwarn class=info style="display:none;border-color:#e0b870;background:rgba(224,184,112,.10)"></div>
<div class=row><button id=embsave type=button>Save</button></div>
<p class=hint id=embmsg></p>
</div>
</details>

<details class=setgroup><summary>Files<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class=hint id=filesStat>-</p>
<p class=hint id=filesQuota></p>
<div class=row><input id=filesProj placeholder="Filter by project"><button id=filesRefresh type=button>Refresh</button><button id=filesRmProj type=button style="color:#f0687a">Delete folder</button></div>
<div id=filesList class=hint>loading&hellip;</div>
<div id=filePrev style="display:none;margin-top:12px;border:1px solid var(--line);border-radius:10px;padding:12px"></div>
<label style="margin-top:10px">Upload a file</label>
<div class=row><input id=upProj placeholder="Project (default: uploads)" style="max-width:160px"><input id=upFile type=file></div>
<div class=row><button id=upBtn type=button>Upload</button><button class=qh type=button aria-expanded=false aria-label="About files">?</button></div>
<p class="hint tip">Files persist across restarts and are never deleted automatically. The assistant saves reports here with <span class=badge>artifact.save</span>; you can download, delete, or add your own.</p>
<div class=hint id=upMsg></div>
</div>
</details>
</div>

<div class=pane id=pane-usage style="display:none">
<div class=eyebrow>Costs</div>
<div class=ptitle>Usage &amp; Budget</div>
<p class=plede>Token usage reported by your providers, tracked since the device last restarted.</p>
<div class=sec>
<div id=usageTiles class=tiles></div>
<div id=usageSummary class=tiles></div>
<p class=hint>Token counts are actual billed usage. Dollar figures are estimates from the rates below - a close guide, not an invoice.</p>
</div>

<div class=sec>
<h2>Spend over time</h2>
<div class=row style="justify-content:space-between">
<div class=row>
<button type=button class=rangeBtn data-days=7 style="padding:5px 10px;font-size:12px">7d</button>
<button type=button class=rangeBtn data-days=30 style="padding:5px 10px;font-size:12px">30d</button>
<button type=button class=rangeBtn data-days=60 style="padding:5px 10px;font-size:12px">60d</button>
</div>
<div class=row>
<button type=button id=unitTok style="padding:5px 10px;font-size:12px">tokens</button>
<button type=button id=unitUsd style="padding:5px 10px;font-size:12px">$ est</button>
</div>
</div>
<canvas id=usageChart width=880 height=260 style="width:100%;height:auto;margin-top:10px"></canvas>
<div id=usageLegend class=row style="flex-wrap:wrap;gap:10px;margin-top:6px"></div>
<p class=hint id=usageChartMsg></p>
</div>

<div class=sec>
<h2>Rates <button class=qh type=button aria-expanded=false aria-label="About rates">?</button></h2>
<p class="hint tip">What each provider charges - used only for the estimates above. Language models: dollars per million input and output tokens. Search: dollars per thousand calls. 0 uses a built-in default, marked ~.</p>
<div class=row>
<select id=rate_prov style="flex:1 1 120px"><option value=openai>OpenAI</option><option value=anthropic>Anthropic</option><option value=mistral>Mistral</option><option value=tavily>Tavily (search)</option></select>
<input id=rate_in type=number min=0 step=0.01 placeholder="$ per 1M input" style="flex:1 1 110px">
<input id=rate_out type=number min=0 step=0.01 placeholder="$ per 1M output" style="flex:1 1 110px">
<input id=rate_call type=number min=0 step=0.01 placeholder="$ per 1,000 calls" style="flex:1 1 100px">
<button id=ratesave type=button>Save</button>
</div>
<p class=hint id=ratemsg></p>
</div>

<div class=sec>
<h2>Downloads <button class=qh type=button aria-expanded=false aria-label="About downloads">?</button></h2>
<p class="hint tip">How much trust the assistant gets when it wants to download a file from the web. Approve asks you for each link; Scan checks the file with AI before keeping it; Full trust downloads immediately.</p>
<div class=row style="gap:8px;flex-wrap:wrap">
<select id=fetchpol style="flex:1 1 160px"><option value=0>Off</option><option value=1 selected>Ask me per link</option><option value=2>Scan, then keep</option><option value=3>Full trust</option></select>
<button id=fetchpolsave type=button>Save</button>
</div>
<p class=hint id=fetchpolmsg></p>
<div id=fetchRows></div>

<h2>Guest moderation <button class=qh type=button aria-expanded=false aria-label="About guest moderation">?</button></h2>
<p class="hint tip">Screens guests, never you. Your own messages and the web page are always exempt. Each switch adds a safety check that costs one moderation call per screened item (Cumulo moderation on a Cumulo key, otherwise Mistral on your key), so leave them off unless guests can reach the bot.</p>
<div class=row><label><input type=checkbox id=modInbound> Check guest messages before answering</label></div>
<p class="hint">If a message cannot be checked, it is not answered. Costs one call per guest message.</p>
<div class=row><label><input type=checkbox id=modOutbound> Check replies sent to guests</label></div>
<p class="hint">A flagged reply is held back. If a check cannot run, the reply still goes out. Costs one call per guest reply.</p>
<div class=row><label><input type=checkbox id=modInjection> Flag suspicious fetched content</label></div>
<p class="hint">Marks fetched web content that looks like a hidden instruction as untrusted, so it is treated as data. It marks, never blocks. Costs one call per fetched item.</p>
<div class=row><button id=modSave type=button>Save</button></div>
<p class=hint id=modmsg></p>

<h2>Budgets <button class=qh type=button aria-expanded=false aria-label="About budgets">?</button></h2>
<p class="hint tip">Caps each provider's spend for the month - by tokens, by dollars, or both; 0 means unlimited. The dollar cap uses your rates above, so set prices for it to count. At any cap, that provider's turns and searches are refused until the reset day - the assistant fails over to another in-budget provider when it can.</p>
<div id=budgetRows></div>
<div class=provrow style="border-top:0;margin-top:6px;padding-top:0">
<label>Set a budget</label>
<div class=row>
<select id=bud_prov style="flex:1 1 120px"><option value=openai>OpenAI</option><option value=anthropic>Anthropic</option><option value=mistral>Mistral</option><option value=tavily>Tavily (search)</option></select>
<input id=bud_tok type=number min=0 placeholder="Tokens per month" style="flex:1 1 120px">
<input id=bud_usd type=number min=0 step=0.01 placeholder="$ per month" style="flex:1 1 100px">
<input id=bud_call type=number min=0 placeholder="Calls per month" style="flex:1 1 120px">
<input id=bud_reset type=number min=1 max=28 value=1 title="Day of month the budget resets" style="flex:0 1 90px">
<button id=budsave type=button>Save</button>
</div>
<p class=hint id=budmsg></p>
</div>
</div>
</div>

)=====";
