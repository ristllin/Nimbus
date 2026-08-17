#pragma once
#include <Arduino.h>

// ui_agent - the Capabilities pane (#pane-harness): the device's providers +
// tool surface, split into four sub-tabs (Connectors / Tools / Models / Skills)
// via the .subtab/.subpane switcher in ui_js.h. Every element ID (and the
// data-sp keys, incl. the historical "llm") is unchanged from the old flat
// Connectors pane so ui_js.h's applyOrch/loadOrch/loadConnectors/loadTools/
// loadSkills wiring works as-is - only display text changed. The single
// #orchsave button collects every orch field by id() regardless of which
// sub-pane holds it. Self-contained (opens + closes the pane). Copy follows
// the AGENTS.md copy style guide.

static const char UI_AGENT[] PROGMEM = R"=====(<div class=pane id=pane-harness style="display:none">
<div class=eyebrow>AI &amp; integrations</div>
<div class=ptitle>Capabilities</div>
<p class=plede>Everything the device can reach and run &mdash; providers, tools, connectors, and skills. Keys are set by you, never by the model.</p>

<div class=subtabs>
<button type=button class=subtab data-sp=connectors>Connectors</button>
<button type=button class=subtab data-sp=tools>Tools</button>
<button type=button class=subtab data-sp=llm>Models</button>
<button type=button class=subtab data-sp=skills>Skills</button>
</div>

<div class=subpane id=subpane-connectors>
<div class=sec>
<h2>Connectors <button class=qh type=button aria-expanded=false aria-label="About connectors">?</button></h2>
<p class="hint tip">Tools that run in your AI provider's cloud &mdash; on the assistant's own turns and on the sessions it starts. Create each credential once and paste it here. <a href="https://ristllin.github.io/Nimbus/guides/connectors" target=_blank>Setup guide &rarr;</a></p>
<p class=hint id=connhost></p>
<div id=conncards></div>
<details style="margin-top:12px"><summary class=hint style="cursor:pointer">Advanced &mdash; edit raw JSON</summary>
<p class=hint>A JSON array; secrets are write-only. Example: <code>{"name":"github","prov":"openai","kind":"mcp","url":"https://api.githubcopilot.com/mcp/","tok":"ghp_...","type":"github","en":1}</code></p>
<div class=row><textarea id=connBlob rows=5 style="width:100%;font-family:monospace" placeholder='[{"name":"web_search","prov":"mistral","kind":"builtin","en":1}]'></textarea></div>
<div class=row><button type=button onclick="saveConnectors()">Save JSON</button><span class=hint id=connstat></span></div>
</details>
</div>

<details class=setgroup><summary>Telegram<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<label>Bot token <button class=qh type=button aria-expanded=false aria-label="About Telegram">?</button></label>
<p class="hint tip">Message the device from Telegram. Create a bot with @BotFather, paste its token here, then approve who may talk to it.</p>
<p id=tgReady class=hint></p>
<div class=row><input id=tgToken type=password placeholder="Bot token from @BotFather"><button type=button onclick="orchApply({tgToken:$('tgToken').value})">Save</button><button type=button onclick="orchApply({clr_tgToken:1})">Clear</button></div>
<label>Who can message this device <button class=qh type=button aria-expanded=false aria-label="About members">?</button></label>
<p class="hint tip">New senders appear here for one-tap approval. Access is enforced by chat ID &mdash; Telegram usernames can change hands.</p>
<div id=tgPending></div>
<div id=tgChips></div>
<div class=row><input id=tgAddId placeholder="Chat ID"><input id=tgAddName placeholder="Name (optional)" style="flex:1"><button id=tgAddBtn type=button>Add</button></div>
<label class=pr style="margin-top:8px;color:#e0b870"><input type=checkbox id=tgPublic> <b>Open access</b></label>
<span class=hint>Anyone who finds this bot on Telegram can use it &mdash; and your API credits.</span>
<label class=pr><input type=checkbox id=ttsOn> Voice replies <button class=qh type=button aria-expanded=false aria-label="About voice replies">?</button></label>
<p class="hint tip">When on, the assistant may answer with a spoken voice note when that fits &mdash; never audio duplicating the same text. When off, it always replies in text.</p>
</div>
</details>
</div>

<div class=subpane id=subpane-tools style="display:none">
<div class=sec>
<h2>Tools <span class="badge" id=toolstat></span> <button class=qh type=button aria-expanded=false aria-label="About tools">?</button></h2>
<p class="hint tip">What the assistant can do right now &mdash; the live on-device tool surface. External MCP clients can call it too, over <code>POST /mcp</code>.</p>
<div id=toollist></div>
</div>
<div class=sec>
<h2>Web search <button class=qh type=button aria-expanded=false aria-label="About web search">?</button></h2>
<p class="hint tip">A <a href="https://tavily.com" target=_blank>Tavily</a> API key lets the assistant search the web live.</p>
<div class=row>
<input id=tavKey type=password placeholder="Tavily API key">
<button type=button onclick="orchApply({tavKey:$('tavKey').value})">Save</button>
<button type=button onclick="if(confirm('Remove the Tavily key?'))orchApply({clr_tavKey:1})">Clear</button>
</div>
<p class=hint id=tavstat></p>
</div>
</div>

<div class=subpane id=subpane-llm style="display:none">
<div class=sec>
<h2>Models <span class="badge ext" id=orchoff style="display:none">Notifier mode &mdash; these settings apply when you switch to Orchestrator</span></h2>

<details class=setgroup open><summary>Providers &amp; keys<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class=hint>Keys are write-only &mdash; a saved key shows as &quot;set&quot; and is never displayed again. Model choices unlock after the key verifies.</p>
<div id=provs></div>
</div>
</details>

<details class=setgroup><summary>Custom endpoint<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<label>Base URL <button class=qh type=button aria-expanded=false aria-label="About custom endpoints">?</button></label>
<p class="hint tip">Point the assistant at any OpenAI-, Anthropic-, or Mistral-compatible server. An <b>http://</b> base uses plain HTTP for a LAN server &mdash; e.g. a local Ollama at <code>http://192.168.1.50:11434/v1</code> with the openai wire, model <code>qwen2.5</code>, and a blank key. An https:// or bare host stays TLS.</p>
<input id=custBase placeholder="https://proxy.example/v1">
<div class=row><input id=custKey type=password placeholder="API key (blank for a keyless LAN endpoint)"><button type=button onclick="orchApply({clr_custKey:1})">Clear</button></div>
<div class=row>
<select id=custConv><option value=openai>openai wire</option><option value=anthropic>anthropic wire</option><option value=mistral>mistral wire</option></select>
<input id=custModel placeholder="Model ID">
</div>
</div>
</details>

<details class=setgroup><summary>Routing<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<label>Primary provider <button class=qh type=button aria-expanded=false aria-label="About the primary provider">?</button></label>
<p class="hint tip">The provider that runs the assistant. Automatic picks the first verified provider in your fallback order.</p>
<select id=orchHost>
<option value="">Automatic</option>
<option value=openai>OpenAI</option><option value=anthropic>Anthropic</option>
<option value=mistral>Mistral</option><option value=custom>Custom</option>
</select>
<label>Fallback order <button class=qh type=button aria-expanded=false aria-label="About the fallback order">?</button></label>
<p class="hint tip">Check a provider to enable it; use the arrows to set the order tried when the primary fails.</p>
<div id=provPrioList></div>
<label>Session fallback order <button class=qh type=button aria-expanded=false aria-label="About the session order">?</button></label>
<p class="hint tip">The provider order for the sessions the assistant spawns.</p>
<div id=subPrioList></div>
</div>
</details>

<details class=setgroup><summary>Voice<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<label>Dictation <button class=qh type=button aria-expanded=false aria-label="About dictation">?</button></label>
<p class="hint tip">Turns your voice &mdash; from the microphone or Telegram voice notes &mdash; into text. Works as soon as the provider's key is set, independent of chat verification.</p>
<select id=sttProv><option value=mistral>Mistral (Voxtral)</option><option value=openai>OpenAI</option></select>
<label>Spoken replies</label>
<select id=ttsProv><option value=mistral>Mistral (Voxtral)</option><option value=openai>OpenAI</option></select>
<label>Voice</label>
<select id=ttsVoice></select>
<div class=row id=vcascade>
<select id=vGender style="display:none"></select>
<select id=vPersona style="display:none"></select>
<select id=vEmotion style="display:none"></select>
</div>
<p class=hint id=voiceHint></p>
</div>
</details>

<details class=setgroup><summary>Tool use<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<label class=pr><input type=checkbox id=orchLoop checked> Enabled <button class=qh type=button aria-expanded=false aria-label="About tool use">?</button></label>
<label class=pr><input type=checkbox id=midFail checked> Switch providers mid-task if one fails</label>
<p class="hint tip">When on, the assistant uses its tools mid-turn &mdash; memory, sessions, web search, device control &mdash; and iterates before answering. When off, it answers in a single step.</p>
<label class=pr><input type=checkbox id=orchTrace checked> Record tool calls &amp; thinking <button class=qh type=button aria-expanded=false aria-label="About activity recording">?</button></label>
<p class="hint tip">Keeps a per-turn record of what the assistant did &mdash; each tool call, its result, and the reasoning between them &mdash; viewable in the chat. Stored on the SD card with the same 30-day retention as history.</p>
<label>Tool rounds <span class=hint>(1&ndash;32)</span></label>
<input id=loopRounds type=number min=1 max=32>
<label>Time limit, seconds <span class=hint>(30&ndash;3600)</span></label>
<input id=loopDeadline type=number min=30 max=3600>
<label>Compact conversations after, KB <span class=hint>(8&ndash;512; 0 turns automatic compaction off)</span> <button class=qh type=button aria-expanded=false aria-label="About compaction">?</button></label>
<p class="hint tip">When a chat accumulates this much history, the assistant folds it into a compact summary and starts a fresh provider thread &mdash; conversations stay fast and inexpensive without losing continuity. Send /compact to run it on demand.</p>
<input id=compactKB type=number min=0 max=512>
<label>Concurrent connections <button class=qh type=button aria-expanded=false aria-label="About concurrent connections">?</button></label>
<p class="hint tip">One is the default and the stable choice &mdash; the device does one secure connection at a time, which uses the least memory. Two lets voice transcription run alongside a provider turn, but needs memory headroom and can fail a heavy turn. Applies after restart.</p>
<select id=tlsSlots><option value=1>1 (default) &mdash; one at a time</option><option value=2>2 &mdash; concurrent (needs headroom)</option></select>
<label class=pr style="margin-top:8px"><input type=checkbox id=tlsVerify checked> Validate provider TLS certificates <button class=qh type=button aria-expanded=false aria-label="About TLS validation">?</button></label>
<p class="hint tip">Checks each provider's certificate against a built-in bundle, so a hostile network can't impersonate a provider and capture your keys. Turn off only for a self-hosted server with a self-signed certificate.</p>
<label style="margin-top:8px">Capability validation <button class=qh type=button aria-expanded=false aria-label="About capability validation">?</button></label>
<p class="hint tip">How the device confirms a provider key actually works before it tells the assistant a connector is available &mdash; so it relies on tested capabilities, not guesses. Off trusts the key as-is. Passive marks a provider verified once a real call or a manual check succeeds. Active also re-checks on a schedule so the status stays current.</p>
<select id=capProbe onchange=capProbeUpd()><option value=1>Passive (default)</option><option value=0>Off</option><option value=2>Active &mdash; re-check on a schedule</option></select>
<div id=capProbeActive style="display:none;margin-top:6px">
<label>Re-check every, hours</label>
<input id=capProbeH type=number min=1 max=168 onchange=capProbeUpd()>
<p class="hint" id=capProbeCost></p>
</div>
</div>
</details>

<div class=row style="margin-top:10px"><button id=orchsave type=button>Save Changes</button></div>
<p id=orchmsg class=hint></p>
</div>
</div>

<div class=subpane id=subpane-skills style="display:none">
<div class=sec>
<h2>Skills <button class=qh type=button aria-expanded=false aria-label="About skills">?</button></h2>
<p class="hint tip">Saved instruction sets the assistant applies when it starts a matching session.</p>
<p class=hint id=skStat>&mdash;</p>
<div id=skList class=hint>loading&hellip;</div>
<label style="margin-top:10px">Skill ID <span class=hint>(a-z 0-9 - _ &middot; max 23 chars)</span></label>
<div class=row><input id=skId placeholder="deep-research" style="max-width:200px"><button id=skLoad type=button>Load</button></div>
<textarea id=skMd rows=8 spellcheck=false placeholder="---&#10;title: My skill&#10;inject: spawn&#10;---&#10;Instructions the session receives when it starts with this skill."></textarea>
<div class=row><button id=skSave type=button>Save</button><button id=skDel type=button style="background:#333;color:#cde">Delete</button><button class=qh type=button aria-expanded=false aria-label="About skill storage">?</button></div>
<p class="hint tip">Skills live on the SD card at <span class=badge>/mem/skills/&lt;id&gt;/SKILL.md</span>. Front matter: <span class=badge>title</span>, optional <span class=badge>version</span> and <span class=badge>inject: spawn|context|both</span>. Only you can write skills &mdash; the assistant reads them with <span class=badge>skill.get</span> and receives them when a session names the skill.</p>
<p class=hint id=skMsg></p>
</div>
<div class=sec>
<div id=skillspanel></div>
</div>
</div>

</div>

)=====";
