#pragma once
#include <Arduino.h>

// ui_shell - page head, the Command Center design system, and the sidebar shell
// (Phase 3 C1: the mockup design - scratchpad/mock_v2.html / docs/webui-prd.md -
// applied to the REAL wired page). Desktop: fixed left sidebar (brand + mode
// switch + 8-area nav). Phone: the same nav docks as a bottom tab bar. Every
// legacy class the JS builds against (.sec .info .badge .vfy .provrow .net .bars
// .memv .warnbox .tiles/.tile .modeswitch …) is restyled here, not renamed, so
// ui_js.h keeps working unchanged. Fragment order lives in web_pages.h;
// tools/webui_concat_check.py guards the concatenation.

static const char UI_SHELL[] PROGMEM = R"=====(<!doctype html>
<meta name=viewport content="width=device-width,initial-scale=1">
<meta charset=utf-8>
<title>Nimbus</title>
<link rel=icon type="image/svg+xml" href=/logo.svg>
<style>
:root{
--bg:#141518;--raise:#1c1e23;--raise2:#232630;--raise3:#2a2e37;
--line:#2a2d36;--line2:#363b46;
--ink:#eceef2;--ink2:#a7adba;--ink3:#6f7684;
--teal:#5ad6c4;--teal-d:#2ea394;--teal-soft:rgba(90,214,196,.12);
--amber:#f0b45a;--amber-soft:rgba(240,180,90,.13);
--warm:#f0947a;--warm-soft:rgba(240,148,122,.12);
--ok:#63d19a;--warn:#eab54a;--crit:#f0687a;--info:#6cb8ff;
--mono:ui-monospace,"SF Mono",Menlo,Consolas,monospace;
--shadow:0 1px 0 rgba(255,255,255,.02),0 10px 34px rgba(0,0,0,.30);
}
*{box-sizing:border-box}
html,body{overflow-x:hidden}
/* Fluid, centered content column that caps at 1,280 px (CUM-25). The fixed
   sidebar sits at the viewport's left edge; the content box centers in the space
   beside it and stops growing at 1,280 px on wide screens. */
body{font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;background:var(--bg);color:var(--ink);
margin:0 auto;padding:20px 30px 40px 254px;max-width:1280px;font-size:14.5px;line-height:1.55;-webkit-font-smoothing:antialiased}
/* Robust anti-overflow: long URLs/JSON/code + wide content can never push the page
   sideways. Break within any element; scroll wide code/tables inside their own box. */
code,.memv,#idToken,#cxToken,#btMac{overflow-wrap:anywhere;word-break:break-word}
pre{max-width:100%;white-space:pre-wrap;overflow-wrap:anywhere}
.sec,.provrow,.setbody,.tile,.info{min-width:0;overflow-wrap:anywhere}
.sec table,.setbody table{table-layout:fixed;width:100%}
.sec td,.setbody td{overflow-wrap:anywhere}
.sec pre,.setbody pre{overflow-x:auto}
/* Flex rows: children default to min-width:auto and REFUSE to shrink below their
   content, which is what shoves a long <code>/<textarea> past the border. Letting
   every flex child shrink to 0 makes the wrap rules above actually take effect -
   the structural guarantee so no single component can ever push the page sideways. */
.row{min-width:0;flex-wrap:wrap}.row>*{min-width:0;max-width:100%}
p code{overflow-wrap:anywhere;word-break:break-word}
/* Pane entry: a subtle rise on tab switch (mock parity) - panes toggle display in JS,
   so the animation re-fires each time one is shown. Respects reduced-motion. */
@keyframes rise{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.pane{animation:rise .24s ease}
@media(prefers-reduced-motion:reduce){.pane{animation:none}}
h1{color:var(--teal);font-size:18px}
h2{color:var(--ink);font-size:14.5px;margin:0 0 8px;font-weight:650}
a{color:var(--teal);text-decoration:none}
label{font-size:12px;color:var(--ink3);display:block;margin-top:12px;font-weight:600;text-transform:uppercase;letter-spacing:.04em}
input,select,textarea{width:100%;box-sizing:border-box;padding:9px 11px;margin:4px 0;border-radius:10px;border:1px solid var(--line2);background:var(--raise2);color:var(--ink);font-size:14px;font-family:inherit}
input:focus,select:focus,textarea:focus{outline:2px solid var(--teal-soft);border-color:var(--teal)}
button{padding:8px 14px;border:0;border-radius:10px;background:var(--teal);color:#052420;font-weight:650;font-size:13.5px;cursor:pointer}
button:hover{filter:brightness(1.06)}
.memv{background:var(--raise2);border:1px solid var(--line);border-radius:10px;padding:10px;font-size:12px;white-space:pre-wrap;word-break:break-word;color:var(--ink2);min-height:2em;font-family:var(--mono)}
.vfy{margin-left:8px}.vfy.ok{background:rgba(99,209,154,.14);color:var(--ok)}.vfy.bad{background:rgba(240,104,122,.14);color:var(--crit)}.vfy.unk{background:var(--raise3);color:var(--ink2)}
.provrow{border-top:1px solid var(--line);padding-top:12px;margin-top:12px}
.provhead{display:flex;align-items:center;gap:8px}
table{width:100%;border-collapse:collapse;font-size:12.5px}td,th{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);color:var(--ink2)}th{color:var(--ink3);font-size:10.5px;text-transform:uppercase;letter-spacing:.06em}
.info{font-size:13px;color:var(--ink2);border:1px solid var(--line);background:var(--raise);border-radius:14px;padding:14px;margin:10px 0;line-height:1.65}
.sec{border:1px solid var(--line);background:var(--raise);border-radius:16px;padding:16px;margin:14px 0;box-shadow:var(--shadow)}
.badge{display:inline-block;padding:1px 8px;border-radius:11px;font-size:11px;background:rgba(99,209,154,.14);color:var(--ok)}
.badge.ext{background:rgba(108,184,255,.13);color:var(--info)}
.hint{font-size:12px;color:var(--ink3);margin:2px 0 0}
.row{display:flex;align-items:center;gap:8px}
.row input{flex:1}
.row button{background:var(--raise3);color:var(--ink2);font-weight:550;padding:8px 11px}
.eff{font-size:12px;color:var(--ok)}
.ovr{color:var(--amber)}
.pr{display:inline-flex;align-items:center;gap:6px;margin-right:14px;font-size:14px;color:var(--ink)}
.pr input{width:auto}
.net{display:flex;justify-content:space-between;padding:10px;border:1px solid var(--line);border-radius:10px;margin:6px 0;cursor:pointer;background:var(--raise2)}
.net:hover{background:var(--raise3)}.bars{color:var(--ink3);font-size:13px}#nets{margin:8px 0}
#msg{font-size:13px;color:var(--ok)}
.warnbox{background:var(--amber-soft);border:1px solid rgba(240,180,90,.3);border-radius:10px;padding:10px;color:var(--amber);font-size:13px}
/* ---- Feedback-state system (CUM-31): pending -> result for every async action.
   One status element renders exactly one of pending/ok/none/error; nothing ever
   just disappears, and an error always names the next step. ---- */
[data-fb]{font-size:12.5px;margin:6px 0;line-height:1.5}
.fb-line{display:flex;align-items:center;gap:8px}
.fb-dot{width:8px;height:8px;border-radius:50%;flex:none;background:var(--ink3)}
.fb-pending .fb-dot{background:var(--info)}
.fb-ok .fb-dot{background:var(--ok)}
.fb-none .fb-dot{background:var(--ink3)}
.fb-error .fb-dot{background:var(--crit)}
.fb-pending .fb-msg{color:var(--ink2)}
.fb-ok .fb-msg{color:var(--ok)}
.fb-none .fb-msg{color:var(--ink2)}
.fb-error .fb-msg{color:var(--crit)}
.fb-bar{height:5px;border-radius:4px;background:var(--line);margin-top:7px;overflow:hidden}
.fb-bar>i{display:block;height:100%;border-radius:4px;background:var(--teal);transition:width .25s ease}
.fb-error .fb-bar>i{background:var(--crit)}
.fb-bar.fb-indet>i{width:35%;background:var(--info);animation:fbslide 1.1s ease-in-out infinite}
@keyframes fbslide{0%{margin-left:-35%}100%{margin-left:100%}}
@media(prefers-reduced-motion:reduce){.fb-bar.fb-indet>i{animation:none;width:100%;opacity:.4}}
/* Chat file drop zone (CUM-57): highlight while a file is dragged over it. */
#chatDrop.dropping{outline:2px dashed var(--teal);outline-offset:-6px;background:var(--teal-soft)}
/* ---- Global search (CUM-62): sidebar trigger + command palette ---- */
.searchbtn{display:flex;align-items:center;gap:9px;margin:0 4px 10px;padding:8px 11px;border-radius:11px;border:1px solid var(--line2);background:var(--raise2);color:var(--ink3);font-size:13px;font-weight:550;cursor:pointer;width:calc(100% - 8px)}
.searchbtn:hover{color:var(--ink);filter:none}
.searchbtn svg{width:16px;height:16px;stroke:currentColor;fill:none;stroke-width:1.8;flex:none}
.searchbtn kbd{margin-left:auto;font-size:10.5px;background:var(--raise3);border:1px solid var(--line2);border-radius:5px;padding:1px 5px;color:var(--ink3);font-family:var(--mono)}
#searchOverlay{position:fixed;inset:0;z-index:300;background:rgba(0,0,0,.45);display:flex;align-items:flex-start;justify-content:center;padding:12vh 16px 16px}
#searchBox{width:100%;max-width:640px;background:var(--raise);border:1px solid var(--line2);border-radius:16px;box-shadow:var(--shadow);overflow:hidden;display:flex;flex-direction:column;max-height:70vh}
#searchInput{border:0;border-bottom:1px solid var(--line);border-radius:0;background:transparent;color:var(--ink);font-size:16px;padding:15px 16px;margin:0}
#searchInput:focus{outline:none;border-color:var(--line)}
#searchResults{overflow-y:auto;padding:6px}
.sgroup{font-size:10.5px;text-transform:uppercase;letter-spacing:.08em;color:var(--ink3);font-weight:700;padding:10px 10px 4px}
.sresult{display:block;width:100%;text-align:left;border:0;background:transparent;color:var(--ink);padding:8px 10px;border-radius:9px;cursor:pointer;font-size:13.5px;font-weight:500}
.sresult:hover,.sresult.sel{background:var(--teal-soft);color:var(--teal);filter:none}
.sresult .scx{display:block;font-size:11.5px;color:var(--ink3);margin-top:1px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sresult.sel .scx{color:var(--teal-d)}
.sempty{padding:18px 12px;color:var(--ink3);font-size:13px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin:10px 0}
.tile{background:var(--raise);border:1px solid var(--line);border-radius:14px;padding:14px 15px;box-shadow:var(--shadow)}
.tile .k{font-size:10.5px;text-transform:uppercase;letter-spacing:.1em;color:var(--ink3);font-weight:650}
.tile .v{font-size:24px;font-weight:730;margin-top:3px;color:var(--ink);letter-spacing:-.01em;font-variant-numeric:tabular-nums}
.tile .v small{font-size:12px;color:var(--ink3);font-weight:400}
.tile .cx{font-size:11.5px;color:var(--ink2);margin-top:2px}
.tile .g{height:5px;border-radius:4px;background:var(--line);margin-top:8px;position:relative;overflow:hidden}
.tile .g i{display:block;height:100%;border-radius:4px}
.tile .g .mk{position:absolute;top:-1px;bottom:-1px;width:2px;background:var(--warn);opacity:.85}
.subtabs{display:flex;gap:5px;flex-wrap:wrap;margin:2px 0 14px;padding:5px;background:var(--raise2);border:1px solid var(--line);border-radius:12px}
.subtab{flex:1 1 auto;display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:8px 12px;border:0;border-radius:9px;font-size:13.5px;font-weight:600;color:var(--ink2);cursor:pointer;background:transparent;transition:.15s}
.subtab:hover{color:var(--ink);background:var(--raise3)}
.subtab.on{color:var(--ink);background:var(--raise);box-shadow:var(--shadow)}
.onbmb{flex:1;background:var(--raise2);color:var(--ink2);border:1px solid var(--line2)}
.onbmb.on{background:var(--teal);color:#00201c;border-color:var(--teal)}
.setgroup{border:1px solid var(--line);background:var(--raise);border-radius:16px;margin:12px 0;box-shadow:var(--shadow)}
.setgroup>summary{list-style:none;cursor:pointer;padding:14px 16px;font-weight:650;font-size:14.5px;display:flex;align-items:center;gap:10px}
.setgroup>summary::-webkit-details-marker{display:none}
.setgroup>summary .chev{margin-left:auto;color:var(--ink3);transition:.2s}
.setgroup[open]>summary .chev{transform:rotate(90deg)}
.setgroup .setbody{padding:2px 16px 16px;border-top:1px solid var(--line)}
.eyebrow{font-size:11px;text-transform:uppercase;letter-spacing:.12em;color:var(--teal);font-weight:700;margin:4px 0 2px}
.ptitle{font-size:21px;font-weight:700;margin:0 0 2px;letter-spacing:-.01em}
.plede{color:var(--ink2);font-size:13.5px;margin:0 0 14px;max-width:62ch}
/* ---- tap-? help: button.qh toggles the adjacent .hint.tip (AGENTS.md copy style guide) ---- */
/* vertical-align:middle (not a fixed px offset) so the icon centers correctly
   next to text at every font-size/line-height it's reused against - labels
   (12px), headers, and table cells all differ, and one fixed offset can't be
   right for all of them. */
button.qh{display:inline-flex;align-items:center;justify-content:center;width:18px;height:18px;padding:0;margin-left:6px;border-radius:50%;border:1px solid var(--line2);background:var(--raise2);color:var(--ink3);font-size:11px;font-weight:650;line-height:1;vertical-align:middle;flex:none;cursor:pointer}
button.qh:hover{color:var(--ink);border-color:var(--ink3);filter:none}
button.qh[aria-expanded=true]{background:var(--teal-soft);color:var(--teal);border-color:var(--teal)}
.hint.tip{display:none}
.hint.tip.open{display:block;margin:6px 0 2px}
/* ---- sidebar / bottom-nav shell ---- */
.side{position:fixed;top:0;left:0;bottom:0;width:224px;background:var(--raise);border-right:1px solid var(--line);
padding:16px 12px;display:flex;flex-direction:column;gap:3px;z-index:50}
.brand{display:flex;align-items:center;gap:10px;padding:4px 8px 12px}
.orb{width:34px;height:34px;border-radius:50%;flex:none;box-shadow:0 0 0 4px var(--teal-soft),0 0 16px -3px var(--teal);background:#fff}
.brand b{font-size:15px}
.brand .fw{font-size:10.5px;color:var(--ink3);font-weight:400;display:block;line-height:1.2}
.modeswitch{display:flex;background:var(--raise2);border:1px solid var(--line);border-radius:11px;padding:3px;gap:2px;margin:0 4px 10px}
.modeswitch button{flex:1;border:0;background:transparent;color:var(--ink3);font-size:11.5px;font-weight:650;padding:6px 4px;border-radius:8px}
.modeswitch button.on{background:var(--teal);color:#052420}
.modeswitch button.on.nf{background:var(--amber);color:#241500}
.tabs{display:flex;flex-direction:column;gap:2px}
.tab{display:flex;align-items:center;gap:11px;padding:9px 11px;border-radius:11px;border:0;background:transparent;color:var(--ink2);font-size:13.5px;font-weight:550;cursor:pointer;width:100%;text-align:left}
.tab svg{width:17px;height:17px;stroke:currentColor;fill:none;stroke-width:1.7;flex:none}
.tab:hover{background:var(--raise2);color:var(--ink);filter:none}
.tab.on{background:var(--teal-soft);color:var(--teal)}
.tab .tl{flex:1}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(20px);background:var(--teal);color:#052420;font-weight:650;padding:9px 18px;border-radius:20px;opacity:0;transition:opacity .2s,transform .2s;pointer-events:none;z-index:99}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
/* Icon rail (CUM-25): on narrower desktops/tablets the sidebar collapses to
   icons only, reclaiming width for the content. Labels return above 1080 px.
   The full nav is still keyboard-reachable; each button keeps its title tooltip. */
@media(min-width:861px) and (max-width:1080px){
.side{width:66px;padding:14px 8px;align-items:center}
.brand{justify-content:center;padding:4px 0 12px}
.brand b,.brand .fw{display:none}
.modeswitch{display:none}
.tabs .tl{display:none}
.searchbtn{justify-content:center;width:auto;margin:0 auto 10px;padding:9px}
.searchbtn .tl,.searchbtn kbd{display:none}
.tab{justify-content:center;padding:10px 0}
.tab svg{width:19px;height:19px}
body{padding-left:92px}
}
@media(max-width:860px){
body{padding:16px 12px calc(84px + env(safe-area-inset-bottom))}
.side{top:auto;right:0;bottom:0;left:0;width:auto;height:auto;border-right:0;border-top:1px solid var(--line);
flex-direction:row;align-items:center;padding:6px 4px calc(6px + env(safe-area-inset-bottom));gap:0;overflow-x:auto}
.brand{display:none}
.modeswitch{display:none}
.searchbtn{display:none}
.tabs{flex-direction:row;flex:1;justify-content:space-around;gap:0}
.tab{flex-direction:column;gap:3px;font-size:9.5px;padding:5px 7px;border-radius:9px;text-align:center;width:auto}
.tab svg{width:19px;height:19px}
.mobmode{display:flex !important}
}
.mobmode{display:none;position:fixed;top:10px;right:12px;z-index:60}
</style>

<aside class=side>
<div class=brand><img class=orb src=/logo.svg alt=""><span><b id=brandName>Nimbus</b><span class=fw id=fwver></span></span></div>
<div class=modeswitch id=modehdr title="Switching modes restarts the device">
<button data-m=0 type=button>Notifier</button>
<button data-m=1 type=button>Orchestrator</button>
</div>
<button class=searchbtn id=globalSearchBtn type=button aria-label="Search (Control K)"><svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4-4"/></svg><span class=tl>Search</span><kbd>Ctrl K</kbd></button>
<nav class=tabs>
<button class=tab data-p=home title=Home><svg viewBox="0 0 24 24"><path d="M4 13h6V4H4zM14 20h6v-9h-6zM14 8h6V4h-6zM4 20h6v-4H4z"/></svg><span class=tl>Home</span></button>
<button class=tab data-p=chat title=Chat><svg viewBox="0 0 24 24"><path d="M21 15a2 2 0 0 1-2 2H8l-4 4V5a2 2 0 0 1 2-2h13a2 2 0 0 1 2 2z"/></svg><span class=tl>Chat</span></button>
<button class=tab data-p=memory title=Memory><svg viewBox="0 0 24 24"><path d="M4 7c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3zM4 7v10c0 1.7 3.6 3 8 3s8-1.3 8-3V7"/></svg><span class=tl>Memory</span></button>
<button class=tab data-p=assistant title=Assistant><svg viewBox="0 0 24 24"><circle cx="6" cy="12" r="3"/><circle cx="18" cy="12" r="3"/><path d="M9 12h6"/></svg><span class=tl>Assistant</span></button>
<button class=tab data-p=device title=Device><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a7 7 0 0 0 0-6l1.6-1.3-2-3.4-2 .8a7 7 0 0 0-1.7-1L15 2H9l-.3 2.1a7 7 0 0 0-1.7 1l-2-.8-2 3.4L4.6 9a7 7 0 0 0 0 6l-1.6 1.3 2 3.4 2-.8a7 7 0 0 0 1.7 1L9 22h6l.3-2.1a7 7 0 0 0 1.7-1l2 .8 2-3.4z"/></svg><span class=tl>Device</span></button>
</nav>
</aside>
<div id=toast></div>

)=====";
