#pragma once
#include <Arduino.h>

// ui_js - one fragment of the Nimbus config page (the page script: auth/token, tabs, state/orch polling, all control wiring).
// Split MECHANICALLY from the original single-blob web_pages.h (P4 commit 1):
// the fragments concatenate BYTE-IDENTICALLY to the original CONFIG_HTML -
// tools/webui_concat_check.py asserts it. Served in order by webui.cpp's
// chunked '/' handler; web_pages.h lists the order.

static const char UI_JS[] PROGMEM = R"=====(<script>
const $=id=>document.getElementById(id);
// --- Per-device auth (owner R2: identify ONCE per browser, then forever) ---
// The token persists in localStorage (NOT per-tab sessionStorage - that was the
// "QR is a dead end" bug: phones open QR links in an ephemeral webview, the token
// died with the tab and the URL had been stripped, forcing a manual copy every
// time). Every request carries it (the server now gates ALL routes); with no
// token stored, the page renders an identify gate and NOTHING else.
// Storage access can THROW (Safari private browsing, blocked site data). It used to
// throw straight out of the init block below, so the fetch shim never installed and
// every request went out token-less - a permanent identify gate with no way to explain
// it. Every storage touch is guarded now, with an in-memory token so the session still
// works when nothing can be persisted.
let _memTok='';
function nimbusTok(){try{return localStorage.getItem('nimbusTok')||_memTok;}catch(e){return _memTok;}}
// Store the token. Returns true only when it is DURABLY stored (survives a reload).
function setTok(t){_memTok=t;try{localStorage.setItem('nimbusTok',t);return localStorage.getItem('nimbusTok')===t;}catch(e){return false;}}
(function(){
  try{const legacy=sessionStorage.getItem('nimbusTok');   // migrate pre-R2 tabs
      if(legacy&&!nimbusTok())setTok(legacy);}catch(e){}
  const u=new URLSearchParams(location.search).get('t');
  // Strip the token from the address bar ONLY once it is durably stored - when storage
  // is unavailable the ?t= is the one thing keeping a reload authenticated. Preserve
  // non-secret handoff state (notably ?onboard=provider) instead of discarding the
  // wizard's destination while moving from the setup AP to the LAN origin.
  if(u&&setTok(u)){
    const q=new URLSearchParams(location.search);q.delete('t');
    history.replaceState(null,'',location.pathname+(q.toString()?'?'+q.toString():''));
  }
  // Base path of the served app. On the LAN this is "" (page at "/"); through the
  // cloud tunnel the page is served under "/d/<deviceId>/", so absolute API paths
  // ("/api/state") must be prefixed to stay inside the tunnel subtree. Computed once
  // from the current location: the directory of the current path, minus a trailing
  // slash. "/" -> "", "/d/<id>/" -> "/d/<id>".
  const _base=location.pathname.replace(/\/[^/]*$/,'');
  const _f=window.fetch;
  window.fetch=function(url,opt){
    opt=opt||{};
    opt.headers=Object.assign({},opt.headers||{},{'X-Nimbus-Token':nimbusTok()});
    // Rewrite same-origin absolute paths under the app base (no-op on LAN). Leave
    // protocol-relative ("//host") and full URLs untouched.
    if(_base&&typeof url==='string'&&url.charAt(0)==='/'&&url.charAt(1)!=='/')url=_base+url;
    return _f(url,opt).then(r=>{if(r.status===401)showAuth();return r;});
  };
})();
// An unauthenticated tab must STOP polling. Every token-gated GET (state/orch/
// health/telegram/mem) 401s without a valid token, and the device treats a burst
// of 401s as "someone wants to configure" and parks the e-ink on the Config QR -
// which permanently blocked the idle screensaver from arming (owner: "a full
// night, no logo"). canPoll() gates every background poll; a successful identify
// reloads the page so polling resumes clean. Set on no-token or the first 401.
let _authPaused=false;
function canPoll(){return !!nimbusTok() && !_authPaused;}
// Full-screen identify gate: shown before ANY data when no token is stored, or on
// a 401 (wrong/rotated token). One successful identification is permanent for
// this browser. Mirrors the guidance printed on the device's Connectivity screen.
function showAuth(){
  _authPaused=true;   // stop the background pollers from re-flooding the device with 401s
  if($('authgate'))return;
  const b=document.createElement('div');b.id='authgate';
  b.style.cssText='position:fixed;inset:0;background:#14181c;color:#dde;z-index:9999;display:flex;align-items:center;justify-content:center;text-align:center;padding:24px';
  b.innerHTML='<div style="max-width:420px">'+
    '<img src=/logo.svg alt="" style="width:96px;height:96px;display:block;margin:0 auto 14px;background:#fff;border-radius:50%;padding:6px">'+
    '<h2 style="margin:0 0 8px">Sign in to Nimbus</h2>'+
    '<p style="color:#9ab">Scan Settings &gt; Connectivity &gt; Sign-in QR. The QR opens a signed-in link, so there is no code to copy during normal setup.</p>'+
    '<input id=authtok placeholder="recovery access token" style="width:240px;padding:8px;font-size:15px"> '+
    '<button id=authuse style="padding:8px 16px;font-size:15px">Continue</button>'+
    '<p style="color:#678;font-size:12px">New device? Open 192.168.4.1 on its setup hotspot. First-time setup signs you in automatically.</p></div>';
  document.body.appendChild(b);
  // When storage is blocked, the in-memory token would die with the reload and bring
  // the gate straight back - an unescapable loop for someone who typed the address by
  // hand (no ?t= to re-seed from). Carry it in the URL instead; that survives.
  $('authuse').onclick=()=>{const t=$('authtok').value.trim();if(!t)return;
    if(setTok(t))location.reload();
    else location.search='?t='+encodeURIComponent(t);};
}
// No stored token -> gate immediately, before any load/poll renders data.
if(!nimbusTok())document.addEventListener('DOMContentLoaded',showAuth);
// Tab switching: show one .pane, highlight its .tab. Default = the first tab (Home).
document.querySelectorAll('.tab').forEach(b=>b.onclick=()=>{
  document.querySelectorAll('.pane').forEach(p=>p.style.display='none');
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('on'));
  $('pane-'+b.dataset.p).style.display='';
  b.classList.add('on');
  // Lazy-loads per area (Phase 3 C1 IA)
  if(b.dataset.p==='gov')loadLoops();
  if(b.dataset.p==='mem'){if(typeof loadMemDash==='function')loadMemDash();if(typeof loadFiles==='function')loadFiles();}
  if(b.dataset.p==='harness'){if(typeof loadOrch==='function')loadOrch();if(typeof loadConnectors==='function')loadConnectors();if(typeof loadTools==='function')loadTools();if(typeof loadSkills==='function')loadSkills();}
  if(b.dataset.p==='usage'){if(typeof loadOrch==='function')loadOrch();if(typeof loadFetchQ==='function')loadFetchQ();   // usage tiles + budget rows + downloads
    if(typeof loadUsageHistory==='function')loadUsageHistory();}          // spend graph buckets
  if(b.dataset.p==='chat'&&typeof loadChatHistory==='function')loadChatHistory();   // unified history refresh
  if(b.dataset.p==='set'){                 // Settings: ring sim init + connectivity secrets
    if(typeof loadThemes==='function')loadThemes();
    if(typeof loadConnect==='function')loadConnect();
    if(typeof loadWifi==='function')loadWifi();     // saved Wi-Fi networks
  }
});
document.querySelector('.tab').classList.add('on');
// Sub-tab switching (Harness pane): mirror of the top switcher, scoped to
// .subtab/data-sp and #subpane-<x>. .subpane is a different class from .pane, so the
// top switcher never touches these. Default = the first sub-tab (Connectors).
document.querySelectorAll('.subtab').forEach(b=>b.onclick=()=>{
  document.querySelectorAll('.subpane').forEach(p=>p.style.display='none');
  document.querySelectorAll('.subtab').forEach(t=>t.classList.remove('on'));
  $('subpane-'+b.dataset.sp).style.display='';
  b.classList.add('on');
  // Refresh the area we just revealed (all also load once at page init).
  const sp=b.dataset.sp;
  if(sp==='connectors'){if(typeof loadConnectors==='function')loadConnectors();if(typeof loadOrch==='function')loadOrch();}
  else if(sp==='tools'&&typeof loadTools==='function')loadTools();
  else if(sp==='llm'&&typeof loadOrch==='function')loadOrch();
  else if(sp==='skills'&&typeof loadSkills==='function')loadSkills();
});
{const st=document.querySelector('.subtab'); if(st)st.classList.add('on');}
// Tap-? help: every button.qh toggles the first .hint.tip among its own or its
// parent's following siblings. Delegated, so dynamically built rows (paramRow,
// connector cards) work with no extra wiring. Placement invariant: the tip must
// FOLLOW the ? button or the button's immediate parent (label/heading/cell).
document.addEventListener('click',e=>{
  const b=e.target.closest&&e.target.closest('button.qh'); if(!b)return;
  e.preventDefault();   // a ? inside a <label>/<summary> must not activate its control
  let n=b,h=null;
  for(let up=0;up<2&&!h;up++){
    for(let s=n.nextElementSibling;s;s=s.nextElementSibling)
      if(s.classList&&s.classList.contains('tip')){h=s;break}
    n=n.parentElement; if(!n)break;
  }
  if(!h)return;
  const open=h.classList.toggle('open');
  b.setAttribute('aria-expanded',open?'true':'false');
});
// TTS voices, fetched LIVE from the device (Mistral: its /v1/audio/voices catalog with
// gender/accent, cached device-side; OpenAI: the verified gpt-4o-mini-tts set) so the
// picker can't drift from the provider. Minimal bundled fallback if the fetch fails.
// OpenAI stays a single flat #ttsVoice select. Mistral now returns STRUCTURED
// entries ({value,label,name,gender,lang,emotion}) that drive a cascading
// gender -> persona -> emotion picker (#vGender/#vPersona/#vEmotion); the three
// picks resolve to one entry's slug and save it exactly like the flat select did.
function capProbeUpd(){
  var m=$('capProbe'),box=$('capProbeActive'),cost=$('capProbeCost');
  if(!m)return;
  var active=(m.value==2||m.value===2);
  if(box)box.style.display=active?'block':'none';
  if(cost){
    var h=parseInt(($('capProbeH')||{}).value||24)||24;
    cost.textContent=active
      ?('Re-checks each provider key about every '+h+' h. Each check is a free provider metadata call, so the added cost is negligible - no tokens are spent. (Functional per-connector testing, which would use tokens, is not run.)')
      :'';
  }
}
function fillVoices(prov, sel){
  const FB={mistral:[{value:'en_paul_neutral',label:'Paul (male, US) - neutral',name:'Paul',gender:'male',lang:'US',emotion:'neutral'},
                     {value:'gb_oliver_neutral',label:'Oliver (male, UK) - neutral',name:'Oliver',gender:'male',lang:'UK',emotion:'neutral'},
                     {value:'gb_jane_neutral',label:'Jane (female, UK) - neutral',name:'Jane',gender:'female',lang:'UK',emotion:'neutral'},
                     {value:'fr_marie_neutral',label:'Marie (female, FR) - neutral',name:'Marie',gender:'female',lang:'FR',emotion:'neutral'}],
            openai:[{value:'alloy',label:'Alloy - neutral'},{value:'nova',label:'Nova - female'},{value:'onyx',label:'Onyx - deep male'}]};
  const tv=$('ttsVoice'), g=$('vGender'), pp=$('vPersona'), em=$('vEmotion'), h=$('voiceHint');
  const showCascade=on=>{[g,pp,em].forEach(e=>{if(e)e.style.display=on?'':'none';}); if(tv)tv.style.display=on?'none':'';};
  const renderFlat=list=>{if(!tv)return; tv.innerHTML='';
    list.forEach(v=>{const o=document.createElement('option');o.value=v.value;o.textContent=v.label;if(v.value===sel)o.selected=true;tv.appendChild(o);});
    tv.onchange=()=>orchApply({ttsVoice:tv.value});
    showCascade(false);
    if(h)h.textContent=(prov==='openai')?'OpenAI voices (male / female / neutral).':'Mistral voices, live from the provider.';};
  const buildCascade=list=>{if(!g||!pp||!em){renderFlat(list);return;} showCascade(true);
    const uniq=a=>a.filter((v,i)=>a.indexOf(v)===i);
    const gOf=v=>v.gender||'', nOf=v=>v.name||v.value, eOf=v=>v.emotion||'';
    const cur=list.find(v=>v.value===sel)||list[0]||{};
    const fill=(selEl,vals,chosen)=>{selEl.innerHTML='';
      vals.forEach(v=>{const o=document.createElement('option');o.value=v;o.textContent=v||'neutral';if(v===chosen)o.selected=true;selEl.appendChild(o);});};
    const drawPersona=()=>{const names=uniq(list.filter(v=>gOf(v)===g.value).map(nOf));
      const chosen=names.indexOf(pp.value)>=0?pp.value:(gOf(cur)===g.value&&names.indexOf(nOf(cur))>=0?nOf(cur):names[0]);
      fill(pp,names,chosen);};
    const drawEmotion=()=>{const emos=uniq(list.filter(v=>gOf(v)===g.value&&nOf(v)===pp.value).map(eOf));
      const chosen=emos.indexOf(em.value)>=0?em.value:(emos.indexOf(eOf(cur))>=0?eOf(cur):emos[0]);
      fill(em,emos,chosen);};
    const resolve=()=>{const ent=list.find(v=>gOf(v)===g.value&&nOf(v)===pp.value&&eOf(v)===em.value)
        ||list.find(v=>gOf(v)===g.value&&nOf(v)===pp.value)||{};
      if(ent.value)orchApply({ttsVoice:ent.value});};
    fill(g,uniq(list.map(gOf)),gOf(cur)); drawPersona(); drawEmotion();
    g.onchange=()=>{drawPersona();drawEmotion();resolve();};
    pp.onchange=()=>{drawEmotion();resolve();};
    em.onchange=resolve;
    if(h)h.textContent='Mistral voices, live from the provider: pick gender, then persona, then emotion.';};
  const render=list=>{
    const structured=prov!=='openai'&&list.some(v=>v.gender||v.name);
    structured?buildCascade(list):renderFlat(list);};
  fetch('/api/voices?provider='+encodeURIComponent(prov)).then(r=>r.json())
    .then(l=>render(Array.isArray(l)&&l.length?l:(FB[prov]||FB.mistral)))
    .catch(()=>render(FB[prov]||FB.mistral));
}
// Provider priority as an ordered, checkable list -> a comma string (no free-text).
function renderPrio(hostId, field, csv){
  const ALL=['openai','anthropic','mistral','custom'], host=$(hostId); if(!host)return;
  let order=csv.split(',').map(s=>s.trim()).filter(x=>ALL.includes(x));
  let rows=order.map(p=>({p,on:true})).concat(ALL.filter(x=>!order.includes(x)).map(p=>({p,on:false})));
  const save=()=>orchApply({[field]:rows.filter(r=>r.on).map(r=>r.p).join(',')});
  const draw=()=>{
    host.innerHTML='';
    rows.forEach((r,i)=>{
      const row=document.createElement('div'); row.className='row'; row.style.margin='3px 0';
      const cb=document.createElement('input'); cb.type='checkbox'; cb.checked=r.on; cb.style.flex='0';
      cb.onchange=()=>{r.on=cb.checked; save(); draw();};
      const nm=document.createElement('span'); nm.style.flex='1'; nm.textContent=(i+1)+'. '+(BUDLBL[r.p]||r.p)+(r.on?'':' (off)');
      const up=document.createElement('button'); up.textContent='↑'; up.disabled=i===0;
      up.onclick=()=>{const t=rows[i-1];rows[i-1]=rows[i];rows[i]=t; save(); draw();};
      const dn=document.createElement('button'); dn.textContent='↓'; dn.disabled=i===rows.length-1;
      dn.onclick=()=>{const t=rows[i+1];rows[i+1]=rows[i];rows[i]=t; save(); draw();};
      row.append(cb,nm,up,dn); host.appendChild(row);
    });
  };
  draw();
}
// curated override params: key must match nimbus::Param enum ordinal.
// type: 'num' | 'bool' | 'select'(with opts). label/def/value come from /api/state.
let META={};   // key -> {name,value,def,overridden}
function fmt(k,v){return (META[k]&&META[k].kind=='bool')?(v?'On':'Off'):v;}

function paramRow(p){
  const wrap=document.createElement('div'); wrap.style.marginBottom='10px';
  const lab=document.createElement('label'); lab.textContent=p.label||p.name;
  if(p.desc){const q=document.createElement('button'); q.type='button'; q.className='qh';
    q.setAttribute('aria-expanded','false'); q.setAttribute('aria-label','About '+(p.label||p.name));
    q.textContent='?'; lab.appendChild(q);}
  wrap.appendChild(lab);
  if(p.desc){const h=document.createElement('div'); h.className='hint tip'; h.textContent=p.desc; wrap.appendChild(h);}
  const row=document.createElement('div'); row.className='row';
  let inp;
  if(p.kind=='bool'){inp=document.createElement('select');
    inp.innerHTML='<option value=0>Off</option><option value=1>On</option>';}
  else if(p.kind=='posture'){inp=document.createElement('select');
    inp.innerHTML='<option value=0>Dark - off; one LED for alerts</option>'+
      '<option value=1>Calm - a soft activity glow (recommended)</option>'+
      '<option value=2>Full - every session a color arc</option>';}
  else {inp=document.createElement('input'); inp.type='number';
    if(p.min!==undefined){inp.min=p.min;inp.max=p.max;inp.step=p.step||1;}}
  inp.value=p.value; inp.id='p_'+p.key; row.appendChild(inp);
  // Dirty-mark on edit so the 3s poll's in-place sync (applyState) never stomps an
  // unsaved change; cleared only on a SUCCESSFUL Set (audit P1.1).
  inp.oninput=inp.onchange=()=>{inp.dataset.dirty=1;};
  const setb=document.createElement('button'); setb.textContent='Set';
  setb.onclick=()=>apply({['p_'+p.key]:inp.value}).then(ok=>{if(ok)delete inp.dataset.dirty;});
  const rst=document.createElement('button'); rst.textContent='Reset';
  rst.onclick=()=>apply({['clr_'+p.key]:'1'}).then(ok=>{if(ok)delete inp.dataset.dirty;});
  row.appendChild(setb); row.appendChild(rst);
  if(p.kind=='num'&&p.min!==undefined){const rg=document.createElement('span');
    rg.className='hint'; rg.style.marginLeft='6px'; rg.textContent='allowed '+p.min+'–'+p.max;
    row.appendChild(rg);}
  wrap.appendChild(row);
  const eff=document.createElement('div'); eff.className='eff'; eff.id='eff_'+p.key;
  eff.innerHTML=effHtml(p);
  wrap.appendChild(eff);
  return wrap;
}
function effHtml(p){return p.overridden
  ? '<span class=ovr>custom '+fmt(p.key,p.value)+'</span> (default '+fmt(p.key,p.def)+')'
  : 'default '+fmt(p.key,p.def);}

// Context-rich device tiles (Home Overview): a bare number is meaningless, so each
// tile shows the value WITH its scale (free/total), a threshold marker, and colour.
function _tile(k,v,cx,pct,col,mark){
  const g=(pct>0||col)?('<div class=g><i style="width:'+Math.max(0,Math.min(100,pct))+'%;background:'+(col||'#555')+'"></i>'+
    (mark!==undefined?'<span class=mk style="left:'+Math.min(100,mark)+'%"></span>':'')+'</div>'):'';
  return '<div class=tile><div class=k>'+k+'</div><div class=v>'+v+'</div><div class=cx>'+cx+'</div>'+g+'</div>';
}
function renderDevTiles(d){
  const box=$('devtiles'); if(!box)return;
  const kb=x=>Math.round((x||0)/1024), mb=x=>((x||0)/1048576).toFixed(1);
  const heapK=kb(d.heap), heapTotK=kb(d.heapTotal)||300;
  const heapCol=heapK<26?'#c55':(heapK<34?'#e0b870':'#5c5');
  let t=_tile('Free RAM',heapK+'<small>K</small>','of '+heapTotK+'K · floor 34K'+(d.heapMin!==undefined?' · low '+kb(d.heapMin)+'K':''),
              heapK/heapTotK*100,heapCol,34/heapTotK*100);
  if(d.psramTotal)t+=_tile('PSRAM',mb(d.psramFree)+'<small>/'+mb(d.psramTotal)+' MB</small>','for large tasks',
              d.psramFree/d.psramTotal*100,'#7fd1c8');
  const bt=d.batt;
  if(bt&&bt.valid){const m=bt.minsToEmpty;
    // Trend wording only (owner 2026-07-16): charging/onExtPower are voltage-trend
    // INFERENCES - no charge-detect hardware exists, so never present them as fact.
    const tl=(m===undefined||m<0)?(bt.onExtPower?'V stable':'estimating'):(m>=60?Math.floor(m/60)+'h '+(m%60)+'m':m+'m left');
    // mvTrue = the ADC's top-band under-read corrected by the BATTCAL anchor - the
    // SAME correction behind the percent next to it, so a full pack reads 8.40V/100%
    // instead of the old 7.91V/100% (owner: "we know for a fact it's 8.4").
    const bmv=bt.mvTrue||bt.millivolts;
    t+=_tile('Battery',bt.percent+'<small>%</small>',(bmv?(bmv/1000).toFixed(2)+'V · ':'')+tl+(bt.charging?' · V rising':''),
              Math.max(0,bt.percent),bt.percent<20?'#c55':'#7fd1c8');
  } else t+=_tile('Power','ext','on external power',100,'#8cf');
  if(bt&&bt.dieTempC!==undefined&&bt.dieTempC>0)
    t+=_tile('Die temp',bt.dieTempC+'<small>°C</small>','guard trip 70°',bt.dieTempC/90*100,bt.dieTempC>60?'#e0b870':'#5c5',70/90*100);
  t+=_tile('Mode',(d.mode==1?'Orch':'Notif'),(d.jobs||0)+' active session'+((d.jobs||0)==1?'':'s'),0,'');
  box.innerHTML=t;
}
// Last LAN address seen, so a changed one can refresh the sign-in links (below).
// null = nothing seen yet, which must NOT count as a change.
let _lastStaIp=null;
// Glass-box availability, cached off the polls the UI already runs (no extra
// requests). Turn details are SD-gated AND gated on the Activity recording knob,
// so the chat says which one is missing instead of just showing nothing.
let GB={trace:true,sd:true};
function _chatTraceHint(){
  const el=$('chatTrace'); if(!el)return;
  let m='';
  if(!GB.trace)m='Activity recording is off, so turn details aren’t captured. Turn it on in Capabilities → Models → Tool use.';
  else if(!GB.sd)m='No SD card, so turn details can’t be stored.';
  el.textContent=m; el.style.display=m?'':'none';
}
function applyState(d){
  if(d.storeSD!==undefined){GB.sd=!!d.storeSD&&!d.sdLost;_chatTraceHint();}
  // ---- Cloud access (cumulo-nimbus tunnel) ----
  if(d.cloud&&$('cloudLine')){
    var c=d.cloud;
    $('cloudLine').textContent=c.line||'';
    var cc=$('cloudCode');
    if(c.state==='pairing'&&c.code){cc.style.display='block';
      cc.innerHTML='Enter this code at <b>app.cumulo-nimbus.ai</b> while signed in: <b></b>';
      cc.lastChild.textContent=c.code;}   // code is from the untrusted pairing server: textContent, never innerHTML
    else{cc.style.display='none';}
    $('cloudPair').style.display=(c.paired||c.state==='pairing')?'none':'inline-block';
    $('cloudUnpair').style.display=c.paired?'inline-block':'none';
    $('cloudOff').style.display=(c.optIn&&!c.paired&&c.state!=='pairing')?'inline-block':'none';
    var cpost=function(a,msg){var f=new FormData();f.append('action',a);
      fetch('/api/cloud',{method:'POST',body:f}).then(jok).then(()=>{toast(msg);setTimeout(loadState,1500);}).catch(failToast);};
    $('cloudPair').onclick=()=>cpost('pair','Starting pairing…');
    $('cloudUnpair').onclick=()=>cpost('unpair','Unpaired');
    $('cloudOff').onclick=()=>cpost('optout','Cloud access off');
  }
  // ---- Firmware update (OTA) ----
  if(d.ota!==undefined&&$('fwState')){
    $('fwCur').textContent=(d.fw||'')+' ('+(d.build||'')+')';
    $('fwLatest').textContent=d.otaLatest?(d.otaLatest+(d.otaNotes?(' - '+d.otaNotes):'')):'not checked yet';
    var st=d.ota; if(st==='error'&&d.otaErr)st+=' ('+d.otaErr+')';
    var busy=(d.ota==='downloading'||d.ota==='verifying'||d.ota==='rebooting'||d.ota==='checking');
    $('fwState').textContent=st+(d.ota==='downloading'&&d.otaPct>=0?(' '+d.otaPct+'%'):'');
    $('fwLast').textContent=d.lastOta||'-';
    $('fwBarWrap').style.display=(d.ota==='downloading'||d.ota==='verifying')?'block':'none';
    if(d.otaPct>=0)$('fwBar').style.width=d.otaPct+'%';
    var fi=$('fwInstall');fi.style.display=(d.ota==='available')?'inline-block':'none';
    $('fwCheck').disabled=busy;fi.disabled=busy;
    if(d.autoUpd!==undefined){const au=$('autoUpd');
      if(au&&document.activeElement!==au){au.checked=!!d.autoUpd;
        au.onchange=()=>{const f=new FormData();f.append('autoUpd',au.checked?'1':'0');
          fetch('/api/config',{method:'POST',body:f}).then(()=>toast(au.checked?'Auto-update on':'Auto-update off'));};}}
    $('fwCheck').onclick=()=>{fetch('/api/ota/check',{method:'POST'}).then(jok)
      .then(()=>{toast('Checking…');setTimeout(loadState,2500);}).catch(failToast);};
    fi.onclick=()=>{
      if(!confirm('Install firmware '+(d.otaLatest||'')+'?\n\nThe device will download, verify the signature, and restart - about two minutes. Keep it powered on.'))return;
      fetch('/api/ota/apply',{method:'POST'}).then(jok).then(()=>{toast('Updating…');setTimeout(loadState,2000);}).catch(failToast);};
  }
  renderDevTiles(d);
  if(d.fw){const fv=$('fwver'); if(fv)fv.textContent=d.fw+(d.build&&d.build!==d.fw?(' ('+d.build+')'):''); fv&&(fv.title='firmware version (build id)');}
  // Low-battery preferences. OUTSIDE the telemetry gate below on purpose: they
  // live in the always-open Battery mode group, so a board with no pack fitted
  // must still be able to see and change them.
  if(d.batt){
    const lr=$('lbRing');
    if(lr&&document.activeElement!==lr){lr.checked=!!d.batt.lbRing;
      lr.onchange=()=>{const f=new FormData();f.append('lbRing',lr.checked?'1':'0');
        fetch('/api/config',{method:'POST',body:f}).then(jok)
          .then(()=>toast(lr.checked?'Low-battery light on':'Low-battery light off')).catch(failToast);};}
    const ls=$('lbSaver');
    if(ls&&document.activeElement!==ls){ls.checked=!!d.batt.lbSaver;
      ls.onchange=()=>{const f=new FormData();f.append('lbSaver',ls.checked?'1':'0');
        fetch('/api/config',{method:'POST',body:f}).then(jok)
          .then(()=>toast(ls.checked?'Power saving on':'Power saving off')).catch(failToast);};}
  }
  // battery analytics panel (Device tab) - shown only when telemetry is valid
  var bt=d.batt;
  if(bt&&bt.valid){
    $('battsec').style.display='';
    $('battbar').style.width=Math.max(0,Math.min(100,bt.percent))+'%';
    $('battpct').textContent=bt.percent+'%';
    var _bmv=bt.mvTrue||bt.millivolts;
    $('battmv').textContent=(_bmv?_bmv+' mV':'-');
    if(document.activeElement!==$('sleepMv')&&$('sleepMv'))$('sleepMv').value=(bt.sleepMv!==undefined?bt.sleepMv:6000);
    if(document.activeElement!==$('wakeMv')&&$('wakeMv'))$('wakeMv').value=(bt.wakeMv!==undefined?bt.wakeMv:6500);
    if($('sleepOvr'))$('sleepOvr').checked=!!bt.sleepOvr;
    if($('brightOvr'))$('brightOvr').checked=!!bt.brightOvr;
    if(document.activeElement!==$('battCapMah')&&$('battCapMah'))$('battCapMah').value=(bt.capMah!==undefined?bt.capMah:3500);
    if(document.activeElement!==$('battRtop')&&$('battRtop'))$('battRtop').value=(bt.rtop!==undefined?bt.rtop:220000);
    if(document.activeElement!==$('battRbot')&&$('battRbot'))$('battRbot').value=(bt.rbot!==undefined?bt.rbot:100000);
    window._battState={rtop:bt.rtop,rbot:bt.rbot};   // for the divider-change confirm
    // never hide the measurement behind the correction: raw stays one hover away
    $('battmv').title=(bt.mvTrue&&bt.mvTrue!==bt.millivolts)
      ?('ADC-corrected (BATTCAL anchor). Raw reading: '+bt.millivolts+' mV - the S3 ADC under-reads a full 2S pack.')
      :'raw ADC reading (uncalibrated - run BATTCAL on a full pack to correct the top band)';
    var m=bt.minsToEmpty;
    $('batttime').textContent=(m===undefined||m<0)?(bt.onExtPower?'voltage stable - no drain':'estimating…')
      :(m>=60?(Math.floor(m/60)+'h '+(m%60)+'m'):(m+'m'));
    $('battrate').textContent=(bt.ratePctHr!==undefined&&bt.ratePctHr>0)?(bt.ratePctHr.toFixed(1)+' %/hr'):'-';
    $('batthealth').textContent=(bt.health!==undefined)?(bt.health+'%'+(bt.segments?(' ('+bt.segments+' cycles learned)'):' (baseline pending)')):'-';
    // Voltage-trend wording only: charging/onExtPower are inferences from the ADC
    // trend - there is no charge-detect hardware, so never state them as fact.
    $('battsrc').textContent=bt.charging?'voltage rising (inferred)':bt.onExtPower?'voltage stable (inferred)':'draining';
    $('battcal').textContent=bt.calibrated?'calibrated ✓':'not calibrated (reads low near full)';
    var bcb=$('battcalBtn'); if(bcb) bcb.onclick=()=>{
  var pb=$('protSave'); if(pb)pb.onclick=function(){
    var f=new FormData();
    f.append('sleepMv',$('sleepMv').value||'6000');
    f.append('wakeMv',$('wakeMv').value||'6500');
    f.append('sleepOvr',$('sleepOvr').checked?'1':'0');
    f.append('brightOvr',$('brightOvr').checked?'1':'0');
    if($('brightOvr').checked&&!confirm('Allow full LED brightness?\n\nSustained full brightness can overheat the device and damage it permanently. The thermal guard stays active. This resets at restart.'))return;
    if($('sleepOvr').checked&&!confirm('Skip low-battery protection?\n\nThe battery can discharge to a point where it no longer recharges and must be replaced. This applies to measurement runs only and resets at restart.'))return;
    // battery hardware (divider resistors + capacity) - send only if present + changed
    if($('battCapMah'))f.append('battCapMah',$('battCapMah').value||'3500');
    if($('battRtop'))f.append('battRtop',$('battRtop').value||'220000');
    if($('battRbot'))f.append('battRbot',$('battRbot').value||'100000');
    var dividerChanged=$('battRtop')&&$('battRbot')&&window._battState&&(+$('battRtop').value!==window._battState.rtop||+$('battRbot').value!==window._battState.rbot);
    if(dividerChanged&&!confirm('Changed the sense resistors?\n\nThis re-scales every voltage reading, so the full-charge calibration is now stale. Re-run Calibrate on a fully charged pack afterward.'))return;
    fetch('/api/config',{method:'POST',body:f}).then(function(){pb.textContent='Saved';setTimeout(function(){pb.textContent='Save';},1200);});
  };
      if(!confirm('Set the battery to 100%?\n\nOnly do this with the pack fully charged - the reading becomes this device\'s 100% anchor.')) return;
      fetch('/api/battcal',{method:'POST'}).then(jok).then(()=>{toast('Calibrating…');setTimeout(loadState,1200);}).catch(failToast);};
  } else { $('battsec').style.display='none'; }
  // Device identity readouts (P2): live SSID/mDNS; the input is only synced when
  // the user isn't mid-edit (3 s poll would stomp their typing otherwise).
  if(d.apSsid!==undefined){const e=$('idApSsid');if(e)e.textContent=d.apSsid;
    const c=$('cxApSsid');if(c)c.textContent=d.apSsid;}
  // #cxLan is owned by loadConnect(), which has the per-origin sign-in links. Writing
  // plain text here too meant the 3 s state poll wiped those links a moment after the
  // Connectivity tab drew them. Re-fetch instead when the address actually changes
  // (new DHCP lease / different network), so the links can't go stale while the tab
  // sits open.
  if(d.mdns!==undefined){const e=$('idMdns');if(e)e.textContent=d.mdns;}
  if(d.staIp!==undefined&&d.staIp!==_lastStaIp){
    if(_lastStaIp!==null&&typeof loadConnect==='function')loadConnect();
    _lastStaIp=d.staIp;
  }
  if(d.devName!==undefined){const i=$('devName');
    if(i&&document.activeElement!==i&&!i.dataset.dirty)i.value=d.devName;
    const c=$('cxName');if(c)c.textContent=d.devName||'Nimbus';
    // Header brand shows the CONFIGURED name so multiple boards are distinguishable.
    const bn=$('brandName');if(bn)bn.textContent=d.devName||'Nimbus';
    if(d.devName)document.title=d.devName;}
  if(d.clock){
    const wasSynced=window.CLOCK&&window.CLOCK.synced; window.CLOCK=d.clock;
    const tzi=$('devTz');
    if(tzi&&document.activeElement!==tzi&&!tzi.dataset.dirty)tzi.value=d.clock.tz||'';
    const ck=$('devClock');
    if(ck){
      // Browser-drift hint: >2 min apart usually means the tz string is wrong
      // (SNTP time itself is UTC-exact), so say so next to the clock.
      const drift=Math.abs(Date.now()/1000-d.clock.epoch);
      ck.textContent=d.clock.local+(d.clock.synced&&drift>120?' (differs from this browser - timezone?)':'');
    }
    const bd=$('clockBadge');
    if(bd)bd.textContent=d.clock.synced?'synced':'waiting for internet time';
    const sb=$('clockSyncBtn');
    if(sb)sb.disabled=!d.sta;
    const gc=$('govClock');
    if(gc){
      if(!d.clock.synced){
        // Build ONCE, then just toggle visibility - rewriting innerHTML every
        // 3 s poll would destroy the Sync-now button mid-click (prism).
        if(!gc.firstChild){
          gc.innerHTML='<p class="hint tip">The device clock hasn\'t synced, so daily and weekly routines '+
            '&mdash; including nightly memory upkeep &mdash; can\'t run yet. It sets itself from the '+
            'internet once Wi-Fi connects. <button id=govSync type=button>Sync now</button></p>';
          const b=$('govSync'); if(b)b.onclick=()=>apply({clockSync:1});
        }
        gc.style.display='';
      } else gc.style.display='none';
    }
    // The instant the clock lands, re-render the routine rows ("waiting for
    // clock sync" -> real next-run deltas) if the Routines pane is on screen.
    if(!wasSynced&&d.clock.synced&&typeof loadLoops==='function')loadLoops();
  }
  const kb=x=>Math.round((x||0)/1024)+' KB', mb=x=>((x||0)/1048576).toFixed(1)+' MB';
  // Adaptive size: KB < 1 MB, MB < 1 GB, else GB.
  const sz=x=>{x=x||0; return x<1048576?kb(x):(x<1073741824?mb(x):(x/1073741824).toFixed(1)+' GB');};
  $('info').innerHTML=
    'mode <b>'+(d.mode==1?'Orchestrator':'Notifier')+'</b> &middot; sessions <b>'+d.jobs+'</b><br>'+
    // Device-WIDE figures (not per-orchestrator). Internal RAM is the scarce pool.
    'RAM (internal): <b>'+kb(d.heap)+'</b> free of '+kb(d.heapTotal)+
      (d.heapMin!==undefined?(' &middot; low-water '+kb(d.heapMin)):'')+
      '<span class=hint> - shared by all tasks; brief dips are normal</span><br>'+
    (d.psramTotal?('PSRAM (external): <b>'+mb(d.psramFree)+'</b> free of '+mb(d.psramTotal)+
      '<span class=hint> - used for large tasks</span><br>'):'')+
    (d.storeTotal?('Data store ('+(d.storeLabel||'flash')+'): <b>'+sz(d.storeFree)+'</b> free of '+sz(d.storeTotal)+
      '<span class=hint> - memories and history'+(d.storeSD?'':' (insert an SD card for more space)')+'</span><br>'):'')+
    'battery '+(d.batt&&d.batt.valid
      ? '<b>'+d.batt.percent+'%</b>'+(d.batt.charging?' (V rising)':'')
      : '<span class="badge ext">no battery telemetry</span>')+'<br>'+
    (d.sta?('sta <b>'+d.staIp+'</b> ('+d.rssi+' dBm) &middot; '):'')+
    'ap <b>'+d.apIp+'</b> &middot; <b>'+d.mdns+'</b>';
  // profile radios
  document.querySelectorAll('input[name=profile]').forEach(r=>{
    r.checked=(+r.value===d.profile);
    r.onchange=()=>apply({profile:r.value});
  });
  $('effprof').textContent='effective: '+d.effectiveProfileName+
    (d.effectiveProfile!==d.profile?' (adjusted automatically for power)':'');
  // mode
  $('mode').value=d.mode;
  $('mode').onchange=()=>apply({mode:$('mode').value});
  // Prominent header mode switch (same effect as the Device-tab selector) - the owner
  // couldn't find how to switch modes; it now lives at the top of every tab.
  const mh=$('modehdr'); if(mh){[...mh.children].forEach(b=>{
    const mv=+b.dataset.m; b.classList.toggle('on',mv===d.mode); b.classList.toggle('nf',mv===0);
    b.onclick=()=>{ if(mv===d.mode)return;
      if(!confirm('Switch to '+(mv?'Orchestrator':'Notifier')+'?\n\nThe device restarts to change modes. This takes a few seconds.'))return;
      apply({mode:mv}); };
  });}
  // Display panel: hardware identity, bound at boot. Confirm before changing -
  // picking the wrong one leaves the fitted screen dark until it is set back.
  const sm=$('scrModel'); if(sm&&d.scrModel!==undefined&&sm!==document.activeElement)sm.value=d.scrModel;
  const tc=$('tchCal'); if(tc&&d.tchCal!==undefined&&tc!==document.activeElement)tc.value=d.tchCal||'';
  // device identity: mark dirty while editing; Save persists (reboot-to-apply)
  $('devName').oninput=()=>{$('devName').dataset.dirty=1;};
  $('devNameSave').onclick=()=>apply({devName:$('devName').value})
    .then(ok=>{if(ok)delete $('devName').dataset.dirty;});
  const tz=$('devTz');
  if(tz){tz.oninput=()=>{tz.dataset.dirty=1;};
    $('devTzSave').onclick=()=>apply({devTz:tz.value})
      .then(ok=>{if(ok)delete tz.dataset.dirty;});}
  const csb=$('clockSyncBtn');
  if(csb)csb.onclick=()=>apply({clockSync:1});
  // params - rebuild the DOM ONLY on first load or when the param SET changes;
  // otherwise value-sync each row in place. The old unconditional innerHTML=''
  // rebuild every 3s poll destroyed the very input the user was typing into
  // (focus + in-progress value lost) - the field-QA "everything I type gets
  // deleted" bug (audit P1.1). Focused or dirty fields are never overwritten.
  META={}; d.params.forEach(p=>META[p.key]=p);
  const host=$('params');
  const psig=d.params.map(p=>p.key).join();
  if(host.dataset.sig!==psig){
    host.dataset.sig=psig; host.innerHTML='';
    d.params.forEach(p=>host.appendChild(paramRow(p)));
  } else d.params.forEach(p=>{
    const i=$('p_'+p.key);
    if(i&&document.activeElement!==i&&!i.dataset.dirty&&String(i.value)!==String(p.value))i.value=p.value;
    const e=$('eff_'+p.key); if(e)e.innerHTML=effHtml(p);
  });
}

let _toastT;
function toast(msg){const t=$('toast'); if(!t)return; t.textContent=msg||'Saved';
  t.classList.add('show'); clearTimeout(_toastT); _toastT=setTimeout(()=>t.classList.remove('show'),1400);}
function loadState(){if(!canPoll())return;fetch('/api/state').then(r=>r.json()).then(applyState).catch(()=>{});}
// The setup-AP password + auth token are OFF /api/state (secrets); fetch them
// from the token-gated /api/connect only when the Connectivity tab is opened.
// The fetch shim attaches the token to every request, GETs included.
// Unauthenticated -> 401 -> show a "scan the QR" hint.
function loadConnect(){
  fetch('/api/connect')
   .then(r=>r.ok?r.json():Promise.reject(r.status))
   .then(c=>{
     if($('cxName'))$('cxName').textContent=c.name||'Nimbus';
     if($('cxApSsid'))$('cxApSsid').textContent=c.apSsid||'';
     if($('cxApPass'))$('cxApPass').textContent=c.apPass||'';
     if($('cxToken'))$('cxToken').textContent=c.token||'';
     // Access token also shown in Device → identity (owner ask); click to copy.
     if($('idToken')){$('idToken').textContent=c.token||'-';
       $('idToken').onclick=()=>{if(navigator.clipboard&&c.token){navigator.clipboard.writeText(c.token);toast('Token copied');}};}
     // Every address links to ITSELF. This used to render one link labelled with the
     // mDNS name whose href was the raw-IP URL, so clicking "nimbus.local" signed you in
     // at the IP origin and the name kept asking for the token forever (the token is
     // stored per origin). Each address now carries its own ?t= so either one works.
     if($('cxLan')&&(c.url||c.mdnsUrl)){
       const link=(href,label)=>'<a href="'+href+'" target=_blank rel=noopener>'+label+'</a>';
       const parts=[];
       if(c.mdnsUrl&&c.mdns)parts.push(link(c.mdnsUrl,c.mdns));
       if(c.url)parts.push(link(c.url,c.ip||c.apIp||'this device'));
       $('cxLan').innerHTML=parts.join(' &middot; ');
     }
     // Bluetooth (Notifier link) state
     if($('btState'))$('btState').textContent=c.bleOn?(c.bleConn?'on · a device is connected':'on · advertising'):'off (Notifier mode only)';
     if($('btBonds'))$('btBonds').textContent=(c.bleBonds||0)+' paired'+((c.bleBonds||0)?' · pairing survives restart':'');
     if($('btMac'))$('btMac').textContent=c.bleMac||'-';
   })
   .catch(()=>{const h='🔒 Scan the Config QR on the device';
     if($('cxApPass'))$('cxApPass').textContent=h;
     if($('cxToken'))$('cxToken').textContent=h;});
}
// Forget all BLE bonds (Connectivity → Bluetooth) - every paired device must re-pair.
(function(){const b=$('btForget'); if(!b)return; b.onclick=()=>{
  if(!confirm('Forget all paired devices?\n\nEach computer pairs again automatically the next time its broker connects.'))return;
  fetch('/api/ble/forget',{method:'POST'}).then(r=>r.ok?r.json():Promise.reject(r.status)).then(()=>{
    toast('Bluetooth pairings cleared'); if(typeof loadConnect==='function')loadConnect();
  }).catch(()=>toast('Couldn\'t forget pairings - try again'));};
})();
// Rotate the auth token: POST /api/token/regen (authenticated with the current token),
// then persist the NEW token locally so THIS browser stays in; every other browser 401s
// on its next call and gets the identify gate.
(function(){const b=$('regenTok'); if(!b)return; b.onclick=()=>{
  if(!confirm('Generate a new access token?\n\nEvery browser is signed out except this one. Scan the new Config QR on each other device.'))return;
  fetch('/api/token/regen',{method:'POST'}).then(r=>r.ok?r.json():Promise.reject(r.status)).then(o=>{
    if(o&&o.token){setTok(o.token);
      if($('cxToken'))$('cxToken').textContent=o.token;
      toast('New token set');}
  }).catch(()=>toast('Couldn\'t generate a token - try again'));};
})();
// Erase storage: type-to-confirm SD wipe (keeps config, keeps the token, so no
// localStorage cleanup needed - the device reboots on the same network).
(function(){const b=$('sdReset'); if(!b)return; b.onclick=()=>{
  const p=prompt('Erase all stored data on the SD card?\n\nThis deletes every memory, all conversation history, saved files, and media. Wi-Fi, keys, and settings are kept. The device restarts.\n\nType ERASE STORAGE to confirm:');
  if(p===null)return;
  if(p.trim().toUpperCase()!=='ERASE STORAGE'){toast('Not erased - the phrase didn\'t match');return;}
  const fd=new FormData();fd.append('confirm','ERASE STORAGE');
  fetch('/api/sdreset',{method:'POST',body:fd}).then(r=>r.ok?r.json():Promise.reject(r.status))
    .then(()=>toast('Erasing storage - the device is restarting'))
    .catch(()=>toast('Couldn\'t erase - try again'));};
})();
// Factory reset: type-to-confirm, then POST the exact confirm phrase the device requires.
(function(){const b=$('factoryReset'); if(!b)return; b.onclick=()=>{
  const p=prompt('Erase all content and settings?\n\nThis removes Wi-Fi, API keys, the Telegram list, Bluetooth pairings, memory, and the access token, then restarts into first-time setup.\n\nType FACTORY RESET to confirm:');
  if(p===null)return;
  if(p.trim().toUpperCase()!=='FACTORY RESET'){toast('Not reset - the phrase didn\'t match');return;}
  const fd=new FormData();fd.append('confirm','FACTORY RESET');
  fetch('/api/factory-reset',{method:'POST',body:fd}).then(r=>r.ok?r.json():Promise.reject(r.status)).then(()=>{
    // The erase wipes this device's auth token, so our stored one is now stale -
    // drop it so we don't 401-loop against the old token when it reboots into the
    // setup AP + onboarding wizard (which re-issues a fresh token via the captive
    // portal / GET /?t=).
    _memTok='';try{localStorage.removeItem('nimbusTok');}catch(e){}
    document.body.innerHTML='<div style="max-width:460px;margin:18vh auto;padding:0 20px;text-align:center;font-family:inherit;color:var(--ink)">'+
      '<img src=/logo.svg style="width:52px;height:52px"><h2 style="margin:14px 0 8px">Device is resetting…</h2>'+
      '<p style="color:var(--ink2);line-height:1.5">Everything has been erased and the device is restarting into first-time setup. '+
      'Reconnect to its <b>&ldquo;…-setup&rdquo;</b> Wi-Fi network to run the setup wizard.</p></div>';
  }).catch(()=>toast('Reset failed - try again'));};
})();
// --- Local Loops ---
function loadLoops(){
  const el=$('loopList'); if(!el)return;
  const esc=s=>(s||'').replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));
  fetch('/api/loops').then(r=>r.ok?r.json():Promise.reject(r.status)).then(list=>{
    if(!list.length){el.innerHTML='<span class=hint>No routines yet - create one below, or ask in chat.</span>';return;}
    const fmtIn=s=>{if(s<=0)return 'due';if(s<3600)return 'in '+Math.round(s/60)+'m';
      if(s<172800)return 'in '+Math.round(s/3600)+'h';return 'in '+Math.round(s/86400)+'d';};
    el.innerHTML='<table><tbody>'+list.map(l=>{
      let sched=l.kind==='interval'?('every '+Math.round(l.everySec/60)+'m'):
        (l.kind==='once'?'one-time wakeup':(l.kind+' '+(l.at||'')));
      // Next-run: wall-clock routines can't fire before the clock syncs - say so
      // instead of showing nothing (the silent-BlockedNoClock gap). A one-time
      // wakeup is epoch-based like interval and never waits on the civil clock.
      const CK=window.CLOCK;
      if(l.kind!=='interval'&&l.kind!=='once'&&CK&&!CK.synced)sched+='<br><span class=hint>waiting for clock sync</span>';
      else if(l.enabled&&l.nextRun>0&&CK&&CK.synced)sched+='<br><span class=hint>'+fmtIn(l.nextRun-CK.epoch)+'</span>';
      const pend=l.byAgent&&!l.approved;
      const status=pend?'<b style="color:#e5a">pending approval</b>':(l.enabled?esc(l.lastResult||'ok'):'<span class=hint>paused</span>');
      const btns=pend
        ?'<button data-act=approve data-id="'+l.id+'">Approve</button> <button data-act=delete data-id="'+l.id+'">Deny</button>'
        :(l.enabled?'<button data-act=pause data-id="'+l.id+'">Pause</button>':'<button data-act=resume data-id="'+l.id+'">Resume</button>')+(l.reserved?'':' <button data-act=delete data-id="'+l.id+'">Delete</button>');
      return '<tr><td><b>'+esc(l.name)+'</b><br><span class=hint>'+esc((l.prompt||'').slice(0,64))+'</span></td><td>'+sched+'</td><td>'+status+'<br><span class=hint>'+(l.firesToday||0)+' today &middot; '+(l.tokensToday||0)+' tok</span></td><td>'+btns+'</td></tr>';
    }).join('')+'</tbody></table>';
    el.querySelectorAll('button[data-act]').forEach(b=>b.onclick=()=>{
      const fd=new FormData(); fd.append('action',b.dataset.act); fd.append('id',b.dataset.id);
      fetch('/api/loops',{method:'POST',body:fd}).then(()=>setTimeout(loadLoops,500));
    });
  }).catch(()=>{el.innerHTML='<span class=hint>Couldn\'t load routines</span>';});
}
function initLoopForm(){
  const kind=$('lpKind'); if(!kind)return;
  const upd=()=>{$('lpEveryRow').style.display=kind.value==='interval'?'':'none';
    $('lpAtRow').style.display=kind.value==='interval'?'none':'';
    $('lpDaysRow').style.display=kind.value==='weekly'?'':'none';};
  kind.onchange=upd; upd();
  $('lpCreate').onclick=()=>{
    const name=$('lpName').value.trim(), prompt=$('lpPrompt').value.trim();
    if(!name||!prompt){$('lpMsg').textContent='Name and prompt are required.';return;}
    const sched={kind:kind.value};
    if(kind.value==='interval') sched.every_seconds=Math.max(300,(+$('lpEvery').value||360)*60);
    else{ sched.at=$('lpAt').value; if(kind.value==='weekly') sched.days=[...document.querySelectorAll('.lpday:checked')].map(c=>c.value); }
    const fd=new FormData(); fd.append('action','create'); fd.append('name',name);
    fd.append('prompt',prompt); fd.append('chatId',$('lpChat').value.trim());
    fd.append('schedule',JSON.stringify(sched));
    fetch('/api/loops',{method:'POST',body:fd}).then(r=>r.json()).then(x=>{
      $('lpMsg').textContent=x.ok?'Created':(x.error||'Couldn\'t create - try again');
      if(x.ok){$('lpName').value='';$('lpPrompt').value='';} setTimeout(loadLoops,500);
    }).catch(()=>$('lpMsg').textContent='Couldn\'t create - try again');
  };
}
document.addEventListener('DOMContentLoaded',initLoopForm);
// Shared response guard: a 401 body is valid JSON, so `.then(r=>r.json())` alone
// used to toast a false "Saved" while the auth bar appeared. Throw the status so
// the catch can say what actually happened.
function jok(r){if(!r.ok)throw r.status;return r.json();}
// Fetch plain text, surfacing the SERVER's reason on failure. The glass-box
// endpoints answer 404 with a human sentence ("off: …", "nosd: …", "evicted: …")
// precisely so the chat can say why instead of showing an empty panel.
function _tfetch(url){
  return fetch(url).then(r=>r.text().then(t=>{
    if(r.ok)return t;
    const i=t.indexOf(': ');
    throw new Error((i>0?t.slice(i+2):t)||('Error '+r.status));
  }));
}
function failToast(e){toast(e===401?'Sign in required':'Couldn\'t save - try again');}
// Live ring preview: POSTs the currently-selected profile radio; the device
// drives the ring for ~4s and auto-reverts. Nothing persists.
document.addEventListener('DOMContentLoaded',()=>{const b=document.getElementById('prevBtn');
 if(b)b.onclick=()=>{const r=document.querySelector('input[name=profile]:checked');
  const body=new URLSearchParams();body.set('profile',r?r.value:'1');
  fetch('/api/preview',{method:'POST',body}).then(jok).then(()=>toast('Previewing…')).catch(failToast);};});
function apply(body){
  // Resolves to true/false so callers can react HONESTLY: the old bare
  // .catch(failToast) swallowed the rejection, so chained .then()s reported
  // success even on a 401/network failure (audit P1.5).
  return fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams(body).toString()})
   .then(jok).then(()=>{toast('Saved');loadState();return true;})
   .catch(e=>{failToast(e);return false;});
}

// Revert-to-defaults (owner 2026-07-18): drop EVERY per-param override so the
// active battery mode runs on its pure reference preset. Confirmed - it
// discards all customization in one click.
if($('revertProf'))$('revertProf').onclick=()=>{
  if(!confirm('Reset this mode to its defaults?\n\nEvery customized value returns to the preset.'))return;
  apply({revert_overrides:1}).then(ok=>{
    if(ok){$('revertMsg').textContent='All customizations cleared - this mode is back on its defaults.';}
  });
};

// ---- Orchestrator control surface (ROUND 3 Part A) ----
const PROVLBL={openai:'OpenAI',anthropic:'Anthropic',mistral:'Mistral'};
let ORCH=null, VPOLL={};   // verify polls PER PROVIDER (review: a single global let a
                           // second provider's verify kill the first's completion poll,
                           // stranding its button disabled at "checking..." forever)

// LED theme palettes come from the device (GET /api/themes -> [{name,colors:[[r,g,b],...]}]),
// the single source of truth in nimbus::theme - the page no longer hardcodes RGBs.
// Fetched once and cached; #themeChips renders a swatch strip per theme, click = save.
let THEMES=null, _curTheme=null, _themesReq=false;
const THEME_FB=[{name:'teal',colors:[[127,209,200],[80,190,180],[40,150,155],[170,230,220]]}];
function renderThemeChips(){
  const box=$('themeChips'); if(!box||!THEMES)return; box.innerHTML='';
  THEMES.forEach(t=>{const name=t.name, cols=t.colors||[];
    const row=document.createElement('button');row.type='button';
    row.style.cssText='display:flex;align-items:center;gap:0;width:100%;margin:3px 0;padding:4px;cursor:pointer;border-radius:8px;background:#181818;border:'+(name===_curTheme?'2px solid #7fd1c8':'1px solid #0003');
    const lab=document.createElement('span');lab.textContent=name+(name===_curTheme?' ✓':'');
    lab.style.cssText='color:#ccc;font-size:13px;min-width:78px;text-align:left;padding-left:6px';
    const strip=document.createElement('span');strip.style.cssText='display:flex;flex:1;height:22px;border-radius:5px;overflow:hidden;margin-left:6px';
    cols.forEach(c=>{const sw=document.createElement('span');sw.style.cssText='flex:1;background:rgb('+c.join(',')+')';strip.appendChild(sw);});
    row.appendChild(lab);row.appendChild(strip);
    row.onclick=()=>orchApply({theme:name});box.appendChild(row);});
}
function loadThemes(){
  if(THEMES||_themesReq)return; _themesReq=true;
  fetch('/api/themes').then(r=>r.json())
    .then(o=>{const a=(o&&o.themes)||o;THEMES=(Array.isArray(a)&&a.length)?a:THEME_FB;
              if(o&&o.roles)ROLES=o.roles;renderThemeChips();renderStatusLegend();renderRingSimControls();ringSimStart();})
    .catch(()=>{THEMES=THEME_FB;renderThemeChips();renderStatusLegend();renderRingSimControls();ringSimStart();});
}
// Status legend fallback ONLY (used when /api/themes is unreachable): the LIVE
// legend now arrives as roles[] from the device itself (generated from
// status_style.cpp), so it can never drift. ROLES holds it.
let ROLES=null;
const STATUS_LEGEND=[
  {s:'Running',       role:0, pat:'comet',   d:'working'},
  {s:'Needs input',   role:1, pat:'breathe', d:'waiting on you'},
  {s:'Approval',      role:3, pat:'blink',   d:'needs your decision'},
  {s:'Done',          role:2, pat:'fade',    d:'finished'},
  {s:'Error',         role:-1,pat:'solid',   d:'something went wrong'},
];
function renderStatusLegend(){
  const box=$('statusLegend'); if(!box)return; box.innerHTML='';
  const t=(THEMES||[]).find(x=>x.name===_curTheme)||(THEMES||[])[0]; const cols=(t&&t.colors)||[];
  const rows=ROLES?ROLES.map(r=>({s:r.status,role:r.role,pat:r.anim,d:r.desc})):STATUS_LEGEND;
  rows.forEach(e=>{
    const row=document.createElement('div');
    row.style.cssText='display:flex;align-items:center;gap:8px;margin:2px 0;font-size:12px;color:#bbb';
    const sw=document.createElement('span');
    const c=e.role<0?(cols[cols.length-1]||[200,40,40]):(cols[e.role]||[120,120,120]);
    sw.style.cssText='width:16px;height:16px;border-radius:4px;flex:0 0 auto;background:rgb('+c.join(',')+')';
    const lab=document.createElement('span');lab.style.cssText='min-width:92px;color:#ddd';lab.textContent=e.s;
    const pat=document.createElement('span');pat.style.cssText='min-width:64px;color:#7fd1c8';pat.textContent=e.pat;
    const dsc=document.createElement('span');dsc.textContent=e.d;
    row.appendChild(sw);row.appendChild(lab);row.appendChild(pat);row.appendChild(dsc);box.appendChild(row);});
}

// ---- Live LED-ring simulator: draws the 45-LED ring in the SELECTED theme + the
// selected status's animation, so a theme/mode choice is previewable on the page.
// Reads THEMES/_curTheme live each frame, so applying a theme recolours it instantly.
// Mirrors the on-device status language: comet=running, breathe=needs-you/approval/
// error, fade=done-ember, static=idle; Full lights the whole ring, Balanced/Dark the
// single cue LED (Dark idle/done = off).
let _rs={status:'Running',role:0,anim:'comet',posture:'Full',t0:null,ready:false};
function ringPalette(){const t=(THEMES||[]).find(x=>x.name===_curTheme)||(THEMES||[])[0];return (t&&t.colors)||[[127,209,200]];}
function ringStatuses(){
  const base=ROLES?ROLES.map(r=>({s:r.status,role:r.role,anim:r.anim})):STATUS_LEGEND.map(e=>({s:e.s,role:e.role,anim:e.pat}));
  return base.concat([{s:'Idle',role:-2,anim:'static'}]);
}
function _rsBtn(on){return 'padding:5px 10px;font-size:12px;border-radius:7px;cursor:pointer;border:1px solid '+(on?'#7fd1c8':'#333')+';background:'+(on?'#12312e':'#1a1a1a')+';color:'+(on?'#7fd1c8':'#9ab');}
function renderRingSimControls(){
  const sb=$('ringsimStatus'), pb=$('ringsimPosture'); if(!sb||!pb)return;
  sb.innerHTML='';pb.innerHTML='';
  ringStatuses().forEach(st=>{const b=document.createElement('button');b.type='button';b.textContent=st.s;
    b.style.cssText=_rsBtn(st.s===_rs.status);
    b.onclick=()=>{_rs.status=st.s;_rs.role=st.role;_rs.anim=st.anim;_rs.t0=null;renderRingSimControls();};sb.appendChild(b);});
  ['Full','Balanced','Dark'].forEach(p=>{const b=document.createElement('button');b.type='button';b.textContent=p;
    b.style.cssText=_rsBtn(p===_rs.posture);b.onclick=()=>{_rs.posture=p;_rs.t0=null;renderRingSimControls();};pb.appendChild(b);});
}
function ringSimStart(){
  const cv=$('ringsim'); if(!cv||_rs.ready)return; _rs.ready=true;
  const ctx=cv.getContext('2d'), N=45, R=190, cx=220, cy=220;
  const reduce=matchMedia('(prefers-reduced-motion:reduce)').matches;
  function frame(ts){
    if(_rs.t0===null)_rs.t0=ts; const el=(ts-_rs.t0)/1000;
    const pal=ringPalette();
    const col=_rs.role===-2?[70,80,90]:(_rs.role<0?(pal[pal.length-1]||[220,60,70]):(pal[_rs.role]||pal[0]));
    ctx.clearRect(0,0,440,440);
    ctx.beginPath();ctx.arc(cx,cy,R,0,Math.PI*2);ctx.strokeStyle='rgba(255,255,255,.04)';ctx.lineWidth=2;ctx.stroke();
    const breathe=0.5+0.5*Math.sin(el*2.4), comet=(el*0.16)%1;
    const single=_rs.posture!=='Full', dark=_rs.posture==='Dark', idle=_rs.role===-2, fade=_rs.anim==='fade', err=_rs.role===-1;
    // Rainbow theme: rotating full-wheel gradient (~6 s/rev, matches the device's
    // nowMs*11 16-bit phase = 60.4 deg/s). Error keeps its fixed alert hue and Idle
    // stays dull - same exemptions as the firmware Animator.
    const rb=_curTheme==='rainbow'&&!err&&!idle;
    const ledCol=i=>{if(!rb)return 'rgb('+col[0]+','+col[1]+','+col[2]+')';
      return 'hsl('+(((i/N)*360+el*60.4)%360).toFixed(1)+',100%,55%)';};
    for(let i=0;i<N;i++){
      const a=(i/N)*Math.PI*2-Math.PI/2, x=cx+Math.cos(a)*R, y=cy+Math.sin(a)*R;
      let alpha=0, glow=0;
      if(single){
        if(i!==0){alpha=0;}
        else if(dark){ if(err){alpha=0.3+0.7*breathe;glow=8+8*breathe;} else {alpha=0;} }  // Dark = OFF, only Error breathes red
        else if(_rs.anim==='comet'||_rs.anim==='breathe'){alpha=0.3+0.7*breathe;glow=8+8*breathe;}  // Balanced single cue
        else if(fade){alpha=0.38;glow=4;}
        else if(idle){alpha=0.14;glow=0;}
        else {alpha=0.55;glow=4;}
      } else if(idle){alpha=0.12;glow=0;}
      else if(_rs.anim==='comet'){let d=Math.abs(((i/N-comet)+1)%1);d=Math.min(d,1-d);const h=Math.max(0,1-d*7);alpha=0.25+0.75*h;glow=5+14*h;}
      else if(_rs.anim==='breathe'){alpha=0.3+0.65*breathe;glow=6+8*breathe;}
      else if(fade){alpha=0.38;glow=4;}
      else {alpha=0.7;glow=5;}
      if(alpha<=0)continue;
      const cc=ledCol(i);
      ctx.beginPath();ctx.arc(x,y,7,0,Math.PI*2);
      ctx.globalAlpha=alpha;ctx.fillStyle=cc;
      ctx.shadowColor=cc;ctx.shadowBlur=glow;ctx.fill();ctx.shadowBlur=0;ctx.globalAlpha=1;
    }
    if(!reduce)requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

function vfyBadge(v,ts){
  if(ts===0) return '<span class="badge vfy unk">unverified</span>';
  if(v===1)  return '<span class="badge vfy ok">verified</span>';
  if(v===0)  return '<span class="badge vfy bad">key rejected</span>';
  // v===-1: couldn't verify. In Notifier mode the TLS handshake can't get enough
  // free memory - guide the user to verify from Orchestrator mode.
  const hint=(ORCH && !ORCH.running) ? 'verify from Orchestrator mode' : 'Couldn\'t verify - retry';
  return '<span class="badge vfy unk">'+hint+'</span>';
}
function modelSel(id,cur,choices,verified){
  const s=document.createElement('select'); s.id=id; s.disabled=!verified;
  const d=document.createElement('option'); d.value=''; d.textContent='(default)';
  s.appendChild(d);
  choices.split(',').forEach(m=>{const o=document.createElement('option');
    o.value=m;o.textContent=m;if(m===cur)o.selected=true;s.appendChild(o);});
  if(!verified){const t=document.createElement('option');
    t.value='';t.textContent='Verify the key to unlock';t.selected=!cur;s.appendChild(t);}
  s.dataset.sig=choices+'|'+(verified?1:0);   // rebuild-only-on-change signature (P1.1)
  s.onchange=()=>orchApply({[id]:s.value});
  return s;
}
function provRow(name,p){
  const w=document.createElement('div'); w.className='provrow'; w.id='prov_'+name;
  const h=document.createElement('div'); h.className='provhead';
  h.innerHTML='<b>'+(PROVLBL[name]||name)+'</b>'+vfyBadge(p.verify,p.vts);
  w.appendChild(h);
  const row=document.createElement('div'); row.className='row';
  const k=document.createElement('input'); k.type='password'; k.id='key_'+name;
  k.placeholder=p.hasKey?'Key set - type to replace':'API key';
  row.appendChild(k);
  const vb=document.createElement('button'); vb.type='button'; vb.id='vfy_'+name;
  const canV=!ORCH||ORCH.running;   // verify needs Orchestrator-mode heap (lexical ORCH - window.ORCH was always undefined)
  vb.textContent=canV?(p.hasKey?'Verify':'Save & Verify'):'Save Key';
  vb.title=canV?'check the key against the provider':'The key is saved now; to verify it, switch to Orchestrator mode';
  vb.onclick=()=>saveAndVerify(name);
  row.appendChild(vb);
  const cb=document.createElement('button'); cb.type='button'; cb.textContent='Clear';
  cb.onclick=()=>{if(confirm('Remove the '+(PROVLBL[name]||name)+' key?'))orchApply({['clr_'+keyField(name)]:1});};
  row.appendChild(cb); w.appendChild(row);
  const mrow=document.createElement('div'); mrow.className='row';
  const l1=document.createElement('div'); l1.style.flex='1';
  l1.innerHTML='<label>Orchestrator model</label>';
  l1.appendChild(modelSel('orchM_'+name,p.orchModel,p.choices,p.verify===1));
  const l2=document.createElement('div'); l2.style.flex='1';
  l2.innerHTML='<label>Session model</label>';
  l2.appendChild(modelSel('subM_'+name,p.subModel,p.choices,p.verify===1));
  mrow.appendChild(l1); mrow.appendChild(l2); w.appendChild(mrow);
  return w;
}
function keyField(p){return p==='openai'?'oaiKey':p==='anthropic'?'antKey':'mistKey';}

// In-place sync of a provider row on the 5s poll - the old unconditional rebuild of
// #provs destroyed the API-key input mid-typing (the field-QA "key deletes itself
// after 1-2s" bug, audit P1.1). The key input's VALUE is never server-synced (the
// server never returns keys) - only its placeholder; selects rebuild only when their
// choices/verified signature changes and never while focused.
function provSync(name,p){
  const w=$('prov_'+name); if(!w)return;
  const h=w.querySelector('.provhead');
  if(h)h.innerHTML='<b>'+(PROVLBL[name]||name)+'</b>'+vfyBadge(p.verify,p.vts);
  const k=$('key_'+name);
  if(k&&document.activeElement!==k&&!k.value)
    k.placeholder=p.hasKey?'Key set - type to replace':'API key';
  const vb=$('vfy_'+name);
  if(vb&&!vb.disabled){const canV=!ORCH||ORCH.running;
    vb.textContent=canV?(p.hasKey?'Verify':'Save & Verify'):'Save Key';}
  syncModelSel('orchM_'+name,p.orchModel,p.choices,p.verify===1);
  syncModelSel('subM_'+name,p.subModel,p.choices,p.verify===1);
}
function syncModelSel(id,cur,choices,verified){
  const s=$(id); if(!s||document.activeElement===s)return;   // mid-pick: hands off
  const sig=choices+'|'+(verified?1:0);
  if(s.dataset.sig!==sig){s.replaceWith(modelSel(id,cur,choices,verified));return;}
  if(s.value!==(cur||''))s.value=cur||'';
}
function applyOrch(d){
  if($('fetchpol')&&d.fetchPol!=null)$('fetchpol').value=d.fetchPol;
  ORCH=d;
  $('orchoff').style.display=d.running?'none':'inline-block';
  const host=$('provs');
  const sig=Object.keys(d.providers).join();
  if(host.dataset.sig!==sig){       // first load / provider set changed -> build
    host.dataset.sig=sig; host.innerHTML='';
    Object.keys(d.providers).forEach(n=>host.appendChild(provRow(n,d.providers[n])));
  } else Object.keys(d.providers).forEach(n=>provSync(n,d.providers[n]));
  // only fill inputs the user is not editing right now
  const set=(id,v)=>{const e=$(id);if(e&&document.activeElement!==e)e.value=v;};
  set('custBase',d.cust.base); set('custConv',d.cust.conv); set('custModel',d.cust.model);
  $('custKey').placeholder=d.cust.hasKey?'Key set - type to replace':'API key';
  set('orchHost',d.orchHost);
  renderPrio('provPrioList','provPrio',d.provPrio||'');
  renderPrio('subPrioList','subPrio',d.subPrio||'');
  if(d.sttProv){set('sttProv',d.sttProv); $('sttProv').onchange=()=>orchApply({sttProv:$('sttProv').value});}
  if(d.ttsProv){set('ttsProv',d.ttsProv);
    $('ttsProv').onchange=()=>orchApply({ttsProv:$('ttsProv').value,ttsVoice:''});
    // Only rebuild the voice list when the PROVIDER changed (or first load). The
    // 5s state poll re-runs applyOrch; re-filling every time would clobber a voice
    // the user just picked (this was the "switch doesn't update / reverts" bug).
    if(window._vp!==d.ttsProv){window._vp=d.ttsProv;fillVoices(d.ttsProv, d.ttsVoice||'');}}
  // Theme swatches: palettes are fetched from /api/themes (cached in THEMES) and
  // rendered against the live selection. Fetch lazily on first sight of d.theme.
  if(d.theme!==undefined){_curTheme=d.theme; if(THEMES){renderThemeChips();renderStatusLegend();}else loadThemes();}
  if(d.hasTav!==undefined){$('tavKey').placeholder=d.hasTav?'Key set - type to replace':'Tavily API key';
    // Honest three-state status (owner: "unverified? verified but no confirmation?"):
    // verified via a real search / rejected / saved-but-unverified / no key.
    const tv=!d.hasTav?'Web search is off - add a key to enable it'
      :d.tavVerify===1?'✓ Web search ready - key verified'
      :d.tavVerify===0?'✗ Key rejected by Tavily - check it'
      :d.tavVts>0?'⚠ Key saved but not yet verified - save again to retry'
      :'⏳ Key saved - verifying…';
    $('tavstat').textContent=tv;
    $('tavstat').style.color=!d.hasTav?'':d.tavVerify===1?'#7fd1c8':d.tavVerify===0?'#f0687a':'#e0b870';}
  $('tgToken').placeholder=d.hasTg?'Token set - type to replace':'Bot token';
  // Telegram READINESS (P2.6): every gate that silently eats a reply, made visible.
  {const r=$('tgReady');
   if(r){const parts=[];
    if(!d.hasTg)parts.push('✗ No bot token - Telegram is off (create one with @BotFather)');
    else if(!d.tgLive)parts.push(d.running
      ?'⚠ Token saved - restart the device to start Telegram'
      :'⚠ Token saved - Telegram runs in Orchestrator mode (switch modes to start it)');
    else parts.push('✓ Bot active');
    // Verify-on-save verdict (owner: "run a tiny verification"): a real getMe check.
    if(d.hasTg){
      if(d.tgVerify===1)parts.push('✓ Token verified'+(d.tgBot?(' - bot @'+d.tgBot):''));
      else if(d.tgVerify===0)parts.push('✗ Token rejected by Telegram - check it');
      else if(d.tgVts>0)parts.push('⚠ Couldn’t verify the token - retry');
      else parts.push('⏳ Verifying the token…');
      // Resolve the save-flow message once the verdict lands (it used to say
      // "verifying…" forever - the verdict only appeared in this readiness line).
      if(_tgVerifyWatch&&(d.tgVerify===1||d.tgVerify===0||d.tgVts>0)){
        _tgVerifyWatch=false;
        const m=$('orchmsg');
        if(m){
          m.textContent=d.tgVerify===1?'✓ Telegram token verified - the bot is live on the new token.'
                       :d.tgVerify===0?'✗ Telegram rejected the token - double-check it.'
                       :'⚠ Couldn’t verify the token yet - it is saved; try again in a moment.';
          m.style.cssText=d.tgVerify===1?'color:#7fd1c8;font-weight:bold':'color:#e0b870;font-weight:bold';
        }
      }
    }
    const ids=(d.tgAllow||'').split(',').filter(x=>x.trim());
    parts.push(ids.length?('✓ '+ids.length+' allowed chat'+(ids.length>1?'s':'')) :
      '✗ No one approved yet - message the bot once, then approve it below');
    const sp=d.sttProv&&d.providers&&d.providers[d.sttProv];
    parts.push(sp&&sp.hasKey?('✓ voice notes ('+d.sttProv+' STT)') :
      '⚠ No '+(d.sttProv||'dictation')+' key - voice notes can’t be transcribed');
    r.textContent=parts.join('  ·  ');
    r.style.color=parts.some(p=>p[0]==='✗')?'#e77':(parts.some(p=>p[0]==='⚠')?'#e0b870':'#7fd1c8');}}
  $('orchLoop').checked=!!d.orchLoop;
  if($('midFail'))$('midFail').checked=!!d.midFail;
  if(d.orchTrace!==undefined){$('orchTrace').checked=!!d.orchTrace;
    GB.trace=!!d.orchTrace;_chatTraceHint();}   // glass-box honesty (no extra poll)
  // "Voice replies" toggle (P2.5): applies immediately (like the SFX controls).
  if(d.ttsOn!==undefined){const tv=$('ttsOn');
    if(tv){tv.checked=!!d.ttsOn; tv.onchange=()=>orchApply({ttsOn:tv.checked?1:0});}}
  if(d.loopRounds!==undefined)set('loopRounds',d.loopRounds);
  if(d.compactKB!==undefined)set('compactKB',d.compactKB);
  if(d.tlsSlots!==undefined)set('tlsSlots',d.tlsSlots);
  if(d.tlsVerify!==undefined){const tf=$('tlsVerify');if(tf)tf.checked=!!d.tlsVerify;}
  if(d.capProbe!==undefined)set('capProbe',d.capProbe);
  if(d.capProbeH!==undefined)set('capProbeH',d.capProbeH);
  if($('capProbe'))capProbeUpd();
  if(d.loopDeadline!==undefined)set('loopDeadline',d.loopDeadline);
  // Token usage tiles (real billed in/out tokens, last turn + session running total)
  if(d.usage&&$('usageTiles')){const u=d.usage,kt=n=>(n||0)>=1000?((n/1000).toFixed(1)+'K'):(''+(n||0));
    $('usageTiles').innerHTML=
      _tile('This session',kt(u.sessIn+u.sessOut),'in '+kt(u.sessIn)+' · out '+kt(u.sessOut)+' · '+(u.turns||0)+' turns',0,'')+
      _tile('Last turn',kt(u.lastIn+u.lastOut),'in '+kt(u.lastIn)+' · out '+kt(u.lastOut),0,'');
    _usageBP=u.byProvider||[];
    renderBudgets(_usageBP);
    renderUsageSummary();
    renderUsageChart();
    // Populate the rate editor from the stored rates (review: without this, the
    // initially-selected provider's fields stayed blank and a Save silently wiped
    // its stored rates to 0). Never clobber a field the owner is typing in.
    const _ra=document.activeElement;
    if(_ra!==$('rate_in')&&_ra!==$('rate_out')&&_ra!==$('rate_call'))_fillRateInputs();}
  if(d.sfxLvlN!==undefined){$('sfxLvlN').value=d.sfxLvlN;$('sfxLvlO').value=d.sfxLvlO;$('sfxTheme').value=d.sfxTheme;
    if(d.sfxVol!==undefined){$('sfxVol').value=d.sfxVol;$('sfxVolPct').textContent=d.sfxVol+'%';}
    $('sfxtier').textContent='tier: '+d.sfxTier+(d.sfxSync&&d.sfxSync!=='idle'?' · '+d.sfxSync:'');}
  set('directive',d.directive);
  $('memview').textContent=d.mem||'(empty)';
  const j=$('jobs'), dj=$('dashJobs');
  if(!d.jobs||!d.jobs.length){const t=d.running?'none running':'(Orchestrator mode is off)';
    if(j)j.textContent=t; if(dj)dj.textContent=t;}
  else{let h='<table><tr><th>Session</th><th>Provider</th><th>Model</th><th>Category</th><th>State</th></tr>';
    d.jobs.forEach(x=>{h+='<tr><td>'+x.tag+'</td><td>'+x.backend+'</td><td>'+x.model+
      '</td><td>'+x.category+'</td><td>'+x.state+'</td></tr>';});
    h+='</table>'; if(j)j.innerHTML=h; if(dj)dj.innerHTML=h;}
}
// Per-provider monthly budget rows (owner: budget per provider). Each provider that
// has usage OR a set limit renders a labelled bar: tokens for LLM hosts, calls for
// search providers. Over-budget rows go red. 0 limit shows the raw count, no bar.
const BUDLBL={openai:'OpenAI',anthropic:'Anthropic',mistral:'Mistral',tavily:'Tavily',custom:'Custom'};
// ---- W18: URL-download queue (policy select + pending/approve cards) --------
function loadFetchQ(){fetch('/api/fetchq').then(r=>r.json()).then(renderFetchQ).catch(()=>{});}
function renderFetchQ(rows){
  const box=$('fetchRows'); if(!box)return;
  const esc=t=>(t||'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
  if(!rows.length){box.innerHTML='';return;}
  let h='';
  rows.forEach(r=>{
    const st=r.state;
    const badge=st==='pending'?'<span class="badge vfy warn">waiting for you</span>'
      :st==='done'?'<span class="badge vfy ok">saved</span>'
      :st==='failed'?'<span class="badge vfy bad">failed</span>'
      :st==='denied'?'<span class="badge vfy bad">denied</span>'
      :'<span class="badge vfy">'+st+'</span>';
    h+='<div class=provrow style="border-top:0;padding-top:8px;margin-top:8px">'+
       '<div class=row style="justify-content:space-between;gap:8px"><div style="min-width:0">'+
       '<b>'+esc(r.project)+'/'+esc(r.name)+'</b> '+badge+
       '<div class=hint style="overflow-wrap:anywhere">'+esc(r.url)+'</div>'+
       (r.err?'<div class=hint>'+esc(r.err)+'</div>':'')+
       (r.bytes?'<div class=hint>'+Math.round(r.bytes/1024)+' KB</div>':'')+'</div>'+
       (st==='pending'?('<div class=row style="gap:6px;flex:0 0 auto">'+
         '<button type=button onclick="fetchQAct('+r.id+',\'approve\')">Approve</button>'+
         '<button type=button class=warn onclick="fetchQAct('+r.id+',\'deny\')">Deny</button></div>'):'')+
       '</div></div>';
  });
  box.innerHTML=h;
}
function fetchQAct(id,op){
  fetch('/api/fetchq',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'id='+id+'&op='+op}).then(()=>loadFetchQ());
}
if($('fetchpolsave'))$('fetchpolsave').onclick=()=>{
  orchApply({fetchPol:$('fetchpol').value}).then(ok=>{
    if(ok!==false)$('fetchpolmsg').textContent='Saved.';
  });
};
function renderBudgets(bp){
  const box=$('budgetRows'); if(!box)return;
  if(!bp.length){box.innerHTML='<p class=hint>No provider usage yet this month. Set a budget below to start tracking against a cap.</p>';return;}
  const kt=n=>(n||0)>=1000?((n/1000).toFixed(1)+'K'):(''+(n||0));
  let h='';
  bp.forEach(p=>{
    const isCall=(p.callLimit>0)||(p.calls>0&&!p.tokens);
    const used=isCall?(p.calls||0):(p.tokens||0);
    const lim=isCall?(p.callLimit||0):(p.tokenLimit||0);
    const unit=isCall?'calls':'tokens';
    // W16: when a $ cap is set the bar tracks DOLLARS (the enforced axis);
    // otherwise the token/call axis as before. estCents is server-computed -
    // the same formula the refusal gate uses.
    const hasUsd=(p.centsLimit||0)>0;
    const pct=hasUsd?Math.min(100,(p.estCents||0)/p.centsLimit*100)
                    :(lim>0?Math.min(100,used/lim*100):0);
    const col=p.over?'#f0687a':(pct>80?'#e0b870':'#7fd1c8');
    const io=(p.tokIn||p.tokOut)?('in '+kt(p.tokIn)+' / out '+kt(p.tokOut)+' · '):'';
    const noRates=hasUsd&&!(p.rateIn||p.rateOut||p.rateCall);
    const spend=hasUsd?(_usd(p.estCents||0,false)+' / '+_usd(p.centsLimit,false))
                      :_usd((p.estCents!=null?p.estCents:_cost(p.prov,p.tokIn||0,p.tokOut||0,p.calls||0).c),
                            p.estCents==null);
    const right=(lim>0?(kt(used)+' / '+kt(lim)+' '+unit):(kt(used)+' '+unit))+
                ' · '+spend+' · resets day '+(p.resetDay||1);
    h+='<div class=provrow style="border-top:0;padding-top:8px;margin-top:8px"><div class=row style="justify-content:space-between">'+
       '<b>'+(BUDLBL[p.prov]||p.prov)+(p.over?' <span class="badge vfy bad">over budget</span>':'')+'</b>'+
       '<span class=hint>'+io+right+'</span></div>'+
       ((lim>0||hasUsd)?'<div class=g style="margin-top:4px"><i style="width:'+pct+'%;background:'+col+'"></i></div>':'')+
       (noRates?'<p class=hint style="margin:2px 0 0">The $ cap needs prices &mdash; set rates above or this cap can\'t count.</p>':'')+'</div>';
  });
  box.innerHTML=h;
}
// ---- Spend graphs + $ estimates (owner ask) ----------------------------------
// $ figures are ESTIMATES: cents = tokens x the per-provider rates (owner-editable;
// 0 falls back to the built-in defaults below, marked ~). Counts stay honest raw data.
const DEF_RATES={openai:{i:250,o:1000,c:0},anthropic:{i:300,o:1500,c:0},
                 mistral:{i:200,o:600,c:0},tavily:{i:0,o:0,c:800},custom:{i:0,o:0,c:0}};
const PROVCOL={openai:'#7fd1c8',anthropic:'#f0b45a',mistral:'#f0947a',tavily:'#6cb8ff',custom:'#a7adba'};
let _usageBP=[],_usageHist=null,_chartDays=30,_chartUnit='tok';
// Per-FIELD default fallback (review: the all-or-nothing gate priced output tokens
// at $0 when only the input rate was set, silently under-reporting ~80%). Any field
// stored as 0 uses its ~default; def=true when a nonzero default was actually used,
// carried PER COMPUTATION (the old sticky global marked owner-set rows with ~).
function _rateFor(prov){
  const p=_usageBP.find(x=>x.prov===prov)||{};
  const d=DEF_RATES[prov]||{i:0,o:0,c:0};
  return {i:p.rateIn||d.i,o:p.rateOut||d.o,c:p.rateCall||d.c,
          def:(!p.rateIn&&!!d.i)||(!p.rateOut&&!!d.o)||(!p.rateCall&&!!d.c)};
}
function _cost(prov,tin,tout,calls){
  const r=_rateFor(prov);
  return {c:tin/1e6*r.i + tout/1e6*r.o + calls/1000*r.c, def:r.def};
}
function _costCents(prov,tin,tout,calls){return _cost(prov,tin,tout,calls).c;}
function _usd(cents,approx){return (cents>=100?'$'+(cents/100).toFixed(2):'$'+(cents/100).toFixed(3))+(approx?'~':'');}
function _kt2(n){return (n||0)>=1e6?((n/1e6).toFixed(2)+'M'):(n||0)>=1000?((n/1000).toFixed(1)+'K'):(''+(n||0));}
function renderUsageSummary(){
  const box=$('usageSummary'); if(!box)return;
  // All-time + this billing period come from the ledger; 7/30-day from the buckets.
  let atI=0,atO=0,atC=0,pI=0,pO=0,pC=0,atCost=0,pCost=0,atDef=false,pDef=false;
  _usageBP.forEach(p=>{atI+=p.totIn||0;atO+=p.totOut||0;atC+=p.totCalls||0;
    pI+=p.tokIn||0;pO+=p.tokOut||0;pC+=p.calls||0;
    const a=_cost(p.prov,p.totIn||0,p.totOut||0,p.totCalls||0);atCost+=a.c;atDef=atDef||a.def;
    const b=_cost(p.prov,p.tokIn||0,p.tokOut||0,p.calls||0);pCost+=b.c;pDef=pDef||b.def;});
  const win=(days)=>{let i=0,o=0,c=0,cost=0,def=false;
    // today==0 = clock never synced this boot; the persisted buckets carry REAL day
    // keys, so an unguarded cut of (0-days) would sum the whole retained history
    // into a "last 7 days" tile (review). Show zeros until the clock is sane.
    if(_usageHist&&_usageHist.today){const cut=_usageHist.today-days;
      _usageHist.days.forEach(b=>{if(b.d>cut){i+=b.in;o+=b.out;c+=b.calls;
        const w=_cost(b.prov,b.in,b.out,b.calls);cost+=w.c;def=def||w.def;}});}
    return {t:i+o,c:c,cost:cost,def:def};};
  const w7=win(7),w30=win(30);
  box.innerHTML=
    _tile('All time',_kt2(atI+atO),_usd(atCost,atDef)+' est · '+_kt2(atC)+' calls',0,'')+
    _tile('This period',_kt2(pI+pO),_usd(pCost,pDef)+' est · billing window',0,'')+
    _tile('Last 30 days',_kt2(w30.t),_usd(w30.cost,w30.def)+' est · '+_kt2(w30.c)+' calls',0,'')+
    _tile('Last 7 days',_kt2(w7.t),_usd(w7.cost,w7.def)+' est · '+_kt2(w7.c)+' calls',0,'');
}
function loadUsageHistory(){
  fetch('/api/usage/history').then(jok).then(d=>{_usageHist=d;renderUsageSummary();renderUsageChart();}).catch(()=>{});
}
function renderUsageChart(){
  const cv=$('usageChart'); if(!cv||!_usageHist)return;
  const ctx=cv.getContext('2d'),W=cv.width,H=cv.height,padL=52,padB=22,padT=10;
  ctx.clearRect(0,0,W,H);
  const today=_usageHist.today||0,days=_chartDays;
  if(!today){$('usageChartMsg').textContent='Device clock not set yet - daily history starts once it syncs.';return;}
  // day -> {prov: value}; value = tokens or est cents per the unit toggle.
  const perDay={},provs=new Set();
  _usageHist.days.forEach(b=>{const off=today-b.d;
    if(off<0||off>=days)return;
    const v=_chartUnit==='tok'?(b.in+b.out):_costCents(b.prov,b.in,b.out,b.calls);
    if(!perDay[b.d])perDay[b.d]={};
    perDay[b.d][b.prov]=(perDay[b.d][b.prov]||0)+v;
    provs.add(b.prov);});
  const plist=[...provs].sort();
  let maxV=0;
  for(let i=0;i<days;i++){const d=today-days+1+i,row=perDay[d];if(!row)continue;
    maxV=Math.max(maxV,Object.values(row).reduce((a,b)=>a+b,0));}
  const msg=$('usageChartMsg');
  if(!maxV){msg.textContent='No usage recorded in this window yet - run a chat/Telegram turn and it will appear here.';}
  else msg.textContent='';
  const scale=maxV?(H-padT-padB)/maxV:0;
  // gridlines + y labels
  ctx.font='11px system-ui';ctx.fillStyle='rgba(160,170,185,.8)';ctx.strokeStyle='rgba(255,255,255,.06)';
  for(let g=0;g<=3;g++){const v=maxV*g/3,y=H-padB-v*scale;
    ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-6,y);ctx.stroke();
    ctx.fillText(_chartUnit==='tok'?_kt2(Math.round(v)):('$'+(v/100).toFixed(v>=100?1:2)),4,y+4);}
  // stacked daily bars
  const bw=Math.max(2,Math.floor((W-padL-10)/days)-2);
  for(let i=0;i<days;i++){
    const d=today-days+1+i,row=perDay[d];
    const x=padL+i*((W-padL-10)/days);
    let y=H-padB;
    if(row)plist.forEach(p=>{const v=row[p]||0;if(!v)return;const h=v*scale;
      ctx.fillStyle=PROVCOL[p]||'#888';ctx.fillRect(x,y-h,bw,h);y-=h;});
    // x labels: ~6 ticks
    if(days<=7||i%Math.ceil(days/6)===0){
      const dt=new Date(d*86400000);ctx.fillStyle='rgba(160,170,185,.8)';
      ctx.fillText((dt.getUTCMonth()+1)+'/'+dt.getUTCDate(),x-2,H-6);}
  }
  // legend
  const lg=$('usageLegend');
  if(lg)lg.innerHTML=plist.map(p=>'<span class=hint><span style="display:inline-block;width:9px;height:9px;border-radius:2px;background:'+(PROVCOL[p]||'#888')+';margin-right:4px"></span>'+(BUDLBL[p]||p)+'</span>').join('');
}
document.querySelectorAll('.rangeBtn').forEach(b=>b.onclick=()=>{_chartDays=+b.dataset.days;_syncChartBtns();renderUsageChart();});
if($('unitTok'))$('unitTok').onclick=()=>{_chartUnit='tok';_syncChartBtns();renderUsageChart();};
if($('unitUsd'))$('unitUsd').onclick=()=>{_chartUnit='usd';_syncChartBtns();renderUsageChart();};
function _syncChartBtns(){
  document.querySelectorAll('.rangeBtn').forEach(b=>b.style.cssText='padding:5px 10px;font-size:12px'+(+b.dataset.days===_chartDays?';background:var(--teal);color:#052420':';background:var(--raise3);color:var(--ink2)'));
  const t=$('unitTok'),u=$('unitUsd');
  if(t)t.style.cssText='padding:5px 10px;font-size:12px'+(_chartUnit==='tok'?';background:var(--teal);color:#052420':';background:var(--raise3);color:var(--ink2)');
  if(u)u.style.cssText='padding:5px 10px;font-size:12px'+(_chartUnit==='usd'?';background:var(--teal);color:#052420':';background:var(--raise3);color:var(--ink2)');
}
// Rates editor: dollars in the UI, integer cents on the wire.
function _fillRateInputs(){
  const prov=$('rate_prov').value,p=_usageBP.find(x=>x.prov===prov)||{};
  $('rate_in').value=p.rateIn?(p.rateIn/100):'';
  $('rate_out').value=p.rateOut?(p.rateOut/100):'';
  $('rate_call').value=p.rateCall?(p.rateCall/100):'';
  const d=DEF_RATES[prov]||{};
  $('rate_in').placeholder='$/1M in'+(d.i?' (~'+(d.i/100)+')':'');
  $('rate_out').placeholder='$/1M out'+(d.o?' (~'+(d.o/100)+')':'');
  $('rate_call').placeholder='$/1k calls'+(d.c?' (~'+(d.c/100)+')':'');
}
if($('rate_prov'))$('rate_prov').onchange=_fillRateInputs;
if($('ratesave'))$('ratesave').onclick=()=>{
  const prov=$('rate_prov').value;
  const c=(v)=>Math.max(0,Math.round((parseFloat(v)||0)*100));
  orchApply({rates:prov+':'+c($('rate_in').value)+':'+c($('rate_out').value)+':'+c($('rate_call').value)})
    .then(ok=>{if(ok!==false){$('ratemsg').textContent='Rates saved for '+(BUDLBL[prov]||prov)+' - estimates updated.';loadOrch();loadUsageHistory();}});
};
_syncChartBtns();
// History loads lazily on the Usage tab (review: an unconditional page-load fetch
// pulled the multi-KB bucket payload in EVERY mode, including Notifier).
function loadOrch(){if(!canPoll())return;fetch('/api/orch').then(r=>r.json()).then(applyOrch).catch(()=>{});}
// Connectors: GET is SANITIZED (secrets -> hasTok/hasOauth flags), shown as status
// only; the textarea is for authoring and a save REPLACES the whole list (so a
// round-trip can never silently strip a stored token).
function connEsc(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
// Which providers actually run this connector, and how, right now.
function connAvail(prov,host,keyed){
  if(prov==='any')prov=host||'openai';
  if(!keyed||!keyed[prov])return{cls:'unk',txt:prov+' key missing'};
  if(prov==='openai')return host==='openai'?{cls:'ok',txt:'live on your turns'}:{cls:'ext',txt:'via sessions'};
  if(prov==='anthropic')return{cls:'ext',txt:'via sessions'};
  if(prov==='mistral')return host==='mistral'?{cls:'ok',txt:'live on your turns'}:{cls:'unk',txt:'idle - set host to Mistral'};
  return{cls:'unk',txt:'configured'};
}
// The kind a connector attaches as depends on the chosen provider.
var MISTRAL_BUILTINS={web_search:1,web_search_premium:1,code_interpreter:1,image_generation:1,document_library:1};
function connKindFor(prov,k){
  if(prov==='mistral')return MISTRAL_BUILTINS[k.id]?'builtin':'connector'; // hosted tool vs Studio connector
  if(prov==='openai'&&k.cid)return 'connector';                            // OpenAI first-party connector_id
  return 'mcp';                                                            // BYO remote MCP by URL (+ token)
}
// The connector_id to prefill: OpenAI first-party default, or the Studio name for Mistral.
function connCidDefault(prov,k){ return k.cid || (prov==='mistral'?k.id:''); }
function loadConnectors(){fetch('/api/connectors').then(r=>r.json()).then(d=>{
  if(Array.isArray(d))d={configured:d,known:[],keyed:{},host:''};       // legacy-shape guard
  const cfg=d.configured||[],known=d.known||[],keyed=d.keyed||{},host=d.host||'';
  $('connhost').textContent=host?('The assistant is running on '+host+'. Connectors on '+host+' are live on its own turns; the others are reachable through sessions it spawns.'):'';
  const byId={};cfg.forEach(c=>{byId[c.type||c.name]=c;if(!byId[c.name])byId[c.name]=c;});
  const knownIds={};known.forEach(k=>knownIds[k.id]=1);
  const extra=cfg.filter(c=>!knownIds[c.type]&&!knownIds[c.name]);
  window._connKnown=known;
  let html='';
  known.forEach(k=>{html+=connCard(k,byId[k.id],keyed,host);});
  extra.forEach(c=>{html+=connCard({id:c.name,name:c.name,providers:c.prov,kind:c.kind,cid:c.cid||'',cred:'Credential / token',desc:'Custom connector.',docs:''},c,keyed,host);});
  $('conncards').innerHTML=html||'<p class=hint>No connectors available.</p>';
  $('connstat').textContent=cfg.length?(cfg.length+' configured'):'';
}).catch(()=>{});}
function connCard(k,c,keyed,host){
  const set=!!c,en=set&&(c.en==1||c.en===true);
  const provs=(k.providers||'').split(',').filter(Boolean);if(!provs.length)provs.push('any');
  const curProv=set?c.prov:provs[0];
  let status;
  if(!set)status='<span class="badge vfy unk">not set</span>';
  else if(!en)status='<span class="badge vfy unk">off</span>';
  else{const a=connAvail(curProv,host,keyed);status='<span class="badge vfy '+a.cls+'">'+connEsc(a.txt)+'</span>';}
  const id=k.id,kind=connKindFor(curProv,k);
  const badges=provs.map(p=>'<span class="badge ext" style="margin-left:4px">'+connEsc(p)+'</span>').join('');
  const provSel='<select id="cc_'+id+'_prov" onchange="connProvChange(\''+connEsc(id)+'\')">'+provs.map(p=>'<option'+(p===curProv?' selected':'')+'>'+connEsc(p)+'</option>').join('')+'</select>';
  let h='<div style="display:flex;align-items:center;gap:6px;flex-wrap:wrap"><b>'+connEsc(k.name)+'</b>'+badges+'<span style="margin-left:auto">'+status+'</span></div>';
  h+='<p class=cx style="margin:6px 0">'+connEsc(k.desc||'')+(k.docs?(' <a href="https://ristllin.github.io/Nimbus/guides/connectors#'+connEsc(k.docs)+'" target=_blank>setup&rarr;</a>'):'')+'</p>';
  h+='<div class=row style="margin-top:6px">'+provSel+'</div>';
  // token field only for remote MCP (a pasted PAT/bearer). Mistral connectors carry
  // no device secret (Studio-auth); OpenAI first-party needs the OAuth block (helper
  // script / raw editor), not a single token.
  const showUrl=kind==='mcp',showCid=kind==='connector',showTok=kind==='mcp';
  h+='<div class=row id="cc_'+id+'_urlrow" style="margin-top:6px;display:'+(showUrl?'flex':'none')+'"><input id="cc_'+id+'_url" placeholder="MCP server URL (e.g. https://api.githubcopilot.com/mcp/)" value="'+connEsc(set?(c.url||''):'')+'"></div>';
  h+='<div class=row id="cc_'+id+'_cidrow" style="margin-top:6px;display:'+(showCid?'flex':'none')+'"><input id="cc_'+id+'_cid" placeholder="connector_id (Studio name / UUID)" value="'+connEsc(set&&c.cid?c.cid:connCidDefault(curProv,k))+'"></div>';
  const ph=(set&&c.hasTok)?'•••• saved - leave blank to keep':(k.cred||'Credential / token');
  h+='<div class=row id="cc_'+id+'_tokrow" style="margin-top:6px;display:'+(showTok?'flex':'none')+'"><input id="cc_'+id+'_tok" type=password placeholder="'+connEsc(ph)+'"></div>';
  h+='<div class=row style="margin-top:6px"><label class=pr><input type=checkbox id="cc_'+id+'_en"'+(en?' checked':'')+'> enabled</label>';
  h+='<button type=button onclick="saveConnCard(\''+connEsc(id)+'\')">Save</button>';
  if(set)h+='<button type=button onclick="delConn(\''+connEsc(c.name)+'\')">Remove</button>';
  h+='</div>';
  return '<div class=tile style="margin:8px 0">'+h+'</div>';
}
function connFindKnown(id){return (window._connKnown||[]).find(k=>k.id===id)||{id:id,kind:'mcp',cid:''};}
function connProvChange(id){
  const k=connFindKnown(id),prov=$('cc_'+id+'_prov').value,kind=connKindFor(prov,k);
  const set=(row,on)=>{const e=$('cc_'+id+'_'+row);if(e)e.style.display=on?'flex':'none';};
  set('urlrow',kind==='mcp');set('cidrow',kind==='connector');set('tokrow',kind==='mcp');
  if(kind==='connector'){const ci=$('cc_'+id+'_cid');if(ci&&!ci.value)ci.value=connCidDefault(prov,k);}
}
function saveConnCard(id){
  const k=connFindKnown(id),prov=$('cc_'+id+'_prov').value,kind=connKindFor(prov,k);
  const g=s=>{const e=$('cc_'+id+'_'+s);return e?e.value:'';};
  const patch={name:id,type:id,prov:prov,kind:kind,en:$('cc_'+id+'_en').checked?1:0};
  if(kind==='mcp')patch.url=g('url');
  if(kind==='connector')patch.cid=g('cid');
  if(kind!=='builtin'){const t=g('tok');if(t)patch.tok=t;}
  fetch('/api/connectors',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({patch:JSON.stringify(patch)}).toString()})
   .then(jok).then(x=>{toast(x.ok?'Saved':'Rejected');loadConnectors();}).catch(failToast);
}
function delConn(name){if(!confirm('Remove the '+name+' connector?'))return;
  fetch('/api/connectors',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({del:name}).toString()})
   .then(jok).then(x=>{toast('Removed');loadConnectors();}).catch(failToast);
}
function saveConnectors(){
  const v=$('connBlob').value.trim(); if(!v){toast('Paste a JSON array first');return;}
  try{const p=JSON.parse(v); if(!Array.isArray(p))throw 0;}catch(e){toast('Not a JSON array');return;}
  fetch('/api/connectors',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({blob:v}).toString()})
   .then(jok).then(d=>{toast(d.ok?'Connectors saved':'Rejected');$('connBlob').value='';loadConnectors();})
   .catch(failToast);
}
function orchApply(body){
  // Resolves to true/false (never rejects) - see apply() / audit P1.5.
  return fetch('/api/orch',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams(body).toString()})
   .then(jok).then(()=>{toast('Saved');loadOrch();return true;})
   .catch(e=>{failToast(e);return false;});
}
function saveAndVerify(p){
  const inp=$('key_'+p), body={};
  if(inp.value) body[keyField(p)]=inp.value;
  // In Notifier mode the live TLS verify can't get enough free RAM, so just SAVE the
  // key (writing NVS works in any mode) and tell the user to verify from Orchestrator.
  // (Lexical ORCH, not window.ORCH - a top-level `let` never creates a window
  // property, so the old guard was ALWAYS false and this branch was unreachable.)
  if(ORCH && !ORCH.running){
    const had=Object.keys(body).length;
    (had?orchApply(body):Promise.resolve()).then(()=>{
      toast(had?'Key saved - verify from Orchestrator mode':'Switch to Orchestrator mode to verify');
      loadOrch();});
    return;
  }
  $('vfy_'+p).textContent='Checking…'; $('vfy_'+p).disabled=true;
  const prevTs=ORCH.providers[p].vts;
  const go=Object.keys(body).length?orchApply(body):Promise.resolve();
  go.then(()=>fetch('/api/verify',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'provider='+encodeURIComponent(p)}))
    .then(()=>{
      clearInterval(VPOLL[p]); let tries=0;   // clear only THIS provider's poll
      VPOLL[p]=setInterval(()=>{
        fetch('/api/orch').then(r=>r.json()).then(d=>{
          if(d.providers[p].vts!==prevTs||++tries>20){
            clearInterval(VPOLL[p]);
            // Re-enable BEFORE applyOrch: with the P1.1 in-place sync (no full
            // rebuild) a disabled button is deliberately left alone, so the old
            // "rebuild recreates it enabled" no longer happens implicitly. Clear
            // the typed key too (it was saved to NVS before the verify) - the old
            // rebuild used to wipe it implicitly.
            const vb=$('vfy_'+p); if(vb)vb.disabled=false;
            if(inp)inp.value='';
            applyOrch(d);
          }
        }).catch(()=>{});
      },2000);
    }).catch(()=>{const vb=$('vfy_'+p); if(vb)vb.disabled=false; loadOrch();});
}
$('sfxLvlN').onchange=()=>orchApply({sfxLvlN:$('sfxLvlN').value});
$('sfxLvlO').onchange=()=>orchApply({sfxLvlO:$('sfxLvlO').value});
$('sfxTheme').onchange=()=>orchApply({sfxTheme:$('sfxTheme').value});
if($('tchCalSave'))$('tchCalSave').onclick=()=>{
  const v=$('tchCal').value.trim();
  // Mirror the device parser's rules EXACTLY (nimbus/touch_cal.h). A shape-only
  // check let min>=max and out-of-range values through: the device rejected them
  // and the page still said "Saved", so the owner was told a change happened
  // that did not.
  if(v){
    const p=v.split(',').map(x=>x.trim());
    if(p.length<4||p.length>5||p.some(x=>!/^\d+$/.test(x))){toast('Need 4 or 5 numbers');return;}
    const n=p.map(Number);
    if(n.slice(0,4).some(x=>x>4095)){toast('Values must be 0-4095');return;}
    if(n[0]>=n[1]||n[2]>=n[3]){toast('Each min must be below its max');return;}
    if(p.length===5&&n[4]>7){toast('Flags must be 0-7');return;}
  }
  orchApply({tchCal:v});
};
if($('scrModel'))$('scrModel').onchange=function(){
  const v=this.value,was=v==='tft'?'eink':'tft';
  if(!confirm('Change the display to '+(v==='tft'?'Touch':'E-ink')+'?\n\nSet this only if the screen fitted to the device changed. Restart to apply. If it does not match the hardware, the screen stays blank until you set it back.')){this.value=was;return;}
  orchApply({scrModel:v}).then(ok=>{if(ok)toast('Restart to apply');});
};
$('sfxVol').oninput=()=>{$('sfxVolPct').textContent=$('sfxVol').value+'%';};
$('sfxVol').onchange=()=>orchApply({sfxVol:$('sfxVol').value});
$('sfxPlay').onclick=()=>{const b=new URLSearchParams();b.set('slug',$('sfxSlug').value);
  fetch('/api/audio/sfx',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
  .then(r=>toast(r.ok?('Playing '+$('sfxSlug').value):'Play failed')).catch(()=>toast('Play failed'));};
// Prominent, HONEST save feedback (audit P1.5): success only in the success
// branch (orchApply resolves true/false now), failure named explicitly, and the
// verdict is styled + persistent until the next action (the 1.4s toast + tiny
// gray hint were both easy to miss and reported "Saved." even on failure).
function saveVerdict(ok,okMsg){
  const m=$('orchmsg'); if(!m)return;
  m.textContent=ok?('✓ '+okMsg):'✗ Save failed - nothing was stored. Check the connection and try again.';
  m.style.cssText=ok?'color:#7fd1c8;font-weight:bold':'color:#e77;font-weight:bold';
}
$('orchsave').onclick=()=>{
  $('orchmsg').textContent='Saving…';$('orchmsg').style.cssText='';
  // provPrio/subPrio deliberately NOT here (review HIGH): those inputs were replaced
  // by the #provPrioList/#subPrioList checkbox lists, which persist per-interaction
  // via renderPrio's own save() - the stale $('provPrio').value deref threw a
  // TypeError that made this button store NOTHING, silently, for every field.
  orchApply({custBase:$('custBase').value,custKey:$('custKey').value,
    custConv:$('custConv').value,custModel:$('custModel').value,
    orchHost:$('orchHost').value,
    tgToken:$('tgToken').value,
    orchLoop:$('orchLoop').checked?1:0,
    midFail:$('midFail')&&$('midFail').checked?1:0,
    orchTrace:$('orchTrace').checked?1:0,
    loopRounds:$('loopRounds').value,loopDeadline:$('loopDeadline').value,tlsSlots:$('tlsSlots').value,compactKB:$('compactKB').value,
    tlsVerify:$('tlsVerify').checked?1:0,
    capProbe:$('capProbe')?$('capProbe').value:1,
    capProbeH:($('capProbeH')&&$('capProbeH').value)?$('capProbeH').value:24})
  .then(ok=>{saveVerdict(ok,'Orchestrator settings saved.');
    if(ok){const hadTok=!!$('tgToken').value;$('custKey').value='';$('tgToken').value='';
      // The device runs a getMe verify on the arbited task (~a few s). Re-poll a few
      // times so the "verifying… → verified/rejected" verdict lands without a refresh.
      // _tgVerifyWatch makes applyOrch RESOLVE this message when the verdict arrives
      // (owner: "it never clears the verifying message").
      if(hadTok){saveVerdict(true,'Saved - verifying the Telegram token…');
        _tgVerifyWatch=true;
        [1500,3500,7000,15000].forEach(t=>setTimeout(loadOrch,t));}}});
};
let _tgVerifyWatch=false;
$('savedir').onclick=()=>orchApply({sysPrompt:$('directive').value})
  .then(ok=>saveVerdict(ok,'Directive saved - applies from the next turn.'));
$('clearmem').onclick=()=>{
  if(confirm('Erase the assistant\'s memory?\n\nYour directive is kept.'))
    orchApply({clearMem:1});
};

// ---- Wi-Fi: saved networks, scan, join (P3: BOUNDED, visible timeout, explicit verdicts) ----
// The old flow polled /scan forever and left "Saving..." on screen when the
// device reassociated mid-request - both read as "stuck". Every wait is now
// capped with a live countdown and ends in an explicit verdict.
// The device remembers several networks, so this is list CRUD (same shape as
// Routines above): loadWifi() renders what is saved, every button posts ONE
// action to /api/wifi, and the list is re-read afterwards rather than guessed at.
// No password is ever read back - joining a saved network re-uses the stored one.
let timer;
let _wifiSaved=[];              // SSIDs already remembered - marks the scan rows
function _wesc(s){return (s||'').replace(/[<>&"]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]));}
// One action -> one verdict. A 4xx body carries the sentence to show, so failures
// say what happened instead of a generic "failed" (and a 401 still trips the shim).
function wifiPost(action,extra){
 const fd=new FormData(); fd.append('action',action);
 Object.keys(extra||{}).forEach(k=>fd.append(k,extra[k]));
 return fetch('/api/wifi',{method:'POST',body:fd}).then(r=>
  r.json().catch(()=>({})).then(b=>{
   if(!r.ok)throw new Error(b.error||'Couldn\'t reach the device. Try again.');
   return b;}));
}
function loadWifi(){
 const box=$('wifiKnown'); if(!box)return;
 fetch('/api/wifi').then(jok).then(d=>{
  const nets=d.networks||[];
  _wifiSaved=nets.map(n=>n.ssid);
  if($('wifiCount'))$('wifiCount').textContent=nets.length+' of '+(d.max||5);
  if(!nets.length)box.innerHTML='<span class=hint>No saved networks yet. Add one below.</span>';
  else box.innerHTML='<table><tbody>'+nets.map(n=>
   '<tr><td><b>'+_wesc(n.ssid)+'</b>'+(n.current?' <span class=badge>in use</span>':'')+
   (n.open?' <span class=hint>open network</span>':'')+
   '</td><td style="text-align:right;white-space:nowrap">'+
   (n.current?'':'<button data-wcon="'+_wesc(n.ssid)+'">Connect</button> ')+
   '<button data-wfor="'+_wesc(n.ssid)+'"'+(n.current?' data-wcur=1':'')+'>Forget</button></td></tr>')
   .join('')+'</tbody></table>';
  box.querySelectorAll('button[data-wcon]').forEach(b=>b.onclick=()=>wifiConnect(b.dataset.wcon));
  box.querySelectorAll('button[data-wfor]').forEach(b=>b.onclick=()=>wifiForget(b.dataset.wfor,!!b.dataset.wcur));
  if($('wifiApMsg'))$('wifiApMsg').textContent=d.apUp
   ?('Temporary setup hotspot "'+(d.apSsid||'')+'" is available at '+(d.apIp||'')+'.')
   :(d.sta
     ?('Home Wi-Fi is connected at '+(d.staIp||'its LAN address')+'. The temporary setup hotspot is off; this is normal.')
     :'The device is offline and its temporary setup hotspot is unavailable. Restart the device.');
 }).catch(()=>{box.innerHTML='<span class=hint>Couldn\'t load saved networks. Try again.</span>';});
}
// WATCH the join with a bounded countdown (12 x 2.5 s = 30 s): poll /api/state for
// sta=true instead of leaving an eternal "Saving...".
function wifiWatchJoin(ssid){
 let left=12;
 const done=v=>{clearInterval(w);$('savewifi').disabled=false;$('msg').textContent=v;loadWifi();};
 $('msg').textContent='Saved. Joining "'+ssid+'"… up to 30s';
 const w=setInterval(()=>fetch('/api/state').then(r=>r.json()).then(s=>{
  if(s.sta&&s.staIp)return done('Connected: '+s.staIp+'  ('+(s.mdns||'')+')');
  if(--left<=0)return done('Not connected after 30 s - check the password. The device shows the live status on its display.');
  $('msg').textContent='Joining "'+ssid+'"… ~'+Math.ceil(left*2.5)+'s left';
 }).catch(()=>{ /* transient while the radio reassociates - countdown continues */
  if(--left<=0)done('Device unreachable - rejoin its setup network, or find the device on your own Wi-Fi.');
 }),2500);
}
function wifiConnect(ssid){
 $('savewifi').disabled=true;
 $('msg').textContent='Joining "'+ssid+'"…';
 wifiPost('connect',{ssid:ssid}).then(()=>wifiWatchJoin(ssid))
  .catch(e=>{$('savewifi').disabled=false;$('msg').textContent=e.message;});
}
function wifiForget(ssid,inUse){
 if(!confirm('Forget "'+ssid+'"?\n\nIts password is erased. You\'ll need it again to rejoin.'+
   (inUse?'\n\nThis is the network in use, so the device won\'t rejoin it after a restart.':'')))return;
 wifiPost('forget',{ssid:ssid}).then(d=>{
  toast('Network forgotten');
  $('msg').textContent=d.count?'':'No saved networks left - the device starts in setup mode after a restart.';
  loadWifi();
 }).catch(e=>{$('msg').textContent=e.message;});
}
function scan(){
 if(timer)return;                                   // one scan at a time
 $('scan').disabled=true;$('nets').innerHTML='';
 let left=12;                                       // 12 x 2.5 s = 30 s cap
 $('msg').textContent='Scanning… up to 30s';
 const stop=v=>{clearInterval(timer);timer=null;$('scan').disabled=false;$('msg').textContent=v;};
 // ?scan=1 returns the saved list AND the radio scan together, so "saved" can
 // never be marked from a stale list.
 const poll=()=>fetch('/api/wifi?scan=1').then(jok).then(d=>{
  const sc=d.scan||{};
  _wifiSaved=(d.networks||[]).map(n=>n.ssid);
  if(sc.scanning){
   if(--left<=0)return stop('Scan timed out (30 s) - try again.');
   $('msg').textContent='Scanning… ~'+Math.ceil(left*2.5)+'s left';return;
  }
  // The device now DISTINGUISHES "the scan failed" from "the scan found nothing"
  // - that distinction is the entire point of the scan-hang fix. Dropping the
  // error here re-merged them and printed 'No networks found.' at someone
  // surrounded by access points, which is not just unhelpful, it asserts the
  // opposite of the truth.
  if(sc.error)return stop(sc.error);
  const list=sc.networks||[];
  stop('Found '+list.length+' network(s).');
  list.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
   const el=document.createElement('div');el.className='net';
   const nm=document.createElement('span');
   nm.textContent=(n.ssid||'(hidden)')+(n.enc?' (lock)':'')+(_wifiSaved.indexOf(n.ssid)>=0?' - saved':'');
   const bars=document.createElement('span');bars.className='bars';bars.textContent=n.rssi+' dBm';
   el.appendChild(nm);el.appendChild(bars);
   el.onclick=()=>{$('ssid').value=n.ssid;$('pass').focus();};
   $('nets').appendChild(el);});
  if(!list.length)$('nets').textContent='No networks found.';
 }).catch(()=>{if(--left<=0)stop('Scan failed - device unreachable.');});
 // The POST starts the radio scan; the GET above only reads it. Ordering them
 // keeps exactly one scan request in flight, and `timer` is set synchronously so
 // a second click is still refused while the first is being kicked off.
 timer=setInterval(poll,2500);
 wifiPost('scan').catch(()=>{}).then(poll);
}
$('scan').onclick=scan;
$('savewifi').onclick=()=>{
 const ssid=$('ssid').value,pass=$('pass').value;
 if(!ssid){$('msg').textContent='Enter a network name.';return}
 $('savewifi').disabled=true;
 $('msg').textContent='Saving…';
 // The device SAVES without switching when it is already online (adding a backup
 // must not drop the connection you are adding it from), and joins immediately
 // only when it is offline - first-time setup. Report whichever actually happened
 // rather than always promising a join.
 wifiPost('add',{ssid:ssid,pass:pass}).then(d=>{
   $('pass').value='';
   if(d&&d.saved){
     $('savewifi').disabled=false;
     $('msg').textContent='Saved. The device will use it when this network isn\u2019t available.';
     loadWifi();
   } else { wifiWatchJoin(ssid); }
 }).catch(e=>{$('savewifi').disabled=false;$('msg').textContent=e.message;});
};
// The escape hatch: stop joining and guarantee the setup network, so this page
// stays reachable however wrong the saved credentials are.
(function(){
 const b=$('wifiAp'); if(b)b.onclick=()=>{
  if(!confirm('Publish the setup network?\n\nThe device stops trying to join Wi-Fi until you resume, so this page stays reachable over its own network.'))return;
  wifiPost('publishap').then(d=>{toast('Setup network published');
   if($('wifiApMsg'))$('wifiApMsg').textContent='Setup network "'+(d.apSsid||'')+'" is coming up. Joining is paused until you resume.';
   setTimeout(loadWifi,1500);
  }).catch(e=>{if($('wifiApMsg'))$('wifiApMsg').textContent=e.message;});};
 const c=$('wifiResume'); if(c)c.onclick=()=>{
  wifiPost('resume').then(()=>{toast('Joining resumed');
   if($('wifiApMsg'))$('wifiApMsg').textContent='Joining resumed. The device is trying its saved networks again.';
   setTimeout(loadWifi,1500);
  }).catch(e=>{if($('wifiApMsg'))$('wifiApMsg').textContent=e.message;});};
})();

loadState();
setInterval(loadState,3000);

// ---- Health panel (Home tab, P5): live component status + probe buttons ----
const HCLR={ok:'#1d3a1d',degraded:'#3a2f15',absent:'#3a1d1d',unknown:'#26333f'};
const HFG ={ok:'#5c5',degraded:'#e0b870',absent:'#c55',unknown:'#8cf'};
// Skeleton (heading + list + probe buttons + result line) is built ONCE; loadHealth
// only rewrites #hphead/#healthlist. The old whole-panel innerHTML rebuild destroyed
// #hpmsg right after every test (owner: "it writes something for a millisecond but
// nothing really changes") - and the 8 s poll wiped it again anyway.
function healthSkeleton(box){
 if($('healthlist'))return;
 box.innerHTML='<h2>Health <span id=hphead class=hint style="font-weight:normal"></span></h2>'+
  '<div id=healthlist></div>'+
  '<div class=row style="margin-top:8px"><button id=hpMic type=button>Mic Test</button>'+
  '<button id=hpBeep type=button>Speaker Tone</button><button id=hpLb type=button>Loopback Test</button>'+
  '<button id=hpSd type=button>Probe SD</button></div><p id=hpmsg class=hint></p>';
 const m=$('hpmsg'), P=(u,l)=>{m.textContent=l+' running…';
   fetch(u,{method:'POST'}).then(r=>r.json()).then(x=>{m.textContent=l+': '+JSON.stringify(x);loadHealth();}).catch(()=>m.textContent=l+' failed - try again');};
 $('hpMic').onclick=()=>P('/api/audio/mic','Mic test');
 $('hpBeep').onclick=()=>P('/api/audio/beep','Speaker tone');
 $('hpLb').onclick=()=>P('/api/audio/loopback','Loopback test');
 $('hpSd').onclick=()=>{m.textContent='Probing the SD card…';
   fetch('/api/sdprobe').then(r=>r.json()).then(x=>{m.textContent='SD: '+JSON.stringify(x);loadHealth();}).catch(()=>m.textContent='SD probe failed - try again');};
}
function loadHealth(){
 if(!canPoll())return;
 const box=$('healthpanel'); if(!box)return;
 fetch('/api/health').then(jok).then(d=>{
  healthSkeleton(box);
  $('hphead').innerHTML='&mdash; '+(d.ok||0)+' ok &middot; '+(d.degraded||0)+
    ' degraded &middot; '+(d.absent||0)+' absent';
  let h='';
  (d.components||[]).forEach(c=>{
   h+='<div class=row style="margin:4px 0"><span style="min-width:130px;color:'+(HFG[c.state]||'#ccc')+
      '">'+c.label+'</span><span class=badge style="background:'+(HCLR[c.state]||'#333')+';color:'+
      (HFG[c.state]||'#ccc')+'">'+c.state+'</span><span class=hint style="flex:1">'+
      (c.detail||'').replace(/</g,'&lt;')+'</span></div>';
  });
  $('healthlist').innerHTML=h;
 }).catch(()=>{if(!$('healthlist'))box.innerHTML='<p class=hint>Health unavailable</p>';});
}
loadHealth();
setInterval(loadHealth,8000);
// ---- Memory dashboard (Part B Ph3/Ph4) ----
function loadMemStats(){
  if(!canPoll())return;
  fetch('/api/mem/stats').then(r=>r.json()).then(d=>{
    $('memstat').textContent=d.vectors+' memories · '+d.scratchItems+' scratch · '+
      (d.store||'')+' (cap '+(d.maxVectors||0)+') · embed '+(d.embedAvailable?'ready':'no key');
    // Storage-tier banner: bulk should live on the SD card. No card -> degraded
    // (vectors capped on internal flash); flashFull -> persist paused.
    const tb=$('tierbanner');
    if(tb){ if(!d.sdPresent){tb.style.display='block';tb.className='warnbox';
        tb.textContent='⚠ No SD card - memory is limited to '+(d.maxVectors||0)+
          ' entries on internal storage, with no durable media or history. Insert a FAT32 SD card for full storage.'
          +(d.flashFull?' Internal storage is full - new memories are not being saved.':'');}
      else{tb.style.display='none';} }
    $('emblock').style.display=d.embedLocked?'inline-block':'none';
    _embState=d;   // remember current config + lock + count for the save flow
    const set=(id,v)=>{const e=$(id);if(e&&document.activeElement!==e)e.value=v;};
    if(document.activeElement!==$('emb_provider'))set('emb_provider',d.embed.provider);
    populateEmbModels($('emb_provider').value,d.embed.model);
    set('emb_dims',d.embed.dims);
    // Live destructive-change warning: while locked, changing any embed field means the
    // Save will invalidate the stored vectors - surface that inline, not only at Save.
    ['emb_provider','emb_model','emb_dims'].forEach(id=>{const e=$(id); if(e)e.oninput=e.onchange=(ev)=>{
      if(ev&&ev.target&&ev.target.id==='emb_provider')populateEmbModels($('emb_provider').value,'');
      embWarn();};});
    embWarn();
  }).catch(()=>{});
}
// Supported embedding providers/models (owner: dropdown, not free text - only show
// what the device can actually call). Curated; the live verify on save is the real gate.
const EMB_MODELS={
  openai:[{id:'text-embedding-3-small',note:'1536-d · fast, cheap'},{id:'text-embedding-3-large',note:'3072-d · most accurate'}],
  mistral:[{id:'mistral-embed',note:'1024-d'}]
};
let _embState=null;
function populateEmbModels(prov,sel){
  const s=$('emb_model'); if(!s)return; const cur=sel||s.value;
  const list=(EMB_MODELS[prov]||[]).slice();
  if(cur&&!list.some(m=>m.id===cur))list.unshift({id:cur,note:'current'});   // keep an existing custom id visible
  s.innerHTML=list.map(m=>'<option value="'+m.id+'"'+(m.id===cur?' selected':'')+'>'+m.id+(m.note?' - '+m.note:'')+'</option>').join('');
}
function embWarn(){const w=$('embwarn'),d=_embState; if(!w||!d)return;
  const changed=d.embedLocked&&($('emb_provider').value!=d.embed.provider||$('emb_model').value!=d.embed.model||(+$('emb_dims').value)!=d.embed.dims);
  w.style.display=changed?'block':'none';
  w.innerHTML=changed?('<b>⚠ This changes the embedding model.</b> Memories made with different models can’t be compared, so your <b>'+d.vectors+' stored '+(d.vectors===1?'memory':'memories')+'</b> can’t stay as-is. On Save you can <b>erase and start fresh</b>, or cancel. The new model is checked with a live call first.'):'';}
function renderMemList(d){
  const host=$('memlist'); host.innerHTML='';
  const es=d.entries||[];
  if(!es.length){host.innerHTML='<p class=hint>'+(d.error?('Error: '+d.error):'No memories yet')+'</p>';return;}
  es.forEach(e=>{
    const row=document.createElement('div'); row.className='row'; row.style.alignItems='flex-start';
    const t=document.createElement('div'); t.style.flex='1';
    const imp=Math.round((e.importance||0)*100);
    // Lifecycle line: hours-resolution wall clock; "now" derives from the same
    // /api/state clock the badge uses (epoch/3600 == the store's nowHours once
    // synced). Unsynced clock -> no honest ago/left math, show placeholders.
    const nowH=(window.CLOCK&&window.CLOCK.synced)?Math.floor(window.CLOCK.epoch/3600):0;
    const ago=h=>{if(!nowH||!h)return null;const d=nowH-h;
      return d<1?'just now':d<48?(d+'h ago'):(Math.round(d/24)+'d ago');};
    const left=()=>{if(e.ttlHours===undefined)return null;
      if(e.ttlHours<0||e.permanent)return 'never expires';
      if(!nowH||!e.tsHours)return null;
      const l=e.tsHours+e.ttlHours-nowH;
      return l<=0?'expired':(l<48?('expires in '+l+'h'):('expires in '+Math.round(l/24)+'d'));};
    const bits=['importance '+imp+'%'];
    if(e.permanent)bits.push('permanent');
    if(e.distance!==undefined)bits.push('dist '+e.distance.toFixed(2));
    const created=ago(e.tsHours); if(created)bits.push('created '+created);
    const lx=left(); if(lx)bits.push(lx);
    if(e.lastRecallHours!==undefined)
      bits.push(e.lastRecallHours?('last used '+(ago(e.lastRecallHours)||'-')):'never used');
    t.innerHTML='<div>'+(e.content||'').replace(/</g,'&lt;')+'</div>'+
      '<div class=hint>'+bits.join(' · ')+'</div>';
    const del=document.createElement('button'); del.textContent='x'; del.title='delete';
    del.onclick=()=>memVecOp('delete',e.id);
    const pin=document.createElement('button');
    pin.textContent=e.permanent?'unpin':'pin';
    pin.title=e.permanent?'let it decay/expire again':'pin (never forgotten)';
    pin.onclick=()=>memVecOp(e.permanent?'temporary':'permanent',e.id);
    row.appendChild(t); row.appendChild(pin); row.appendChild(del); host.appendChild(row);
  });
}
var _memOffset=0, _memPageN=10;
function loadMemList(){
  const q=$('memq').value.trim();
  $('memvmsg').textContent=q?'Searching…':'';
  if(q)_memOffset=0;   // search shows the top matches, not a browse page
  const url='/api/mem/vector?limit='+_memPageN+(q?('&query='+encodeURIComponent(q)):('&offset='+_memOffset));
  fetch(url)
    .then(r=>r.json()).then(d=>{$('memvmsg').textContent=d.error?('Embedding error: '+d.error):'';window._memTotal=d.total||0;
      // Row deletes can shrink the total below our browse offset, stranding the view
      // on an empty page - clamp to the last real page and refetch once (the offset
      // guard stops it looping when we're already clamped).
      if(d.mode==='browse'&&d.total>0&&d.offset>=d.total){var _c=Math.max(0,(Math.ceil(d.total/_memPageN)-1)*_memPageN);if(_c!==d.offset){_memOffset=_c;loadMemList();return;}}
      renderMemList(d);
      // Pagination controls apply to browse only (search returns the top matches).
      const pg=$('mempage'),pv=$('memprev'),nx=$('memnext');
      if(pg&&pv&&nx){
        if(d.mode==='browse'){
          const from=(d.total?d.offset+1:0),to=Math.min(d.offset+d.limit,d.total);
          pg.textContent=from+'–'+to+' of '+d.total;
          pv.style.display=nx.style.display='';
          pv.disabled=d.offset<=0; nx.disabled=to>=d.total;
        } else { pg.textContent='top '+(d.entries||[]).length+' matches'; pv.style.display=nx.style.display='none'; }
      }
    })
    .catch(()=>{$('memvmsg').textContent='Search failed - try again.';});
}
function memPage(dir){_memOffset=Math.max(0,_memOffset+dir*_memPageN);loadMemList();}
function memVecOp(op,id,confirm){
  fetch('/api/mem/vector?op='+op+(id?('&id='+encodeURIComponent(id)):'')+(confirm?('&confirm='+encodeURIComponent(confirm)):''),{method:'POST'})
    .then(()=>{loadMemList();loadMemStats();});
}
function loadScratch(){
  if(!canPoll())return;
  fetch('/api/mem/scratchpad').then(r=>r.json()).then(d=>{
    let s='';
    if(d.active)s+='Now: '+d.active+'\n';
    ['short','mid','long'].forEach(t=>{if((d[t]||[]).length){s+=t+':\n';d[t].forEach(i=>s+='  - '+i+'\n');}});
    $('scratchview').textContent=s||'(empty)';
  }).catch(()=>{});
}
function loadMemCfg(){
  fetch('/api/mem/config').then(r=>r.json()).then(d=>{
    const set=(id,v)=>{const e=$(id);if(e&&document.activeElement!==e)e.value=v;};
    set('cfg_rc',d.retrieval_count); set('cfg_rt',d.relevance_threshold);
    set('cfg_mv',d.max_vectors);
  }).catch(()=>{});
}
$('memsearch').onclick=loadMemList;
$('memq').addEventListener('keydown',e=>{if(e.key==='Enter')loadMemList();});
$('memdedupe').onclick=()=>memVecOp('dedupe');
$('memflushnp').onclick=()=>{if(confirm('Delete all temporary memories?\n\nMemories marked permanent are kept.'))memVecOp('flushnp');};
if($('memprev'))$('memprev').onclick=()=>memPage(-1);
if($('memnext'))$('memnext').onclick=()=>memPage(1);
if($('memflushall'))$('memflushall').onclick=()=>{
  const n=(window._memTotal||0);
  if(prompt('Delete ALL '+n+' memories, permanent ones included? This cannot be undone.\n\nType DELETE to confirm.')==='DELETE')memVecOp('flush',null,'DELETE');};
$('cfgsave').onclick=()=>{
  fetch('/api/mem/config',{method:'PUT',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'retrieval_count='+(+$('cfg_rc').value)+'&relevance_threshold='+(+$('cfg_rt').value)+
      '&max_vectors='+(+$('cfg_mv').value)})
    .then(jok).then(()=>{toast('Saved');loadMemCfg();}).catch(failToast);
};
// Per-provider budget: composite "prov:tokenLimit:callLimit:resetDay:centsLimit" -> /api/orch.
// The $ field is dollars in the UI, CENTS on the wire (integer, no float drift).
if($('budsave'))$('budsave').onclick=()=>{
  const prov=$('bud_prov').value, tok=+$('bud_tok').value||0, call=+$('bud_call').value||0;
  const cents=Math.round((+$('bud_usd').value||0)*100);
  let rd=+$('bud_reset').value||1; rd=Math.max(1,Math.min(28,rd));
  orchApply({budget:prov+':'+tok+':'+call+':'+rd+':'+cents}).then(ok=>{
    if(ok!==false){$('budmsg').textContent='Budget saved for '+(BUDLBL[prov]||prov)+'.';loadOrch();}
  });
};
$('embsave').onclick=()=>{
  const prov=$('emb_provider').value, model=$('emb_model').value, dims=+$('emb_dims').value;
  const body='provider='+encodeURIComponent(prov)+'&model='+encodeURIComponent(model)+'&dims='+dims;
  const msg=$('embmsg'), btn=$('embsave');
  const locked=_embState&&_embState.embedLocked;
  const changed=locked&&(prov!=_embState.embed.provider||model!=_embState.embed.model||dims!=_embState.embed.dims);
  const save=(reset)=>fetch('/api/mem/embedcfg',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body+(reset?'&reset=1':'')})
    .then(jok).then(()=>{toast(reset?'Memory reset and saved':'Saved');msg.textContent=reset?'Memories erased - new model saved.':'Saved.';loadMemStats();loadMemList();})
    .catch(e=>{failToast(e);msg.textContent='Save failed - try again.';});
  btn.disabled=true; msg.textContent='Verifying the model…';
  // Step 1: verify the CANDIDATE model actually works before we save/wipe anything.
  fetch('/api/mem/embedverify',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body})
    .then(r=>r.json().then(j=>({ok:r.ok,j})))
    .then(({ok,j})=>{
      btn.disabled=false;
      if(!ok||!j.ok){msg.textContent='✗ '+(j.error||'verification failed')+' - not saved.';toast('Verify failed');return;}
      msg.textContent='✓ Verified ('+j.dims+'-d). ';
      // Step 2: if this invalidates existing vectors, make the destruction explicit.
      if(changed){
        if(!confirm('Verified ✓\n\nSwitch the embedding model?\n\nYour '+_embState.vectors+' stored memories become incomparable and must be erased and re-learned from scratch.')){
          msg.textContent='Cancelled - current model kept.';return;}
        save(true);
      } else { save(false); }
    })
    .catch(e=>{btn.disabled=false;failToast(e);msg.textContent='Couldn\'t verify - try again.';});
};
function loadMemDash(){loadMemStats();loadMemList();loadScratch();loadMemCfg();}

// ---- Files (E1 artifact store): browse /mem/files, download, delete, upload ----
function _fbytes(n){n=n||0;return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB';}
function _fesc(s){return (s||'').replace(/[<>&"]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]));}
// Which files the explorer offers a preview for. Kept in step with the server's
// inlineViewable() allowlist, which is the ACTUAL gate - a file uploaded by
// anyone the device talks to must never render as HTML or SVG on this origin,
// where the page's access token lives. This function only decides whether to
// show the link.
function _fviewable(name){
  const n=(name||'').toLowerCase();
  if(/\.(png|jpe?g|gif|webp)$/.test(n))return 'img';
  if(/\.(txt|md|log|csv)$/.test(n))return 'txt';
  return '';
}
// Show a file in place. Images load through the img tag; text is fetched and
// inserted with textContent, never innerHTML - the whole point is that file
// contents are data, not markup.
function _fpreview(project,name,kind){
  const box=$('filePrev'); if(!box)return;
  const tok=nimbusTok();
  const url='/api/files/dl?inline=1&project='+encodeURIComponent(project)+
            '&name='+encodeURIComponent(name)+'&t='+encodeURIComponent(tok);
  box.style.display='block';
  box.innerHTML='';
  const bar=document.createElement('div');
  bar.style.cssText='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;gap:10px';
  const title=document.createElement('span');
  title.style.cssText='font-size:12px;color:var(--ink2);word-break:break-all';
  title.textContent=project+'/'+name;
  const close=document.createElement('a');
  close.href='#'; close.textContent='close'; close.style.cssText='font-size:12px;white-space:nowrap';
  close.onclick=e=>{e.preventDefault();box.style.display='none';box.innerHTML='';};
  bar.appendChild(title); bar.appendChild(close); box.appendChild(bar);

  if(kind==='img'){
    const img=document.createElement('img');
    img.style.cssText='max-width:100%;max-height:60vh;border-radius:8px;display:block';
    img.alt=name;
    img.onerror=()=>{box.appendChild(document.createTextNode('Couldn\'t load that image.'));};
    img.src=url;
    box.appendChild(img);
    return;
  }
  const pre=document.createElement('pre');
  pre.style.cssText='max-height:60vh;overflow:auto;white-space:pre-wrap;word-break:break-word;'+
                    'font-size:12px;margin:0;background:var(--raise2);padding:10px;border-radius:8px';
  pre.textContent='Loading...';
  box.appendChild(pre);
  fetch(url).then(r=>r.ok?r.text():Promise.reject(r.status)).then(t=>{
    // A preview, not a file manager: 64 KB is far past what anyone reads in a
    // panel, and the rest is one click away via "get".
    const CAP=65536;
    pre.textContent=t.length>CAP
      ? t.slice(0,CAP)+'\n\n--- truncated, use "get" for the whole file ---'
      : (t.length?t:'(empty file)');
  }).catch(()=>{pre.textContent='Couldn\'t load that file.';});
}

function loadFiles(){
  const box=$('filesList'); if(!box)return;
  const proj=($('filesProj')&&$('filesProj').value.trim())||'';
  fetch('/api/files/list'+(proj?('?project='+encodeURIComponent(proj)):'')).then(r=>r.ok?r.json():Promise.reject(r.status)).then(d=>{
    if($('filesStat'))$('filesStat').textContent=d.present
      ?((d.count||0)+' files · '+_fbytes(d.bytes)+' used'+(d.freeBytes!==undefined?(' · '+_fbytes(d.freeBytes)+' free'):''))
      :'No SD card - insert a card to store files';
    const files=d.files||[];
    if(!files.length){box.innerHTML='<span class=hint>No files'+(proj?(' in '+_fesc(proj)):'')+'</span>';return;}
    const tok=nimbusTok();
    box.innerHTML='<table><tbody>'+files.map(f=>{
      const dl='/api/files/dl?project='+encodeURIComponent(f.project)+'&name='+encodeURIComponent(f.name)+'&t='+encodeURIComponent(tok);
      // "view" only for what the device is willing to render inline (the server
      // decides; this just avoids offering a link that would download anyway).
      const v=_fviewable(f.name);
      const view=v?('<a href="#" data-vp="'+_fesc(f.project)+'" data-vn="'+_fesc(f.name)+'" data-vk="'+v+'">view</a> &middot; '):'';
      return '<tr><td style="word-break:break-all">'+_fesc(f.project)+'/'+_fesc(f.name)+'</td><td>'+_fesc(f.kind||'')+
        '</td><td style="white-space:nowrap">'+_fbytes(f.bytes)+'</td><td style="white-space:nowrap">'+view+
        '<a href="'+dl+'" target=_blank rel=noopener>get</a> &middot; <a href="#" data-rmp="'+_fesc(f.project)+'" data-rmn="'+_fesc(f.name)+'">delete</a></td></tr>';
    }).join('')+'</tbody></table>';
    box.querySelectorAll('a[data-vp]').forEach(a=>a.onclick=e=>{
      e.preventDefault(); _fpreview(a.dataset.vp,a.dataset.vn,a.dataset.vk);});
    box.querySelectorAll('a[data-rmp]').forEach(a=>a.onclick=e=>{e.preventDefault();
      if(!confirm('Delete '+a.dataset.rmp+'/'+a.dataset.rmn+'?'))return;
      const fd=new FormData();fd.append('project',a.dataset.rmp);fd.append('name',a.dataset.rmn);
      fetch('/api/files/rm',{method:'POST',body:fd}).then(r=>r.ok?loadFiles():Promise.reject(r.status)).catch(()=>toast('Couldn\'t delete - try again'));});
  }).catch(()=>{box.innerHTML='<span class=hint>Files unavailable</span>';});
}
$('filesRefresh')&&($('filesRefresh').onclick=loadFiles);
$('filesProj')&&$('filesProj').addEventListener('keydown',e=>{if(e.key==='Enter')loadFiles();});
// Delete a whole project/folder - the project named in the filter box.
$('filesRmProj')&&($('filesRmProj').onclick=()=>{
  const p=($('filesProj').value||'').trim();
  if(!p){toast('Type a project name in the filter first');return;}
  if(!confirm('Delete the entire "'+p+'" folder and all its files? This cannot be undone.'))return;
  const fd=new FormData();fd.append('project',p);fd.append('confirm','DELETE');
  fetch('/api/files/rmproject',{method:'POST',body:fd}).then(r=>r.ok?r.json():Promise.reject(r.status))
    .then(o=>{toast('Deleted '+((o&&o.removed)||0)+' files');loadFiles();})
    .catch(s=>toast(s===404?'No such folder':'Couldn\'t delete - try again'));});
$('upBtn')&&($('upBtn').onclick=()=>{
  const inp=$('upFile'),f=inp&&inp.files[0]; if(!f){toast('Choose a file first');return;}
  const proj=($('upProj').value.trim())||'uploads';
  const fd=new FormData();fd.append('file',f,f.name);
  $('upMsg').textContent='Uploading '+f.name+'…';
  fetch('/api/files/upload?project='+encodeURIComponent(proj)+'&name='+encodeURIComponent(f.name),{method:'POST',body:fd})
    .then(r=>r.ok?r.json():Promise.reject(r.status)).then(()=>{$('upMsg').textContent='Uploaded ✓';inp.value='';loadFiles();})
    .catch(s=>{$('upMsg').textContent='Upload failed ('+s+') - try again.';});
});
// ---- Skills (P2 dynamic capsules): list + load/save/delete on the SD card ----
function loadSkills(){
  const box=$('skList'); if(!box)return;
  fetch('/api/skills/list').then(jok).then(d=>{
    $('skStat').textContent=d.sd?'SD ready - your skills load into new sessions automatically':'No SD card - built-in skills only (read-only)';
    const sk=d.skills||[];
    if(!sk.length){box.innerHTML='<span class=hint>No skills yet</span>';return;}
    box.innerHTML=sk.map(s=>'<a href="#" data-skid="'+_fesc(s.id)+'" style="margin-right:10px">'+_fesc(s.id)+'</a><span class=hint>'+_fesc(s.title||'')+(s.version?(' · v'+_fesc(s.version)):'')+' · '+_fesc(s.origin||s.source)+(s.pending?' · <b style="color:#e0a349">pending approval</b>':'')+'</span>'+(s.pending?' <a href="#" data-skapprove="'+_fesc(s.id)+'">Approve</a>':'')+'<br>').join('');
    box.querySelectorAll('a[data-skid]').forEach(a=>a.onclick=e=>{e.preventDefault();$('skId').value=a.dataset.skid;loadSkillMd();});
    box.querySelectorAll('a[data-skapprove]').forEach(a=>a.onclick=e=>{e.preventDefault();
      const fd=new FormData();fd.append('id',a.dataset.skapprove);
      fetch('/api/skills/approve',{method:'POST',body:fd}).then(jok)
        .then(()=>{toast('Skill approved');loadSkills();}).catch(()=>toast('Approve failed'));});
  }).catch(()=>{box.innerHTML='<span class=hint>Skills unavailable</span>';});
}
function loadSkillMd(){
  const id=$('skId').value.trim(); if(!id){toast('Enter a skill id first');return;}
  fetch('/api/skills/get?id='+encodeURIComponent(id)).then(jok).then(d=>{
    $('skMd').value=d.md||'';
    $('skMsg').textContent=d.source==='builtin'?'Built-in skill (read-only) - Save stores an editable copy under the same id':'Loaded from the SD card';
  }).catch(()=>{$('skMsg').textContent='Not found - Save creates it';});
}
$('skLoad')&&($('skLoad').onclick=loadSkillMd);
$('skSave')&&($('skSave').onclick=()=>{
  const id=$('skId').value.trim(),md=$('skMd').value;
  if(!id){toast('Enter a skill id first');return;}
  const fd=new FormData();fd.append('id',id);fd.append('md',md);
  fetch('/api/skills/save',{method:'POST',body:fd})
    .then(r=>r.json().then(j=>{if(!r.ok)throw (j.error||r.status);return j;}))
    .then(()=>{$('skMsg').textContent='Saved ✓';loadSkills();})
    .catch(e=>{$('skMsg').textContent='✗ '+e;});
});
$('skDel')&&($('skDel').onclick=()=>{
  const id=$('skId').value.trim(); if(!id){toast('Enter a skill id first');return;}
  if(!confirm('Delete the skill "'+id+'" from the SD card?'))return;
  const fd=new FormData();fd.append('id',id);
  fetch('/api/skills/delete',{method:'POST',body:fd})
    .then(r=>r.json().then(j=>{if(!r.ok)throw (j.error||r.status);return j;}))
    .then(()=>{$('skMsg').textContent='Deleted';$('skMd').value='';loadSkills();})
    .catch(e=>{$('skMsg').textContent='✗ '+e;});
});
const GRPLBL={registry:'Available now',device:'Device actions',
  skill:'Knowledge',connector:'Connectors'};
function loadTools(){
  fetch('/api/tools').then(r=>r.json()).then(d=>{
    $('toolstat').textContent=(d.count||0)+' tools';
    const host=$('toollist'); host.innerHTML='';
    const byG={}; (d.tools||[]).forEach(t=>{(byG[t.group||'registry']=byG[t.group||'registry']||[]).push(t);});
    const mkRow=t=>{
      const row=document.createElement('div'); row.className='row'; row.style.alignItems='flex-start';
      const tag=t.tag?('<span class="badge ext" style="margin-left:6px">'+t.tag+'</span>'):'';
      const loop=t.rides_loop?'<span class=badge style="margin-left:6px">loop</span>':'';
      row.innerHTML='<code style="min-width:150px;display:inline-block">'+
        (t.name||'').replace(/</g,'&lt;')+'</code><span class=hint style="flex:1">'+
        (t.description||'').replace(/</g,'&lt;')+loop+tag+'</span>';
      return row;};
    ['registry','device','connector'].forEach(g=>{   // skills get their OWN panel below
      if(!byG[g])return;
      const h=document.createElement('h2');h.style.cssText='font-size:13px;margin:12px 0 4px';
      h.textContent=GRPLBL[g]||g;host.appendChild(h);
      byG[g].forEach(t=>host.appendChild(mkRow(t)));
    });
    // Skills panel (owner R7: #skillspanel was dead - skills hid inside the flat
    // tool list). Card per capsule; the model reads them via skill.get.
    const sp=$('skillspanel');
    if(sp){sp.innerHTML='';
      if(byG.skill&&byG.skill.length){
        const h=document.createElement('h2');h.style.cssText='font-size:13px;margin:12px 0 4px';
        h.textContent='Skills (knowledge the assistant can read)';sp.appendChild(h);
        const wrap=document.createElement('div');wrap.style.cssText='display:flex;flex-wrap:wrap;gap:8px';
        byG.skill.forEach(t=>{const card=document.createElement('div');
          card.style.cssText='background:#181c20;border:1px solid #2a3138;border-radius:8px;padding:8px 10px;max-width:260px';
          card.innerHTML='<div style="color:#7fd1c8;font-size:13px">'+(t.name||'').replace(/^skill:/,'').replace(/</g,'&lt;')+
            '</div><div class=hint style="font-size:12px">'+(t.description||'').replace(/</g,'&lt;')+'</div>';
          wrap.appendChild(card);});
        sp.appendChild(wrap);
      } else sp.innerHTML='<p class=hint>No skills installed</p>';
    }
    if(!(d.tools||[]).length) host.innerHTML='<p class=hint>No tools registered</p>';
  }).catch(()=>{$('toolstat').textContent='Unavailable';});
}

// --- Audio diagnostics (Device tab): mic VU meter + speaker beep + acoustic loopback.
// All POST so the auth token rides automatically. The meter polls a short mic capture;
// beep/loopback are one-shot. Every call is device-actuating, so a 401 just prompts auth.
(function(){
  let micTimer=null; const bar=$('vubar'), msg=$('audiomsg');
  const poll=()=>fetch('/api/audio/mic',{method:'POST'}).then(r=>r.json()).then(d=>{
    if(!d.ok){bar.style.width='0';msg.textContent='No signal from the microphone.';console.log('mic: no data - check I2S wiring (BCLK 15 / WS 18 / SD 16)');return;}
    bar.style.width=Math.min(100,Math.round(d.rms/40))+'%';      // ~same scale as the on-ring VU
    msg.textContent='Mic level: rms '+d.rms+' · peak '+d.peak;
  }).catch(()=>{});
  $('micBtn').onclick=()=>{
    if(micTimer){clearInterval(micTimer);micTimer=null;$('micBtn').textContent='Mic Meter';bar.style.width='0';msg.textContent='Idle.';return;}
    $('micBtn').textContent='Stop Mic Meter';micTimer=setInterval(poll,250);poll();
  };
  $('beepBtn').onclick=()=>{msg.textContent='Playing a tone…';
    fetch('/api/audio/beep',{method:'POST'}).then(r=>r.json()).then(d=>msg.textContent=d.ok?'✓ Tone played':'✗ No tone - check the speaker.').catch(()=>msg.textContent='Speaker test failed - try again.');};
  $('lbBtn').onclick=()=>{msg.textContent='Playing a tone and listening…';
    fetch('/api/audio/loopback',{method:'POST'}).then(r=>r.json()).then(d=>{
      msg.textContent=(d.tonePresent?'✓ Tone heard':'✗ Tone not heard')+
        ' - toneMag='+d.toneMag+' ctrl='+d.ctrlMag+' rms='+d.rms+' peak='+d.peak+
        (d.tonePresent?'':' (the microphone works; check the speaker)');
    }).catch(()=>msg.textContent='Loopback test failed - try again.');};
})();
// ---- Telegram access (P8): chips + first-message approval + public mode ----
function tgPost(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:new URLSearchParams(b).toString()}).then(jok).then(loadTelegram).catch(failToast);}
// v3.7.0 people: role + limits per chat, fetched alongside the allowlist.
// These three were REFERENCED by the panel below and never defined - the
// ReferenceError threw inside the forEach, rejected the enclosing promise, and
// the whole Telegram panel rendered "Telegram unavailable". Caught by review,
// not by the API tests, which never opened the page.
let tenantRoles={},tenantQuota={};
function loadTenants(){
  if(!canPoll())return Promise.resolve();
  return fetch('/api/tenant').then(jok).then(d=>{
    tenantRoles={};tenantQuota={};
    (d.tenants||[]).forEach(t=>{
      tenantRoles[t.id]=t.role;
      tenantQuota[t.id]={vectors:t.vectors,bytes:t.bytes,ttl:t.ttl,pins:t.pins};
    });
  }).catch(()=>{});   // roles are decoration here; the panel still renders without them
}

function loadTelegram(){
  if(!canPoll())return;
  if(!$('tgChips'))return;
  // Token-gated GET (previews are owner-only) - attach the token explicitly since
  // the global fetch wrapper only adds it to mutating requests.
  fetch('/api/telegram').then(jok).then(d=>{
    $('tgPublic').checked=!!d.public;
    // pending approvals (highlighted)
    const pb=$('tgPending');pb.innerHTML='';
    (d.pending||[]).forEach(p=>{
      const row=document.createElement('div');row.className='sec';row.style.cssText='border-color:#e0b870;padding:8px;margin:6px 0';
      row.innerHTML='<b>'+(p.name||'?')+'</b> <span class=hint>('+p.chatId+')</span> wants access<br>'+
        '<span class=hint>"'+(p.preview||'').replace(/</g,'&lt;')+'"</span><br>';
      const ok=document.createElement('button');ok.textContent='Approve';ok.style.marginTop='6px';
      ok.onclick=()=>tgPost('/api/telegram/approve',{id:p.chatId,name:p.name||''});
      const no=document.createElement('button');no.textContent='Ignore';no.style.cssText='margin:6px 0 0 6px;background:#333;color:#cde';
      no.onclick=()=>tgPost('/api/telegram/deny',{id:p.chatId});
      row.appendChild(ok);row.appendChild(no);pb.appendChild(row);
    });
    // allowed chips - the NAME is click-to-rename (owner ask: label who sent what;
    // the unified chat + the model both read this sidecar).
    const cb=$('tgChips');cb.innerHTML='';TGNAMES={};
    (d.allow||[]).forEach(a=>{
      if(a.name)TGNAMES[a.id]=a.name;
      const chip=document.createElement('span');chip.className='badge';chip.style.cssText='margin:3px 4px 3px 0;padding:4px 8px;font-size:12px';
      const nm=document.createElement('a');nm.href='#';nm.title='Click to rename';
      nm.style.cssText='color:inherit;text-decoration:underline dotted';
      nm.textContent=(a.name||'unnamed');
      nm.onclick=e=>{e.preventDefault();
        const v=prompt('Display name for chat '+a.id+' (shown in Chat and to the assistant):',a.name||'');
        if(v!==null)tgPost('/api/telegram/rename',{id:a.id,name:v.trim()}).then(()=>{if(typeof loadChatHistory==='function')loadChatHistory();});};
      chip.appendChild(nm);
      chip.appendChild(document.createTextNode(' ('+a.id+') '));
      const role=document.createElement('a');role.href='#';
      // v3.7.0 RBAC: click cycles admin -> user -> guest. The device refuses to
      // demote its last admin (409), and the reason is shown rather than the
      // click silently doing nothing.
      const cur=(tenantRoles[a.id]||(a.owner?'admin':'user'));
      const STYLE={admin:'background:rgba(240,180,40,.18);color:#e0a020',
                   user:'background:rgba(90,160,220,.18);color:#5aa0dc',
                   guest:'background:rgba(130,130,130,.15);color:#999'};
      const LABEL={admin:'👑 admin',user:'user',guest:'guest'};
      const TITLE={admin:'Admin - manages people, settings and updates. Click for user.',
                   user:'User - own memories and files, can share files. Click for guest.',
                   guest:'Guest - limited storage, memories expire sooner, no pinning. Click for admin.'};
      const NEXT={admin:'user',user:'guest',guest:'admin'};
      role.style.cssText='margin-left:2px;font-size:11px;text-decoration:none;padding:1px 6px;border-radius:3px;'+(STYLE[cur]||STYLE.guest);
      role.textContent=LABEL[cur]||cur;
      role.title=TITLE[cur]||'';
      role.onclick=async e=>{e.preventDefault();
        const res=await fetch('/api/tenant',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
          body:'id='+encodeURIComponent(a.id)+'&role='+NEXT[cur]});
        if(!res.ok){const j=await res.json().catch(()=>({}));toast(j.error||'could not change role');}
        loadTenants().then(loadTelegram);};
      chip.appendChild(role);
      const q=tenantQuota[a.id];
      if(q&&cur!=='admin'){
        const lim=document.createElement('span');
        lim.style.cssText='margin-left:6px;font-size:10px;color:#888';
        lim.textContent=q.vectors+' memories · '+Math.round(q.bytes/1024)+'KB · '+q.ttl+'h'+(q.pins?' · '+q.pins+' pins':'');
        lim.title='What this person may store. Change it in Capabilities → Connectors → Telegram.';
        chip.appendChild(lim);
      }
      const x=document.createElement('a');x.href='#';x.textContent='×';x.style.cssText='margin-left:6px;color:#c55;text-decoration:none';
      x.onclick=e=>{e.preventDefault();tgPost('/api/telegram/remove',{id:a.id});};
      chip.appendChild(x);cb.appendChild(chip);
    });
    if(!(d.allow||[]).length&&!d.public)cb.innerHTML='<p class=hint>No one allowed yet - anyone who messages the bot will appear above for approval.</p>';
  }).catch(()=>{if($('tgChips'))$('tgChips').innerHTML='<p class=hint>Telegram unavailable</p>';});
}
$('tgAddBtn').onclick=()=>{const id=$('tgAddId').value.trim();if(id){tgPost('/api/telegram/add',{id:id,name:$('tgAddName').value.trim()});$('tgAddId').value='';$('tgAddName').value='';}};
$('tgPublic').onchange=()=>{
  if($('tgPublic').checked && !confirm('Allow anyone to message this device?\n\nAnyone who finds your bot on Telegram can give it instructions and spend your API credits.')){$('tgPublic').checked=false;return;}
  tgPost('/api/telegram/public',{on:$('tgPublic').checked?1:0});
};
loadTenants().then(loadTelegram);
setInterval(()=>loadTenants().then(loadTelegram),7000);
// Unified persistent chat (owner 2026-07-16): EVERY channel's turns (web, Telegram,
// voice, serial) are auto-captured to the episodic store on-device - the pane renders
// that store, so history survives refresh and Telegram/voice conversations appear
// here labeled by channel + sender. Sending still POSTs /api/chat (one real turn).
function _chatBubble(who,text,meta){const log=$('chatLog');const d=document.createElement('div');
  d.style.cssText=who==='u'
    ?'align-self:flex-end;background:var(--teal-soft);border:1px solid rgba(90,214,196,.2);padding:9px 13px;border-radius:14px;border-bottom-right-radius:5px;max-width:84%'
    :'align-self:flex-start;background:var(--raise2);border:1px solid var(--line);padding:9px 13px;border-radius:14px;border-bottom-left-radius:5px;max-width:84%';
  if(meta){const lb=document.createElement('div');
    lb.style.cssText='font-size:9.5px;color:var(--ink3);margin-bottom:3px;letter-spacing:.06em;text-transform:uppercase;font-weight:650';
    lb.textContent=meta; d.appendChild(lb);}                       // textContent - never innerHTML a sender name
  const tx=document.createElement('div'); tx.textContent=text; d.appendChild(tx);
  log.appendChild(d); log.scrollTop=log.scrollHeight; return d;}
// Sender label per row: tags "from:<name>" captured at message time; for Telegram
// prefer the CURRENT display name from the allowlist sidecar (renames re-label
// history retroactively). TGNAMES fills in loadTelegram().
let TGNAMES={};
// Row tags are a COMMA LIST ("from:roy", "via:telegram,turn:m0000a3f2",
// "turn:m…,tool:memory.search,err"). Always read a value through this - the old
// indexOf('from:')===0 + slice(5) broke the moment a second tag was appended.
function _tagVal(tags,key){
  const parts=String(tags||'').split(',');
  for(let i=0;i<parts.length;i++){const p=parts[i].trim();
    if(p.indexOf(key)===0)return p.slice(key.length);}
  return '';
}
// A turn id is a row id ("m" + hex). Pre-v4.2.0 rows carry the OLD boot-relative
// tag ("turn:t2"), which is NOT addressable - no dossier, no stable identity - so
// it must fall back to the legacy adjacency path rather than render a "Turn
// anatomy" button that can only ever answer "removed to save space".
function _turnId(m){
  const t=_tagVal(m&&m.tags,'turn:');
  return /^m[0-9a-f]+$/.test(t)?t:'';
}
function _chatMeta(m){
  const sender=_tagVal(m.tags,'from:');
  if(m.channel==='telegram'){const n=TGNAMES[m.session]||sender||m.session;return 'telegram · '+n;}
  return m.channel;   // web / voice / serial - the channel IS the sender
}
// Glass-box disclosure (A4): tool calls + thinking captured between two chat
// messages group into one collapsed <details> block under the turn. Chronological
// grouping - no tag join needed; rows already arrive in insertion order.
// Compact token count for the turn chip: 1234 -> "1.2k".
function _tok(n){n=+n||0;return n>=1000?(n/1000).toFixed(1)+'k':String(n);}
// Per-turn summary chip under the reply (Glass Box P2), from the ev:turnend row.
// `tools` is the EXECUTED TOOL-CALL count, not provider rounds - labeled as such.
function _turnChip(bubble,row){
  let d={};try{d=JSON.parse(row.text)||{};}catch(e){return;}
  const bits=[];
  if(d.host)bits.push(d.host+(d.model?' · '+d.model:''));
  if(d.tools)bits.push(d.tools+' tool call'+(d.tools>1?'s':''));
  if(d.in||d.out)bits.push(_tok(d.in)+' in / '+_tok(d.out)+' out');
  if(d.ok===false)bits.push('failed'+(d.err?': '+d.err:''));
  if(!bits.length)return;
  const c=document.createElement('div');
  c.style.cssText='font-size:9.5px;color:var(--ink3);margin-top:4px;opacity:.8';
  c.textContent=bits.join(' · ');   // textContent - model/error text is untrusted
  bubble.appendChild(c);
}
function _traceGroup(log,rows){
  if(!rows.length)return;
  const nTools=rows.filter(x=>x.kind==='tool_output').length;
  const nThink=rows.filter(x=>x.kind==='llm_response').length;
  const nSub=rows.filter(x=>x.kind==='log').length;
  const det=document.createElement('details');
  det.style.cssText='align-self:flex-start;max-width:92%;font-size:11px;color:var(--ink3)';
  const sum=document.createElement('summary');
  sum.style.cssText='cursor:pointer;opacity:.75';
  const bits=[];
  if(nTools)bits.push('⚙ '+nTools+' tool call'+(nTools>1?'s':''));
  if(nThink)bits.push('thinking');
  if(nSub)bits.push('🤖 '+nSub+' sub-agent event'+(nSub>1?'s':''));
  sum.textContent=bits.join(' · ')||'trace';
  det.appendChild(sum);
  rows.forEach(x=>{const pre=document.createElement('pre');
    pre.style.cssText='white-space:pre-wrap;word-break:break-word;background:var(--raise2);border:1px solid var(--line);border-radius:8px;padding:7px 9px;margin:5px 0;font-size:10.5px';
    pre.textContent=(x.kind==='llm_response'?'💭 ':'')+x.text;   // textContent - tool output is untrusted
    det.appendChild(pre);
    // The row text is clipped; the full call + result is parked as a blob (P4).
    if(x.blob){const a=document.createElement('button');
      a.type='button';a.className='qh';a.style.cssText='font-size:10px;margin:0 0 6px';
      a.textContent='Show full result';
      a.onclick=()=>{a.disabled=true;a.textContent='Loading…';
        _tfetch('/api/mem/blob?path='+encodeURIComponent(x.blob))
          .then(t=>{pre.textContent=t;a.remove();})
          .catch(e=>{a.disabled=false;a.textContent=String(e&&e.message||e)||'Not available';});};
      det.appendChild(a);}
  });
  // Turn anatomy (P3): the exact system prompt, per-turn input, raw model output
  // and the tool-loop transcript for THIS turn - fetched on demand, never inline.
  const tid=rows.length?_turnId(rows[0]):'';
  if(tid){const b=document.createElement('button');
    b.type='button';b.className='qh';b.style.cssText='font-size:10px;margin:0 0 6px';
    b.textContent='Turn anatomy';
    b.onclick=()=>{b.disabled=true;b.textContent='Loading…';
      _tfetch('/api/trace?turn='+encodeURIComponent(tid))
        .then(t=>{const p=document.createElement('pre');
          p.style.cssText='white-space:pre-wrap;word-break:break-word;background:var(--raise2);border:1px solid var(--line);border-radius:8px;padding:7px 9px;margin:5px 0;font-size:10.5px;max-height:340px;overflow:auto';
          p.textContent=t;det.appendChild(p);b.remove();})
        .catch(e=>{b.disabled=false;b.textContent=String(e&&e.message||e)||'Not available';});};
    det.appendChild(b);}
  log.appendChild(det);
}
function loadChatHistory(){
  const log=$('chatLog'); if(!log||_chatBusy)return;
  // No kind filter: message rows render as bubbles, tool_output/llm_response rows
  // fold into per-turn disclosures. The 'system' session (device timeline) and
  // media/log rows stay out of the chat pane.
  fetch('/api/mem/episodic?limit=200').then(jok).then(d=>{
    const ms=(d.messages||[]).slice().reverse();   // store returns newest-first
    log.innerHTML='';
    // Group by TURN ID, not adjacency: every trace row carries "turn:<user row
    // id>" and the turn's reply row carries the same tag, so an interleaved
    // Telegram/voice turn can no longer dump its tool calls under a web bubble.
    // Rows with no turn: tag are pre-v4.2 history - they keep the old
    // chronological accumulator so old conversations still render.
    const byTurn={},endByTurn={};
    ms.forEach(m=>{
      if(m.session==='system')return;
      if(m.kind==='tool_output'||m.kind==='llm_response'||m.kind==='log'){
        const t=_turnId(m);
        if(!t)return;   // legacy/untagged -> adjacency path below
        // The turn-summary row is a chip under the reply, not a trace <pre> -
        // and must not be counted as a sub-agent event in the disclosure.
        if(_tagVal(m.tags,'ev:')==='turnend'){endByTurn[t]=m;return;}
        (byTurn[t]=byTurn[t]||[]).push(m);
      }
    });
    let trace=[];let any=false;
    ms.forEach(m=>{
      if(m.session==='system')return;
      if(m.kind==='tool_output'||m.kind==='llm_response'||m.kind==='log'){
        if(!_turnId(m))trace.push(m);   // legacy: adjacency path
        return;                                       // tagged rows: bucketed above
      }
      if(m.kind!=='message')return;   // audio/file/etc. keep their own surfaces
      if(trace.length){_traceGroup(log,trace);trace=[];}
      // A user row's OWN id is its turn id; the reply row carries turn:<that id>.
      // Trace renders just above the reply it produced.
      const tid=m.role==='user'?m.id:_turnId(m);
      if(m.role!=='user'&&tid&&byTurn[tid]){_traceGroup(log,byTurn[tid]);delete byTurn[tid];}
      const bub=_chatBubble(m.role==='user'?'u':'a',m.text,m.role==='user'?_chatMeta(m):undefined);
      if(m.role!=='user'&&tid&&endByTurn[tid]){_turnChip(bub,endByTurn[tid]);delete endByTurn[tid];}
      any=true;
    });
    if(trace.length)_traceGroup(log,trace);
    // Orphans: a turn still in flight, or whose reply row fell outside this page.
    Object.keys(byTurn).forEach(k=>_traceGroup(log,byTurn[k]));
    if(!any)log.innerHTML='<p class=hint>No conversation yet - say something below, message the bot on Telegram, or hold the knob to talk.</p>';
    log.scrollTop=log.scrollHeight;
  }).catch(()=>{});
}
let _chatBusy=false;
function sendChatTurn(){
  const t=$('chatInput'),v=(t.value||'').trim(); if(!v||_chatBusy)return;
  _chatBubble('u',v,'web'); t.value='';
  _chatBusy=true; const pend=_chatBubble('a','…'); pend.style.opacity=.6;
  $('chatMsg').textContent='Thinking… a reply can take 5–30 s';
  fetch('/api/chat',{method:'POST',body:new URLSearchParams({text:v})})
    .then(r=>r.ok?r.json():r.json().then(e=>Promise.reject(e.error||r.status)))
    .then(()=>{
      let tries=0; const poll=()=>{tries++;
        fetch('/api/chat').then(r=>r.json()).then(d=>{
          if(d.reply){pend.textContent=d.reply;pend.style.opacity=1;_chatBusy=false;$('chatMsg').textContent='';
            setTimeout(loadChatHistory,900);}   // re-sync from the canonical store
          else if(tries<40){setTimeout(poll,1500);}
          else{pend.textContent='(No reply - check the provider key and try again)';pend.style.opacity=1;_chatBusy=false;$('chatMsg').textContent='';}
        }).catch(()=>{pend.textContent='(Couldn\'t fetch the reply - try again)';_chatBusy=false;});};
      setTimeout(poll,1500);
    })
    .catch(err=>{pend.textContent='✗ '+err;pend.style.opacity=1;_chatBusy=false;$('chatMsg').textContent='';});
}
$('chatSend')&&($('chatSend').onclick=sendChatTurn);
loadChatHistory();
$('chatInput')&&$('chatInput').addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendChatTurn();}});
loadOrch();loadConnectors();
setInterval(loadOrch,5000);
loadMemDash();
loadTools();
loadSkills();
setInterval(()=>{loadMemStats();loadScratch();},6000);

// ===================== First-run onboarding wizard =====================
// Shown over the SPA when /api/state.needsOnboarding is true (a fresh or
// factory-reset device). Two hard-gated steps (Wi-Fi + a verified provider);
// the rest are skippable. Reuses /scan, /savewifi, /api/orch, /api/verify,
// /api/config, and finishes with POST /api/onboard/complete.
(function(){
  const STEPS=[
    {id:'welcome',t:'Welcome'},
    {id:'display',t:'Choose your display',skip:1},
    {id:'wifi',t:'Connect to Wi-Fi',req:1},
    {id:'provider',t:'Add an AI provider',req:1},
    {id:'mode',t:'Choose a mode',skip:1},
    {id:'telegram',t:'Telegram',skip:1},
    {id:'voice',t:'Voice',skip:1},
    {id:'name',t:'Name this device',skip:1},
    {id:'done',t:'All set'},
  ];
  let idx=0, wifiOk=false, provOk=false, pendingMode=null, curMode=0, scanTimer=null;
  let handoffProbe=null;
  // Display step: bootDisp is what the device booted with (a TFT board that was
  // never told it has a TFT boots "eink" and shows a white panel - this step is
  // how a blind-boot board gets fixed). dispSel is the owner's saved choice.
  let bootDisp='eink', dispSel=null;
  const dispChanged=()=>dispSel!==null&&dispSel!==bootDisp;
  const el=id=>document.getElementById(id);
  const msg=t=>{el('onbMsg').textContent=t||'';};
  function esc(s){return (s||'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
  function show(){el('onbov').style.display='';render();}
  function bodyHtml(s){
    if(s.id==='welcome')return '<p class=hint>Two quick required steps &mdash; Wi-Fi and one AI provider key &mdash; then a few optional extras you can skip. Takes about a minute.</p>';
    if(s.id==='display')return '<p class=hint>Which display is fitted on this device? A touch-screen board can\'t ask on its own screen until this is set.</p>'+
      '<div class=row style="gap:10px"><button type=button class="onbmb onbdb" data-d=eink style="padding:20px 10px">E-ink + knob</button>'+
      '<button type=button class="onbmb onbdb" data-d=tft style="padding:20px 10px">Touch screen</button></div>'+
      '<p class=hint id=onb_dispmsg>Saved to the device right away. A change applies after a restart.</p>';
    if(s.id==='wifi')return '<label>Network name</label>'+
      '<div class=row><input id=onb_ssid placeholder="Network name"><button type=button id=onb_scan>Scan</button></div>'+
      '<p class=hint style="margin:4px 0">Nimbus connects to 2.4 GHz networks.</p>'+
      '<div id=onb_nets class=hint style="margin:4px 0"></div>'+
      '<label>Password</label><input id=onb_pass type=password placeholder="Wi-Fi password">'+
      '<div class=row style="margin-top:8px"><button type=button id=onb_wifisave>Connect</button><span class=hint id=onb_wifimsg></span></div>'+
      '<div id=onb_handoff class=hint style="display:none;margin-top:12px;padding:12px;border:1px solid var(--line2);border-radius:8px"></div>';
    if(s.id==='provider')return '<label>Provider</label>'+
      '<select id=onb_prov><option value=openai>OpenAI</option><option value=anthropic>Anthropic</option><option value=mistral>Mistral</option></select>'+
      '<label>API key</label><input id=onb_key type=password placeholder="Paste your API key">'+
      '<div class=row style="margin-top:8px"><button type=button id=onb_provsave>Save &amp; Verify</button><span class=hint id=onb_provmsg></span></div>'+
      '<p class=hint>The key stays on the device and is never shown again. It is checked with the provider before you continue.</p>';
    if(s.id==='mode')return '<p class=hint>Notifier shows AI-coding-session status on the LED ring. Orchestrator turns the device into an autonomous assistant (Telegram, voice, memory). Change it anytime from the header.</p>'+
      '<div class=row><button type=button class=onbmb data-m=1>Orchestrator</button><button type=button class=onbmb data-m=0>Notifier</button></div>'+
      '<p class=hint id=onb_modemsg>Applied when you finish - a mode change restarts the device.</p>';
    if(s.id==='telegram')return '<label>Telegram bot token (optional)</label>'+
      '<div class=row><input id=onb_tg type=password placeholder="Token from @BotFather"><button type=button id=onb_tgsave>Save</button></div>'+
      '<span class=hint id=onb_tgmsg></span>'+
      '<p class=hint>Lets you chat with the device from Telegram. You can also set this up later in Capabilities &rarr; Connectors.</p>';
    if(s.id==='voice')return '<label>Speech-to-text provider</label>'+
      '<select id=onb_stt><option value=mistral>Mistral (Voxtral)</option><option value=openai>OpenAI</option></select>'+
      '<label>Text-to-speech provider</label>'+
      '<select id=onb_tts><option value=mistral>Mistral (Voxtral TTS)</option><option value=openai>OpenAI</option></select>'+
      '<div class=row style="margin-top:8px"><button type=button id=onb_voicesave>Save</button><span class=hint id=onb_voicemsg></span></div>';
    if(s.id==='name')return '<label>Device name</label>'+
      '<div class=row><input id=onb_name placeholder="Nimbus"><button type=button id=onb_namesave>Save</button></div>'+
      '<span class=hint id=onb_namemsg></span>'+
      '<p class=hint>Shown on the display, the network, and the setup Wi-Fi. Applies after the next restart.</p>';
    if(s.id==='done')return '<p class=hint>You’re ready &mdash; Wi-Fi is connected and your provider is verified. Click Finish to open the dashboard.</p>'+
      '<div class=hint id=onb_summary></div>';
    return '';
  }
  function render(){
    const s=STEPS[idx];
    el('onbTitle').textContent=s.t;
    el('onbDots').innerHTML=STEPS.map((x,i)=>'<span style="width:9px;height:9px;border-radius:50%;background:'+(i===idx?'var(--teal)':i<idx?'var(--ok)':'var(--line2)')+'"></span>').join('');
    el('onbBody').innerHTML=bodyHtml(s);
    el('onbBack').style.visibility=idx>0?'visible':'hidden';
    el('onbSkip').style.display=s.skip?'':'none';
    el('onbNext').textContent=s.id==='done'?'Finish':'Next';
    msg('');
    wire(s);
  }
  function rememberStep(){try{localStorage.setItem('nimbusOnboardStep',String(idx));}catch(e){}}
  function next(){ idx=Math.min(idx+1,STEPS.length-1);rememberStep();render(); }
  function wire(s){
    if(s.id==='display'){
      [...document.querySelectorAll('.onbdb')].forEach(b=>{
        b.classList.toggle('on',b.dataset.d===(dispSel!==null?dispSel:bootDisp));
        b.onclick=()=>{const v=b.dataset.d;
          orchApply({scrModel:v}).then(ok=>{
            if(!ok){el('onb_dispmsg').textContent='Couldn\'t save - try again.';return;}
            dispSel=v;render();
            el('onb_dispmsg').textContent=dispChanged()?'Saved ✓ - applies when the device restarts at the end of setup.':'Saved ✓';
          });};
      });
    } else if(s.id==='wifi'){
      el('onb_scan').onclick=doScan;
      el('onb_wifisave').onclick=doWifi;
      refreshWifiState();
    } else if(s.id==='provider'){
      el('onb_provsave').onclick=doProvider;
    } else if(s.id==='mode'){
      [...document.querySelectorAll('.onbmb')].forEach(b=>{
        b.classList.toggle('on',+b.dataset.m===(pendingMode!==null?pendingMode:curMode));
        b.onclick=()=>{pendingMode=+b.dataset.m;render();};
      });
    } else if(s.id==='telegram'){
      el('onb_tgsave').onclick=()=>{const v=el('onb_tg').value.trim();if(!v){el('onb_tgmsg').textContent='Enter a token first.';return;}
        orchApply({tgToken:v}).then(ok=>{el('onb_tgmsg').textContent=ok?'Saved ✓':'Rejected';});};
    } else if(s.id==='voice'){
      el('onb_voicesave').onclick=()=>orchApply({sttProv:el('onb_stt').value,ttsProv:el('onb_tts').value})
        .then(ok=>{el('onb_voicemsg').textContent=ok?'Saved ✓':'Rejected';});
    } else if(s.id==='name'){
      el('onb_namesave').onclick=()=>{const v=el('onb_name').value.trim();if(!v){el('onb_namemsg').textContent='Enter a name.';return;}
        apply({devName:v}).then(ok=>{el('onb_namemsg').textContent=ok?'Saved ✓ (applies after restart)':'Rejected';});};
    } else if(s.id==='done'){
      const mn=pendingMode!==null?pendingMode:curMode;
      el('onb_summary').innerHTML='&bull; Wi-Fi: '+(wifiOk?'connected':'not connected')+'<br>&bull; Provider: '+(provOk?'verified':'not verified')+'<br>&bull; Mode: '+(mn?'Orchestrator':'Notifier')+
        (dispChanged()?'<br>&bull; Display: '+(dispSel==='tft'?'Touch screen':'E-ink + knob')+' - the device restarts when you finish to apply it':'');
    }
  }
  function doScan(){
    if(scanTimer)return; const b=el('onb_scan'); b.disabled=true; let left=12;
    const nets=el('onb_nets'); nets.textContent='Scanning…';
    const stop=t=>{clearInterval(scanTimer);scanTimer=null;if(b)b.disabled=false;if(t)nets.textContent=t;};
    const poll=()=>fetch('/scan').then(r=>r.json()).then(d=>{
      if(d.scanning){if(--left<=0)return stop('Scan timed out - try again.');return;}
      if(d.error)return stop(d.error);   // same gap as the Settings scan - say what happened
      stop('');
      const list=(d.networks||[]).sort((a,b)=>b.rssi-a.rssi);
      if(!list.length){nets.textContent='No networks found.';return;}
      nets.innerHTML=''; list.forEach(n=>{const e=document.createElement('div');e.className='net';
        e.innerHTML='<span>'+esc(n.ssid||'(hidden)')+(n.enc?' 🔒':'')+'</span><span class=bars>'+n.rssi+' dBm</span>';
        e.onclick=()=>{el('onb_ssid').value=n.ssid;el('onb_pass').focus();};nets.appendChild(e);});
    }).catch(()=>{if(--left<=0)stop('Scan failed - device unreachable.');});
    poll();scanTimer=setInterval(poll,2500);
  }
  function doWifi(){
    const ssid=el('onb_ssid').value,pass=el('onb_pass').value;
    if(!ssid){el('onb_wifimsg').textContent='Enter a network name.';return;}
    const b=el('onb_wifisave');b.disabled=true;el('onb_wifimsg').textContent='Saving…';
    fetch('/savewifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
    .then(r=>r.ok?r.json():Promise.reject(r.status)).then(()=>{
      let left=12; el('onb_wifimsg').textContent='Joining "'+ssid+'"…';
      const w=setInterval(()=>fetch('/api/state').then(r=>r.json()).then(st=>{
        if(st.sta&&st.staIp){clearInterval(w);wifiOk=true;
          if(st.scrModel==='tft')beginWifiHandoff(st,b,ssid);
          else{b.disabled=false;el('onb_wifimsg').textContent='Connected: '+st.staIp;}}
        else if(--left<=0){clearInterval(w);b.disabled=false;el('onb_wifimsg').textContent='Not connected after 30 s - check the password.';}
      }).catch(()=>{if(--left<=0){clearInterval(w);b.disabled=false;el('onb_wifimsg').textContent='Device unreachable - rejoin its setup network and try again.';}}),2500);
    }).catch(()=>{b.disabled=false;el('onb_wifimsg').textContent='Save failed - try again.';});
  }
  function beginWifiHandoff(st,b,ssid){
    if(handoffProbe)return;
    // Already on the device's LAN origin - the handoff has happened. Re-firing it
    // (e.g. navigating Back to the Wi-Fi step) would POST /api/wifi/handoff again
    // and bounce the user straight back to the provider step, making the Wi-Fi
    // step unreachable on TFT boards. Nothing to do here.
    if(location.hostname===st.staIp)return;
    const tok=nimbusTok(),dest=new URL('http://'+st.staIp+'/');
    dest.searchParams.set('t',tok);dest.searchParams.set('onboard','provider');
    const url=dest.toString(),box=el('onb_handoff');
    b.disabled=true;el('onb_wifimsg').textContent='Joined "'+ssid+'".';
    box.style.display='';
    box.innerHTML='<b>Switching to your Wi-Fi…</b><br>Nimbus must close this setup network to protect the TFT. Your computer should rejoin <b>'+esc(ssid)+'</b> automatically. Setup will continue at <a id=onb_handoff_link></a> - keep this page open.';
    const link=el('onb_handoff_link');link.href=url;link.textContent='http://'+st.staIp;

    // An image is a header-free cross-origin reachability probe. Start only after
    // the AP-drop grace so a response via the AP cannot trigger a premature move;
    // once the host rejoins its normal Wi-Fi, the exact-IP logo loads and we carry
    // the token + next wizard step into that new browser origin.
    const probeAfter=delay=>{
      handoffProbe=setTimeout(function tryLan(){
        const img=new Image();let settled=false;
        img.onload=()=>{if(settled)return;settled=true;location.assign(url);};
        img.onerror=()=>{if(settled)return;settled=true;handoffProbe=setTimeout(tryLan,1500);};
        img.src='http://'+st.staIp+'/logo.svg?handoff='+Date.now();
      },delay);
    };
    fetch('/api/wifi/handoff',{method:'POST'})
      .then(r=>r.ok?r.json():Promise.reject(r.status))
      .then(d=>probeAfter((d.dropInMs||4000)+1200))
      // The device has a 20 s fallback teardown even if acknowledgement failed.
      // Keep the exact-IP link visible and begin probing after that fallback.
      .catch(()=>probeAfter(21200));
  }
  function refreshWifiState(){fetch('/api/state').then(r=>r.json()).then(st=>{if(st.sta&&st.staIp){wifiOk=true;const m=el('onb_wifimsg');
      if(st.scrModel==='tft')beginWifiHandoff(st,el('onb_wifisave'),'your Wi-Fi');
      else if(m)m.textContent='Already connected: '+st.staIp;}}).catch(()=>{});}
  function doProvider(){
    const p=el('onb_prov').value,key=el('onb_key').value.trim();
    if(!key){el('onb_provmsg').textContent='Paste an API key.';return;}
    const b=el('onb_provsave');b.disabled=true;el('onb_provmsg').textContent='Saving & verifying…';
    const body={};body[keyField(p)]=key;
    orchApply(body).then(()=>fetch('/api/verify',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'provider='+encodeURIComponent(p)}))
    .then(()=>{
      let tries=0;const vp=setInterval(()=>fetch('/api/orch').then(r=>r.json()).then(d=>{
        const v=d.providers&&d.providers[p]?d.providers[p].verify:-1;
        if(v===1){clearInterval(vp);b.disabled=false;provOk=true;el('onb_provmsg').textContent='Verified ✓';el('onb_key').value='';}
        else if(v===0){clearInterval(vp);b.disabled=false;el('onb_provmsg').textContent='Rejected - check the key.';}
        else if(++tries>20){clearInterval(vp);b.disabled=false;el('onb_provmsg').textContent='Still verifying - tap Save & Verify again in a moment.';}
      }).catch(()=>{}),2000);
    }).catch(()=>{b.disabled=false;el('onb_provmsg').textContent='Verify failed - try again.';});
  }
  function finish(){
    const b=el('onbNext');b.disabled=true;msg('Finishing…');
    fetch('/api/onboard/complete',{method:'POST'}).then(r=>r.ok?r.json():r.json().then(e=>Promise.reject(e.error||r.status)))
    .then(res=>{
      try{localStorage.removeItem('nimbusOnboardStep');}catch(e){}
      const mn=pendingMode!==null?pendingMode:curMode;
      // The server decides whether the display change needs a restart (stored
      // panel vs the driver bound at boot) - robust to the TFT Wi-Fi handoff
      // reloading this page, which would lose a client-side dispChanged().
      const needDisplayRestart=res&&res.restart===true;
      if(pendingMode!==null&&pendingMode!==curMode){
        // The mode switch reboots the device, which also applies a display change.
        msg('Setup complete - switching mode and restarting…');
        apply({mode:mn}).then(()=>setTimeout(()=>location.reload(),4000));
      } else if(needDisplayRestart){
        msg('Setup complete - restarting to apply the display…');
        fetch('/api/onboard/restart',{method:'POST'}).catch(()=>{});
        setTimeout(()=>location.reload(),6000);
      } else { location.reload(); }
    }).catch(e=>{b.disabled=false;msg('Couldn\'t finish: '+e+' - complete the Wi-Fi and provider steps.');});
  }
  el('onbBack').onclick=()=>{if(idx>0){idx--;rememberStep();render();}};
  el('onbSkip').onclick=()=>next();
  el('onbNext').onclick=()=>{
    const s=STEPS[idx];
    if(s.id==='done')return finish();
    if(s.id==='wifi'&&!wifiOk){msg('Connect to Wi-Fi to continue.');return;}
    if(s.id==='wifi'&&handoffProbe){msg('Switching Wi-Fi - keep this page open until it continues.');return;}
    if(s.id==='provider'&&!provOk){msg('Add and verify a provider key to continue.');return;}
    next();
  };
  // Kick off: only when we have a token (a fresh AP device gets one via /?t=).
  function maybeStart(){
    if(!nimbusTok())return;
    fetch('/api/state').then(r=>r.json()).then(s=>{curMode=s.mode||0;bootDisp=s.scrModel||'eink';if(s.needsOnboarding){
      const requested=new URLSearchParams(location.search).get('onboard');
      if(requested==='provider')idx=STEPS.findIndex(x=>x.id==='provider');
      else try{const saved=+localStorage.getItem('nimbusOnboardStep');if(saved>=0&&saved<STEPS.length)idx=saved;}catch(e){}
      rememberStep();show();
    }}).catch(()=>{});
  }
  if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',maybeStart);else maybeStart();
})();
</script>
)=====";
