#include "nimbus/harness/apply.h"

#include "nimbus/harness/log.h"

// Lifted from src/agent/orchestrator.cpp applyTurn (Stage E). Every summary
// token, refusal string, and risk note moved BYTE-IDENTICAL - grep-diff the
// literals against the pre-lift file when in doubt; test_harness_apply pins
// the security-relevant ones.

namespace agent {

using nimbus::orch::ActionKind;
using nimbus::orch::ToolResult;
using nimbus::orch::Turn;
using nimbus::orch::ValidatedAction;

std::string applyDeviceElement(const std::string& json, bool scheduledTurn,
                               const std::string& chatId, const ApplyDeps& d,
                               std::string* riskNote) {
  // One device[] element: validate + policy-strip + execute. Shared by the
  // end-of-turn device[] loop below AND the mid-turn `device.control` registry
  // tool, so both paths carry the IDENTICAL security envelope (protected-key
  // block, scheduled-turn strips, risk notes). Every summary token is the
  // pre-lift byte-identical literal (test_harness_apply pins them).
  std::string devSummary;
  JsonDocument dd;
  if (deserializeJson(dd, json)) { return "skip "; }
  ValidatedAction va =
      nimbus::orch::validateAction(dd.as<ArduinoJson::JsonVariantConst>());
  if (!va.allowed) {
    // Blocked/unknown: log the policy reason ("protected-BLOCKED"/"unsupported"),
    // never the offending value (the portable validator never puts a secret here).
    return va.reason.empty() ? std::string("skip ") : va.reason + " ";
  }
  switch (va.kind) {
    case ActionKind::Config: {
      // POLICY: strip the knobs a SCHEDULED/unattended turn may not flip -
      // a loop/injected turn must not change the device's audio, identity, or
      // physical-risk switches (prompt-injection persistence rail). The
      // stripped copy goes to execConfig, which applies writes with no policy.
      ValidatedAction ex = va;
      if (va.hasTtsOn && scheduledTurn) {
        devSummary += "ttsOn-refused(scheduled) ";
        ex.hasTtsOn = false;
      } else if (va.hasTtsOn) {
        devSummary += va.ttsOn ? "ttsOn " : "ttsOff ";
      }
      if (va.hasSleepOvr && scheduledTurn) {
        devSummary += "sleepOvr-refused(unattended) ";
        ex.hasSleepOvr = false;
      } else if (va.hasSleepOvr) {
        devSummary += va.sleepOvr ? "sleepOvr:ON(deep-discharge-risk) " : "sleepOvr:off ";
        // Owner-visible, NOT model-optional: the log line alone left zero
        // trace unless the owner went looking (review) - the note rides the
        // delivered reply itself, whatever the model chose to say.
        if (riskNote)
          *riskNote += va.sleepOvr
              ? "\n⚠ low-battery sleep OVERRIDDEN - the pack can deep-discharge (resets at reboot)"
              : "\n✓ low-battery sleep protection re-enabled";
      }
      if (va.hasBrightOvr && scheduledTurn) {
        devSummary += "brightOvr-refused(unattended) ";
        ex.hasBrightOvr = false;
      } else if (va.hasBrightOvr) {
        devSummary += va.brightOvr ? "brightOvr:ON(heat-risk) " : "brightOvr:off ";
        if (riskNote)
          *riskNote += va.brightOvr
              ? "\n⚠ LED cap OVERRIDDEN to 100% - sustained heat risk to the panel (resets at reboot)"
              : "\n✓ LED 60% safety cap re-enabled";
      }
      if (va.hasDevName && scheduledTurn) {
        devSummary += "devName-refused(scheduled) ";
        ex.hasDevName = false;
      } else if (va.hasDevName) {
        devSummary += "devName(reboot-to-apply) ";
      }
      if (d.execConfig) d.execConfig(ex);
      // Talk-to-configure knobs (posture/profile/theme/attnHoldMs) mutate the
      // main-task-owned Config/theme, so they route through the DeviceSink like
      // led/lights - the main loop applies + persists them (same pipeline as
      // the menu/web UI). NVS-only knobs above stay inline (any-task safe).
      if (d.stageDevice &&
          (va.hasPosture || va.hasProfile || va.hasTheme || va.hasAttnHoldMs))
        d.stageDevice(va);
      devSummary += "cfg ";
      break;
    }
    case ActionKind::Tts:
      // EXPLICIT tts action - but ONLY when the owner's "Voice replies" toggle is
      // ON (default ON for a speaker board, N12). Field bug (P2.5): the model
      // reliably chose to speak whenever the owner seemed nearby (voice input),
      // and this gate bypass meant there was NO way to mute it. When OFF the action
      // is a silent no-op - the text reply still delivers on its own channel.
      if (!d.ttsEnabled) { devSummary += "tts(voice-off) "; break; }
      if (d.speak && !va.text.empty()) {
        std::string s = va.text;
        if (s.size() > 400) s = s.substr(0, 400);   // bound the synth/upload
        d.speak(s);
        // Capture spoken output to history so "what did you just say?" works even
        // when the turn had no text reply (the tts action was previously invisible).
        if (d.captureAssistant) d.captureAssistant(chatId, s);
        devSummary += "tts ";
      } else devSummary += "tts(no-sink) ";
      break;
    // led / lights / reboot execute through the DeviceSink: main.cpp stages the
    // validated action thread-safely and the main loop (which owns the ring +
    // power) applies it. Without a sink installed they remain logged no-ops.
    case ActionKind::Led:
      if (d.stageDevice) { d.stageDevice(va); devSummary += "led "; }
      else devSummary += "led(noop) ";
      break;
    case ActionKind::Lights:
      if (d.stageDevice) { d.stageDevice(va); devSummary += "lights "; }
      else devSummary += "lights(noop) ";
      break;
    case ActionKind::Reboot:
      // prism guard: a scheduled loop must NOT reboot - a reboot in the
      // boot-relative clock window would reset the daily cost ceiling.
      if (scheduledTurn) { devSummary += "reboot-refused(scheduled) "; }
      else if (d.stageDevice) { d.stageDevice(va); devSummary += "reboot "; }
      else devSummary += "reboot(noop) ";
      break;
    case ActionKind::OrchModel: {
      // The agent reroutes its OWN provider host + model (owner: "switch to opus").
      // Validate the model against that provider's choice list, then set BOTH host
      // and model - NVS-only, any-task safe. Keys + provider PRIORITY stay owner-only
      // (never settable here); this host/model switch is the one deliberate exception.
      // Refuse a switch to a provider with NO API key configured - modelIsValid only
      // checks the static choice list, so without this the agent could reroute its own
      // host to a keyless provider and strand every subsequent turn (prism). Keys are
      // owner-only (web UI); the agent can only move between providers already keyed.
      if (!d.modelIsValid || !d.modelIsValid(va.orchProvider, va.orchModel)) {
        devSummary += "orch_model(invalid) ";
      } else if (!d.providerHasKey || !d.providerHasKey(va.orchProvider)) {
        devSummary += "orch_model(no-key) ";
      } else {
        d.setOrchHostModel(va.orchProvider, va.orchModel);
        devSummary += "orch_model(" + va.orchProvider + "/" + va.orchModel + ") ";
      }
      break;
    }
    default:                       devSummary += "skip ";         break;
  }
  return devSummary;
}

void applyTurn(const Turn& t, const std::string& chatId, const ApplyDeps& d,
               ApplyState& st, bool& spawnedOut) {
  // memory (device enforces the cap; setModel returns true if it had to truncate)
  if (t.memory.length() && d.setModelMemory && d.setModelMemory(chatId, t.memory))
    hlog::log("orchestrator: model-memory truncated to cap");
  if (t.memory.length() && d.syncMemEcho) d.syncMemEcho();

  // scratchpad - inline persistent-tier update. A FREE write
  // beside `memory`; the device replaces only the tiers the model returned.
  // ADMIN-gated (prism v4.1 #13): the device has ONE global scratchpad and it is
  // rendered into EVERY chat's prompt, so a guest/member turn writing it plants
  // text in the admin's next context - the same prompt-injection channel the
  // memory.scratchpad tool already guards with manageTenants. Non-admin turns
  // skip the write silently (their turn is otherwise unaffected).
  if (t.scratchpad.present && d.applyScratch) {
    if (d.whoFor(chatId).perms().manageTenants) d.applyScratch(t.scratchpad);
    else hlog::log("orchestrator: scratchpad write skipped (not admin)");
  }

  // device[] - validate each element and apply ONLY the allowed ones. The
  // classification (allowed vs protected-BLOCKED) is the portable security core;
  // the per-element work is the shared applyDeviceElement (also the mid-turn
  // `device.control` tool's engine).
  std::string devSummary;
  for (const auto& da : t.device)
    devSummary += applyDeviceElement(da.json, st.scheduledTurn, chatId, d, &st.riskNote);
  if (!devSummary.empty()) hlog::logf("orchestrator: device[%s]", devSummary.c_str());

  // mem_write[] / mem_query[] (Phase 3) - dispatch through the memory ToolRegistry
  // (embed + store / search), in-process on this task. mem_query results are parked
  // for the NEXT turn's inputs (deferred-result pattern). Persist once if anything
  // was written. Runs BEFORE reply so a "remember X, ok?" turn stores then confirms.
  // v3.7.0: every memory tool runs under THIS turn's principal - the routing
  // chat is the namespace, and only an owner chat may touch the shared one.
  const nimbus::orch::Principal who = d.whoFor(chatId);
  if (d.memDispatch && (!t.mem_write.empty() || !t.mem_query.empty())) {
    auto memWork = [&] {
      bool memMutated = false;
      for (const auto& w : t.mem_write) {
        JsonDocument a;
        a["content"]    = w.content;
        a["importance"] = w.importance;
        a["permanent"]  = w.permanent;
        if (!w.ttl.empty()) a["ttl"] = w.ttl;
        ToolResult r = d.memDispatch("memory.write", a.as<ArduinoJson::JsonObjectConst>(), who);
        hlog::logf("orchestrator: mem_write -> %s", (r.success ? r.output : r.error).c_str());
        if (r.success && d.fire) d.fire("memsaved");   // heavy tier: "Orders received."
        memMutated = true;
      }
      for (const auto& q : t.mem_query) {
        JsonDocument a;
        a["query"] = q;
        ToolResult r = d.memDispatch("memory.search", a.as<ArduinoJson::JsonObjectConst>(), who);
        if (st.pendingMemResults)
          *st.pendingMemResults += q + " -> " + (r.success ? r.output : r.error) + "\n";
      }
      if (memMutated && d.persistMemory) d.persistMemory();
    };
    // Serialize this in-process registry dispatch with the AsyncTCP web/MCP task,
    // which touches the same VectorMemory/Scratchpad (this runs on the turn task).
    if (d.withMemoryLock) d.withMemoryLock(memWork);
    else memWork();
  }

  // spawn[] - ENQUEUE only (the poll loop dispatches one per cycle).
  spawnedOut = false;
  if (d.enqueueSpawn)
    for (const auto& s : t.spawn) { d.enqueueSpawn(s, chatId, st.scheduledTurn); spawnedOut = true; }

  // session_ops[] (Phase 3) - manage sub-agents. spawn maps to the same enqueue
  // path as spawn[]; terminate stops a running job; list is a no-op (the running-
  // sessions digest is already injected every turn). tell/poll are NOT supported
  // on this fabric - sub-agents are fire-and-forget jobs, and the adapter's
  // answer() only resumes a NeedsInput (HITL) job, it can't inject a follow-up
  // user message into a live conversation. Rather than fail silently, we feed the
  // model that boundary via the deferred [MEMORY RESULTS] channel so it learns to
  // use spawn + await instead. Results/replies flow back through [FRESH RESULTS].
  for (const auto& op : t.session_ops) {
    if (op.op == "spawn") {
      nimbus::orch::Spawn s;
      s.task     = op.task.empty() ? op.message : op.task;  // tolerate either field
      s.provider = op.provider;
      s.model    = op.model;
      s.skill    = op.skill;      // v4.0.0: the wire finally carries the capsule id
      s.name     = op.name;
      s.project  = op.project;
      s.attach   = op.attach;
      s.category = "research";
      s.note     = "On it.";
      if (s.task.length() && d.enqueueSpawn) { d.enqueueSpawn(s, chatId, st.scheduledTurn); spawnedOut = true; }
    } else if (op.op == "terminate") {
      bool ok = d.cancelSession && d.cancelSession(op.id);
      hlog::logf("orchestrator: session terminate %s -> %s", op.id.c_str(),
                 ok ? "ok" : "not found");
      if (d.deliver)
        d.deliver(chatId, ok ? ("Stopped " + op.id + ".")
                             : ("No running session '" + op.id + "' to stop."));
    } else if (op.op == "list") {
      // no-op: [ACTIVE SESSIONS] is already assembled into every turn's prompt.
    } else {  // tell / poll
      if (st.pendingMemResults)
        *st.pendingMemResults += std::string("[SESSION] '") + op.op +
          "' isn't supported: sub-agents are fire-and-forget. Use spawn to start work"
          " and await/[FRESH RESULTS] to read their output.\n";
      hlog::logf("orchestrator: session op '%s' unsupported (fire-and-forget fabric)",
                 op.op.c_str());
    }
  }
  if (spawnedOut && d.noteSpawned) d.noteSpawned();

  // await[] - aim the round-robin at the first awaited tag, poll now.
  if (!t.await_.empty() && d.awaitTag) d.awaitTag(t.await_[0]);

  // reply / ask
  // reply + ask arrive as ONE message (owner-flagged: two consecutive bot messages
  // read as a glitch in human conversation). The ask still drives the needs-you
  // attention state via emitAsk().
  if ((t.reply.length() || t.ask.length()) && d.fire) d.fire("reply");
  st.lastReply = t.reply;   // captured for the Local Loops semantic-repeat hash
  const std::string riskTail = st.riskNote;   // owner-visible override note (never silent)
  st.riskNote.clear();
  if (t.reply.length() && t.ask.length()) {
    if (d.deliver) d.deliver(chatId, t.reply + riskTail + "\n\n" + t.ask);
    if (d.emitAsk) d.emitAsk();
  } else if (t.reply.length()) {
    if (d.deliver) d.deliver(chatId, t.reply + riskTail);
  } else if (t.ask.length()) {
    if (d.deliver) d.deliver(chatId, t.ask + riskTail);
    if (d.emitAsk) d.emitAsk();
  } else if (riskTail.length()) {
    // No reply/ask, but a risk switch flipped - the note must still land (never silent).
    if (d.deliver) d.deliver(chatId, riskTail);
  } else if (!spawnedOut && !st.toolRepliedThisTurn) {
    // No user-visible output this turn. There is NO fabricated "Done." on async
    // channels (the owner's complaint: scheduled loops pinging "Done." to Telegram).
    // But web chat / on-device voice / serial are SYNCHRONOUS - their client resolves
    // its poll only on a delivered message, so a reply-less device/spawn turn would
    // otherwise wait out the timeout and show a FALSE failure. Signal completion there;
    // the device wiring no-ops it for Telegram (see orchestrator turnComplete).
    if (d.turnComplete) d.turnComplete(chatId);
  }
  // (ApplyState.quietFallback / the quietOk plumbing are now vestigial - a later
  // cleanup can drop them; they no longer change behavior.)

  // Spoken replies are AGENT-CHOSEN but OWNER-GATED (P2.5): the model picks the
  // moment (reply.speak / reply.telegram voice:true / the orch_turn `tts` action),
  // and every one of those paths honors the owner's "Voice replies" toggle.

  // Episodic auto-capture (Q2): record the assistant's turn output.
  if (d.captureAssistant) {
    if (t.reply.length()) d.captureAssistant(chatId, t.reply);
    if (t.ask.length())   d.captureAssistant(chatId, t.ask);
  }
}

}  // namespace agent
