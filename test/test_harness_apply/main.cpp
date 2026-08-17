#include <unity.h>

#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "nimbus/harness/apply.h"

// Stage E suite - the SECURITY RAILS of turn application, host-tested for the
// first time: scheduled-turn refusals (reboot/ttsOn/devName/sleepOvr/brightOvr),
// owner-visible risk notes, the tts voice-off gate, orch_model key/choice
// validation, reply+ask single-message composition, bare-"Done." suppression,
// the tell/poll unsupported boundary, and mem_write/mem_query dispatch.

using agent::ApplyDeps;
using agent::ApplyState;
using harness_test::LogCapture;
using nimbus::orch::ToolResult;
using nimbus::orch::Turn;
using nimbus::orch::ValidatedAction;

// ---- recording deps ---------------------------------------------------------
struct Rec {
  std::vector<ValidatedAction> execed;    // execConfig calls (policy-stripped)
  std::vector<ValidatedAction> staged;    // stageDevice calls
  std::vector<std::string> spoken;
  std::vector<std::pair<std::string, std::string>> delivered;  // chat, text
  std::vector<std::string> memCalls;      // "name:argsJson"
  std::vector<std::pair<std::string, std::string>> hostModelSet;
  std::vector<std::string> enqueued;      // spawn tasks
  std::vector<std::string> cancelled;
  std::vector<std::string> captured;      // assistant episodic captures
  std::vector<std::string> cues;          // fire()
  int persists = 0, emitAsks = 0, locks = 0, noteSpawns = 0;
  std::string awaited;
  bool cancelResult = true;
  bool memSuccess = true;
  std::string pendingMem;
  bool lastSpawnQuiet = false;   // the quiet flag passed to the last enqueueSpawn
  std::vector<std::string> turnCompletes;   // chatIds passed to turnComplete()
};

static ApplyDeps makeDeps(Rec& r, bool tts = false, bool withSinks = true) {
  ApplyDeps d;
  d.ttsEnabled = tts;
  d.execConfig = [&r](const ValidatedAction& va) { r.execed.push_back(va); };
  if (withSinks) {
    d.stageDevice = [&r](const ValidatedAction& va) { r.staged.push_back(va); };
    d.speak = [&r](const std::string& s) { r.spoken.push_back(s); };
  }
  d.modelIsValid = [](const std::string&, const std::string& m) {
    return m.rfind("good-", 0) == 0;   // models named good-* validate
  };
  d.providerHasKey = [](const std::string& p) { return p != "keyless"; };
  d.setOrchHostModel = [&r](const std::string& p, const std::string& m) {
    r.hostModelSet.push_back({p, m});
  };
  d.setModelMemory = [](const std::string&, const std::string&) { return false; };
  d.syncMemEcho = [] {};
  d.withMemoryLock = [&r](const std::function<void()>& fn) { r.locks++; fn(); };
  d.memDispatch = [&r](const char* name, ArduinoJson::JsonObjectConst args,
                             const nimbus::orch::Principal&) {
    std::string aj;
    serializeJson(args, aj);
    r.memCalls.push_back(std::string(name) + ":" + aj);
    ToolResult res;
    res.success = r.memSuccess;
    res.output = "ok-output";
    res.error = "err-output";
    return res;
  };
  d.persistMemory = [&r] { r.persists++; };
  d.enqueueSpawn = [&r](const nimbus::orch::Spawn& s, const std::string&, bool quiet) {
    r.enqueued.push_back(s.task);
    r.lastSpawnQuiet = quiet;
  };
  d.turnComplete = [&r](const std::string& chatId) { r.turnCompletes.push_back(chatId); };
  d.cancelSession = [&r](const std::string& id) {
    r.cancelled.push_back(id);
    return r.cancelResult;
  };
  d.awaitTag = [&r](const std::string& t) { r.awaited = t; };
  d.noteSpawned = [&r] { r.noteSpawns++; };
  d.deliver = [&r](const std::string& c, const std::string& t) {
    r.delivered.push_back({c, t});
  };
  d.emitAsk = [&r] { r.emitAsks++; };
  d.captureAssistant = [&r](const std::string&, const std::string& t) {
    r.captured.push_back(t);
  };
  d.fire = [&r](const char* cue) { r.cues.push_back(cue); };
  return d;
}

static ApplyState makeState(Rec& r, bool scheduled = false) {
  ApplyState st;
  st.scheduledTurn = scheduled;
  st.pendingMemResults = &r.pendingMem;
  return st;
}

static Turn turnWithDevice(const char* json) {
  Turn t;
  nimbus::orch::DeviceAction da;
  da.json = json;
  t.device.push_back(da);
  return t;
}

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- scheduled-turn refusal rails -------------------------------------------

static void test_scheduled_turn_refuses_reboot() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r, /*scheduled=*/true);
  bool spawned = false;
  agent::applyTurn(turnWithDevice("{\"reboot\":true}"), "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.staged.size());   // never reached the DeviceSink
  TEST_ASSERT_TRUE(LogCapture::contains("reboot-refused(scheduled)"));
}

static void test_owner_turn_allows_reboot() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r, /*scheduled=*/false);
  bool spawned = false;
  agent::applyTurn(turnWithDevice("{\"reboot\":true}"), "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.staged.size());
}

static void test_scheduled_turn_strips_risky_config_knobs() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r, /*scheduled=*/true);
  bool spawned = false;
  agent::applyTurn(
      turnWithDevice("{\"config\":{\"ttsOn\":true,\"sleepOvr\":true,"
                     "\"brightOvr\":true,\"devName\":\"evil\",\"ledBrightness\":40}}"),
      "1001", d, st, spawned);
  // The action still executes - but with every guarded knob stripped.
  TEST_ASSERT_EQUAL(1, (int)r.execed.size());
  const ValidatedAction& ex = r.execed[0];
  TEST_ASSERT_FALSE(ex.hasTtsOn);
  TEST_ASSERT_FALSE(ex.hasSleepOvr);
  TEST_ASSERT_FALSE(ex.hasBrightOvr);
  TEST_ASSERT_FALSE(ex.hasDevName);
  TEST_ASSERT_TRUE(ex.hasLedBrightness);   // benign knob still applies
  TEST_ASSERT_EQUAL(40, ex.ledBrightness);
  // No risk note is composed for a refused switch...
  TEST_ASSERT_EQUAL_STRING("", st.riskNote.c_str());
  // ...and the refusals are all logged.
  TEST_ASSERT_TRUE(LogCapture::contains("ttsOn-refused(scheduled)"));
  TEST_ASSERT_TRUE(LogCapture::contains("sleepOvr-refused(unattended)"));
  TEST_ASSERT_TRUE(LogCapture::contains("brightOvr-refused(unattended)"));
  TEST_ASSERT_TRUE(LogCapture::contains("devName-refused(scheduled)"));
}

static void test_owner_turn_risk_notes_ride_the_reply() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t = turnWithDevice("{\"config\":{\"sleepOvr\":true}}");
  t.reply = "Overridden.";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.execed.size());
  TEST_ASSERT_TRUE(r.execed[0].hasSleepOvr);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  // The owner-visible note is appended to the delivered reply, model-optional or not.
  TEST_ASSERT_TRUE(r.delivered[0].second.find("Overridden.") == 0);
  TEST_ASSERT_TRUE(r.delivered[0].second.find(
                       "low-battery sleep OVERRIDDEN") != std::string::npos);
  // Consumed: the note never leaks into a later turn.
  TEST_ASSERT_EQUAL_STRING("", st.riskNote.c_str());
}

static void test_risk_note_delivered_even_without_reply() {
  // A tool already replied (toolRepliedThisTurn) and the turn has no reply text:
  // the risk note must still land as its own message.
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  st.toolRepliedThisTurn = true;
  bool spawned = false;
  agent::applyTurn(turnWithDevice("{\"config\":{\"brightOvr\":true}}"),
                   "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_TRUE(r.delivered[0].second.find("LED cap OVERRIDDEN") !=
                   std::string::npos);
}

// ---- protected keys ---------------------------------------------------------

static void test_protected_config_never_reaches_exec() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(
      turnWithDevice("{\"config\":{\"token\":\"sk-live-123\",\"ledBrightness\":10}}"),
      "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.execed.size());   // whole action refused
  TEST_ASSERT_TRUE(LogCapture::contains("protected-BLOCKED"));
  // The secret value must never appear in any log line.
  TEST_ASSERT_FALSE(LogCapture::contains("sk-live-123"));
}

// ---- tts gate ---------------------------------------------------------------

static void test_tts_action_gated_by_owner_toggle() {
  Rec r;
  ApplyDeps d = makeDeps(r, /*tts=*/false);
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(turnWithDevice("{\"tts\":\"hello\"}"), "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.spoken.size());
  TEST_ASSERT_TRUE(LogCapture::contains("tts(voice-off)"));

  Rec r2;
  ApplyDeps d2 = makeDeps(r2, /*tts=*/true);
  ApplyState st2 = makeState(r2);
  agent::applyTurn(turnWithDevice("{\"tts\":\"hello\"}"), "1001", d2, st2, spawned);
  TEST_ASSERT_EQUAL(1, (int)r2.spoken.size());
  TEST_ASSERT_EQUAL_STRING("hello", r2.spoken[0].c_str());
}

static void test_tts_text_bounded_400() {
  Rec r;
  ApplyDeps d = makeDeps(r, /*tts=*/true);
  ApplyState st = makeState(r);
  bool spawned = false;
  std::string longText(900, 'x');
  agent::applyTurn(turnWithDevice(
                       ("{\"tts\":\"" + longText + "\"}").c_str()),
                   "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.spoken.size());
  TEST_ASSERT_EQUAL(400, (int)r.spoken[0].size());
}

// ---- orch_model validation --------------------------------------------------

static void test_orch_model_valid_and_keyed_applies() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(
      turnWithDevice("{\"type\":\"orch_model\",\"provider\":\"anthropic\",\"model\":\"good-fable\"}"),
      "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.hostModelSet.size());
  TEST_ASSERT_EQUAL_STRING("anthropic", r.hostModelSet[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("good-fable", r.hostModelSet[0].second.c_str());
}

static void test_orch_model_invalid_model_refused() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(
      turnWithDevice("{\"type\":\"orch_model\",\"provider\":\"anthropic\",\"model\":\"bogus\"}"),
      "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.hostModelSet.size());
  TEST_ASSERT_TRUE(LogCapture::contains("orch_model(invalid)"));
}

static void test_orch_model_keyless_provider_refused() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(
      turnWithDevice("{\"type\":\"orch_model\",\"provider\":\"keyless\",\"model\":\"good-x\"}"),
      "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.hostModelSet.size());
  TEST_ASSERT_TRUE(LogCapture::contains("orch_model(no-key)"));
}

// ---- reply / ask / Done. ----------------------------------------------------

static void test_reply_and_ask_deliver_as_one_message() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  t.reply = "Here's the status.";
  t.ask = "Want details?";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING("Here's the status.\n\nWant details?",
                           r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL(1, r.emitAsks);
  // Both halves are episodically captured.
  TEST_ASSERT_EQUAL(2, (int)r.captured.size());
  TEST_ASSERT_EQUAL_STRING("Here's the status.", st.lastReply.c_str());
}

static void test_empty_turn_signals_completion_not_done() {
  // The hardcoded "Done." ack was removed: an empty reply delivers NO message at the
  // harness level (async channels stay silent). Instead it signals turnComplete, and
  // the DEVICE wiring resolves only the synchronous channels (web/voice/serial) - so
  // a reply-less turn never false-fails a web/voice poll, and Telegram stays silent.
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(Turn{}, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());          // no fabricated message
  TEST_ASSERT_EQUAL(1, (int)r.turnCompletes.size());      // completion signalled once
  TEST_ASSERT_EQUAL_STRING("1001", r.turnCompletes[0].c_str());
}

// A spawn or a tool-reply already stands on its own -> no completion signal needed.
static void test_completion_suppressed_after_spawn() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  Turn t; nimbus::orch::Spawn s; s.task = "x"; t.spawn.push_back(s);
  bool spawned = false;
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.turnCompletes.size());
}

// Interactive spawn keeps quiet=false (ack shown); a scheduled turn passes quiet=true
// (ack suppressed downstream in the job engine so a loop emits one deliverable).
static void test_spawn_quiet_flag_tracks_scheduled_turn() {
  {
    Rec r; ApplyDeps d = makeDeps(r); ApplyState st = makeState(r, /*scheduled*/false);
    Turn t; nimbus::orch::Spawn s; s.task = "x"; t.spawn.push_back(s);
    bool sp = false; agent::applyTurn(t, "1001", d, st, sp);
    TEST_ASSERT_FALSE(r.lastSpawnQuiet);
  }
  {
    Rec r; ApplyDeps d = makeDeps(r); ApplyState st = makeState(r, /*scheduled*/true);
    Turn t; nimbus::orch::Spawn s; s.task = "x"; t.spawn.push_back(s);
    bool sp = false; agent::applyTurn(t, "1001", d, st, sp);
    TEST_ASSERT_TRUE(r.lastSpawnQuiet);
  }
}

static void test_done_suppressed_after_spawn_or_tool_reply() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  nimbus::orch::Spawn s;
  s.task = "research something";
  t.spawn.push_back(s);
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_TRUE(spawned);
  TEST_ASSERT_EQUAL(0, (int)r.delivered.size());   // no bare "Done." after a spawn
  TEST_ASSERT_EQUAL(1, r.noteSpawns);

  Rec r2;
  ApplyDeps d2 = makeDeps(r2);
  ApplyState st2 = makeState(r2);
  st2.toolRepliedThisTurn = true;
  bool spawned2 = false;
  agent::applyTurn(Turn{}, "1001", d2, st2, spawned2);
  TEST_ASSERT_EQUAL(0, (int)r2.delivered.size());  // tool already answered
}

// ---- sessions ---------------------------------------------------------------

static void test_session_ops_spawn_terminate_tell() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  nimbus::orch::SessionOp sp;
  sp.op = "spawn";
  sp.task = "dig into logs";
  t.session_ops.push_back(sp);
  nimbus::orch::SessionOp term;
  term.op = "terminate";
  term.id = "job0042";
  t.session_ops.push_back(term);
  nimbus::orch::SessionOp tell;
  tell.op = "tell";
  tell.id = "job0042";
  tell.message = "hurry up";
  t.session_ops.push_back(tell);
  t.reply = "ok";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_TRUE(spawned);
  TEST_ASSERT_EQUAL_STRING("dig into logs", r.enqueued[0].c_str());
  TEST_ASSERT_EQUAL_STRING("job0042", r.cancelled[0].c_str());
  TEST_ASSERT_TRUE(r.delivered[0].second.find("Stopped job0042.") == 0);
  // tell -> the unsupported boundary is fed back through deferred results.
  TEST_ASSERT_TRUE(r.pendingMem.find("'tell' isn't supported") != std::string::npos);
}

static void test_await_aims_round_robin() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  t.await_.push_back("job0007");
  t.reply = "checking";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL_STRING("job0007", r.awaited.c_str());
}

// ---- memory -----------------------------------------------------------------

static void test_mem_write_and_query_dispatch_under_lock() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  nimbus::orch::MemWrite w;
  w.content = "owner likes tea";
  w.importance = 0.9;
  w.permanent = true;
  t.mem_write.push_back(w);
  t.mem_query.push_back("what drinks");
  t.reply = "noted";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, r.locks);
  TEST_ASSERT_EQUAL(2, (int)r.memCalls.size());
  TEST_ASSERT_TRUE(r.memCalls[0].find("memory.write:") == 0);
  TEST_ASSERT_TRUE(r.memCalls[0].find("owner likes tea") != std::string::npos);
  TEST_ASSERT_TRUE(r.memCalls[1].find("memory.search:") == 0);
  TEST_ASSERT_EQUAL(1, r.persists);   // persisted exactly once
  TEST_ASSERT_TRUE(r.pendingMem.find("what drinks -> ok-output") != std::string::npos);
  // The memsaved cue fired for the successful write; reply cue for the text.
  TEST_ASSERT_TRUE(std::find(r.cues.begin(), r.cues.end(), "memsaved") != r.cues.end());
}

static void test_mem_query_only_does_not_persist() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  t.mem_query.push_back("just a lookup");
  t.reply = "looking";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, r.persists);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scheduled_turn_refuses_reboot);
  RUN_TEST(test_owner_turn_allows_reboot);
  RUN_TEST(test_scheduled_turn_strips_risky_config_knobs);
  RUN_TEST(test_owner_turn_risk_notes_ride_the_reply);
  RUN_TEST(test_risk_note_delivered_even_without_reply);
  RUN_TEST(test_protected_config_never_reaches_exec);
  RUN_TEST(test_tts_action_gated_by_owner_toggle);
  RUN_TEST(test_tts_text_bounded_400);
  RUN_TEST(test_orch_model_valid_and_keyed_applies);
  RUN_TEST(test_orch_model_invalid_model_refused);
  RUN_TEST(test_orch_model_keyless_provider_refused);
  RUN_TEST(test_reply_and_ask_deliver_as_one_message);
  RUN_TEST(test_empty_turn_signals_completion_not_done);
  RUN_TEST(test_completion_suppressed_after_spawn);
  RUN_TEST(test_spawn_quiet_flag_tracks_scheduled_turn);
  RUN_TEST(test_done_suppressed_after_spawn_or_tool_reply);
  RUN_TEST(test_session_ops_spawn_terminate_tell);
  RUN_TEST(test_await_aims_round_robin);
  RUN_TEST(test_mem_write_and_query_dispatch_under_lock);
  RUN_TEST(test_mem_query_only_does_not_persist);
  UNITY_END();
  return 0;
}
