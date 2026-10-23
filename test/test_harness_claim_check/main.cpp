// test_harness_claim_check - the engine's claim check: the final-answer check
// (one correction round, with the turn's own fields as pending effects) and the
// owner tail after apply, against the turn's real outcomes.
#include <unity.h>

#include <string>
#include <vector>

#include "../support/mock_claim_turns.h"
#include "nimbus/harness/apply.h"
#include "nimbus/harness/claim_check.h"
#include "nimbus/orch/head_loop.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/spawn_claim.h"
#include "nimbus/orch/tool_taint.h"

using agent::ClaimCheck;
using nimbus::orch::FxKind;
using nimbus::orch::HeadLoopConfig;
using nimbus::orch::HeadLoopHooks;
using nimbus::orch::HeadOutcome;
using nimbus::orch::HeadStep;
using nimbus::orch::HeadToolCall;
using nimbus::orch::HeadToolResult;

void setUp() {}
void tearDown() {}

static std::string turnJson(const std::string& reply, const std::string& extra = "") {
  std::string j = "{\"reply\":\"" + reply + "\"";
  if (!extra.empty()) j += "," + extra;
  return j + "}";
}

// Counter-test: a reply consistent with what the turn did costs zero extra rounds.
static void test_consistent_replies_need_no_correction() {
  struct Case { const char* reply; const char* fields; FxKind tool; bool useTool; };
  const Case cases[] = {
      {"Logged.", "\"mem_write\":[{\"content\":\"ran 5 km\",\"importance\":0.6}]", FxKind::Read, false},
      {"Noted in my notes.", "\"scratchpad\":{\"active\":\"plan\",\"short\":null,\"mid\":null,\"long\":null}",
       FxKind::Read, false},
      {"I have started two research sub-agents.",
       "\"session_ops\":[{\"op\":\"spawn\",\"task\":\"a\"},{\"op\":\"spawn\",\"task\":\"b\"}]", FxKind::Read, false},
      {"Reading it aloud now.", "\"device\":[{\"type\":\"tts\",\"text\":\"hi\"}]", FxKind::Read, false},
      {"The plan is saved as plan.md.", "", FxKind::FileWrite, true},
      {"I checked the timetable.", "", FxKind::Read, true},
      {"Here is the plan you asked for.", "", FxKind::Read, false},
  };
  for (const Case& c : cases) {
    ClaimCheck cc;
    cc.beginTurn();
    if (c.useTool) cc.noteTool(c.tool, false, c.tool == FxKind::FileWrite ? "saved plan.md (1 KB)" : "ok");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", cc.finalCheck(turnJson(c.reply, c.fields)).c_str(), c.reply);
    TEST_ASSERT_FALSE(cc.corrected());
  }
}

static void test_unbacked_claim_gets_one_correction_with_the_record() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.noteTool(FxKind::MemWrite, false, "stored memory mb2accd88");
  const uint32_t before = ClaimCheck::correctedCount();
  const std::string fix =
      cc.finalCheck(turnJson("Run logged. The strength block is written and waiting whenever you want it."));
  TEST_ASSERT_EQUAL(0, (int)fix.find("[CLAIM CHECK]"));
  TEST_ASSERT_TRUE(fix.find("written and waiting") != std::string::npos);
  TEST_ASSERT_TRUE(fix.find("mb2accd88") != std::string::npos);
  TEST_ASSERT_TRUE(cc.corrected());
  TEST_ASSERT_EQUAL(before + 1, ClaimCheck::correctedCount());
  // A turn that cannot be read is the shape check's case, not this one's.
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck("{not json").c_str());
}

static void test_tail_after_apply_uses_the_real_outcomes() {
  // The final check saw a mem_write item (pending), so "Logged" passed; apply
  // then recorded the write as refused: the owner gets the correction line.
  ClaimCheck cc;
  cc.beginTurn();
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("Logged.", "\"mem_write\":[{\"content\":\"x\"}]")).c_str());
  cc.effects().add(FxKind::MemWrite, false);
  const uint32_t before = ClaimCheck::tailedCount();
  const std::string tail = cc.tailAfterApply("Logged.");
  TEST_ASSERT_EQUAL_STRING("\n\n(Correction: that has not been saved.)", tail.c_str());
  TEST_ASSERT_EQUAL(before + 1, ClaimCheck::tailedCount());
  // The same reply with the write stored: no tail.
  ClaimCheck ok;
  ok.beginTurn();
  ok.effects().add(FxKind::MemWrite, true, "m1");
  TEST_ASSERT_EQUAL_STRING("", ok.tailAfterApply("Logged.").c_str());
  TEST_ASSERT_EQUAL_STRING("", ok.tailAfterApply("  ").c_str());
}

static void test_scratchpad_durability_fresh_device_and_full_nvs() {
  const char* reply = "Put it in my notes, which are loaded into every single turn, reboot or not.";
  ClaimCheck fresh;   // fresh device: the save landed
  fresh.beginTurn();
  fresh.effects().add(FxKind::Scratch, true);
  TEST_ASSERT_EQUAL_STRING("", fresh.tailAfterApply(reply).c_str());
  ClaimCheck full;    // full NVS: kept in RAM only
  full.beginTurn();
  full.effects().add(FxKind::ScratchRamOnly, true);
  TEST_ASSERT_TRUE(full.tailAfterApply(reply).find("until the next restart") != std::string::npos);
}

static void test_hosted_tools_leave_sent_checked_and_changed_unchecked() {
  for (int hosted = 0; hosted < 2; hosted++) {
    for (const char* reply : {"I sent the email to Sam.", "I checked your calendar.", "I updated the event."}) {
      ClaimCheck cc;
      cc.beginTurn();
      cc.setHostedTools(hosted != 0);
      const bool flagged = !cc.finalCheck(turnJson(reply)).empty();
      TEST_ASSERT_EQUAL_MESSAGE(!hosted, flagged, reply);
    }
    // Device-side claims are checked either way.
    ClaimCheck cc;
    cc.beginTurn();
    cc.setHostedTools(hosted != 0);
    TEST_ASSERT_FALSE(cc.finalCheck(turnJson("Logged.")).empty());
  }
}

static void test_skip_spawned_leaves_the_spawn_class_to_the_shape_check() {
  ClaimCheck cc;
  cc.beginTurn();
  TEST_ASSERT_FALSE(cc.finalCheck(turnJson("Six sub-agents started this turn.")).empty());
  ClaimCheck skip;
  skip.beginTurn();
  TEST_ASSERT_EQUAL_STRING("", skip.finalCheck(turnJson("Six sub-agents started this turn."), true).c_str());
}

static void test_unsourced_fresh_result_needs_a_hedge() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.noteFresh(0, 1);
  TEST_ASSERT_FALSE(cc.finalCheck(turnJson("Central line: no disruptions reported.")).empty());
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("The line status was not checked live.")).c_str());
  TEST_ASSERT_TRUE(cc.tailAfterApply("Central line: no disruptions reported.").find("not checked live") !=
                   std::string::npos);
  ClaimCheck src;
  src.beginTurn();
  src.noteFresh(1, 0);
  TEST_ASSERT_EQUAL_STRING("", src.finalCheck(turnJson("Central line: no disruptions reported.")).c_str());
}

static void test_figures_against_the_turn_sources() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.setSources("[USER]\n[image] the label reads 20 grams of protein, 3 grams of net carbs", "");
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("| Bar (label) | 20 g protein | 3 g carbs |")).c_str());
  const std::string fix = cc.finalCheck(turnJson("| Bar (label) | 60 g | 200 | 21 g |"));
  TEST_ASSERT_TRUE(fix.find("60, 200, 21") != std::string::npos);
  // A tool result is a source too.
  cc.noteTool(FxKind::Read, false, "label lookup: 200 kcal, 60 g bar, 21 g protein");
  TEST_ASSERT_EQUAL_STRING("", cc.finalCheck(turnJson("| Bar (label) | 60 g | 200 | 21 g |")).c_str());
}

static void test_device_actions_back_their_claims_only_when_allowed() {
  FxKind k;
  TEST_ASSERT_TRUE(ClaimCheck::deviceActionEffect("{\"type\":\"tts\",\"text\":\"hi\"}", k));
  TEST_ASSERT_EQUAL((int)FxKind::Speak, (int)k);
  TEST_ASSERT_TRUE(ClaimCheck::deviceActionEffect("{\"type\":\"lights\",\"value\":\"off\"}", k));
  TEST_ASSERT_EQUAL((int)FxKind::Config, (int)k);
  TEST_ASSERT_FALSE(ClaimCheck::deviceActionEffect("{\"type\":\"nope\"}", k));
  TEST_ASSERT_FALSE(ClaimCheck::deviceActionEffect("not json", k));
}

// Live-eval shapes: a scratchpad save that landed backs "saved"; a RAM-only one
// does not (but still backs "noted"); a memory written with permanent:true
// through the tool backs "pinned" and "never decays".
static void test_scratch_outcomes_and_permanent_tool_writes() {
  const std::string saved = turnJson("Noted, it's saved on the device.");
  ClaimCheck ok;
  ok.beginTurn();
  ok.noteTool(FxKind::Scratch, false, "added to short");
  TEST_ASSERT_EQUAL_STRING("", ok.finalCheck(saved).c_str());
  ClaimCheck ram;
  ram.beginTurn();
  ram.noteTool(FxKind::Scratch, false, "added to short; kept until restart only (saving failed)");
  TEST_ASSERT_FALSE(ram.finalCheck(saved).empty());
  TEST_ASSERT_EQUAL_STRING("", ram.finalCheck(turnJson("Noted.")).c_str());
  TEST_ASSERT_EQUAL((int)nimbus::orch::kFxPermanent,
                    (int)ClaimCheck::toolCallFlags("memory.write", R"({"content":"x","permanent":true})"));
  TEST_ASSERT_EQUAL(0, (int)ClaimCheck::toolCallFlags("memory.write", R"({"content":"x"})"));
  TEST_ASSERT_EQUAL(0, (int)ClaimCheck::toolCallFlags("files.read", R"({"permanent":true})"));
  ClaimCheck pin;
  pin.beginTurn();
  pin.noteTool(FxKind::MemWrite, false, "stored memory m52ff9c86",
               ClaimCheck::toolCallFlags("memory.write", R"({"content":"x","permanent":true})"));
  TEST_ASSERT_EQUAL_STRING("", pin.finalCheck(turnJson("Saved as a permanent note. It is pinned, so it can never decay.")).c_str());
}

static void test_begin_turn_clears_everything() {
  ClaimCheck cc;
  cc.beginTurn();
  cc.noteTool(FxKind::MemWrite, false, "stored memory m1");
  cc.setHostedTools(true);
  cc.finalCheck(turnJson("I sent it, and the block is written and waiting."));
  TEST_ASSERT_TRUE(cc.corrected());
  cc.beginTurn();
  TEST_ASSERT_TRUE(cc.effects().empty());
  TEST_ASSERT_FALSE(cc.corrected());
  TEST_ASSERT_FALSE(cc.finalCheck(turnJson("I sent it.")).empty());   // hosted flag reset
}

// Every registered tool's ledger kind comes from the shared tool table, per
// effect (written out here independently of the lookup, so a reordered table
// fails). Unknown names read as a lookup.
static FxKind expectedKind(nimbus::orch::ToolEffect e) {
  using E = nimbus::orch::ToolEffect;
  struct Pair { E effect; FxKind kind; };
  static const Pair kPairs[] = {
      {E::MemWrite, FxKind::MemWrite}, {E::MemUpdate, FxKind::MemUpdate}, {E::MemPin, FxKind::MemPin},
      {E::MemDelete, FxKind::MemDelete}, {E::Scratch, FxKind::Scratch}, {E::FileWrite, FxKind::FileWrite},
      {E::SkillSave, FxKind::SkillSave}, {E::Send, FxKind::Send}, {E::Share, FxKind::Send},
      {E::Speak, FxKind::Speak}, {E::Schedule, FxKind::Schedule}, {E::Config, FxKind::Config},
      {E::Cancel, FxKind::Config}, {E::Admin, FxKind::Config}, {E::Media, FxKind::Config},
  };
  for (const Pair& p : kPairs)
    if (p.effect == e) return p.kind;
  return FxKind::Read;   // Read, and Spawn (apply records the real outcome)
}

static void test_every_tool_kind_comes_from_the_shared_table() {
  size_t n = 0;
  const nimbus::orch::ToolTaintRow* rows = nimbus::orch::toolTaintTable(n);
  TEST_ASSERT_GREATER_THAN(10, (int)n);
  for (size_t i = 0; i < n; i++)
    TEST_ASSERT_EQUAL_MESSAGE((int)expectedKind(rows[i].effect), (int)agent::fxKindForTool(rows[i].name),
                              rows[i].name);
  TEST_ASSERT_EQUAL((int)FxKind::Read, (int)agent::fxKindForTool("notion.search_pages"));
}

// ---- the one correction round, on the real head loop --------------------------
// The engine's wiring in miniature: every dispatched tool joins the ledger (the
// engine's dispatch wrapper), and validateFinal is lane L's chain with the claim
// check in it (the turn-shape check is a stub here; it has its own suites).

struct Round {
  ClaimCheck cc;
  std::vector<HeadStep> script;
  size_t idx = 0;
  std::vector<std::vector<HeadToolResult>> prior;   // what each model call was handed
  int shapeCalls = 0;

  HeadOutcome run(int maxRounds = 12) {
    cc.beginTurn();
    HeadLoopConfig cfg;
    cfg.maxRounds = maxRounds;
    cfg.validateFinal = nimbus::orch::chainFinalChecks({
        [this](const std::string&) { shapeCalls++; return std::string(); },
        [this](const std::string& ft) { return cc.finalCheck(ft, /*skipSpawned=*/true); },
    });
    HeadLoopHooks h;
    h.step = [this](bool, const std::vector<HeadToolResult>& p, uint32_t, const std::string&) {
      prior.push_back(p);
      if (idx >= script.size()) {
        HeadStep s;
        s.finished = true;
        s.finalTurn = "{\"reply\":\"fallback\"}";
        return s;
      }
      return script[idx++];
    };
    h.dispatch = [this](const HeadToolCall& c) {
      HeadToolResult r;
      r.id = c.id;
      r.name = c.name;
      r.output = c.name == "artifact.save" ? "saved personal/packing.md (120 bytes)" : "ok";
      cc.noteTool(agent::fxKindForTool(c.name), r.isError, r.output);
      return r;
    };
    h.nowMs = [] { return (uint32_t)0; };
    h.freeHeap = [] { return (uint32_t)100000; };
    h.largestBlock = [] { return (uint32_t)100000; };
    return nimbus::orch::runHeadLoop(cfg, h);
  }
};

static HeadStep finalStep(const std::string& turn) {
  HeadStep s;
  s.finished = true;
  s.finalTurn = turn;
  return s;
}
static HeadStep toolStep(const std::string& name) {
  HeadStep s;
  s.toolCalls.push_back(HeadToolCall{"call_" + name, name, "{}"});
  return s;
}

static const char* kUnbacked = "The packing list is saved to your files as packing.md.";

// An unbacked claim runs exactly ONE extra round, which carries the claim
// correction; a model that answers the same way again gets no third round, and
// the after-apply check adds the owner line. claimsCorrected counts the round.
static void test_round_unbacked_twice_gets_one_round_then_the_line() {
  Round r;
  r.script = {finalStep(turnJson(kUnbacked)), finalStep(turnJson(kUnbacked)), finalStep(turnJson("third"))};
  const uint32_t before = ClaimCheck::correctedCount();
  const HeadOutcome o = r.run();
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(1, o.corrections);
  TEST_ASSERT_EQUAL(2u, r.idx);   // two model calls, never a third
  TEST_ASSERT_EQUAL(before + 1, ClaimCheck::correctedCount());
  TEST_ASSERT_EQUAL(1u, r.prior[1].size());
  TEST_ASSERT_EQUAL_STRING(nimbus::orch::kFinalCheckCallId, r.prior[1][0].id.c_str());
  TEST_ASSERT_EQUAL(0, (int)r.prior[1][0].output.find("[CLAIM CHECK]"));
  TEST_ASSERT_TRUE(r.prior[1][0].output.find("packing.md") != std::string::npos);
  const std::string tail = r.cc.tailAfterApply(kUnbacked);
  TEST_ASSERT_TRUE(tail.find("(Correction: nothing has been written for that yet.)") != std::string::npos);
}

// The correction round may do the work: a file written in that round backs the
// claim, so the reply goes out with no line.
static void test_round_that_writes_the_file_adds_no_line() {
  Round r;
  r.script = {finalStep(turnJson(kUnbacked)), toolStep("artifact.save"), finalStep(turnJson(kUnbacked))};
  const uint32_t tailedBefore = ClaimCheck::tailedCount();
  const HeadOutcome o = r.run();
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(1, o.corrections);
  TEST_ASSERT_EQUAL(3u, r.idx);
  TEST_ASSERT_EQUAL_STRING("", r.cc.tailAfterApply(kUnbacked).c_str());
  TEST_ASSERT_EQUAL(tailedBefore, ClaimCheck::tailedCount());
}

// On the forced final round no correction is possible: the check is not asked,
// no extra round runs, and the owner line carries the correction instead.
static void test_round_forced_final_gets_the_line_and_no_extra_round() {
  Round r;
  r.script = {toolStep("memory.search"), finalStep(turnJson(kUnbacked))};
  const uint32_t before = ClaimCheck::correctedCount();
  const HeadOutcome o = r.run(/*maxRounds=*/1);
  TEST_ASSERT_TRUE(o.ok);
  TEST_ASSERT_EQUAL(0, o.corrections);
  TEST_ASSERT_EQUAL(0, r.shapeCalls);
  TEST_ASSERT_EQUAL(before, ClaimCheck::correctedCount());
  TEST_ASSERT_FALSE(r.cc.tailAfterApply(kUnbacked).empty());
}

// Counter-tests: a reply its own items or tools back costs zero extra rounds,
// and a spawn claim is the turn-shape check's (skipSpawned): the claim check
// never adds a second spawn correction to the same round.
static void test_round_consistent_reply_and_spawn_claims_cost_no_claim_round() {
  Round r;
  r.script = {finalStep(turnJson("Saved: the dentist moved to 3pm on Thursday.",
                                 "\"mem_write\":[{\"content\":\"dentist moved to 3pm Thursday\"}]"))};
  const uint32_t before = ClaimCheck::correctedCount();
  HeadOutcome o = r.run();
  TEST_ASSERT_EQUAL(0, o.corrections);
  TEST_ASSERT_EQUAL(1u, r.idx);
  TEST_ASSERT_EQUAL(1, r.shapeCalls);

  Round s;
  s.script = {finalStep(turnJson("I have started three research sub-agents."))};
  o = s.run();
  TEST_ASSERT_EQUAL(0, o.corrections);   // the stub shape check passed it; the claim check skips spawns
  TEST_ASSERT_EQUAL(before, ClaimCheck::correctedCount());
  // After apply with no spawn accepted, the line is the spawn check's own wording.
  TEST_ASSERT_TRUE(s.cc.tailAfterApply("I have started three research sub-agents.").find(
                       nimbus::orch::kSpawnNotStartedLine) != std::string::npos);
}

// ---- one delivered reply per turn (bench C1 on the host) ----------------------
using harness_test::kMockClaimSavedTurn;
using harness_test::kNotSavedLine;

// How the turn's memory write ends.
enum class WriteEnd { Stored, Duplicate, Queued, Refused, FailedNoCode, NoStore, Unverified };

static nimbus::orch::ToolResult writeResult(WriteEnd e) {
  using nimbus::orch::ToolResult;
  ToolResult r = ToolResult::fail("memory engine unavailable");   // FailedNoCode
  if (e == WriteEnd::Stored) {
    r = ToolResult::ok("stored memory m1a2b3c4");
    r.code = nimbus::orch::kMemOutcomeStored;
  } else if (e == WriteEnd::Duplicate) {
    r = ToolResult::ok("duplicate of an existing memory (importance bumped)");
    r.code = nimbus::orch::kMemOutcomeDuplicate;
  } else if (e == WriteEnd::Queued) {   // FAULT embed_fail: the embedding outage queue
    r = ToolResult::ok("queued memory p1, not stored yet: the embedding provider is out of credit.");
    r.code = nimbus::orch::kMemOutcomeQueued;
  } else if (e == WriteEnd::Refused) {
    r = ToolResult::fail("the embedding provider is out of credit (not stored: the waiting list is full)");
    r.code = nimbus::orch::kMemOutcomeRefused;
  }
  return r;
}

// What one applied turn put in front of the owner and into the history.
struct Applied {
  std::vector<std::string> delivered, captured;
  int writes = 0;
};

// The LAN mock's "mock-claim-saved" turn through the real parse and apply, the way the device's
// single-shot custom head runs it (no tool loop, so no correction round: the
// check after apply is the only one).
static Applied applyMockTurn(WriteEnd end) {
  nimbus::orch::Turn t;
  nimbus::orch::ParseError perr;
  TEST_ASSERT_TRUE(nimbus::orch::parseHeadReply(kMockClaimSavedTurn, t, perr) == nimbus::orch::ReplyShape::Turn);
  TEST_ASSERT_EQUAL(1, (int)t.mem_write.size());
  Applied out;
  agent::ApplyDeps d;
  d.deliver = [&out](const std::string&, const std::string& text) { out.delivered.push_back(text); };
  d.captureAssistant = [&out](const std::string&, const std::string& text) { out.captured.push_back(text); };
  if (end != WriteEnd::NoStore)
    d.memDispatch = [&out, end](const char*, ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
      out.writes++;
      return writeResult(end);
    };
  ClaimCheck cc;
  cc.beginTurn();
  agent::ApplyState st;
  st.claims = &cc;
  st.unverifiedConnector = end == WriteEnd::Unverified;
  bool spawned = false;
  agent::applyTurn(t, "serial", d, st, spawned);
  return out;
}

// Every way a memory write can fail to land under a "Saved it" reply gives
// exactly one delivered reply, ending with the correction, and the history row
// is that same text once; a write that lands gives the same single reply with
// no line. claimsTailed moves once per corrected reply.
static void test_a_write_that_does_not_land_under_a_saved_reply_gets_one_corrected_reply() {
  const struct {
    WriteEnd end;
    bool lands;
    int writes;
    const char* what;
  } cases[] = {
      {WriteEnd::Stored, true, 1, "stored"},
      {WriteEnd::Duplicate, true, 1, "duplicate"},
      {WriteEnd::Queued, false, 1, "queued (embedding outage)"},
      {WriteEnd::Refused, false, 1, "refused"},
      {WriteEnd::FailedNoCode, false, 1, "failed with no outcome code"},
      {WriteEnd::NoStore, false, 0, "no memory store on this host"},
      {WriteEnd::Unverified, false, 0, "refused: unverified connector result"},
  };
  for (const auto& c : cases) {
    const uint32_t tailed = ClaimCheck::tailedCount();
    const Applied a = applyMockTurn(c.end);
    TEST_ASSERT_EQUAL_MESSAGE(c.writes, a.writes, c.what);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)a.delivered.size(), c.what);
    const std::string want =
        c.lands ? std::string("Saved it to memory.") : std::string("Saved it to memory.\n\n") + kNotSavedLine;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), a.delivered[0].c_str(), c.what);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)a.captured.size(), c.what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a.delivered[0].c_str(), a.captured[0].c_str(), c.what);
    TEST_ASSERT_EQUAL_MESSAGE(tailed + (c.lands ? 0 : 1), ClaimCheck::tailedCount(), c.what);
  }
}

// The 22 Oct incident (Lumi, v4.5.16): reply.telegram voice:true answered "sent to
// Telegram" with no receipt, the reply told the owner a voice message was sent, and
// "claims: ok n=1" let it through; nothing reached Telegram. A voice claim is backed
// only by a voice send whose result names Telegram's message_id, with or without
// hosted tools (a connector never sends this device's voice message).
static void test_voice_claim_needs_a_telegram_receipt() {
  using nimbus::orch::kFxVoice;
  const std::string voiceArgs = R"({"text":"Here is your evening summary.","voice":true})";
  const uint8_t flags = ClaimCheck::toolCallFlags("reply.telegram", voiceArgs);
  TEST_ASSERT_EQUAL_UINT8(kFxVoice, flags);
  TEST_ASSERT_EQUAL_UINT8(0, ClaimCheck::toolCallFlags("reply.telegram", R"({"text":"hi"})"));
  TEST_ASSERT_TRUE(agent::fxKindForTool("reply.telegram") == FxKind::Send);
  const char* claims[] = {"I've sent you a voice message with the summary.", "Voice message sent.",
                          "I sent the recap to Telegram as a voice note.",
                          "Your summary has been sent as audio.",
                          "The voice message has been sent to your Telegram."};
  const char* receipt = "delivered to Telegram as a voice message (message_id 812, 210 KB)";
  for (const char* reply : claims) {
    for (int hosted = 0; hosted < 2; hosted++) {
      ClaimCheck incident;
      incident.beginTurn();
      incident.setHostedTools(hosted != 0);
      incident.noteTool(agent::fxKindForTool("reply.telegram"), false, "sent to Telegram", flags);
      const std::string fix = incident.finalCheck(turnJson(reply));
      TEST_ASSERT_TRUE_MESSAGE(fix.find("no voice message reached Telegram") != std::string::npos, reply);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("\n\n(Correction: no voice message was delivered.)",
                                       incident.tailAfterApply(reply).c_str(), reply);
      ClaimCheck delivered;
      delivered.beginTurn();
      delivered.setHostedTools(hosted != 0);
      delivered.noteTool(agent::fxKindForTool("reply.telegram"), false, receipt, flags);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", delivered.finalCheck(turnJson(reply)).c_str(), reply);
    }
  }
  // The new honest failure backs nothing, so a "sent" reply is corrected too, and an
  // honest reply about it is not.
  ClaimCheck failed;
  failed.beginTurn();
  failed.noteTool(FxKind::Send, true,
                  "the voice message did not reach Telegram: Telegram did not accept it (Bad Request: "
                  "chat not found); nothing was delivered", flags);
  TEST_ASSERT_FALSE(failed.finalCheck(turnJson("I sent it to you.")).empty());
  TEST_ASSERT_EQUAL_STRING("", failed.finalCheck(turnJson("The voice message could not be sent, so here is "
                                                          "the text instead.")).c_str());
  // A voice reply that stopped part way: the delivered messages back a claim about
  // them (precision first), and the failure is still on the record.
  ClaimCheck partial;
  partial.beginTurn();
  partial.noteTool(FxKind::Send, true,
                   "only 2 of the 3 voice messages reached Telegram (message_id 811, 812); message 3 did "
                   "not: truncated at 4096 B (the speech audio did not arrive whole)", flags);
  TEST_ASSERT_EQUAL_STRING("", partial.finalCheck(turnJson("I sent you two voice messages; the third "
                                                           "one failed.")).c_str());
  TEST_ASSERT_EQUAL(1, partial.effects().countFailed(FxKind::Send));
  TEST_ASSERT_TRUE(nimbus::orch::voiceDelivered(partial.effects()));
  // A receipt for a text send never backs a voice claim; voice switched off sends text,
  // which backs a plain "sent" but not a voice message.
  ClaimCheck text;
  text.beginTurn();
  text.noteTool(FxKind::Send, false, "sent to Telegram as text (message_id 5)", 0);
  TEST_ASSERT_FALSE(text.finalCheck(turnJson("Voice message sent.")).empty());
  ClaimCheck off;
  off.beginTurn();
  off.noteTool(FxKind::Send, false, "sent to Telegram as text (voice replies are switched off, so it was not spoken)",
               flags);
  TEST_ASSERT_EQUAL_STRING("", off.finalCheck(turnJson("I sent it to you on Telegram.")).c_str());
  TEST_ASSERT_FALSE(off.finalCheck(turnJson("I sent it to you as a voice message.")).empty());
  // Precision: a send that only mentions a voice is a plain send, backed by any send.
  ClaimCheck file;
  file.beginTurn();
  file.noteTool(FxKind::Send, false, "queued for Telegram delivery: recording.m4a", 0);
  for (const char* reply : {"Sent you the recording of your voice.", "I shared the audio file you asked for.",
                            "I sent the transcript of your audio."})
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", file.finalCheck(turnJson(reply)).c_str(), reply);
  // Precision: the owner's own voice messages, offers and earlier sends are not claims.
  for (const char* reply : {"Thanks for your voice message, here is the answer.",
                            "You sent me a voice message about the trip.",
                            "Want me to send it as a voice message?",
                            "I'll send you a voice note tonight.",
                            "I could not send the voice message."}) {
    ClaimCheck none;
    none.beginTurn();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", none.finalCheck(turnJson(reply)).c_str(), reply);
  }
  // A text send's claim keeps the plain backing (S2 extends receipts to text later), and
  // a clause join keeps a voice word out of a text claim.
  ClaimCheck plain;
  plain.beginTurn();
  plain.noteTool(FxKind::Send, false, "sent to Telegram as text", 0);
  TEST_ASSERT_EQUAL_STRING("", plain.finalCheck(turnJson("I sent the text version since voice messages "
                                                         "are off.")).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_consistent_replies_need_no_correction);
  RUN_TEST(test_voice_claim_needs_a_telegram_receipt);
  RUN_TEST(test_unbacked_claim_gets_one_correction_with_the_record);
  RUN_TEST(test_tail_after_apply_uses_the_real_outcomes);
  RUN_TEST(test_scratchpad_durability_fresh_device_and_full_nvs);
  RUN_TEST(test_hosted_tools_leave_sent_checked_and_changed_unchecked);
  RUN_TEST(test_skip_spawned_leaves_the_spawn_class_to_the_shape_check);
  RUN_TEST(test_unsourced_fresh_result_needs_a_hedge);
  RUN_TEST(test_figures_against_the_turn_sources);
  RUN_TEST(test_device_actions_back_their_claims_only_when_allowed);
  RUN_TEST(test_scratch_outcomes_and_permanent_tool_writes);
  RUN_TEST(test_begin_turn_clears_everything);
  RUN_TEST(test_every_tool_kind_comes_from_the_shared_table);
  RUN_TEST(test_round_unbacked_twice_gets_one_round_then_the_line);
  RUN_TEST(test_round_that_writes_the_file_adds_no_line);
  RUN_TEST(test_round_forced_final_gets_the_line_and_no_extra_round);
  RUN_TEST(test_round_consistent_reply_and_spawn_claims_cost_no_claim_round);
  RUN_TEST(test_a_write_that_does_not_land_under_a_saved_reply_gets_one_corrected_reply);
  return UNITY_END();
}
