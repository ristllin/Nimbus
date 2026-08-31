#pragma once

// chat_page - the self-contained web chat surface nimbusd serves at "/".
//
// One dependency-free HTML document: inline CSS and JS, no external scripts,
// styles, fonts, or images. This is load-bearing, not stylistic - the cloud
// relay forwards the device's response with `x-content-type-options: nosniff`
// and the tunnel offers no path to any origin but this one, so anything fetched
// from a CDN would simply fail to load. The page talks ONLY to this daemon's own
// gated API over RELATIVE URLs, so it works unchanged whether it is reached at
// the domain root ("/") or under the relay prefix ("/d/<instance>/").
//
// Data flow: the composer POSTs to api/message; the page then polls
// api/replies?after=<seq> every ~2s for new entries (owner prompts and assistant
// replies) and renders them. Honest states per the CUM-211 rule: a missing
// provider key is stated plainly instead of leaving the page silent, and a turn
// that completes with no reply says so rather than spinning forever.
//
// ASCII-only, product voice (AGENTS.md section 6). Assistant/owner text is
// rendered with textContent, never innerHTML, so a reply can never inject markup.
namespace nimbusd {

inline const char* chatPageHtml() {
  return R"NIMBUS(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Nimbus</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body {
    background: #0f1115; color: #e6e8ec;
    font: 15px/1.5 -apple-system, system-ui, Segoe UI, Roboto, Helvetica, Arial, sans-serif;
    display: flex; flex-direction: column;
  }
  header {
    padding: 12px 16px; border-bottom: 1px solid #23262d;
    font-weight: 600; letter-spacing: .2px; flex: 0 0 auto;
  }
  header .dot {
    display: inline-block; width: 8px; height: 8px; border-radius: 50%;
    background: #4c9a5a; margin-right: 8px; vertical-align: middle;
  }
  #banner {
    display: none; margin: 10px 16px 0; padding: 10px 12px;
    background: #2a2118; border: 1px solid #5a4630; border-radius: 8px;
    color: #f0d9b8; font-size: 14px;
  }
  #log {
    flex: 1 1 auto; overflow-y: auto; padding: 16px;
    display: flex; flex-direction: column; gap: 10px;
  }
  .row { display: flex; }
  .row.user { justify-content: flex-end; }
  .row.assistant { justify-content: flex-start; }
  .row.system { justify-content: center; }
  .bubble {
    max-width: 80%; padding: 9px 13px; border-radius: 14px;
    white-space: pre-wrap; word-wrap: break-word;
  }
  .bubble.user { background: #2f6df0; color: #fff; border-bottom-right-radius: 4px; }
  .bubble.assistant { background: #1b1e25; border: 1px solid #2b2f38; border-bottom-left-radius: 4px; }
  .bubble.system { background: transparent; color: #8b909b; font-size: 13px; text-align: center; }
  #thinking { display: none; padding: 0 16px 6px; color: #8b909b; font-size: 13px; }
  form {
    flex: 0 0 auto; display: flex; gap: 8px; padding: 12px 16px;
    border-top: 1px solid #23262d; background: #0f1115;
  }
  textarea {
    flex: 1 1 auto; resize: none; height: 44px; max-height: 140px;
    padding: 11px 12px; border-radius: 10px;
    background: #161922; color: #e6e8ec; border: 1px solid #2b2f38;
    font: inherit;
  }
  textarea:focus { outline: none; border-color: #2f6df0; }
  button {
    flex: 0 0 auto; padding: 0 18px; border: none; border-radius: 10px;
    background: #2f6df0; color: #fff; font: inherit; font-weight: 600; cursor: pointer;
  }
  button:disabled { opacity: .5; cursor: default; }
</style>
</head>
<body>
<header><span class="dot"></span>Nimbus</header>
<div id="banner"></div>
<div id="log"></div>
<div id="thinking">Thinking...</div>
<form id="composer" autocomplete="off">
  <textarea id="msg" placeholder="Message Nimbus" rows="1"></textarea>
  <button id="send" type="submit">Send</button>
</form>
<script>
(function () {
  var lastSeq = 0;
  var awaiting = false;
  var awaitSince = 0;
  var turnCountAtSend = 0;
  var lastTurnCount = 0;
  var providerConfigured = true;
  var polling = false;

  function el(id) { return document.getElementById(id); }

  function addBubble(role, text) {
    var log = el('log');
    var atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 48;
    var row = document.createElement('div');
    row.className = 'row ' + role;
    var b = document.createElement('div');
    b.className = 'bubble ' + role;
    b.textContent = text;
    row.appendChild(b);
    log.appendChild(row);
    if (atBottom) log.scrollTop = log.scrollHeight;
  }

  function setThinking(on) { el('thinking').style.display = on ? 'block' : 'none'; }

  function setBanner(msg) {
    var bn = el('banner');
    if (!msg) { bn.style.display = 'none'; return; }
    bn.textContent = msg;
    bn.style.display = 'block';
  }

  function apply(d) {
    var arr = (d && d.replies) || [];
    for (var i = 0; i < arr.length; i++) {
      var r = arr[i];
      if (r.seq > lastSeq) lastSeq = r.seq;
      var role = (r.role === 'assistant') ? 'assistant' : 'user';
      addBubble(role, r.text);
      if (role === 'assistant' && awaiting && r.seq > awaitSince) {
        awaiting = false; setThinking(false);
      }
    }
    lastTurnCount = (d && typeof d.turnCount === 'number') ? d.turnCount : lastTurnCount;
    providerConfigured = !(d && d.providerConfigured === false);
    if (!providerConfigured) {
      setBanner("No provider key configured. This instance can't reply yet - add a provider key, or chat on Telegram.");
      if (awaiting) { awaiting = false; setThinking(false); }
    } else {
      setBanner('');
    }
    // Honest silence guard: our turn ran to completion but delivered nothing.
    if (awaiting && providerConfigured && d && d.turnInFlight === false &&
        d.turnCount > turnCountAtSend) {
      awaiting = false; setThinking(false);
      addBubble('system', 'No reply came back. Check the provider key or try again.');
    }
  }

  function poll() {
    if (polling) return;
    polling = true;
    fetch('api/replies?after=' + lastSeq, { headers: { 'Accept': 'application/json' } })
      .then(function (r) { if (!r.ok) throw new Error('http ' + r.status); return r.json(); })
      .then(function (d) { polling = false; apply(d); })
      .catch(function () { polling = false; });
  }

  function send() {
    var ta = el('msg');
    var text = ta.value.replace(/\s+$/, '');
    if (!text) return;
    ta.value = '';
    var btn = el('send');
    btn.disabled = true;
    fetch('api/message', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ chat_id: 'owner', text: text })
    }).then(function (r) {
      btn.disabled = false;
      if (r.status !== 202) { addBubble('system', 'Message not accepted (HTTP ' + r.status + ').'); return; }
      awaiting = true;
      awaitSince = lastSeq;
      turnCountAtSend = lastTurnCount;
      if (providerConfigured) setThinking(true);
      poll();
    }).catch(function () {
      btn.disabled = false;
      addBubble('system', 'Could not send. Check the connection and try again.');
    });
  }

  el('composer').addEventListener('submit', function (e) { e.preventDefault(); send(); });
  el('msg').addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); }
  });

  poll();
  setInterval(poll, 2000);
})();
</script>
</body>
</html>
)NIMBUS";
}

}  // namespace nimbusd
