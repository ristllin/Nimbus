#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "nimbus/harness/apply.h"
#include "nimbus/orch/model_catalog.h"     // lane F: the model store
#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_refresh.h"
#include "nimbus/orch/model_selectable.h"
#include "nimbus/orch/models_tool.h"
#include "nimbus/orch/provider_slots.h"

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
  bool configChanges = true;   // what execConfig answers: did a stored value change?
  std::string memCode;     // the outcome code memory.write returns ("" = none)
  std::string pendingMem;
  bool lastSpawnQuiet = false;   // the quiet flag passed to the last enqueueSpawn
  bool acceptSpawns = true;      // what the job queue answers (false: refused)
  std::vector<std::string> turnCompletes;   // chatIds passed to turnComplete()
};

static ApplyDeps makeDeps(Rec& r, bool tts = false, bool withSinks = true) {
  ApplyDeps d;
  d.ttsEnabled = tts;
  d.execConfig = [&r](const ValidatedAction& va) {
    r.execed.push_back(va);
    return r.configChanges;   // 23 Oct: false simulates a nothing-stored action
  };
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
    if (std::string(name) == "memory.write") res.code = r.memCode;
    return res;
  };
  d.persistMemory = [&r] { r.persists++; };
  d.enqueueSpawn = [&r](const nimbus::orch::Spawn& s, const std::string&, bool quiet) {
    r.enqueued.push_back(s.task);
    r.lastSpawnQuiet = quiet;
    return r.acceptSpawns;
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
  d.providerHasKey = [](const std::string& p) { return p != "zai"; };
  agent::applyTurn(
      turnWithDevice("{\"type\":\"orch_model\",\"provider\":\"zai\",\"model\":\"good-x\"}"),
      "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.hostModelSet.size());
  TEST_ASSERT_TRUE(LogCapture::contains("orch_model(no-key)"));
}


// Lane F: orch_model asks THE check over the model store (the device's modelIsValid is
// store::modelSelectable). claude-sonnet-5-5 is in the Oct 2026 store but was in no
// compiled list and not in Lumi's 8-id NVS list, so "switch to Sonnet 5.5" was refused
// ("No Sonnet 5.5 in what I can access"); now an id in the file applies, and an id the
// file does not list (nor the cold-start list) is still refused.
static void test_orch_model_id_in_the_store_applies() {
  std::string body;
  {
    FILE* f = std::fopen("test/support/fixtures/models_2026-10/anthropic.json", "rb");
    TEST_ASSERT_NOT_NULL(f);
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);
  }
  nimbus::orch::ModelHarvest h;
  h.provider = "anthropic";
  nimbus::orch::harvestBody(h, body.data(), body.size());
  nimbus::orch::finishHarvest(h);
  nimbus::orch::ModelStoreMeta meta;
  meta.choices = nimbus::orch::directChoices("anthropic", h.rows, 0);
  static std::string store;
  store = nimbus::orch::serializeModelStore(meta, h.rows);
  for (const char* model : {"claude-sonnet-5-5", "claude-opus-4-7", "claude-imaginary-9"}) {
    Rec r;
    ApplyDeps d = makeDeps(r);
    d.modelIsValid = [](const std::string& p, const std::string& m) {
      nimbus::orch::SelectableSources src;
      src.storeText = &store;
      src.coldStart = nimbus::orch::staticModelChoices(p.c_str());
      return nimbus::orch::modelSelectable(p, m, nimbus::orch::PickRole::Any, src);
    };
    ApplyState st = makeState(r);
    bool spawned = false;
    const std::string turn =
        std::string("{\"type\":\"orch_model\",\"provider\":\"anthropic\",\"model\":\"") + model + "\"}";
    agent::applyTurn(turnWithDevice(turn.c_str()), "1001", d, st, spawned);
    const bool listed = std::string(model) != "claude-imaginary-9";
    TEST_ASSERT_EQUAL_MESSAGE(listed ? 1 : 0, (int)r.hostModelSet.size(), model);
    if (listed) TEST_ASSERT_EQUAL_STRING(model, r.hostModelSet[0].second.c_str());
  }
  TEST_ASSERT_FALSE(nimbus::orch::modelInChoices(meta.choices, "claude-opus-4-7"));   // past the short list
}

// ---- reply / ask / Done. ----------------------------------------------------

// CUM-514 (L20 docs audit): the order the prompt now states - a turn's mem_write
// is stored BEFORE its reply is delivered (so a reply delivered at all means the
// write already ran; whether it SUCCEEDED the model still cannot see).
static void test_mem_write_is_stored_before_the_reply_is_delivered() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  std::vector<std::string> order;
  auto mem = d.memDispatch;
  d.memDispatch = [&order, mem](const char* n, ArduinoJson::JsonObjectConst a,
                                const nimbus::orch::Principal& p) {
    order.push_back(std::string("mem:") + n);
    return mem(n, a, p);
  };
  d.deliver = [&order](const std::string&, const std::string& t) { order.push_back("deliver:" + t); };
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  t.reply = "I'll remember that.";
  nimbus::orch::MemWrite w;
  w.content = "Dana uses 45x240 mm studs";
  t.mem_write.push_back(w);
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(2, (int)order.size());
  TEST_ASSERT_EQUAL_STRING("mem:memory.write", order[0].c_str());
  TEST_ASSERT_EQUAL_STRING("deliver:I'll remember that.", order[1].c_str());
}

// The tell/poll nudge names the real channel, never the retired await[] field.
static void test_tell_poll_nudge_does_not_mention_await() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  nimbus::orch::SessionOp op;
  op.op = "tell";
  op.id = "job0001";
  t.session_ops.push_back(op);
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_TRUE(r.pendingMem.find("[FRESH RESULTS]") != std::string::npos);
  TEST_ASSERT_TRUE(r.pendingMem.find("await") == std::string::npos);
}

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
  // One message, one history row: the ask rides the reply's row.
  TEST_ASSERT_EQUAL(1, (int)r.captured.size());
  TEST_ASSERT_EQUAL_STRING("Here's the status.\n\nWant details?", r.captured[0].c_str());
  TEST_ASSERT_EQUAL_STRING("Here's the status.", st.lastReply.c_str());
}

// Row 52: a question written into both fields is delivered once; the ask is
// still emitted (the client offers a reply). Counter-test: an ask that is not
// the reply's ending, or only appears mid-reply, is still appended.
static void test_lane_l_an_ask_the_reply_already_ends_with_is_not_repeated() {
  struct Case { const char* reply; const char* ask; const char* want; };
  const Case cases[] = {
      {"Here's the status. Want details?", "Want details?", "Here's the status. Want details?"},
      {"Here's the status. Want details?\n", "  Want details?", "Here's the status. Want details?\n"},
      {"Want details? Here's the status.", "Want details?", "Want details? Here's the status.\n\nWant details?"},
      {"Here's the status.", "Want details?", "Here's the status.\n\nWant details?"},
  };
  for (const Case& c : cases) {
    Rec r;
    ApplyDeps d = makeDeps(r);
    ApplyState st = makeState(r);
    bool spawned = false;
    Turn t;
    t.reply = c.reply;
    t.ask = c.ask;
    agent::applyTurn(t, "1001", d, st, spawned);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.delivered.size(), c.reply);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.want, r.delivered[0].second.c_str(), c.reply);
    TEST_ASSERT_EQUAL_MESSAGE(1, r.emitAsks, c.reply);
  }
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

// CUM-536: a turn that read a sub-agent result whose connector never ran stores
// none of it. Every mem_write item is refused (no memory.write call, no persist,
// no memsaved cue), the refusal is fed back for the next turn, and a mem_query in
// the same turn still runs.
static void test_unverified_connector_turn_refuses_mem_write() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  st.unverifiedConnector = true;
  bool spawned = false;
  Turn t;
  nimbus::orch::MemWrite w;
  w.content = "Dana's calendar this week: Team Stand-up Mon 09:00";
  t.mem_write.push_back(w);
  t.mem_query.push_back("dana calendar");
  t.reply = "Here is your week";
  agent::applyTurn(t, "web", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.memCalls.size());
  TEST_ASSERT_TRUE(r.memCalls[0].find("memory.search:") == 0);   // the query still ran
  TEST_ASSERT_EQUAL(0, r.persists);
  TEST_ASSERT_TRUE(std::find(r.cues.begin(), r.cues.end(), "memsaved") == r.cues.end());
  TEST_ASSERT_TRUE(r.pendingMem.find("[MEMORY] Not stored: this turn carries a sub-agent "
                                     "result whose connector was NOT called") != std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains(
      "orchestrator: mem_write refused (1 item(s)) - unverified connector result"));
  // The same turn without the flag stores as before (the rail is the flag, not the text).
  Rec r2;
  ApplyDeps d2 = makeDeps(r2);
  ApplyState st2 = makeState(r2);
  agent::applyTurn(t, "web", d2, st2, spawned);
  TEST_ASSERT_TRUE(r2.memCalls[0].find("memory.write:") == 0);
  TEST_ASSERT_EQUAL(1, r2.persists);
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

// ---- lane S: the claim check against the turn's real outcomes ---------------

static Turn replyTurn(const char* reply) {
  Turn t;
  t.reply = reply;
  return t;
}

// Apply with a claim check attached; returns the one delivered text.
static std::string applyChecked(Rec& r, ApplyDeps& d, const Turn& t, agent::ClaimCheck& cc,
                                bool unverified = false, bool* spawnedOut = nullptr) {
  ApplyState st = makeState(r);
  st.unverifiedConnector = unverified;
  st.claims = &cc;
  bool spawned = false;
  agent::applyTurn(t, "1001", d, st, spawned);
  if (spawnedOut) *spawnedOut = spawned;
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  if (!t.reply.empty()) TEST_ASSERT_EQUAL_STRING(r.delivered[0].second.c_str(), st.delivered.c_str());
  return r.delivered[0].second;
}

static void test_claims_failed_memory_write_gets_the_correction_line() {
  for (int unverified = 0; unverified < 2; unverified++) {
    Rec r;
    r.memSuccess = unverified != 0;   // case 0: the store failed; case 1: refused (CUM-536)
    ApplyDeps d = makeDeps(r);
    Turn t = replyTurn("Logged your run.");
    nimbus::orch::MemWrite w;
    w.content = "ran 5 km";
    t.mem_write.push_back(w);
    agent::ClaimCheck cc;
    cc.beginTurn();
    const std::string out = applyChecked(r, d, t, cc, unverified != 0);
    TEST_ASSERT_EQUAL_STRING("Logged your run.\n\n(Correction: that has not been saved.)", out.c_str());
    // The history keeps what the owner saw.
    TEST_ASSERT_EQUAL_STRING(out.c_str(), r.captured[0].c_str());
  }
  // Counter-case: the write stored, no line.
  Rec r;
  ApplyDeps d = makeDeps(r);
  Turn t = replyTurn("Logged your run.");
  t.mem_write.push_back(nimbus::orch::MemWrite{"ran 5 km", 0.5, false, ""});
  agent::ClaimCheck cc;
  cc.beginTurn();
  TEST_ASSERT_EQUAL_STRING("Logged your run.", applyChecked(r, d, t, cc).c_str());
}

// Lane P meets lane S: a queued write is NOT a stored one, so a "saved" claim
// over it gets the correction line - the owner is told the fact is only waiting
// in the embedding-outage queue, not remembered.
static void test_claims_queued_write_backs_no_save_claim() {
  Rec r;
  r.memCode = nimbus::orch::kMemOutcomeQueued;
  r.memSuccess = true;             // the dispatcher answered ok: the fact is queued
  ApplyDeps d = makeDeps(r);
  Turn t = replyTurn("Saved your pool locker code.");
  nimbus::orch::MemWrite w;
  w.content = "my pool locker code is 4417";
  t.mem_write.push_back(w);
  agent::ClaimCheck cc;
  cc.beginTurn();
  const std::string out = applyChecked(r, d, t, cc);
  TEST_ASSERT_EQUAL_STRING("Saved your pool locker code.\n\n(Correction: that has not been saved.)",
                           out.c_str());
  TEST_ASSERT_TRUE(r.pendingMem.find("[MEMORY] mem_write \"my pool locker code is 4417\" -> ") !=
                   std::string::npos);
}

static void test_claims_refused_enqueue_is_not_a_spawn() {
  for (int accept = 0; accept < 2; accept++) {
    Rec r;
    r.acceptSpawns = accept != 0;
    ApplyDeps d = makeDeps(r);
    Turn t = replyTurn("I have started a research sub-agent on it.");
    nimbus::orch::SessionOp op;
    op.op = "spawn";
    op.task = "find car seat reviews";
    t.session_ops.push_back(op);
    agent::ClaimCheck cc;
    cc.beginTurn();
    bool spawned = true;
    const std::string out = applyChecked(r, d, t, cc, false, &spawned);
    TEST_ASSERT_EQUAL(accept != 0, spawned);   // spawnedOut counts accepted spawns only
    TEST_ASSERT_EQUAL(!accept, out.find("did not start") != std::string::npos);
    TEST_ASSERT_EQUAL(accept ? 1 : 0, cc.effects().countOk(nimbus::orch::FxKind::Spawn));
    TEST_ASSERT_EQUAL(accept ? 0 : 1, cc.effects().countFailed(nimbus::orch::FxKind::SpawnRefused));
  }
}

static void test_claims_ram_only_scratchpad_against_a_durability_claim() {
  const char* reply = "Put it in my notes, loaded into every single turn, reboot or not.";
  for (int ramOnly = 0; ramOnly < 2; ramOnly++) {
    Rec r;
    ApplyDeps d = makeDeps(r);
    d.principalFor = [](const std::string& c) {
      return nimbus::orch::principalForRole(c, nimbus::orch::Role::Admin);   // the scratchpad is admin-only
    };
    d.applyScratch = [ramOnly](const nimbus::orch::ScratchUpdate&) {
      return ramOnly ? agent::ScratchOutcome::AppliedRamOnly : agent::ScratchOutcome::Applied;
    };
    Turn t = replyTurn(reply);
    t.scratchpad.present = true;
    t.scratchpad.hasLong = true;
    t.scratchpad.longItems = {"no em dashes"};
    agent::ClaimCheck cc;
    cc.beginTurn();
    const std::string out = applyChecked(r, d, t, cc);
    TEST_ASSERT_EQUAL_MESSAGE(ramOnly != 0, out.find("until the next restart") != std::string::npos,
                              ramOnly ? "full NVS" : "fresh device");
  }
}

// A device action that did not run backs nothing (review finding): voice
// replies off, so a tts action is a no-op and "read it aloud" gets its line.
static void test_claims_noop_device_action_backs_nothing() {
  for (int voice = 0; voice < 2; voice++) {
    Rec r;
    ApplyDeps d = makeDeps(r, /*tts=*/voice != 0);
    Turn t = turnWithDevice("{\"type\":\"tts\",\"text\":\"hello\"}");
    t.reply = "I read it aloud for you.";
    agent::ClaimCheck cc;
    cc.beginTurn();
    const std::string out = applyChecked(r, d, t, cc);
    TEST_ASSERT_EQUAL_MESSAGE(!voice, out.find("has not been sent") != std::string::npos,
                              voice ? "voice on" : "voice off");
  }
}

// The ask rides the same message: a claim there is checked too.
static void test_claims_ask_is_checked_with_the_reply() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  Turn t;
  t.ask = "Logged. Want the weekly total too?";
  agent::ClaimCheck cc;
  cc.beginTurn();
  const std::string out = applyChecked(r, d, t, cc);
  TEST_ASSERT_TRUE(out.find("(Correction: that has not been saved.)") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING(out.c_str(), r.captured[0].c_str());
}

static void test_claims_no_check_attached_is_byte_identical() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(replyTurn("Logged."), "1001", d, st, spawned);
  TEST_ASSERT_EQUAL_STRING("Logged.", r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL_STRING("Logged.", r.captured[0].c_str());
}

// Lane P: every mem_write[] outcome is reported (13-14 Oct: 8 facts lost while the
// turn said ok). Walks every outcome code the memory tools emit, plus an unknown
// one: queued and refused (and unknown) add a MEMORY RESULTS line, memsaved fires
// only for stored, the counters are exact, and a persist runs only when the store
// changed.
static void test_every_mem_write_outcome_is_reported() {
  struct Row {
    const char* code; bool success;
    bool reported; bool memsaved; bool persists; int stored, queued, refused;
    const char* words;
  };
  const Row rows[] = {
      {nimbus::orch::kMemOutcomeStored, true, false, true, true, 1, 0, 0, ""},
      {nimbus::orch::kMemOutcomeDuplicate, true, false, false, true, 0, 0, 0, ""},
      {nimbus::orch::kMemOutcomeQueued, true, true, false, false, 0, 1, 0, "-> ok-output"},
      {nimbus::orch::kMemOutcomeRefused, false, true, false, false, 0, 0, 1, "-> NOT stored: err-output"},
      // A dispatcher with no codes: success is a store, a failure is reported.
      {"", true, false, true, true, 1, 0, 0, ""},
      {"", false, true, false, false, 0, 0, 1, "-> NOT stored: err-output"},
      // A code this build does not know is never counted as saved.
      {"brand_new_code", true, true, false, false, 0, 0, 1, "-> NOT stored: ok-output"},
  };
  for (const Row& row : rows) {
    Rec r;
    r.memCode = row.code;
    r.memSuccess = row.success;
    ApplyDeps d = makeDeps(r);
    ApplyState st = makeState(r);
    bool spawned = false;
    Turn t;
    nimbus::orch::MemWrite w;
    w.content = "my pool locker code is 4417";
    t.mem_write.push_back(w);
    t.reply = "I'll remember that";
    agent::applyTurn(t, "1001", d, st, spawned);
    const bool reported = r.pendingMem.find("[MEMORY] mem_write \"my pool locker code is 4417\"") !=
                          std::string::npos;
    TEST_ASSERT_EQUAL_MESSAGE(row.reported, reported, row.code);
    if (row.reported) TEST_ASSERT_TRUE_MESSAGE(r.pendingMem.find(row.words) != std::string::npos, row.code);
    const bool saved = std::find(r.cues.begin(), r.cues.end(), "memsaved") != r.cues.end();
    TEST_ASSERT_EQUAL_MESSAGE(row.memsaved, saved, row.code);
    TEST_ASSERT_EQUAL_MESSAGE(row.persists ? 1 : 0, r.persists, row.code);
    TEST_ASSERT_EQUAL_MESSAGE(row.stored, st.memStored, row.code);
    TEST_ASSERT_EQUAL_MESSAGE(row.queued, st.memQueued, row.code);
    TEST_ASSERT_EQUAL_MESSAGE(row.refused, st.memRefused, row.code);
  }
  // A host with no memory store reports every write as not stored (never a
  // silent drop: the hosted daemon shipped without the dispatcher bound).
  {
    Rec r0;
    ApplyDeps d0 = makeDeps(r0);
    d0.memDispatch = nullptr;
    ApplyState st0 = makeState(r0);
    bool sp0 = false;
    Turn t0;
    nimbus::orch::MemWrite w0;
    w0.content = "my pool locker code is 4417";
    t0.mem_write.push_back(w0);
    t0.reply = "I'll remember that";
    agent::applyTurn(t0, "1001", d0, st0, sp0);
    TEST_ASSERT_TRUE(r0.pendingMem.find("-> NOT stored: this host has no long-term memory store") !=
                     std::string::npos);
    TEST_ASSERT_EQUAL(1, st0.memRefused);
    TEST_ASSERT_EQUAL(0, r0.persists);
  }
  // The excerpt is bounded and never splits a UTF-8 character.
  Rec r;
  r.memCode = nimbus::orch::kMemOutcomeQueued;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  nimbus::orch::MemWrite w;
  w.content = std::string(79, 'a') + "\xC3\xA9" + std::string(50, 'b');
  t.mem_write.push_back(w);
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_TRUE(r.pendingMem.find(std::string(79, 'a') + "...") != std::string::npos);
  TEST_ASSERT_TRUE(r.pendingMem.find(std::string(50, 'b')) == std::string::npos);
}


// ---- ASK-DUPLICATE (Lumi 22 Oct, m0000127a) -----------------------------------

static std::pair<Rec, ApplyState> applyReplyAsk(const std::string& reply, const std::string& ask) {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t;
  t.reply = reply;
  t.ask = ask;
  agent::applyTurn(t, "1001", d, st, spawned);
  return {r, st};
}

// The 22 Oct incident pair (wording scrubbed): the head ended its reply with a
// question about the calendar check and wrote the same question, reworded, into ask,
// so the owner got it twice. Now one message, the needs-you state still fires, and
// the history keeps only what was sent.
static void test_ask_duplicate_the_incident_pair_is_dropped() {
  const std::string reply =
      "Your commute looks clear and the bus runs on time. Should I check your calendar for "
      "anything that clashes with the 8:15 bus?\n";
  const std::string ask = "Do you want me to check the calendar for clashes with tomorrow's 8:15 bus?";
  auto out = applyReplyAsk(reply, ask);
  Rec& r = out.first;
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING(reply.c_str(), r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL(1, r.emitAsks);                       // needs-you still fires
  TEST_ASSERT_EQUAL(1, (int)r.captured.size());           // only what was sent
  TEST_ASSERT_EQUAL_STRING(reply.c_str(), r.captured[0].c_str());
}

// Orchestrator ruling: a reply ending with a rhetorical question plus a different,
// real question keeps the question - it is appended after a blank line and rides the
// reply's history row.
static void test_ask_duplicate_a_different_question_after_a_rhetorical_one_is_kept() {
  const std::string reply = "Lunch is logged at 640 kcal. Not bad for a Monday, right?";
  const std::string ask = "Should I book the dentist appointment for Thursday afternoon?";
  auto out = applyReplyAsk(reply, ask);
  Rec& r = out.first;
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_EQUAL_STRING((reply + "\n\n" + ask).c_str(), r.delivered[0].second.c_str());
  TEST_ASSERT_EQUAL(1, r.emitAsks);
  TEST_ASSERT_EQUAL(1, (int)r.captured.size());           // one row, as one message
  TEST_ASSERT_EQUAL_STRING((reply + "\n\n" + ask).c_str(), r.captured[0].c_str());
}

// Lane FSN's finding (storm leg g): the owner got one message but the history got two
// rows (reply, then ask), so a reload of the web chat and the storm leg saw two
// messages for one turn. Now one row per message the owner received. The row is the
// delivered text minus the risk note (the note is a device warning, not something the
// head said); the corrections still ride it.
static void test_fsn_reply_plus_different_question_is_one_row() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t = turnWithDevice("{\"config\":{\"brightOvr\":true}}");
  t.reply = "LED cap lifted for the photo shoot. Bright enough now?";
  t.ask = "Should I put the cap back at 22:00 tonight?";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  const std::string& sent = r.delivered[0].second;
  const std::string note = "\n\u26a0 LED cap OVERRIDDEN to 100% - sustained heat risk to the panel "
                           "(resets at reboot)";
  TEST_ASSERT_TRUE(sent.find(note) != std::string::npos);     // the note was sent
  TEST_ASSERT_EQUAL(1, r.emitAsks);
  TEST_ASSERT_EQUAL(1, (int)r.captured.size());               // one row, not two
  std::string minusNote = sent;
  minusNote.erase(minusNote.find(note), note.size());
  TEST_ASSERT_EQUAL_STRING(minusNote.c_str(), r.captured[0].c_str());
  TEST_ASSERT_EQUAL_STRING((t.reply + "\n\n" + t.ask).c_str(), r.captured[0].c_str());
}

// Counter-case: the reply already asks the same thing, so the ask was never sent and
// the one row is the reply only (no copy of the ask, no risk note).
static void test_fsn_dropped_duplicate_is_one_row_with_the_reply_only() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  ApplyState st = makeState(r);
  bool spawned = false;
  Turn t = turnWithDevice("{\"config\":{\"brightOvr\":true}}");
  t.reply = "LED cap lifted. Should I put the cap back at 22:00 tonight?";
  t.ask = "Do you want the cap put back at 22:00?";
  agent::applyTurn(t, "1001", d, st, spawned);
  TEST_ASSERT_EQUAL(1, (int)r.delivered.size());
  TEST_ASSERT_TRUE(r.delivered[0].second.find("Do you want") == std::string::npos);
  TEST_ASSERT_EQUAL(1, r.emitAsks);
  TEST_ASSERT_EQUAL(1, (int)r.captured.size());
  TEST_ASSERT_EQUAL_STRING(t.reply.c_str(), r.captured[0].c_str());
}

// The class: the same question reworded (half its topic words, or all its key nouns,
// in the reply's final sentence) is dropped; a different question, a question earlier
// in the reply, or a reply that does not end with '?' keeps the ask.
static void test_ask_duplicate_same_question_dropped_different_kept() {
  struct Case { const char* reply; const char* ask; bool dropped; };
  const Case cases[] = {
      // reworded copies
      {"Lunch is logged at 640 kcal. Want me to add the walk too?\n", "Should I log the walk as well?", true},
      {"Here's the status. Want details?", "Want details?", true},                    // verbatim
      {"Here's the status. Want the details? ", "Would you like the details?", true},
      {"Done. Should I set a reminder for the dentist?", "Shall I?", true},          // no topic of its own
      {"The report is saved. Do you want the PDF sent to Telegram?",
       "Should I send the PDF to Telegram now?", true},
      {"Ready. Want the weekly totals for protein and calories?",
       "Do you want weekly calorie and protein totals?", true},
      // different questions
      {"Lunch is logged. Not bad for a Monday, right?", "Should I book the dentist for Thursday?", false},
      {"The bus is late again, can you believe it?", "Do you want the weekly spend report?", false},
      {"Want details? Here's the status.", "Want details?", false},                  // question earlier
      {"Here's the status.", "Want details?", false},                                 // no question
      {"Saved the recipe. Anything else for dinner?", "Should I order the groceries for Saturday?", false},
  };
  for (const Case& c : cases) {
    TEST_ASSERT_EQUAL_MESSAGE(c.dropped, agent::replyAlreadyAsks(c.reply, c.ask), c.reply);
    auto out = applyReplyAsk(c.reply, c.ask);
    Rec& r = out.first;
    const std::string want = c.dropped ? std::string(c.reply) : std::string(c.reply) + "\n\n" + c.ask;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.delivered[0].second.c_str(), c.reply);
    TEST_ASSERT_EQUAL_MESSAGE(1, r.emitAsks, c.reply);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.captured.size(), c.reply);    // one row per message
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.captured[0].c_str(), c.reply);
  }
  // Either field empty: nothing is "already asked".
  TEST_ASSERT_FALSE(agent::replyAlreadyAsks("", "Want details?"));
  TEST_ASSERT_FALSE(agent::replyAlreadyAsks("Ok?", ""));
}

// ---- orch_model: the provider enum, the miss answer, router ids (R5, L19-39) ----

// Every registry slot is a provider orch_model can switch to (the schema enum
// gained cumulo and zai); anything else is refused before the list is read.
static void test_orch_model_every_provider_slot_applies_and_nothing_else() {
  for (const nimbus::orch::ProviderSlot& slot : nimbus::orch::kProviderSlots) {
    Rec r;
    ApplyDeps d = makeDeps(r);
    ApplyState st = makeState(r);
    bool spawned = false;
    const std::string json = std::string("{\"type\":\"orch_model\",\"provider\":\"") + slot.slug +
                             "\",\"model\":\"good-x\"}";
    agent::applyTurn(turnWithDevice(json.c_str()), "1001", d, st, spawned);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)r.hostModelSet.size(), slot.slug);
    TEST_ASSERT_EQUAL_STRING(slot.slug, r.hostModelSet[0].first.c_str());
  }
  for (const char* p : {"custom", "google", "", "OPENAI"}) {
    Rec r;
    ApplyDeps d = makeDeps(r);
    const std::string json =
        std::string("{\"type\":\"orch_model\",\"provider\":\"") + p + "\",\"model\":\"good-x\"}";
    const std::string sum = agent::applyDeviceElement(json, false, "1001", d, nullptr);
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.hostModelSet.size(), p);
    if (*p) TEST_ASSERT_TRUE_MESSAGE(sum.find("is not a provider") != std::string::npos, sum.c_str());
  }
}

// The miss answer rides the mid-turn device.control result and the next turn's
// [MEMORY RESULTS]; N misses within an hour queue exactly one refresh per provider.
static void test_orch_model_misses_answer_and_queue_one_refresh_per_provider_an_hour() {
  nimbus::orch::ModelRefreshQueue q;
  uint32_t now = 1000;
  const uint32_t fetched = 1792454400;   // 2026-10-20
  Rec r;
  ApplyDeps d = makeDeps(r);
  std::map<std::string, int> queued;      // provider -> requests that queued a refresh
  int asks = 0;
  d.modelMiss = [&](const std::string& p, const std::string&) {
    asks++;
    const nimbus::orch::RefreshAsk a = q.request(p.c_str(), true, now);
    if (a == nimbus::orch::RefreshAsk::Queued) queued[p]++;
    return nimbus::orch::modelMissText(p, fetched, a);
  };
  const char* provs[] = {"anthropic", "openai"};
  int starts = 0;
  for (int i = 0; i < 6; ++i) {
    for (const char* p : provs) {
      now += 60000;   // a minute apart: all inside the hour
      const std::string json = std::string("{\"type\":\"orch_model\",\"provider\":\"") + p +
                               "\",\"model\":\"claude-sonnet-5.5\"}";
      const std::string sum = agent::applyDeviceElement(json, false, "1001", d, nullptr);
      TEST_ASSERT_TRUE_MESSAGE(sum.rfind("orch_model(invalid) ", 0) == 0, sum.c_str());
      const std::string want = std::string("not in ") + p + " list cached 2026-10-20; ";
      TEST_ASSERT_TRUE_MESSAGE(sum.find(want) != std::string::npos, sum.c_str());
      TEST_ASSERT_TRUE_MESSAGE(sum.find(i < 3 ? "refresh queued" : "refreshed within the hour") !=
                                   std::string::npos,
                               sum.c_str());
    }
    if (i == 2) {   // the main loop starts the queued refreshes between turns, one at a time
      while (const char* due = q.due(false, true, false, true)) {
        q.started(due, now);
        starts++;
      }
    }
  }
  TEST_ASSERT_EQUAL(12, asks);
  TEST_ASSERT_EQUAL(2, starts);                 // one refresh per provider in the hour
  TEST_ASSERT_EQUAL(1, queued["anthropic"]);
  TEST_ASSERT_EQUAL(1, queued["openai"]);
  TEST_ASSERT_TRUE(q.due(false, true, false, true) == nullptr);
  TEST_ASSERT_EQUAL(0, (int)r.hostModelSet.size());
  // The end-of-turn device[] path tells the head next turn.
  ApplyState st = makeState(r);
  bool spawned = false;
  agent::applyTurn(turnWithDevice("{\"type\":\"orch_model\",\"provider\":\"anthropic\","
                                  "\"model\":\"claude-sonnet-5.5\"}"),
                   "1001", d, st, spawned);
  TEST_ASSERT_TRUE_MESSAGE(r.pendingMem.find("[DEVICE] orch_model not applied: claude-sonnet-5.5 on anthropic: "
                                             "not in anthropic list cached 2026-10-20") != std::string::npos,
                           r.pendingMem.c_str());
  TEST_ASSERT_TRUE(r.pendingMem.find("models.list") != std::string::npos);
}

// L19-39: a router id named on a direct provider is refused as such (no refresh
// can fix it), naming the bare id when that provider lists it. On cumulo the same
// id applies.
static void test_l19_39_orch_model_router_id_on_a_direct_provider_is_refused() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  int misses = 0;
  d.modelMiss = [&misses](const std::string& p, const std::string&) {
    misses++;
    return "not in " + p + " list";
  };
  d.modelIsValid = [](const std::string& p, const std::string& m) {
    if (p == "cumulo") return m == "openai/gpt-5.6-sol";
    return m == "gpt-5.6-sol";
  };
  std::string sum = agent::applyDeviceElement(
      "{\"type\":\"orch_model\",\"provider\":\"openai\",\"model\":\"openai/gpt-5.6-sol\"}", false, "1001",
      d, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(sum.find("not in openai list: use gpt-5.6-sol (openai/gpt-5.6-sol is a cumulo "
                                    "router id)") != std::string::npos,
                           sum.c_str());
  sum = agent::applyDeviceElement(
      "{\"type\":\"orch_model\",\"provider\":\"anthropic\",\"model\":\"anthropic/claude-opus-5-5\"}",
      false, "1001", d, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(sum.find("not in anthropic list: anthropic/claude-opus-5-5 is a cumulo router id; "
                                    "pick one of anthropic's ids from models.list") != std::string::npos,
                           sum.c_str());
  TEST_ASSERT_EQUAL(0, misses);   // no refresh asked for a category error
  TEST_ASSERT_EQUAL(0, (int)r.hostModelSet.size());
  // Counter-test: the router takes its own ids.
  sum = agent::applyDeviceElement(
      "{\"type\":\"orch_model\",\"provider\":\"cumulo\",\"model\":\"openai/gpt-5.6-sol\"}", false, "1001",
      d, nullptr);
  TEST_ASSERT_EQUAL_STRING("orch_model(cumulo/openai/gpt-5.6-sol) ", sum.c_str());
  TEST_ASSERT_EQUAL(1, (int)r.hostModelSet.size());
}

// L19-11 (CUM-664): a mem_write that records an unbacked cause as a fact is
// refused at apply time (19 Oct: "Roy's Telegram client had a stuck outbox" was
// stopped only by the embedding outage). The write never reaches the store, the
// reason is fed back for the next turn, and the ledger's failed-write effect
// still corrects a "saved" reply. Counter-test: with the cause on record this
// turn, the same write stores.
static void test_mem_write_of_an_unbacked_cause_is_refused() {
  Rec r;
  ApplyDeps d = makeDeps(r);
  agent::ClaimCheck cc;
  cc.beginTurn();   // no records this turn: no cause evidence at all
  ApplyState st = makeState(r);
  st.claims = &cc;
  bool spawned = false;
  Turn t;
  nimbus::orch::MemWrite w;
  w.content = "Roy's Telegram client had a stuck outbox, so the photos arrived corrupt";
  t.mem_write.push_back(w);
  t.reply = "Noted.";
  agent::applyTurn(t, "web", d, st, spawned);
  TEST_ASSERT_EQUAL(0, (int)r.memCalls.size());   // the write never reached the store
  TEST_ASSERT_EQUAL(0, r.persists);
  TEST_ASSERT_TRUE(r.pendingMem.find(
                       "NOT stored: it states a cause this turn's records do not show") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(LogCapture::contains("unbacked cause"));
  // A "saved" reply is still corrected: the refusal left a failed write effect.
  TEST_ASSERT_TRUE(cc.tailAfterApply("Saved.").find("(Correction: that has not been saved.)") !=
                   std::string::npos);
  // Counter-test: with the cause on record this turn, the same write stores.
  Rec r2;
  ApplyDeps d2 = makeDeps(r2);
  agent::ClaimCheck cc2;
  cc2.beginTurn();
  cc2.setSources("[tg] client error: the owner's Telegram client rejected the upload\n"
                 "the photos arrived corrupt (partial upload)\n", "");
  ApplyState st2 = makeState(r2);
  st2.claims = &cc2;
  agent::applyTurn(t, "web", d2, st2, spawned);
  TEST_ASSERT_EQUAL(1, (int)r2.memCalls.size());
  TEST_ASSERT_TRUE(r2.memCalls[0].find("memory.write:") == 0);
  TEST_ASSERT_EQUAL(1, r2.persists);
}


// 23 Oct (lane H follow-up 02, CUM-652): a config action that changed nothing is
// never "done". The summary reports "cfg(no change)", the ledger records a
// failed config effect, and a "switched" reply gets the correction line (the
// live shape: an invented config field, nothing switched, the reply said it
// had). Counter-test: a real change backs the same reply.
static void test_a_no_change_config_action_backs_no_switched_reply() {
  Rec r;
  r.configChanges = false;
  ApplyDeps d = makeDeps(r);
  agent::ClaimCheck cc;
  cc.beginTurn();
  Turn t = turnWithDevice("{\"config\":{\"ledBrightness\":40}}");
  t.reply = "I've switched the brightness to 40 for you.";
  const std::string out = applyChecked(r, d, t, cc);
  TEST_ASSERT_TRUE(LogCapture::contains("cfg(no change)"));
  TEST_ASSERT_TRUE(out.find("(Correction: nothing was changed this turn.)") != std::string::npos);
  // A stored value that really changed backs the same reply.
  Rec r2;
  ApplyDeps d2 = makeDeps(r2);
  agent::ClaimCheck cc2;
  cc2.beginTurn();
  const std::string out2 = applyChecked(r2, d2, t, cc2);
  TEST_ASSERT_EQUAL(0, (int)out2.find("I've switched the brightness to 40 for you."));
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
  RUN_TEST(test_orch_model_id_in_the_store_applies);
  RUN_TEST(test_reply_and_ask_deliver_as_one_message);
  RUN_TEST(test_lane_l_an_ask_the_reply_already_ends_with_is_not_repeated);
  RUN_TEST(test_mem_write_is_stored_before_the_reply_is_delivered);
  RUN_TEST(test_tell_poll_nudge_does_not_mention_await);
  RUN_TEST(test_empty_turn_signals_completion_not_done);
  RUN_TEST(test_completion_suppressed_after_spawn);
  RUN_TEST(test_spawn_quiet_flag_tracks_scheduled_turn);
  RUN_TEST(test_done_suppressed_after_spawn_or_tool_reply);
  RUN_TEST(test_session_ops_spawn_terminate_tell);
  RUN_TEST(test_await_aims_round_robin);
  RUN_TEST(test_mem_write_and_query_dispatch_under_lock);
  RUN_TEST(test_unverified_connector_turn_refuses_mem_write);
  RUN_TEST(test_mem_query_only_does_not_persist);
  RUN_TEST(test_claims_failed_memory_write_gets_the_correction_line);
  RUN_TEST(test_claims_queued_write_backs_no_save_claim);
  RUN_TEST(test_claims_refused_enqueue_is_not_a_spawn);
  RUN_TEST(test_claims_ram_only_scratchpad_against_a_durability_claim);
  RUN_TEST(test_claims_noop_device_action_backs_nothing);
  RUN_TEST(test_claims_ask_is_checked_with_the_reply);
  RUN_TEST(test_claims_no_check_attached_is_byte_identical);
  RUN_TEST(test_every_mem_write_outcome_is_reported);
  RUN_TEST(test_ask_duplicate_the_incident_pair_is_dropped);
  RUN_TEST(test_ask_duplicate_a_different_question_after_a_rhetorical_one_is_kept);
  RUN_TEST(test_ask_duplicate_same_question_dropped_different_kept);
  RUN_TEST(test_fsn_reply_plus_different_question_is_one_row);
  RUN_TEST(test_fsn_dropped_duplicate_is_one_row_with_the_reply_only);
  RUN_TEST(test_orch_model_every_provider_slot_applies_and_nothing_else);
  RUN_TEST(test_orch_model_misses_answer_and_queue_one_refresh_per_provider_an_hour);
  RUN_TEST(test_l19_39_orch_model_router_id_on_a_direct_provider_is_refused);
  RUN_TEST(test_mem_write_of_an_unbacked_cause_is_refused);
  RUN_TEST(test_a_no_change_config_action_backs_no_switched_reply);
  UNITY_END();
  return 0;
}
