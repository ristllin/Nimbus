#pragma once
#include <Arduino.h>

// ui_device - Dashboard pane + the full Settings pane (Phase 3 C1 IA).
// Settings carries EVERY device control (no-regression rail, PRD §4a): mode,
// identity + access token, battery mode + themes + the live ring simulator,
// customization, audio diagnostics + sound effects (incl. the Notifier level),
// battery, and Connectivity (folded in from the retired ui_wifi.h: reach info,
// Bluetooth bonds, Wi-Fi join, token rotation, factory reset). All element IDs
// are unchanged so ui_js.h's wiring works as-is. Copy follows the AGENTS.md
// copy style guide: labels are plain nouns, rationale lives in .hint.tip
// blocks behind the tap-? affordance, danger warnings stay visible.

static const char UI_DEVICE[] PROGMEM = R"=====(<div class=pane id=pane-dash>
<div class=eyebrow>Overview</div>
<div class=ptitle>Home</div>
<p class=plede>Health, activity, and anything that needs your attention.</p>
<div id=devtiles class=tiles></div>
<div class=sec>
<h2>Quick actions</h2>
<div class=row style="flex-wrap:wrap;gap:8px">
<button type=button data-go=chat>Open Chat</button>
<button type=button data-go=memory>Add a file</button>
<button type=button data-go=assistant>Providers &amp; models</button>
<button type=button data-go=device>Check for updates</button>
<button type=button id=homeRestart>Restart</button>
<button type=button id=homePowerOff>Power off</button>
</div>
</div>
<div class=sec id=whatNext style="display:none;border-color:var(--teal)">
<h2>What next</h2>
<p class=hint>You're all set up. A few good next steps:</p>
<div class=row style="flex-wrap:wrap;gap:8px">
<button type=button data-go=chat>Say hello in Chat</button>
<button type=button data-go=assistant>Add more providers or tools</button>
<button type=button data-go=device>Pair with the cloud</button>
</div>
<div class=row style="margin-top:8px"><button id=whatNextDismiss type=button style="background:var(--raise3);color:var(--ink2)">Dismiss</button></div>
</div>
<div class=info id=info>loading&hellip;</div>
<div class=sec>
<h2>Active sessions</h2>
<div id=dashJobs class=hint>none</div>
</div>
<div id=healthpanel class=sec></div>
</div>

<div class=pane id=pane-set style="display:none">
<div class=eyebrow>Device</div>
<div class=ptitle>Settings</div>
<p class=plede>Mode, light, sound, power, and connectivity - all in one place.</p>

<details class=setgroup><summary>Mode &amp; identity<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<label>Mode <button class=qh type=button aria-expanded=false aria-label="About modes">?</button></label>
<p class="hint tip"><b>Notifier</b> turns the ring into a status light for your coding sessions, connected over Bluetooth. <b>Orchestrator</b> runs the AI assistant - Telegram, voice, and memory. Switching modes restarts the device.</p>
<select id=mode>
<option value=0>Notifier - status light</option>
<option value=1>Orchestrator - AI assistant</option>
</select>
<label>Device name <button class=qh type=button aria-expanded=false aria-label="About the device name">?</button></label>
<p class="hint tip">Used for the setup Wi-Fi network (<b><span id=idApSsid>&hellip;</span></b>), the network address (<b><span id=idMdns>&hellip;</span></b>), Bluetooth, and what the assistant calls itself. Leave blank to name it automatically. Takes effect after restart.</p>
<div class=row><input id=devName placeholder="Nimbus" maxlength=24><button id=devNameSave type=button>Save</button></div>
<label>Timezone <button class=qh type=button aria-expanded=false aria-label="About the timezone">?</button></label>
<p class="hint tip">Sets when daily and weekly routines - including nightly memory upkeep - fire. POSIX format: pick a suggestion or type one, e.g. <b>GMT0BST,M3.5.0/1,M10.5.0/2</b> for the UK. Blank = UTC. Applies immediately - check the clock below after saving.</p>
<div class=row><input id=devTz list=tzlist placeholder="UTC0" maxlength=48><button id=devTzSave type=button>Save</button></div>
<datalist id=tzlist>
<option value="UTC0">UTC</option>
<option value="GMT0BST,M3.5.0/1,M10.5.0/2">UK</option>
<option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe</option>
<option value="IST-2IDT,M3.4.4/26,M10.5.0">Israel</option>
<option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option>
<option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
<option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option>
<option value="IST-5:30">India</option>
<option value="JST-9">Japan</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Sydney</option>
</datalist>
<label>Device clock</label>
<p class="hint tip">Set automatically from the internet once Wi-Fi connects - there is no manual clock. Until it syncs, daily and weekly routines wait.</p>
<div class=row><b id=devClock>&hellip;</b><span class=hint id=clockBadge>&hellip;</span><button id=clockSyncBtn type=button>Sync now</button></div>
<div id=idTokenRow>
<label>Device sign-in code <button class=qh type=button aria-expanded=false aria-label="About the device sign-in code">?</button></label>
<p class="hint tip">The Sign-in QR carries this automatically; normal setup never asks you to type it. Use this value only to recover a browser that cannot scan the QR. Tap to copy. Generate a new one under <b>Connectivity</b> below.</p>
<div class="memv" id=idToken style="cursor:pointer" title="tap to copy">&hellip;</div>
</div>
</div>
</details>

<details class=setgroup><summary>Display<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class=hint>This device uses a touch screen.</p>
<label class=pr><input type=checkbox id=scrFlip> Display flip <button class=qh type=button aria-expanded=false aria-label="About display flip">?</button></label>
<p class="hint tip">Turns the screen 180 degrees for an upside-down mount. Touch display only; takes effect right away.</p>
<div id=tchCalWrap>
<label>Touch calibration <button class=qh type=button aria-expanded=false aria-label="About touch calibration">?</button></label>
<p class="hint tip">Only for the touch display. Each panel reads slightly differently, so if taps land off-target, enter the corner readings as <b>minX,maxX,minY,maxY</b> - optionally a fifth number to flip axes (1 swaps X and Y, 2 flips X, 4 flips Y; add them together). Leave blank for the defaults. Applies immediately.</p>
<div class=row><input id=tchCal placeholder="200,3900,240,3850" maxlength=32><button id=tchCalSave type=button>Save</button></div>
</div>
<div id=tOrient style="display:none"><label>Touch orientation</label>
<p class="hint tip">This screen self-calibrates, so there is nothing to measure. If taps land in the wrong place, toggle these until a tap lands where you touch. Applies immediately.</p>
<div class=row style="gap:16px;flex-wrap:wrap"><label class=pr><input type=checkbox id=tSwap> Swap X and Y</label><label class=pr><input type=checkbox id=tFlipX> Flip X</label><label class=pr><input type=checkbox id=tFlipY> Flip Y</label></div></div>
<label>Theme <button class=qh type=button aria-expanded=false aria-label="About themes">?</button></label>
<p class="hint tip">Each theme is a family of colors. A session's status picks its color role and motion - the ring's status language. The legend below shows the mapping.</p>
<div id=themeChips></div>
<button id=prevBtn type=button style="margin-top:6px">Demo on Device</button>
<label>Preview <button class=qh type=button aria-expanded=false aria-label="About the preview">?</button></label>
<p class="hint tip">Pick a status and mode to see the pattern in the selected theme. <b>Demo on Device</b> (above) plays it on the physical ring for a few seconds - nothing is saved.</p>
<div id=ringsimwrap style="display:flex;flex-direction:column;align-items:center;gap:10px;margin:6px 0 2px">
<canvas id=ringsim width=440 height=440 style="width:220px;height:220px"></canvas>
<div id=ringsimStatus style="display:flex;gap:4px;flex-wrap:wrap;justify-content:center"></div>
<div id=ringsimPosture style="display:flex;gap:4px;justify-content:center"></div>
</div>
<div id=statusLegend></div>
</div>
</details>

<details class=setgroup id=battModeGroup><summary>Battery mode<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<div id=profiles>
<label class=pr><input type=radio name=profile value=0> Dark</label>
<label class=pr><input type=radio name=profile value=1> Balanced</label>
<label class=pr><input type=radio name=profile value=2> Full</label>
<button class=qh type=button aria-expanded=false aria-label="About battery modes">?</button>
</div>
<p class="hint tip">The battery mode sets the light. <b>Dark</b>: lights off - only a job error breathes red. <b>Balanced</b>: a single soft cue in the theme color, dimmer, shorter holds. <b>Full</b>: every session a color arc at full brightness. Each battery mode is a preset you can adjust under Customize battery mode.</p>
<label class=pr style="margin-top:8px"><input type=checkbox id=lbRing> Low-battery light <button class=qh type=button aria-expanded=false aria-label="About the low-battery light">?</button></label>
<p class="hint tip">Shows a dim red pulse on the ring for a few seconds each minute while the battery is low. Off by default, because a ring lit all night uses the power it is warning about. The screen notice and the Telegram message are sent either way.</p>
<label class=pr><input type=checkbox id=lbSaver> Save power when low <button class=qh type=button aria-expanded=false aria-label="About saving power when low">?</button></label>
<p class="hint tip">Switches to the Dark battery mode while the battery is low, then returns to the chosen mode once it recovers. On by default.</p>
<label class=pr><input type=checkbox id=battMon> Monitor the battery <button class=qh type=button aria-expanded=false aria-label="About battery monitoring">?</button></label>
<p class="hint tip">Reads the battery pack for the charge readout, low-battery warnings, and sleep protection. On boards built with a pack it is on; on the all-in-one board a battery is optional, so it is off until you turn it on. Takes effect after a restart.</p>
<div class=row id=battRestartRow style="display:none;margin-top:6px"><button type=button id=battRestart>Restart now</button><span class=hint style="align-self:center">Applies the battery monitor change.</span></div>
<p class=hint id=effprof></p>
</div>
</details>

<details class=setgroup id=custProfGroup><summary>Customize battery mode: <span id=custProfName>Balanced</span><span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class=hint>Each value starts at the selected battery mode's default. Set overrides it; Reset returns it.</p>
<div id=params></div>
<div class=row style="margin-top:10px"><button id=revertProf type=button>Revert to Defaults</button></div>
<p class=hint id=revertMsg></p>
</div>
</details>

<details class=setgroup><summary>Sound<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<div class=row><button id=micBtn type=button>Mic Meter</button><button id=beepBtn type=button>Speaker Tone</button><button id=lbBtn type=button>Loopback Test</button><button class=qh type=button aria-expanded=false aria-label="About audio tests">?</button></div>
<p class="hint tip">Checks the microphone and speaker: a live mic level, a speaker tone, and a loopback that plays a tone and listens for it.</p>
<div style="height:16px;background:var(--raise2);border:1px solid var(--line2);border-radius:6px;margin:9px 0;overflow:hidden"><div id=vubar style="height:100%;width:0;background:linear-gradient(90deg,#3a7,#7fd1c8);transition:width .1s"></div></div>
<p id=audiomsg class=hint>Idle.</p>
<b style="display:block;margin-top:12px">Sound effects <span class="badge ext" id=sfxtier></span> <button class=qh type=button aria-expanded=false aria-label="About sound effects">?</button></b>
<p class="hint tip">Sound cues confirm device events. <b>Off</b> is silent; <b>Low</b> plays alerts only; <b>Medium</b> adds connectivity and assistant milestones; <b>High</b> plays everything. The full set syncs to the SD card; a built-in set always works without it.</p>
<label>In Orchestrator mode</label>
<select id=sfxLvlO><option value=0>Off</option><option value=1>Low</option><option value=2>Medium (default)</option><option value=3>High</option></select>
<label>In Notifier mode</label>
<select id=sfxLvlN><option value=0>Off (default)</option><option value=1>Low</option><option value=2>Medium</option><option value=3>High</option></select>
<label>Sound theme</label>
<select id=sfxTheme><option value=pulse>Pulse</option></select>
<label>Volume <span id=sfxVolPct class=hint style="font-weight:normal"></span> <button class=qh type=button aria-expanded=false aria-label="About volume">?</button></label>
<p class="hint tip">Levels near maximum can distort on the built-in speaker.</p>
<input id=sfxVol type=range min=0 max=100 step=5 style="width:100%">
<div class=row style="margin-top:8px"><select id=sfxSlug style="flex:1">
<option value=boot>Startup</option><option value=wifi_up>Wi-Fi connected</option><option value=wifi_down>Wi-Fi lost</option>
<option value=ble_up>Bluetooth connected</option><option value=ble_down>Bluetooth disconnected</option><option value=ble_bond>Bluetooth paired</option>
<option value=agent_spawn>Task started</option><option value=agent_done>Task finished</option>
<option value=error>Error</option><option value=needs_you>Needs your attention</option>
<option value=low_battery>Low battery</option><option value=battery_ok>Battery recovered</option>
<option value=mode_switch>Mode switch</option><option value=turn_start>Assistant thinking</option>
<option value=reply_sent>Reply sent</option><option value=voice_stop>Voice capture ended</option>
<option value=mem_saved>Memory saved</option><option value=ask_cleared>Question dismissed</option>
<option value=sync_done>Sound sync finished</option>
<option value=sd_mounted>SD card mounted</option><option value=sd_lost>SD card lost</option>
<option value=voice_listen>Listening</option>
<option value=net_degraded>Network degraded</option><option value=net_ok>Network restored</option>
</select><button id=sfxPlay type=button>Play</button></div>
</div>
</details>

<details class=setgroup id=battsec style="display:none"><summary>Battery<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<div style="height:20px;background:var(--raise2);border:1px solid var(--line2);border-radius:6px;margin:10px 0 6px;overflow:hidden;position:relative">
<div id=battbar style="height:100%;width:0;background:linear-gradient(90deg,#3a7,#7fd1c8);transition:width .3s"></div>
<span id=battpct style="position:absolute;left:8px;top:2px;font-size:12px;color:#eee">-</span></div>
<table><tbody>
<tr><td>Voltage</td><td id=battmv>-</td></tr>
<tr><td>Estimated time left</td><td id=batttime>-</td></tr>
<tr><td>Discharge rate</td><td id=battrate>-</td></tr>
<tr><td>Health</td><td id=batthealth>-</td></tr>
<tr><td>Power source</td><td id=battsrc>-</td></tr>
<tr><td>Calibration</td><td id=battcal>-</td></tr>
</tbody></table>
<div class=row><button id=battcalBtn type=button>Calibrate Full Charge</button><button class=qh type=button aria-expanded=false aria-label="About calibration">?</button></div>
<div class=hint id=battcalMsg></div>
<p class="hint tip">The voltage sensor reads a full pack low. With the battery fully charged, calibrate to anchor 100% to the current reading - stored per device.</p>
<table><tbody>
<tr><td>Low-battery sleep</td><td><input id=sleepMv type=number min=0 max=6800 step=50 style="width:90px"> mV <button class=qh type=button aria-expanded=false aria-label="About low-battery sleep">?</button><p class="hint tip">Below this pack voltage the device sleeps to protect the battery. Default 6000 mV, about 10% charge. 0 turns protection off.</p></td></tr>
<tr><td>Stay awake above</td><td><input id=wakeMv type=number min=0 max=7600 step=50 style="width:90px"> mV <button class=qh type=button aria-expanded=false aria-label="About the wake threshold">?</button><p class="hint tip">After waking, the device stays on only above this voltage. Lower values allow deeper drain cycles; 7200 mV stops strictly at 10%.</p></td></tr>
<tr><td>Skip low-battery sleep</td><td><label><input id=sleepOvr type=checkbox> override</label> <span class=hint>&#9888; Allows discharge below the safe floor, which can permanently damage the battery. Resets at restart.</span></td></tr>
<tr><td>Full brightness</td><td><label><input id=brightOvr type=checkbox> allow 100%</label> <span class=hint>&#9888; Can overheat and damage the device. The thermal guard stays active. Resets at restart.</span></td></tr>
</tbody></table>
<p class=hint style="margin-top:12px"><b>Battery hardware</b> - match these to the pack and sense resistors actually fitted, so voltage and estimates are correct.</p>
<table><tbody>
<tr><td>Pack capacity</td><td><input id=battCapMah type=number min=100 max=20000 step=50 style="width:90px"> mAh <button class=qh type=button aria-expanded=false aria-label="About pack capacity">?</button><p class="hint tip">The fitted pack: LiitoKala 3500, a reclaimed ~500 mAh cell, and so on. Drives time-left and the capacity readout.</p></td></tr>
<tr><td>Chemistry</td><td><select id=battChem style="width:150px"><option value=liion>Li-ion / LiPo</option><option value=lifepo4>LiFePO4</option></select> <button class=qh type=button aria-expanded=false aria-label="About battery chemistry">?</button><p class="hint tip">Which discharge curve to use. Li-ion runs about 4.2 to 3.0 V per cell; LiFePO4 sits near 3.2 to 3.3 V for most of its life, so its percent is coarser. Pick the one printed on your cell.</p></td></tr>
<tr><td>Cells in series</td><td><select id=battCells style="width:150px"><option value=0>Board default</option><option value=1>1S (one cell)</option><option value=2>2S (two cells)</option></select></td></tr>
<tr><td>Custom curve</td><td><input id=battCurve type=text placeholder="mv:pct,mv:pct,..." style="width:200px"> <button class=qh type=button aria-expanded=false aria-label="About the custom curve">?</button><p class="hint tip">Advanced, optional. Per-cell resting points, highest voltage first, e.g. 4200:100,3700:50,3200:0. Leave blank to use the chemistry curve.</p></td></tr>
<tr><td>Sense resistor (top)</td><td><input id=battRtop type=number min=1000 max=10000000 step=1000 style="width:110px"> &#8486; <button class=qh type=button aria-expanded=false aria-label="About the sense resistors">?</button><p class="hint tip">The two divider resistors between the pack and the ADC pin. Defaults 220k / 100k; some boards use 270k / 120k. Getting these right fixes the voltage reading. After changing them, re-run Calibrate on a full pack.</p></td></tr>
<tr><td>Sense resistor (bottom)</td><td><input id=battRbot type=number min=1000 max=10000000 step=1000 style="width:110px"> &#8486;</td></tr>
</tbody></table>
<div class=row><button id=protSave type=button>Save</button></div>
<p class=hint id=batthint>Estimates improve over the first few charge cycles.</p>
</div>
</details>

<details class=setgroup id=fwsec><summary>Software update<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<table><tbody>
<tr><td>Installed</td><td id=fwCur>-</td></tr>
<tr><td>Latest</td><td id=fwLatest>-</td></tr>
<tr><td>Status</td><td id=fwState>-</td></tr>
<tr><td>Last result</td><td id=fwLast>-</td></tr>
</tbody></table>
<div class=warnbox id=fwBatt style="display:none;margin:8px 0"></div>
<div id=fwBarWrap style="display:none;height:14px;background:var(--raise2);border:1px solid var(--line2);border-radius:6px;margin:8px 0;overflow:hidden"><div id=fwBar style="height:100%;width:0;background:linear-gradient(90deg,#3a7,#7fd1c8);transition:width .5s"></div></div>
<div class=row><button id=fwCheck type=button>Check for Updates</button><button id=fwInstall type=button style="display:none">Install Update</button><button class=qh type=button aria-expanded=false aria-label="About updates">?</button></div>
<div class=hint id=fwMsg></div>
<p class="hint tip">Updates download from this project's GitHub releases over TLS and are cryptographically signed - the device verifies each one and reverts on its own if the new version fails to start. Keep the device powered during an install.</p>
<label class=pr style="margin-top:8px"><input type=checkbox id=autoUpd> Automatic updates <button class=qh type=button aria-expanded=false aria-label="About automatic updates">?</button></label>
<p class="hint tip">Installs new firmware when the device is idle and charged, then restarts.</p>
</div>
</details>

<details class=setgroup><summary>Connectivity<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<b style="display:block;margin-top:10px">This device <button class=qh type=button aria-expanded=false aria-label="About connecting">?</button></b>
<p class="hint tip">To return later, open the <b>On your network</b> address, or scan the Sign-in QR on the device's screen (Settings &gt; Connectivity &gt; Sign-in QR). The sign-in code appears only after you've signed in. Full walk-through: <a href="https://ristllin.github.io/Nimbus/docs/getting-started/first-time-setup" target=_blank rel=noopener>First-time setup</a>.</p>
<table><tbody>
<tr><td>Device name</td><td id=cxName>-</td></tr>
<tr id=cxLanRow><td>On your network</td><td><span id=cxLan>-</span> <button id=cxLanCopy type=button style="display:none;padding:2px 8px;font-size:12px">Copy</button></td></tr>
<tr id=cxTokenRow><td>Device sign-in code</td><td id=cxToken style="word-break:break-all">-</td></tr>
</tbody></table>
<div id=cxSetupAp style="display:none">
<p class=hint>Setting up a new device? Join its setup Wi-Fi network with this password and the setup page opens automatically.</p>
<table><tbody>
<tr><td>Setup Wi-Fi network</td><td id=cxApSsid>-</td></tr>
<tr><td>Setup Wi-Fi password</td><td id=cxApPass>-</td></tr>
</tbody></table>
</div>
<div class=row style="margin-top:6px"><button id=regenTok type=button style="background:rgba(240,104,122,.12);color:var(--crit)">Generate New Code</button></div>

<div id=wifiGroup>
<b style="display:block;margin-top:14px">Wi-Fi <button class=qh type=button aria-expanded=false aria-label="About Wi-Fi">?</button></b>
<p class="hint tip">Connects the device to your network - required for Orchestrator mode. Notifier over Bluetooth works without it. 2.4 GHz networks only.</p>
<div id=wifiConnState class=hint style="margin:4px 0 8px">-</div>
<label>Saved Wi-Fi networks <span id=wifiCount class=hint style="font-weight:normal"></span></label>
<p class=hint>The device remembers several networks and joins whichever one it can see, so carrying it between places keeps it online.</p>
<div id=wifiKnown></div>
<div class=row style="margin-top:6px"><button id=scan type=button>Scan Networks</button></div>
<div id=nets></div>
<p id=msg></p>
<details class=setgroup><summary>Add a hidden network<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class=hint>A visible network is added from the scan list above. Enter the name and password only for a network that does not broadcast its name.</p>
<input id=ssid placeholder="Network name">
<input id=pass type=password placeholder="Password">
<button id=savewifi type=button>Save</button>
</div>
</details>
<details class=setgroup id=wifiRecovery><summary>Recovery<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class="hint tip">The setup hotspot is a recovery network, separate from your home Wi-Fi. Touch/TFT devices normally turn it off after joining home Wi-Fi. Publishing it pauses joining and makes the recovery network available; resume joining once the password is corrected.</p>
<div class=row><button id=wifiAp type=button>Publish Setup Network</button><button id=wifiResume type=button>Resume Joining</button></div>
<p class=hint id=wifiApMsg></p>
</div>
</details>
</div>

<div id=btGroup>
<b style="display:block;margin-top:14px">Bluetooth <button class=qh type=button aria-expanded=false aria-label="About Bluetooth">?</button></b>
<p class="hint tip">In Notifier mode, the ring and screen are driven over an encrypted Bluetooth link from the nimbus-notify broker on your computer. Pairing happens automatically on the broker's first connect - Nimbus won't appear in your computer's Bluetooth list. Bluetooth is off in Orchestrator mode.</p>
<div id=btOrchLine class=hint style="display:none">Bluetooth is off in Orchestrator mode.</div>
<div id=btTable style="display:none">
<table><tbody>
<tr><td>Status</td><td id=btState>-</td></tr>
<tr><td>Paired devices</td><td id=btBonds>-</td></tr>
</tbody></table>
<div class=row style="margin-top:6px"><button id=btForget type=button style="background:rgba(240,104,122,.12);color:var(--crit)">Forget Paired Devices</button></div>
<details class=setgroup><summary>Advanced<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<table><tbody>
<tr><td>Bluetooth address</td><td id=btMac style="word-break:break-all">-</td></tr>
</tbody></table>
</div>
</details>
</div>
</div>
</div>
</details>

<details class=setgroup><summary>Cloud access<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class="hint tip" id=cloudTip>Reach this device from anywhere through CumuloNimbus. Available in Orchestrator mode. Pairing shows a Cloud link code to enter at app.cumulo-nimbus.ai while signed in.</p>
<div id=cloudLine class=hint>Cloud access is off.</div>
<div id=cloudPairCard style="display:none;margin:10px 0;padding:14px;border:1px solid var(--line);border-radius:14px;background:var(--raise2)">
<div style="display:flex;gap:18px;flex-wrap:wrap;align-items:flex-start">
<div id=cloudQr style="width:168px;height:168px;background:#fff;border-radius:12px;padding:10px;flex:none"></div>
<div style="min-width:200px;flex:1">
<div class=eyebrow>Cloud link code</div>
<div id=cloudCode style="font-family:var(--mono);font-size:30px;font-weight:730;letter-spacing:.08em;color:var(--ink);word-break:break-all;line-height:1.2"></div>
<div class=row style="margin-top:8px"><button id=cloudCopy type=button>Copy code</button></div>
<ol class=hint style="margin:12px 0 0;padding-left:18px;line-height:1.7">
<li>Sign in at <b>app.cumulo-nimbus.ai</b>.</li>
<li>Scan the QR, or enter the Cloud link code above.</li>
<li>This device appears in your account once it links.</li>
</ol>
</div>
</div>
</div>
<div class=row style="margin-top:6px">
<button id=cloudPair type=button>Pair with the cloud</button>
<button id=cloudUnpair type=button style="display:none">Unpair</button>
<button id=cloudOff type=button style="display:none">Turn off</button>
</div>
<div class=hint id=cloudMsg></div>
</div>
</details>

<details class=setgroup><summary>Power<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<p class=hint>Restart the device, or turn it off to save power. Either way it keeps its settings.</p>
<div class=row style="margin-top:6px"><button id=deviceRestart type=button>Restart&hellip;</button><button class=qh type=button aria-expanded=false aria-label="About restart">?</button></div>
<p class="hint tip">Restarts the device and brings it back on its own in about a minute. Use it to apply a setting that says it takes effect after a restart. Nothing is erased.</p>
<div class=row style="margin-top:6px"><button id=powerOff type=button>Power Off&hellip;</button><button class=qh type=button aria-expanded=false aria-label="About power off">?</button></div>
<p class="hint tip">Saves everything, turns the screen off, and puts the device into deep sleep. How it comes back on depends on the model: touch models wake when you tap the screen; others wake when you reconnect power.</p>
</div>
</details>

<details class=setgroup><summary style="color:var(--crit)">Danger zone<span class=chev>&rsaquo;</span></summary>
<div class=setbody>
<b style="display:block;margin-top:6px;color:var(--crit)">Erase storage</b>
<div class=row style="margin-top:6px"><button id=sdReset type=button style="background:rgba(240,104,122,.12);color:var(--crit)">Erase Storage&hellip;</button></div>
<p class=hint style="color:var(--crit)">Deletes everything on the SD card (all memories, conversation history, saved files, and media), then restarts. Wi-Fi, keys, and settings are kept. This can take up to a minute.</p>
<div id=sdFormatRow style="display:none"><div class=row style="margin-top:6px"><button id=sdFormat type=button style="background:rgba(240,104,122,.12);color:var(--crit)">Format Card&hellip;</button></div>
<p class=hint style="color:var(--crit)">Reformats the whole card, not only the assistant's data. Use this only if the card looks corrupted. It asks for its own typed confirmation.</p></div>
<b style="display:block;margin-top:14px;color:var(--crit)">Factory reset</b>
<div class=row style="margin-top:6px"><button id=factoryReset type=button style="background:rgba(240,104,122,.12);color:var(--crit)">Factory Reset&hellip;</button></div>
<div class=row style="margin-top:6px"><label><input type=checkbox id=factoryEraseSd> Also erase the SD card</label></div>
<p class=hint style="color:var(--crit)">Erases <b>everything</b> (Wi-Fi, API keys, the Telegram list, Bluetooth pairings, themes, sound settings, memory, the device name, and the device sign-in code), then restarts into first-time setup as a fresh device. This takes a few seconds, or up to a minute when erasing the card.</p>
</div>
</details>

</div>

)=====";
