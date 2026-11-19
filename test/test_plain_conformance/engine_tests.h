#pragma once
// The loop core's own cases (lane RB-E1a), run from main.cpp beside the battery's: scripted
// turns through the plain engine with the stand-in wire of each dialect (txb::StubWire, or
// the real wire once it lands). A tool-less reply, tool rounds, a cut answer, an exchange
// error of each class, a store that answers not stored or has no card, a restart in the
// middle of a turn, the round budget and the wall clock, byte-identical requests, every
// seam at its point, a refused allocation.
#include <unity.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/tool_groups.h"

namespace engine_tests {

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace texts = agent::plain::texts;

inline std::vector<tx::Item> storedOf(txb::Battery& b) {
  std::vector<tx::Item> items;
  const tx::Status st = b.stored(items);
  TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.where);
  return items;
}
inline size_t countKind(const std::vector<tx::Item>& items, tx::ItemKind k) {
  size_t n = 0;
  for (const tx::Item& it : items) n += it.kind == k;
  return n;
}
inline const tx::Item* lastEnd(const std::vector<tx::Item>& items) {
  const tx::Item* e = nullptr;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::TurnEnd) e = &it;
  return e;
}
inline void expectPaired(txb::Battery& b) {
  std::string why;
  for (const auto& r : b.model.requests()) TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(r.view, why), why.c_str());
  const std::vector<tx::Item> items = storedOf(b);
  TEST_ASSERT_TRUE_MESSAGE(txb::itemsPaired(items, why), why.c_str());
}
inline bool bodyHas(const sp::Recorded& r, const std::string& s) { return r.body.find(s) != std::string::npos; }

// ---- the loop shape on every dialect's stand-in wire ------------------------------------------
struct Shape {
  std::vector<sp::Step> steps;
  std::vector<txb::StepShape> shape;
};

inline std::vector<Shape> shapes() {
  const std::string rf = R"({"path":"notes/a.md"})";
  return {
      {{sp::say("Hello.")}, {{0, "Hello."}}},
      {{sp::call("read_file", rf), sp::say("It says alpha.")}, {{1, ""}, {0, "It says alpha."}}},
      {{sp::call("read_file", rf, "Let me look."), sp::say("Alpha.")}, {{1, "Let me look."}, {0, "Alpha."}}},
      {{sp::call("read_file", rf), sp::call("big_read", R"({"tag":"b","bytes":120})"),
        sp::call("read_file", R"({"path":"notes/b.md"})"), sp::say("Three rounds.")},
       {{1, ""}, {1, ""}, {1, ""}, {0, "Three rounds."}}},
      {{sp::calls({{"read_file", rf, ""}, {"big_read", R"({"tag":"c","bytes":120})", ""}}), sp::say("Both.")}, {{2, ""}, {0, "Both."}}},
  };
}

inline void loopShapeOn(plain::Dialect d) {
  txb::Battery b(d);
  b.world.files["notes/a.md"] = "alpha";
  b.world.files["notes/b.md"] = "beta";
  for (const Shape& s : shapes()) {
    const size_t first = b.model.requests().size(), sentBefore = b.sent.size();
    for (const sp::Step& st : s.steps) b.model.push(st);
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("go", out));
    std::vector<const sp::RequestView*> reqs;
    for (size_t i = first; i < b.model.requests().size(); ++i) {
      const sp::RequestView& v = b.model.requests()[i].view;
      TEST_ASSERT_TRUE_MESSAGE(v.parsed, v.parseError.c_str());
      TEST_ASSERT_TRUE(v.tools.size() <= plain::kMaxAlwaysOnTools);
      TEST_ASSERT_EQUAL_STRING("auto", v.toolChoice.c_str());
      reqs.push_back(&v);
    }
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::loopShapeHolds(s.shape, reqs, out.text, why), why.c_str());
    TEST_ASSERT_TRUE(out.delivered);
    // The channel got the model's text and nothing else.
    TEST_ASSERT_EQUAL(sentBefore + 1, b.sent.size());
    TEST_ASSERT_EQUAL_STRING(s.shape.back().text.c_str(), b.sent.back().text.c_str());
    TEST_ASSERT_EQUAL((int)s.steps.size(), (int)out.headRequests);
  }
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
  expectPaired(b);
}

inline void test_loop_shape_on_each_dialect_stub() {
  for (plain::Dialect d : plain::kAllDialects) loopShapeOn(d);
}

// A tool-less reply: one request, the text stored once as the final a item, then d and e.
inline void test_a_tool_less_reply_is_one_request_and_one_message() {
  txb::Battery b;
  b.model.push(sp::say("Hi there."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello", out));
  TEST_ASSERT_EQUAL(1, (int)b.model.requests().size());
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("Hi there.", b.sent[0].text.c_str());
  const std::vector<tx::Item> items = storedOf(b);
  std::string kinds;
  for (const tx::Item& it : items) kinds += tx::kindLetter(it.kind);
  TEST_ASSERT_EQUAL_STRING("utade", kinds.c_str());
  TEST_ASSERT_TRUE(items[2].fin);
  // The request offers the registered always-on tools, composed for the turn.
  const auto& tools = b.model.last().view.tools;
  TEST_ASSERT_TRUE_MESSAGE(std::find(tools.begin(), tools.end(), "read_file") != tools.end(), "no tools offered");
  TEST_ASSERT_EQUAL_STRING("reply", lastEnd(items)->end.cause.c_str());
  TEST_ASSERT_EQUAL(1, (int)lastEnd(items)->end.headRequests);
  // The owner message is in the request before any model call, stamped with its time.
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "[Tue 17 Nov 2026 13:02] hello"));
}

// ---- a cut answer ----------------------------------------------------------------------------
inline void test_a_cut_answer_is_delivered_with_the_cut_line_and_its_calls_never_run() {
  txb::Battery b;
  b.model.respondWith([](const sp::Recorded&, size_t) {
    sp::Step s = sp::call("write_file", R"({"path":"notes/x.md","text":"half)", "Here is the start");
    s.finish = sp::Finish::MaxTokens;
    return s;
  });
  plain::TurnOutcome out;
  TXB_OK(b.owner("write a long note", out));
  TEST_ASSERT_TRUE(b.model.requests().size() >= 1 && b.model.requests().size() <= 2);
  TEST_ASSERT_EQUAL(0, (int)b.world.dispatched.size());
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::CutAnswer);
  TEST_ASSERT_EQUAL(2, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("Here is the start", b.sent[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kCut, b.sent[1].text.c_str());
  const std::vector<tx::Item> items = storedOf(b);
  bool unrun = false;
  for (const tx::Item& it : items) unrun |= it.kind == tx::ItemKind::Result && !it.ok && it.x == texts::tool_results::kNotRunCut;
  TEST_ASSERT_TRUE_MESSAGE(unrun, "the cut call has no 'not run' result");
  TEST_ASSERT_EQUAL_STRING("cut", lastEnd(items)->end.cause.c_str());
  // The next turn renders the cut call with its result: still paired.
  b.model.respondWith(nullptr);
  b.model.push(sp::say("ok"));
  TXB_OK(b.owner("fine", out));
  expectPaired(b);
}

// ---- an exchange error of each class -----------------------------------------------------------
struct ErrorCase {
  const char* name;
  plain::ExchangeErr err;
  sp::Step step;
};

inline std::vector<ErrorCase> errorCases() {
  return {{"transport", plain::ExchangeErr::Transport, sp::transportCut(1000)},
          {"server", plain::ExchangeErr::Server, sp::httpError(500)},
          {"rate", plain::ExchangeErr::RateLimited, sp::httpError(429)},
          {"context", plain::ExchangeErr::ContextRefused, sp::httpError(413)},
          {"key", plain::ExchangeErr::KeyRefused, sp::httpError(401)},
          {"malformed", plain::ExchangeErr::Malformed, sp::raw(200, "not json")},
          {"other", plain::ExchangeErr::Other, sp::httpError(404)}};
}

inline void test_an_exchange_error_of_each_class_ends_the_turn_with_one_plain_line() {
  for (const ErrorCase& c : errorCases()) {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    // A tool round first, so the failure lands mid-turn after an effect-free call ran.
    b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
    const sp::Step fail = c.step;
    b.model.respondWith([fail](const sp::Recorded&, size_t) { return fail; });
    plain::TurnOutcome out;
    TXB_OK(b.owner("read it", out));
    TEST_ASSERT_TRUE_MESSAGE(out.status == plain::StatusLine::ProviderError, c.name);
    TEST_ASSERT_FALSE_MESSAGE(out.delivered, c.name);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)b.sent.size(), c.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(out.statusText.c_str(), b.sent[0].text.c_str(), c.name);
    TEST_ASSERT_TRUE_MESSAGE(!out.statusText.empty(), c.name);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)b.world.dispatched.size(), c.name);   // the one read, never again
    TEST_ASSERT_EQUAL_MESSAGE(c.err == plain::ExchangeErr::KeyRefused, out.parkHost, c.name);
    const std::vector<tx::Item> items = storedOf(b);
    TEST_ASSERT_EQUAL_STRING_MESSAGE((std::string("provider:") + plain::exchangeErrSlug(c.err)).c_str(),
                                     lastEnd(items)->end.cause.c_str(), c.name);
    // The chat goes on: the next turn answers, with the failed turn's call still paired.
    b.model.respondWith(nullptr);
    b.model.push(sp::say("Back."));
    b.advance(60);
    TXB_OK(b.owner("again", out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Back.", out.text.c_str(), c.name);
    expectPaired(b);
  }
}

// ---- storage that refuses ----------------------------------------------------------------------
// The intent append is refused: the effect does not run, the model sees "not run" and one
// device note, the owner gets the reply and one storage line; everything stays paired.
inline void test_an_effect_runs_only_when_its_intent_is_stored() {
  txb::Battery b;
  TXB_OK(b.boot());
  b.files.refuseWhen = [](const char* data, size_t len) {
    return std::string(data, len).find("\"k\":\"i\"") != std::string::npos;
  };
  b.model.push(sp::call("write_file", R"({"path":"notes/x.md","text":"v1"})"));
  b.model.push(sp::say("It did not save."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("save v1", out));
  TEST_ASSERT_EQUAL(0, b.world.runsOf("write_file"));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), texts::tool_results::kNotRunUnrecorded));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), texts::device_notes::kNotStored));
  TEST_ASSERT_EQUAL(2, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("It did not save.", b.sent[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kStorageLine, b.sent[1].text.c_str());
  std::string why;
  for (const auto& r : b.model.requests()) TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(r.view, why), why.c_str());
  // Storage works again: the next effect runs, and no second storage line is sent.
  b.model.push(sp::call("write_file", R"({"path":"notes/x.md","text":"v2"})"));
  b.model.push(sp::say("Saved."));
  TXB_OK(b.owner("save v2", out));
  TEST_ASSERT_EQUAL(1, b.world.runsOf("write_file"));
  TEST_ASSERT_EQUAL(3, (int)b.sent.size());
}

// No card at all: the log answers Io at boot and the chat runs from RAM. Reads answer,
// effects never run, the owner is told once per episode.
inline void test_no_card_runs_from_ram_and_never_runs_an_effect() {
  txb::Battery b;
  b.files.refuseAll = true;
  b.world.files["notes/a.md"] = "alpha";
  b.model.push(sp::calls({{"read_file", R"({"path":"notes/a.md"})", ""}, {"write_file", R"({"path":"notes/a.md","text":"b"})", ""}}));
  b.model.push(sp::say("Read alpha; the write did not run."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read and change it", out));
  TEST_ASSERT_EQUAL(0, b.world.runsOf("write_file"));
  TEST_ASSERT_EQUAL(1, (int)b.world.dispatched.size());
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "alpha"));
  TEST_ASSERT_EQUAL(2, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kStorageLine, b.sent[1].text.c_str());
  b.model.push(sp::say("Still here."));
  b.advance(60);
  TXB_OK(b.owner("still?", out));
  TEST_ASSERT_EQUAL_STRING("Still here.", out.text.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(3, (int)b.sent.size(), "a second storage line in one episode");
  // The second request still carries the first turn: RAM holds the chat.
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "read and change it"));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

// ---- a restart in the middle of a turn ---------------------------------------------------------
// A cut while the effect's result is stored, and one while its intent is stored: after the
// restart the fill replays the stored items, the dangling call is answered "outcome
// unknown" before the next owner message, and nothing runs twice; an effect whose intent
// never reached the card never ran.
inline void restartAt(const char* kind, int wantRuns) {
  txb::Battery b;
  TXB_OK(b.boot());
  const std::string marker = std::string("\"k\":\"") + kind + "\"";
  b.files.killWhen = [marker](const char* data, size_t len) { return std::string(data, len).find(marker) != std::string::npos; };
  b.model.push(sp::call("write_file", R"({"path":"notes/x.md","text":"v1"})"));
  plain::TurnOutcome out;
  const tx::Status st = b.owner("save v1", out);
  TEST_ASSERT_FALSE(st.ok());
  TEST_ASSERT_TRUE_MESSAGE(b.killed, "the cut never fired");
  TXB_OK(b.restartAfterCut());   // the u item was stored: nothing is redelivered
  b.model.push(sp::say("I could not confirm the save."));
  b.advance(30);
  TXB_OK(b.owner("did it save?", out));
  TEST_ASSERT_EQUAL_MESSAGE(wantRuns, b.world.runsOf("write_file"), kind);
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), "save v1"), "the fill lost the interrupted turn");
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), "outcome unknown"), kind);
  expectPaired(b);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), why.c_str());
}

inline void test_a_restart_mid_turn_replays_the_fill_and_runs_nothing_twice() {
  restartAt("r", 1);   // the effect ran; its result was cut
  restartAt("i", 0);   // the intent was cut: the effect never ran
}

// A clean restart between turns: the next request extends the last one (the fill renders
// the same transcript) apart from the new message.
inline void test_the_fill_after_a_restart_renders_the_same_transcript() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  TXB_OK(txb::runTurns(b, {txb::withTool(30, "read a", "read_file", R"({"path":"notes/a.md"})", "alpha it is")}));
  const sp::RequestView before = b.model.last().view;
  TXB_OK(b.restartAfterCut());
  TXB_OK(txb::runTurns(b, {txb::chat(30, "and now?", "same")}));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(sp::isPrefix(before, b.model.last().view, why), why.c_str());
}

// ---- the round budget and the wall clock ------------------------------------------------------
inline sp::Step untilNone(const sp::Recorded& r, uint32_t takesMs) {
  sp::Step s = r.view.toolChoice == "none" ? sp::say("Stopped at the limit.")
                                           : sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" +
                                                                       std::to_string(r.step) + "}");
  s.takesMs = takesMs;
  return s;
}

inline void test_a_turn_at_the_round_budget_gets_one_note_and_one_last_round() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  b.model.respondWith([](const sp::Recorded& r, size_t) { return untilNone(r, 0); });
  plain::TurnOutcome out;
  TXB_OK(b.owner("keep reading", out));
  const auto& reqs = b.model.requests();
  TEST_ASSERT_EQUAL(plain::kDefaultRounds + 1, (int)reqs.size());
  TEST_ASSERT_EQUAL_STRING("none", reqs.back().view.toolChoice.c_str());
  TEST_ASSERT_TRUE(out.trip == plain::GuardTrip::Rounds);
  TEST_ASSERT_EQUAL_STRING("Stopped at the limit.", out.text.c_str());
  const std::vector<tx::Item> items = storedOf(b);
  TEST_ASSERT_EQUAL(1, (int)countKind(items, tx::ItemKind::DeviceNote));
  TEST_ASSERT_TRUE(bodyHas(reqs.back(), "[device] limit reached (rounds)"));
  TEST_ASSERT_EQUAL_STRING("limit:rounds", lastEnd(items)->end.cause.c_str());
}

inline void test_a_turn_at_the_wall_clock_ends_inside_the_hard_wall() {
  // Each answer takes 70 s: the 240 s soft wall trips after four rounds.
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  b.model.respondWith([](const sp::Recorded& r, size_t) { return untilNone(r, 70000); });
  const uint32_t t0 = b.ms;
  plain::TurnOutcome out;
  TXB_OK(b.owner("keep reading slowly", out));
  TEST_ASSERT_TRUE(out.trip == plain::GuardTrip::Time);
  TEST_ASSERT_EQUAL_STRING("none", b.model.last().view.toolChoice.c_str());
  TEST_ASSERT_EQUAL_STRING("Stopped at the limit.", out.text.c_str());
  TEST_ASSERT_TRUE_MESSAGE(b.ms - t0 <= plain::kHardWallMs, "the turn ran past the hard wall");
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "[device] limit reached (time)"));
}

// A provider that never answers (every read hangs its whole window): the turn ends with one
// plain line inside its soft wall, with no tool run twice and no spin.
inline void test_a_hung_provider_ends_the_turn_inside_its_wall_clock() {
  txb::Battery b;
  b.model.respondWith([](const sp::Recorded&, size_t) { return sp::transportCut(120000); });
  const uint32_t t0 = b.ms;
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello?", out));
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::ProviderError);
  TEST_ASSERT_TRUE_MESSAGE(b.ms - t0 <= plain::kSoftWallMs + 10000, "past the soft wall");
  TEST_ASSERT_TRUE(b.model.requests().size() <= 3);
}

// ---- byte-identical requests (prompt-cache stability) ----------------------------------------
inline std::vector<std::string> bodiesOf(plain::Dialect d) {
  txb::Battery b(d);
  b.world.files["notes/a.md"] = "alpha";
  std::vector<txb::TurnScript> turns;
  for (int i = 0; i < 6; ++i)
    turns.push_back(i % 2 ? txb::chat(60, "chat " + std::to_string(i), "reply " + std::to_string(i))
                          : txb::withTool(60, "read " + std::to_string(i), "read_file", R"({"path":"notes/a.md"})",
                                          "alpha " + std::to_string(i)));
  TXB_OK(txb::runTurns(b, turns));
  std::vector<std::string> out;
  for (const auto& r : b.model.requests()) out.push_back(r.body);
  // Inside the run: system and tools never change, and each request extends the one before.
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(b.model.allPrefixStable(why), why.c_str());
  return out;
}

inline std::string withoutCap(std::string body) {
  for (const char* key : {"\"max_tokens\":", "\"max_output_tokens\":"}) {
    const size_t at = body.find(key);
    if (at == std::string::npos) continue;
    size_t end = at + std::char_traits<char>::length(key);
    while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
    body.erase(at, end - at);
  }
  return body;
}

inline void test_identical_state_gives_byte_identical_requests() {
  for (plain::Dialect d : plain::kAllDialects) {
    const std::vector<std::string> a = bodiesOf(d), b = bodiesOf(d);
    TEST_ASSERT_EQUAL(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) TEST_ASSERT_TRUE_MESSAGE(a[i] == b[i], "same state, different bytes");
    // Round after round every earlier message keeps its bytes: the body up to the last
    // message of request N is the start of request N+1 (the output cap, which the exchange
    // fits to each body, is not part of the cached prefix and is left out of the compare).
    if (d == plain::Dialect::OpenAIResponses) continue;
    for (size_t i = 1; i < a.size(); ++i) {
      const std::string prev = withoutCap(a[i - 1]), next = withoutCap(a[i]);
      const size_t lastMsg = prev.rfind("{\"role\":");
      TEST_ASSERT_TRUE(lastMsg != std::string::npos);
      TEST_ASSERT_TRUE_MESSAGE(sp::commonPrefixBytes(prev, next) >= lastMsg, "an earlier message's bytes moved");
    }
  }
}

// ---- every seam at its point --------------------------------------------------------------
inline void test_every_seam_is_called_at_its_point() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  TXB_OK(b.boot());
  std::vector<std::string> log;
  plain::PlainHooks& h = *b.hooks;
  TXB_OK(h.beforeDispatch(plain::HookSlot::Policy, [&](const plain::DispatchCtx& c) {
    log.push_back("beforeDispatch " + c.call->name + (c.effect ? " effect" : "") + (c.pure ? " pure" : ""));
    plain::DispatchDecision d;
    if (c.call->name == "big_read") {
      d.verdict = plain::DispatchVerdict::Answer;
      d.text = "answered by a hook";
    }
    if (c.call->name == "device_control") {
      d.verdict = plain::DispatchVerdict::Refuse;
      d.text = "refused by a hook";
    }
    return d;
  }));
  TXB_OK(h.afterResult(plain::HookSlot::Observer, [&](const plain::ResultCtx& c) {
    log.push_back("afterResult " + c.call->name + (c.stored ? " stored" : "") + (c.output->isError ? " error" : ""));
  }));
  int holds = 0;
  TXB_OK(h.beforeDeliver(plain::HookSlot::Truth, [&](const plain::DeliverCtx& c) {
    log.push_back("beforeDeliver " + *c.text + (c.held ? " held" : ""));
    plain::DeliverDecision d;
    if (!c.held && holds++ == 0) {
      d.verdict = plain::DeliverVerdict::Hold;
      d.deviceNote = "[device] effects this turn: none.";
    }
    return d;
  }));
  TXB_OK(h.afterDeliver(plain::HookSlot::Observer, [&](const plain::DeliveredCtx& c) {
    log.push_back("afterDeliver " + *c.text + (c.sent ? " sent" : ""));
  }));
  TXB_OK(h.idleStep(plain::HookSlot::Compaction, [&](const plain::IdleCtx&) {
    log.push_back("idleStep");
    plain::IdleOutcome o;
    o.worked = true;
    return o;
  }));
  b.model.push(sp::calls({{"read_file", R"({"path":"notes/a.md"})", ""},
                          {"big_read", R"({"tag":"d","bytes":120})", ""},
                          {"device_control", R"({"action":"restart"})", ""}}));
  b.model.push(sp::say("Done, I think."));
  b.model.push(sp::say("Read alpha; the restart was refused."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("do three things", out));
  plain::IdleOutcome idle;
  TXB_OK(b.idle(idle));
  const std::vector<std::string> want = {
      "beforeDispatch read_file pure",   "afterResult read_file stored",
      "beforeDispatch big_read pure",    "afterResult big_read stored",
      "beforeDispatch device_control effect", "afterResult device_control stored error",
      "beforeDeliver Done, I think.",   "beforeDeliver Read alpha; the restart was refused. held",
      "afterDeliver Read alpha; the restart was refused. sent", "idleStep"};
  std::string got;
  for (const std::string& l : log) got += l + " | ";
  TEST_ASSERT_EQUAL_MESSAGE(want.size(), log.size(), got.c_str());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), log[i].c_str());
  // Only the read ran; the hook's answers are the model's results; the hold is one note.
  TEST_ASSERT_EQUAL(1, (int)b.world.dispatched.size());
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "answered by a hook"));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "refused by a hook"));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "[device] effects this turn: none."));
  TEST_ASSERT_EQUAL_STRING("Read alpha; the restart was refused.", out.text.c_str());
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_TRUE(idle.worked && idle.by == plain::HookSlot::Compaction);
  expectPaired(b);
}

inline void test_boot_runs_the_resume_seam_once() {
  txb::Battery b;
  int boots = 0;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->bootResume(plain::HookSlot::Resume, [&](const plain::BootCtx&) {
    ++boots;
    return tx::Status::success();
  }));
  TXB_OK(b.engine->boot(plain::BootCtx{b.epoch, false}));
  TEST_ASSERT_EQUAL(1, boots);
}

// ---- a refused allocation ------------------------------------------------------------------
inline void test_a_refused_allocation_ends_the_turn_with_a_status_never_an_abort() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDispatch(plain::HookSlot::Policy, [](const plain::DispatchCtx& c) -> plain::DispatchDecision {
    if (c.call->name == "write_file") throw std::bad_alloc();
    return {};
  }));
  b.model.push(sp::call("write_file", R"({"path":"notes/x.md","text":"v1"})"));
  plain::TurnOutcome out;
  const tx::Status st = b.owner("save", out);
  TEST_ASSERT_TRUE(st.err == tx::Err::TooLarge);
  TEST_ASSERT_EQUAL(0, b.world.runsOf("write_file"));
  // The owner is told in one plain line, and the turn is closed with its e item.
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kNoMemory, b.sent[0].text.c_str());
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::CrashNotice);
  TEST_ASSERT_EQUAL_STRING("memory", lastEnd(storedOf(b))->end.cause.c_str());
  b.hooks->clear();
  b.model.push(sp::say("It may not have saved."));
  TXB_OK(b.owner("did it?", out));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "outcome unknown"));
  expectPaired(b);
}

// ---- what the review found (fresh-context hostile review, lane RB-E1a) -----------------------
// An effect reached through `more` is an effect: its intent comes first, and a refused intent
// keeps it from running.
inline void test_an_effect_through_more_needs_its_intent() {
  for (int refuse = 0; refuse < 2; ++refuse) {
    txb::Battery b;
    nimbus::orch::registerMore(b.reg);
    TXB_OK(b.boot());
    if (refuse)
      b.files.refuseWhen = [](const char* data, size_t len) {
        return std::string(data, len).find("\"k\":\"i\"") != std::string::npos;
      };
    b.model.push(sp::call("more", R"({"name":"device_control","args":{"action":"restart"}})"));
    b.model.push(sp::say("Done."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("restart please", out));
    const std::vector<tx::Item> items = storedOf(b);
    bool eff = false;
    for (const tx::Item& it : items) eff |= it.kind == tx::ItemKind::Call && it.n == "more" && it.eff;
    TEST_ASSERT_TRUE_MESSAGE(eff, "a more call that reaches an effect is not stored as one");
    TEST_ASSERT_EQUAL_MESSAGE(refuse ? 0 : 1, (int)b.world.dispatched.size(), "the intent rule did not hold through more");
    if (!refuse) TEST_ASSERT_EQUAL(1, (int)countKind(items, tx::ItemKind::Intent));
  }
}

// A result the card refused (kept in RAM), a turn that then closes on the card, a restart:
// the call is answered "outcome unknown" at the next open, so the request stays paired.
inline void test_a_result_lost_to_the_card_is_closed_after_a_restart() {
  txb::Battery b;
  TXB_OK(b.boot());
  b.files.refuseWhen = [](const char* data, size_t len) {
    return std::string(data, len).find("\"k\":\"r\"") != std::string::npos;
  };
  b.model.push(sp::call("write_file", R"({"path":"notes/x.md","text":"v1"})"));
  b.model.push(sp::say("Saved."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("save v1", out));
  TEST_ASSERT_EQUAL(1, b.world.runsOf("write_file"));
  TXB_OK(b.restartAfterCut());
  b.model.push(sp::say("It may have saved."));
  b.advance(30);
  TXB_OK(b.owner("did it?", out));
  TEST_ASSERT_EQUAL(1, b.world.runsOf("write_file"));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "outcome unknown"));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(b.model.last().view, why), why.c_str());
}

// Slow tools inside one round: once the soft wall has passed no further call starts; the
// rest are answered "not run" and the guard's last round follows.
inline void test_no_call_starts_past_the_soft_wall() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDispatch(plain::HookSlot::Policy, [&b](const plain::DispatchCtx&) {
    b.ms += 100000;   // each call takes 100 s
    return plain::DispatchDecision{};
  }));
  b.world.files["notes/a.md"] = "alpha";
  const std::string rf = R"({"path":"notes/a.md"})";
  b.model.push(sp::calls({{"read_file", rf, ""}, {"read_file", rf, ""}, {"read_file", rf, ""}, {"read_file", rf, ""}}));
  b.model.push(sp::say("Stopped at the limit."));
  const uint32_t t0 = b.ms;
  plain::TurnOutcome out;
  TXB_OK(b.owner("read it four times", out));
  TEST_ASSERT_TRUE_MESSAGE(b.world.dispatched.size() < 4, "every slow call ran");
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), texts::tool_results::kNotRunLimit));
  TEST_ASSERT_TRUE(out.trip == plain::GuardTrip::Time);
  TEST_ASSERT_EQUAL_STRING("none", b.model.last().view.toolChoice.c_str());
  TEST_ASSERT_TRUE(b.ms - t0 <= plain::kHardWallMs);
  expectPaired(b);
}

// A round that starts late (a 200 s tool round first) gets only the wall clock that is left:
// a provider that then hangs cannot keep the turn past its walls.
inline void test_a_late_round_gets_only_the_time_left() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDispatch(plain::HookSlot::Policy, [&b](const plain::DispatchCtx&) {
    b.ms += 200000;
    return plain::DispatchDecision{};
  }));
  b.world.files["notes/a.md"] = "alpha";
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  b.model.respondWith([](const sp::Recorded&, size_t) { return sp::transportCut(60000); });
  const uint32_t t0 = b.ms;
  plain::TurnOutcome out;
  TXB_OK(b.owner("read it", out));
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::ProviderError);
  TEST_ASSERT_TRUE_MESSAGE(b.ms - t0 <= plain::kSoftWallMs + 60000, "the late round ran on a full wall clock");
}

// ---- the overflow ladder's rung 3 (ruling 44) ------------------------------------------------
// Ten owner turns on an 85,000 B head: nine of about 7 KB, then one of 27 KB. The last body is
// over the cap by more than one turn and less than two: rung 3 omits the two oldest turns and
// sends the other eight, never every omissible turn.
inline void tenTurns(txb::Battery& b, size_t lastBytes) {
  for (int i = 0; i < 10; ++i) {
    const std::string k = std::to_string(i);
    b.model.push(sp::say("ok " + k));
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("turn " + k + " " + std::string(i == 9 ? lastBytes : 7000, 'w'), out));
  }
}

inline void test_rung_three_omits_only_the_turns_the_request_needs() {
  txb::Battery b;
  tenTurns(b, 27000);
  const auto& reqs = b.model.requests();
  for (size_t i = 0; i + 1 < reqs.size(); ++i)
    TEST_ASSERT_TRUE_MESSAGE(txb::detail::omittedTurnsNamed(reqs[i].view) < 0, "an earlier request already omitted");
  TEST_ASSERT_TRUE_MESSAGE(reqs.back().body.size() <= tx::kFloorHeadBodyCap, "the last request is over the cap");
  TEST_ASSERT_EQUAL_MESSAGE(2, (int)txb::detail::omittedTurnsNamed(reqs.back().view), "rung 3 did not omit the fewest turns");
  for (int i = 2; i < 10; ++i)
    TEST_ASSERT_TRUE_MESSAGE(bodyHas(reqs.back(), "turn " + std::to_string(i) + " w"), "a turn that fitted is not sent");
  std::vector<tx::Item> items;
  TXB_OK(b.storedBeforeLastFinal(items));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::covers(reqs.back().view, b.coverOf(items), why), why.c_str());
}

// A provider's size refusal says nothing about how much is too much: rung 3 may take the larger
// step there (every omissible turn), and the request after it is sent once.
inline void test_a_size_refusal_may_omit_every_omissible_turn() {
  txb::Battery b;
  for (int i = 0; i < 9; ++i) {
    b.model.push(sp::say("ok"));
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("small turn " + std::to_string(i), out));
  }
  b.model.push(sp::httpError(413));
  b.model.push(sp::say("fits now"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("one more", out));
  TEST_ASSERT_EQUAL_STRING("fits now", out.text.c_str());
  TEST_ASSERT_EQUAL(4, (int)txb::detail::omittedTurnsNamed(b.model.last().view));
}

// ---- the stack review (coordinator, 19 Nov): rules keyed on the tool see through `more` -----
inline void withMore(txb::Battery& b) {
  nimbus::orch::registerMore(b.reg);
  auto handler = [](ArduinoJson::JsonObjectConst, const nimbus::orch::Principal&) {
    return nimbus::orch::ToolResult::ok("");
  };
  b.reg.add("delete_file", "", handler);
  b.reg.add("mcp_linear_search", "", handler);   // a connector tool: no class row, outside
}

// A: a connector reached through more is outside content: classed outside, the turn tainted
// for its later calls, the outside change point logged.
inline void test_outside_content_through_more_taints_the_turn() {
  txb::Battery b;
  withMore(b);
  TXB_OK(b.boot());
  bool taintedAtRead = false;
  TXB_OK(b.hooks->beforeDispatch(plain::HookSlot::Policy, [&](const plain::DispatchCtx& c) {
    if (c.call->name == "read_file") taintedAtRead = c.turn->tainted;
    return plain::DispatchDecision{};
  }));
  b.world.files["notes/a.md"] = "alpha";
  b.model.push(sp::call("more", R"({"name":"mcp_linear_search","args":{"q":"open issues"}})"));
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  b.model.push(sp::say("Done."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("check linear", out));
  const std::vector<tx::Item> items = storedOf(b);
  bool outside = false;
  for (const tx::Item& it : items) outside |= it.kind == tx::ItemKind::Result && it.n == "more" && it.cls == tx::ResultClass::Outside;
  TEST_ASSERT_TRUE_MESSAGE(outside, "a connector's result through more was stored as internal");
  TEST_ASSERT_TRUE_MESSAGE(taintedAtRead, "the turn was not tainted after outside content through more");
  const auto& ev = lastEnd(items)->end.events;
  TEST_ASSERT_TRUE(std::find(ev.begin(), ev.end(), "change:outside") != ev.end());
}

// B: a file deleted through more rebuilds the recent-files list at the turn's close.
inline void test_a_delete_through_more_rebuilds_the_recent_files() {
  txb::Battery b;
  withMore(b);
  b.world.files["notes/a.md"] = "alpha";
  b.world.files["notes/b.md"] = "beta";
  b.recentFiles = [&b](std::vector<tx::RecentFile>& out) {
    out.clear();
    for (const auto& kv : b.world.files) out.push_back(tx::RecentFile{kv.first, b.epoch});
    return true;
  };
  TXB_OK(txb::runTurns(b, {txb::chat(30, "hello", "hi"),
                           txb::withTool(30, "delete a", "more", R"({"name":"delete_file","args":{"path":"notes/a.md"}})", "Deleted."),
                           txb::chat(30, "and now?", "ok")}));
  const auto& reqs = b.model.requests();
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(reqs[0], "notes/a.md ("), "the first turn's list does not name the file");
  TEST_ASSERT_TRUE_MESSAGE(!bodyHas(reqs.back(), "notes/a.md ("), "a deleted file is still listed");
  TEST_ASSERT_TRUE(bodyHas(reqs.back(), "notes/b.md ("));
}

// C: a delete through more supersedes the earlier whole read of that path (rule 4).
inline void test_a_write_through_more_supersedes_the_earlier_read() {
  txb::Battery b;
  withMore(b);
  b.world.files["notes/plan.md"] = "PLAN-V1 " + std::string(1500, 'p') + " PLAN-V1-TAIL";
  TXB_OK(txb::runTurns(b, {txb::withTool(30, "read my plan", "read_file", R"({"path":"notes/plan.md"})", "read"),
                           txb::withTool(30, "delete it", "more", R"({"name":"delete_file","args":{"path":"notes/plan.md"}})", "Deleted."),
                           txb::chat(30, "what now?", "nothing")}));
  TEST_ASSERT_TRUE_MESSAGE(!bodyHas(b.model.last(), "PLAN-V1-TAIL"), "the read of a deleted file is still shown whole");
  size_t sup = 0;
  for (const tx::Item& it : storedOf(b)) sup += it.kind == tx::ItemKind::PointerMove && !it.sup.empty();
  TEST_ASSERT_EQUAL(1, (int)sup);
}

// D: an answer with nothing to deliver (empty content, a refusal) ends with one plain line.
inline void test_an_empty_answer_gets_one_plain_line() {
  for (int refusal = 0; refusal < 2; ++refusal) {
    txb::Battery b;
    sp::Step s = sp::say("");
    if (refusal) s.finish = sp::Finish::Refusal;
    b.model.push(s);
    plain::TurnOutcome out;
    TXB_OK(b.owner("hello", out));
    TEST_ASSERT_EQUAL(1, (int)b.sent.size());
    TEST_ASSERT_TRUE(out.status == plain::StatusLine::ProviderError);
    TEST_ASSERT_EQUAL_STRING(out.statusText.c_str(), b.sent[0].text.c_str());
    TEST_ASSERT_TRUE(b.sent[0].text.find(refusal ? texts::device_notes::kRefused : texts::device_notes::kUnreadable) !=
                     std::string::npos);
    TEST_ASSERT_EQUAL_STRING("empty", lastEnd(storedOf(b))->end.cause.c_str());
  }
}

// E: a refusal after the reply went out sends nothing more and still closes the turn.
inline void test_a_refused_allocation_after_the_reply_keeps_the_reply() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->afterDeliver(plain::HookSlot::Observer, [](const plain::DeliveredCtx&) { throw std::bad_alloc(); }));
  b.model.push(sp::say("Here it is."));
  plain::TurnOutcome out;
  const tx::Status st = b.owner("hi", out);
  TEST_ASSERT_TRUE(st.err == tx::Err::TooLarge);
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("Here it is.", b.sent[0].text.c_str());
  TEST_ASSERT_TRUE(out.delivered);
  TEST_ASSERT_EQUAL(1, (int)countKind(storedOf(b), tx::ItemKind::TurnEnd));
}

// F: rung 3 sizes turns by what they render, not by their stored lines (t, d and e lines
// render nothing): 65 short turns, then a 40 KB read in the open turn. Sized by stored lines
// it would free too little and end with the size line; it fits and answers.
inline void test_rung_three_sizes_turns_by_what_they_render() {
  txb::Battery b;
  for (int i = 0; i < 65; ++i) {
    b.model.push(sp::say("ok " + std::to_string(i)));
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("short turn " + std::to_string(i) + " " + std::string(600, 's'), out));
  }
  b.model.push(sp::call("big_read", R"({"tag":"last","bytes":40000})"));
  b.model.push(sp::say("Read it."));
  b.advance(60);
  plain::TurnOutcome out;
  TXB_OK(b.owner("read the big one", out));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Read it.", out.text.c_str(), out.statusText.c_str());
  TEST_ASSERT_TRUE(b.model.last().body.size() <= tx::kFloorHeadBodyCap);
  const long named = txb::detail::omittedTurnsNamed(b.model.last().view);
  TEST_ASSERT_TRUE_MESSAGE(named > 0 && named < 20, "rung 3 omitted none, or far more than needed");
}

// G: an item the arena refuses (here an owner message over the line limit) ends the turn with
// a line that says what happened; the chat goes on.
inline void test_an_item_the_arena_refuses_ends_the_turn_with_a_true_line() {
  txb::Battery b;
  plain::TurnOutcome out;
  TXB_OK(b.boot());
  // A full arena: the conversation's own memory refuses the turn's items.
  TXB_OK(b.arena->init(128, b.platform.allocLarge, b.platform.freeLarge));
  const tx::Status st = b.owner("hello there, a normal message", out);
  TEST_ASSERT_FALSE(st.ok());
  TEST_ASSERT_EQUAL(0, (int)b.model.requests().size());
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kNoMemory, b.sent[0].text.c_str());
  TXB_OK(b.arena->init(tx::kActiveChatArenaBytes, b.platform.allocLarge, b.platform.freeLarge));
  b.model.push(sp::say("Hi."));
  b.advance(30);
  TXB_OK(b.owner("hello", out));
  TEST_ASSERT_EQUAL_STRING("Hi.", out.text.c_str());
}

// ---- the stack review, second batch (coordinator, 19 Nov): H to L --------------------------------
// H: the tier rules at capture. A 12 KB argument renders as its stub from the next request on
// (the call still ran with the whole argument); a 70 KB argument and a 61 KB reply never end the
// turn as a storage failure, and the reply goes out whole.
inline void test_a_long_argument_is_stored_as_its_stub() {
  txb::Battery b;
  const std::string big = std::string(12000, 'k') + "-ARG-TAIL";
  b.model.push(sp::call("write_file", "{\"path\":\"notes/big.md\",\"text\":\"" + big + "\"}"));
  b.model.push(sp::say("Saved."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("save it", out));
  TEST_ASSERT_EQUAL_STRING(big.c_str(), b.world.files["notes/big.md"].c_str());   // ran whole
  TEST_ASSERT_TRUE_MESSAGE(!bodyHas(b.model.last(), "-ARG-TAIL"), "the long argument is re-sent whole");
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "[stub 12009B sha:"));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "notes/big.md"));   // the short argument stays
}

inline void test_a_huge_argument_and_a_huge_reply_keep_the_turn() {
  txb::Battery b;
  // 63 KB: over the stored line's limit and under the 65,535 B string the JSON parsers read.
  b.model.push(sp::call("write_file", "{\"path\":\"notes/huge.md\",\"text\":\"" + std::string(63000, 'h') + "\"}"));
  const std::string reply = std::string(61000, 'r') + " END";
  b.model.push(sp::say(reply));
  plain::TurnOutcome out;
  TXB_OK(b.owner("save a lot", out));
  TEST_ASSERT_EQUAL(1, b.world.runsOf("write_file"));
  TEST_ASSERT_TRUE(out.delivered);
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_TRUE_MESSAGE(b.sent[0].text == reply, "the reply was not delivered whole");
  TEST_ASSERT_EQUAL_STRING("reply", lastEnd(storedOf(b))->end.cause.c_str());
  b.model.push(sp::say("ok"));
  TXB_OK(b.owner("next", out));
  TEST_ASSERT_TRUE(b.model.last().body.find(" END") == std::string::npos);   // the reply is a stub now
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "[stub assistant r"));
}

// I: results pair with their call by q, not by id: a dialect that reuses ids still gets each
// write's superseded reads right.
inline void test_results_pair_by_the_calls_q_when_ids_repeat() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "A1 " + std::string(1500, 'a') + " A-TAIL";
  b.world.files["notes/b.md"] = "B1 " + std::string(1500, 'b') + " B-TAIL";
  b.model.push(sp::calls({{"read_file", R"({"path":"notes/a.md"})", "r1"}, {"read_file", R"({"path":"notes/b.md"})", "r2"}}));
  b.model.push(sp::say("read"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read both", out));
  sp::Step w1 = sp::call("write_file", R"({"path":"notes/a.md","text":"A2"})");
  w1.calls[0].id = "dup";
  sp::Step w2 = sp::call("write_file", R"({"path":"notes/b.md","text":"B2"})");
  w2.calls[0].id = "dup";
  b.model.push(w1);
  b.model.push(w2);
  b.model.push(sp::say("both written"));
  b.advance(30);
  TXB_OK(b.owner("write both", out));
  std::vector<uint64_t> sup;
  for (const tx::Item& it : storedOf(b))
    if (it.kind == tx::ItemKind::PointerMove) sup.insert(sup.end(), it.sup.begin(), it.sup.end());
  TEST_ASSERT_EQUAL_MESSAGE(2, (int)sup.size(), "a write with a reused call id lost its superseded read");
}

// J: a gap note the card refused stays owed: once storage is back one is stored.
inline void test_the_gap_note_is_stored_once_storage_returns() {
  txb::Battery b;
  TXB_OK(b.boot());
  b.world.files["notes/a.md"] = "alpha";
  TXB_OK(b.hooks->beforeDispatch(plain::HookSlot::Policy, [&b](const plain::DispatchCtx&) {
    b.files.refuseAll = !b.files.refuseAll;   // the first call's result and the note are refused
    return plain::DispatchDecision{};
  }));
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  b.model.push(sp::say("done"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read twice", out));
  size_t gaps = 0;
  for (const tx::Item& it : storedOf(b)) gaps += it.kind == tx::ItemKind::DeviceNote && it.note == tx::NoteClass::Gap;
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)gaps, "the stored transcript never names the hole");
}

// K: a held reply is never lost: when the extra round fails, it goes out with the plain line.
inline void test_a_held_reply_survives_a_failed_extra_round() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDeliver(plain::HookSlot::Truth, [](const plain::DeliverCtx& c) {
    plain::DeliverDecision d;
    if (!c.held) {
      d.verdict = plain::DeliverVerdict::Hold;
      d.deviceNote = "[device] effects this turn: none.";
    }
    return d;
  }));
  b.model.push(sp::say("Logged it."));
  b.model.respondWith([](const sp::Recorded&, size_t) { return sp::httpError(500); });
  plain::TurnOutcome out;
  TXB_OK(b.owner("log it", out));
  TEST_ASSERT_EQUAL(2, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("Logged it.", b.sent[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING(out.statusText.c_str(), b.sent[1].text.c_str());
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::ProviderError);
  const std::vector<tx::Item> items = storedOf(b);
  TEST_ASSERT_EQUAL_STRING("provider:server", lastEnd(items)->end.cause.c_str());
  // One a item: the held reply, stored as interim text when it was held (fin false); the d item
  // records that it went out.
  TEST_ASSERT_EQUAL(1, (int)countKind(items, tx::ItemKind::Assistant));
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::Assistant) TEST_ASSERT_FALSE(it.fin);
  TEST_ASSERT_EQUAL(1, (int)countKind(items, tx::ItemKind::Delivery));
}

// L: a status line a seam asks for is sent after the reply; a cut answer is shown to the seam.
inline void test_a_seam_status_line_goes_out_and_a_cut_reaches_the_seam() {
  txb::Battery b;
  TXB_OK(b.boot());
  int cuts = 0;
  TXB_OK(b.hooks->beforeDeliver(plain::HookSlot::Truth, [&cuts](const plain::DeliverCtx& c) {
    cuts += c.cut;
    plain::DeliverDecision d;
    d.status = plain::StatusLine::FailedWrite;
    d.statusText = "receipt line";
    return d;
  }));
  b.model.push(sp::say("Done."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("do it", out));
  TEST_ASSERT_EQUAL(2, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("receipt line", b.sent[1].text.c_str());
  b.model.respondWith([](const sp::Recorded&, size_t) { return sp::cut("half"); });
  TXB_OK(b.owner("again", out));
  TEST_ASSERT_EQUAL(1, cuts);
}

// The mutation pass's survivors after A to G: a router call that reaches nothing reads as
// outside; a pure tool through more needs no intent; a reply in hand when the send itself is
// refused memory still goes out; rung 3 sizes by what renders (one climb) and climbs again
// when a later round grows the body.
inline void test_more_resolution_edges() {
  txb::Battery b;
  withMore(b);
  b.world.files["notes/a.md"] = "alpha";
  b.model.push(sp::calls({{"more", R"({"name":"not_a_tool","args":{}})", ""},
                          {"more", R"({"name":"read_file","args":{"path":"notes/a.md"}})", ""}}));
  b.model.push(sp::say("ok"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("try", out));
  const std::vector<tx::Item> items = storedOf(b);
  // Only the unresolved call takes an intent (the fail-closed class is an effect), not the read.
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)countKind(items, tx::ItemKind::Intent), "a pure read through more took an intent");
  size_t outside = 0;
  for (const tx::Item& it : items) outside += it.kind == tx::ItemKind::Result && it.cls == tx::ResultClass::Outside;
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)outside, "an unresolved router call is not outside (or a read is)");
}

inline void test_a_refused_send_still_delivers_the_reply_in_hand() {
  txb::Battery b;
  TXB_OK(b.boot());
  int calls = 0;
  b.channel.send = [&](const std::string& chat, const std::string& text) {
    if (calls++ == 0) throw std::bad_alloc();
    b.sent.push_back(txb::SentMessage{chat, text});
    return true;
  };
  b.model.push(sp::say("The answer."));
  plain::TurnOutcome out;
  (void)b.owner("q", out);
  // The refused send is retried by the refusal path, with the memory line after it.
  TEST_ASSERT_EQUAL(2, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("The answer.", b.sent[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kNoMemory, b.sent[1].text.c_str());
}

inline size_t ladderEvents(txb::Battery& b) {
  const std::vector<tx::Item> items = storedOf(b);   // named: the e item points into it
  size_t n = 0;
  for (const std::string& e : lastEnd(items)->end.events) n += e == "change:ladder";
  return n;
}

inline void shortTurns(txb::Battery& b, int n) {
  for (int i = 0; i < n; ++i) {
    b.model.push(sp::say("ok " + std::to_string(i)));
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("short turn " + std::to_string(i) + " " + std::string(600, 's'), out));
  }
}

inline void test_rung_three_climbs_once_when_sized_and_again_when_the_body_grows() {
  {
    txb::Battery b;
    shortTurns(b, 65);
    b.model.push(sp::call("big_read", R"({"tag":"one","bytes":40000})"));
    b.model.push(sp::say("Read it."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read the big one", out));
    TEST_ASSERT_EQUAL_STRING("Read it.", out.text.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)ladderEvents(b), "rung 3 needed more than one climb: sized by stored lines");
  }
  {
    txb::Battery b;
    shortTurns(b, 80);
    b.model.push(sp::call("big_read", R"({"tag":"one","bytes":40000})"));
    b.model.push(sp::call("big_read", R"({"tag":"two","bytes":20000})"));
    b.model.push(sp::say("Read both."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read two big ones", out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Read both.", out.text.c_str(), out.statusText.c_str());
    TEST_ASSERT_TRUE(ladderEvents(b) >= 2);
  }
}

// ---- ruling 56: the check of the review fixes --------------------------------------------------
// 1. The allocator hook (ruling 30 and 56): a refusal at every allocation of a whole turn (a
// tool round and a reply), from before the first request to the copy-out of the outcome. No
// std::bad_alloc leaves runTurn, nothing is sent twice, and the next turn answers paired.
namespace alloc_hook {
inline bool armed = false;
inline long refuseAt = -1;
inline long count = 0;
inline void arm(long at) {
  count = 0;
  refuseAt = at;
  armed = true;
}
inline void disarm() { armed = false; }
}  // namespace alloc_hook

inline void hookTurn(txb::Battery& b, plain::TurnOutcome& out, plain::TurnInput& in, bool& escaped) {
  escaped = false;
  try {
    (void)b.engine->runTurn(in, out);
  } catch (const std::bad_alloc&) {
    escaped = true;
  }
}

inline void test_a_refusal_at_every_allocation_of_a_turn_never_leaves_runTurn() {
  // How many allocations a whole turn makes.
  long total = 0;
  {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    TXB_OK(b.boot());
    b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
    b.model.push(sp::say("It says alpha."));
    plain::TurnInput in;
    in.ctx.chatKey = in.ctx.chatId = b.chatKey;
    in.ctx.who = &b.who;
    in.ctx.host = "api.anthropic.com";
    in.ctx.model = "claude-sonnet-5-5";
    in.text = "what does a say?";
    plain::TurnOutcome out;
    bool escaped = false;
    alloc_hook::arm(-1);
    hookTurn(b, out, in, escaped);
    total = alloc_hook::count;
    alloc_hook::disarm();
    TEST_ASSERT_EQUAL_STRING("It says alpha.", out.text.c_str());
  }
  TEST_ASSERT_TRUE(total > 50);
  for (long k = 0; k < total; ++k) {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    TXB_OK(b.boot());
    b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
    b.model.push(sp::say("It says alpha."));
    plain::TurnInput in;
    in.ctx.chatKey = in.ctx.chatId = b.chatKey;
    in.ctx.who = &b.who;
    in.ctx.host = "api.anthropic.com";
    in.ctx.model = "claude-sonnet-5-5";
    in.text = "what does a say?";
    plain::TurnOutcome out;
    bool escaped = false;
    alloc_hook::arm(k);
    hookTurn(b, out, in, escaped);
    alloc_hook::disarm();
    char msg[96];
    std::snprintf(msg, sizeof msg, "a refusal at allocation %ld of %ld", k, total);
    TEST_ASSERT_FALSE_MESSAGE(escaped, msg);
    size_t replies = 0;
    for (const txb::SentMessage& m : b.sent) replies += m.text == "It says alpha.";
    TEST_ASSERT_TRUE_MESSAGE(replies <= 1, msg);
    TEST_ASSERT_TRUE_MESSAGE(b.sent.size() <= 2, msg);
    // The chat goes on: the next turn answers, and its request is paired.
    plain::TurnOutcome next;   // the steps the refused turn did not use come first, then "ok"
    b.model.push(sp::say("ok"));
    in.text = "next";
    in.ctx.turn = 0;
    TXB_OK(b.engine->runTurn(in, next));
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(b.model.last().view, why), (std::string(msg) + ": " + why).c_str());
  }
}

// 2. A tool handler that throws is an error result for that one call; the turn goes on.
inline void test_a_throwing_tool_is_one_failed_call() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  b.model.push(sp::calls({{"throw_tool", "{}", ""}, {"read_file", R"({"path":"notes/a.md"})", ""}}));
  b.model.push(sp::say("One failed, a says alpha."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("try both", out));
  TEST_ASSERT_EQUAL_STRING("One failed, a says alpha.", out.text.c_str());
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), texts::tool_results::kToolFailed));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "alpha"));
  expectPaired(b);
}

// 3. One place decides what the owner gets.
inline void test_the_last_round_with_only_calls_ends_with_the_limit_line() {
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  b.model.respondWith([](const sp::Recorded& r, size_t) {
    return sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(r.step) + "}");
  });
  plain::TurnOutcome out;
  TXB_OK(b.owner("keep reading", out));
  TEST_ASSERT_EQUAL_STRING("none", b.model.last().view.toolChoice.c_str());
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kLimitLine, b.sent[0].text.c_str());
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::LimitReached);
  TEST_ASSERT_EQUAL_STRING("limit:rounds", lastEnd(storedOf(b))->end.cause.c_str());
}

inline void holdOnce(txb::Battery& b) {
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDeliver(plain::HookSlot::Truth, [](const plain::DeliverCtx& c) {
    plain::DeliverDecision d;
    if (!c.held && !c.cut) {
      d.verdict = plain::DeliverVerdict::Hold;
      d.deviceNote = "[device] effects this turn: none.";
    }
    return d;
  }));
}

inline void test_a_held_reply_survives_a_cut_or_empty_extra_round() {
  for (int mode = 0; mode < 2; ++mode) {
    txb::Battery b;
    holdOnce(b);
    b.model.push(sp::say("Logged it."));
    if (mode == 0) b.model.respondWith([](const sp::Recorded&, size_t) { return sp::cut(""); });
    else b.model.push(sp::say(""));
    plain::TurnOutcome out;
    TXB_OK(b.owner("log it", out));
    TEST_ASSERT_TRUE(b.sent.size() >= 1);
    TEST_ASSERT_EQUAL_STRING("Logged it.", b.sent[0].text.c_str());
    TEST_ASSERT_TRUE(out.delivered);
  }
}

inline void test_a_failed_write_receipt_is_never_replaced_by_a_weaker_line() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDeliver(plain::HookSlot::Truth, [](const plain::DeliverCtx&) {
    plain::DeliverDecision d;
    d.status = plain::StatusLine::FailedWrite;
    d.statusText = "receipt line";
    return d;
  }));
  b.model.push(sp::say(""));   // an empty answer would have the provider's line
  plain::TurnOutcome out;
  TXB_OK(b.owner("save it", out));
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("receipt line", b.sent[0].text.c_str());
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::FailedWrite);
}

// 4. Rung 2 follows the rounds of the open turn before rung 3 omits any history.
inline void test_rung_two_advances_before_history_is_omitted() {
  txb::Battery b;
  shortTurns(b, 8);
  for (int i = 0; i < 6; ++i) b.model.push(sp::call("big_read", "{\"tag\":\"r" + std::to_string(i) + "\",\"bytes\":20000}"));
  b.model.push(sp::say("Read six."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read six big ones", out));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Read six.", out.text.c_str(), out.statusText.c_str());
  for (const auto& r : b.model.requests())
    TEST_ASSERT_TRUE_MESSAGE(txb::detail::omittedTurnsNamed(r.view) < 0, "history omitted while open-turn bulk could be stubbed");
}

// 5. Tier rules: a refused bulk write, nested strings, the bulk file read back, unparsable
// arguments.
inline const tx::Item* lastCall(const std::vector<tx::Item>& items) {
  const tx::Item* c = nullptr;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::Call) c = &it;
  return c;
}

inline void test_a_refused_bulk_write_never_promises_view() {
  for (size_t bytes : {12000u, 63000u}) {
    txb::Battery b;
    TXB_OK(b.boot());
    const std::string arg = std::string(bytes, 'v');
    b.model.push(sp::call("write_file", "{\"path\":\"notes/v.md\",\"text\":\"" + arg + "\"}"));
    b.model.push(sp::say("Saved."));
    // The first bulk append (the arguments) is refused; item lines start with {"k":.
    b.files.refuseWhen = [](const char* d, size_t n) { return n > 11000 && std::string(d, std::min<size_t>(n, 8)).find("\"k\"") == std::string::npos; };
    plain::TurnOutcome out;
    TXB_OK(b.owner("save", out));
    b.files.refuseWhen = nullptr;
    const std::vector<tx::Item> items = storedOf(b);
    const tx::Item* c = lastCall(items);
    TEST_ASSERT_NOT_NULL(c);
    if (bytes == 12000u) {
      TEST_ASSERT_TRUE_MESSAGE(c->args.find(arg) != std::string::npos, "a refused bulk write lost arguments that fit inline");
    } else {
      TEST_ASSERT_TRUE_MESSAGE(c->args.find("view id=") == std::string::npos, "a stub promises view of a bulk write that was refused");
      TEST_ASSERT_TRUE(c->args.find("[stub ") != std::string::npos);
    }
    TEST_ASSERT_EQUAL_STRING("Saved.", out.text.c_str());
  }
}

inline void test_nested_long_strings_are_stubbed_and_the_bulk_holds_the_whole() {
  txb::Battery b;
  TXB_OK(b.boot());
  const std::string a1 = std::string(2000, 'x') + "-NESTED-1", a2 = std::string(3000, 'y') + "-NESTED-2";
  const std::string args = "{\"path\":\"notes/n.md\",\"parts\":[\"" + a1 + "\",{\"deep\":\"" + a2 + "\"}]}";
  // Cut at the c line's append: the bulk file must already hold the whole arguments.
  b.files.killWhen = [](const char* d, size_t n) { return std::string(d, n).find("\"k\":\"c\"") != std::string::npos; };
  b.model.push(sp::call("append_file", args));
  plain::TurnOutcome out;
  (void)b.owner("append", out);
  TEST_ASSERT_TRUE(b.killed);
  bool whole = false;
  for (const auto& kv : b.files.files) whole |= kv.first.find(".txb") != std::string::npos && kv.second.find(args) != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(whole, "the bulk file does not hold the whole arguments before the c line");
  txb::Battery k;
  k.model.push(sp::call("append_file", args));
  k.model.push(sp::say("ok"));
  TXB_OK(k.owner("append", out));
  const std::vector<tx::Item> items = storedOf(k);
  const tx::Item* c = lastCall(items);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_TRUE_MESSAGE(c->args.find("-NESTED-1") == std::string::npos && c->args.find("-NESTED-2") == std::string::npos,
                           "a nested long string is stored whole");
  TEST_ASSERT_TRUE(c->args.find("notes/n.md") != std::string::npos);
}

inline void test_unparsable_long_arguments_are_one_stub() {
  txb::Battery b(plain::Dialect::ChatCompletions);
  sp::Step s = sp::call("write_file", "{\"path\":\"notes/h.md\",\"text\":\"" + std::string(63000, 'h'));   // cut JSON
  b.model.push(s);
  b.model.push(sp::say("It was cut."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("write it", out));
  TEST_ASSERT_EQUAL_STRING("It was cut.", out.text.c_str());
  const std::vector<tx::Item> items = storedOf(b);
  const tx::Item* c = lastCall(items);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)c->args.rfind("[stub ", 0), "unparsable long arguments are not one stub");
  TEST_ASSERT_EQUAL(0, b.world.runsOf("write_file"));
}

// 6. An owner message over the line limit: its own true line, not "out of memory".
inline void test_an_owner_message_over_the_line_limit_gets_its_own_line() {
  txb::Battery b;
  plain::TurnOutcome out;
  const tx::Status st = b.owner(std::string(70000, 'x'), out);
  TEST_ASSERT_FALSE(st.ok());
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kMessageTooLong, b.sent[0].text.c_str());
}

// 8. The not-stored note renders once, also after storage returns.
inline void test_the_not_stored_note_renders_once() {
  txb::Battery b;
  TXB_OK(b.boot());
  b.world.files["notes/a.md"] = "alpha";
  TXB_OK(b.hooks->beforeDispatch(plain::HookSlot::Policy, [&b](const plain::DispatchCtx&) {
    b.files.refuseAll = !b.files.refuseAll;
    return plain::DispatchDecision{};
  }));
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));   // refused: the episode starts
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));   // stored: storage is back
  b.model.push(sp::say("done"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read twice", out));
  const std::string& body = b.model.last().body;
  size_t n = 0;
  for (size_t at = body.find(texts::device_notes::kNotStored); at != std::string::npos;
       at = body.find(texts::device_notes::kNotStored, at + 1))
    ++n;
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)n, "the model sees the not-stored note more than once");
}

// ---- red cases for other lanes (ruling 56; on test/pending_roster.txt with ruling:56) ---------
inline void test_a_full_arena_frees_its_oldest_closed_turn() {
  TXB_WAITS_FOR("PENDING RB-E1b", "RB-E1a");
  txb::Battery b;
  shortTurns(b, 3);
  TXB_OK(b.arena->init(4096, b.platform.allocLarge, b.platform.freeLarge));
  shortTurns(b, 6);   // each turn must still answer: the oldest closed turns give way
  TEST_ASSERT_EQUAL(9, (int)b.sent.size());
}

inline void test_view_fetches_a_long_argument_and_a_long_reply_back() {
  TXB_WAITS_FOR("PENDING RB-N3", "RB-E1a");
  txb::Battery b;
  b.model.push(sp::call("write_file", "{\"path\":\"notes/l.md\",\"text\":\"" + std::string(5000, 'l') + "\"}"));
  b.model.push(sp::say(std::string(13000, 'r')));
  plain::TurnOutcome out;
  TXB_OK(b.owner("save", out));
  for (const tx::Item& it : storedOf(b))
    if (it.kind == tx::ItemKind::Call || it.kind == tx::ItemKind::Assistant)
      TEST_ASSERT_TRUE_MESSAGE(!it.handle.empty(), "a stubbed argument or reply has no bulk handle for view");
}

inline void test_a_long_owner_message_goes_to_the_inbox_as_a_file() {
  TXB_WAITS_FOR("PENDING RB-TG1", "RB-E1a");
  txb::Battery b;
  b.model.push(sp::say("Read the file."));
  plain::TurnOutcome out;
  TXB_OK(b.owner(std::string(70000, 'p'), out));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "inbox/"));
}

inline void test_signed_thinking_over_the_line_limit_keeps_the_turn() {
  TXB_WAITS_FOR("PENDING RB-W1b", "RB-E1a");
  txb::Battery b;
  sp::Step s = sp::call("read_file", R"({"path":"notes/a.md"})");
  s.thinking = std::string(63000, 't');
  b.model.push(s);
  b.model.push(sp::say("done"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("think hard", out));
  TEST_ASSERT_EQUAL_STRING("done", out.text.c_str());
}

inline void test_guard_inputs_measure_size_and_spend() {
  TXB_WAITS_FOR("PENDING RB-E1b", "RB-E1a");
  txb::Battery b;
  sp::Step s = sp::say("hi");
  s.usage.cacheRead = 9000;
  b.model.push(s);
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello", out));
  TEST_ASSERT_TRUE_MESSAGE(out.end.promptTokens >= 9000, "the size guard does not count cached prompt tokens");
  TEST_ASSERT_TRUE_MESSAGE(out.end.microUsd > 0, "the spend guard has no price");
}

inline void test_a_routed_head_sends_the_deadline_header() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-E1a");
  txb::Battery b(plain::Dialect::Flat);
  b.model.push(sp::say("hi"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello", out));
  bool header = false;
  for (const auto& h : b.model.last().req.headers) header |= h.first.find("eadline") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(header, "a routed request carries no deadline header");
}

inline void test_the_log_reopens_when_storage_returns() {
  TXB_WAITS_FOR("PENDING RB-I3", "RB-E1a");
  txb::Battery b;
  b.files.refuseAll = true;
  TXB_OK(b.boot());
  b.files.refuseAll = false;
  b.model.push(sp::say("hi"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello", out));
  TEST_ASSERT_TRUE_MESSAGE(!storedOf(b).empty(), "storage came back and nothing was stored");
}

// ---- the sentinel ----------------------------------------------------------------------------
inline void test_an_empty_engine_answers_a_typed_outcome() {
  plain::PlainEngine e{plain::EngineDeps{}};
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(e.runTurn(plain::TurnInput{}, out).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(e.boot(plain::BootCtx{}).err == tx::Err::InvalidArgument);
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(e.idle(plain::IdleCtx{}, io).err == tx::Err::InvalidArgument);
  TEST_ASSERT_NULL(e.head());
}

inline void run() {
  RUN_TEST(test_loop_shape_on_each_dialect_stub);
  RUN_TEST(test_a_tool_less_reply_is_one_request_and_one_message);
  RUN_TEST(test_a_cut_answer_is_delivered_with_the_cut_line_and_its_calls_never_run);
  RUN_TEST(test_an_exchange_error_of_each_class_ends_the_turn_with_one_plain_line);
  RUN_TEST(test_an_effect_runs_only_when_its_intent_is_stored);
  RUN_TEST(test_no_card_runs_from_ram_and_never_runs_an_effect);
  RUN_TEST(test_a_restart_mid_turn_replays_the_fill_and_runs_nothing_twice);
  RUN_TEST(test_the_fill_after_a_restart_renders_the_same_transcript);
  RUN_TEST(test_a_turn_at_the_round_budget_gets_one_note_and_one_last_round);
  RUN_TEST(test_a_turn_at_the_wall_clock_ends_inside_the_hard_wall);
  RUN_TEST(test_a_hung_provider_ends_the_turn_inside_its_wall_clock);
  RUN_TEST(test_identical_state_gives_byte_identical_requests);
  RUN_TEST(test_every_seam_is_called_at_its_point);
  RUN_TEST(test_boot_runs_the_resume_seam_once);
  RUN_TEST(test_a_refused_allocation_ends_the_turn_with_a_status_never_an_abort);
  RUN_TEST(test_an_effect_through_more_needs_its_intent);
  RUN_TEST(test_a_result_lost_to_the_card_is_closed_after_a_restart);
  RUN_TEST(test_no_call_starts_past_the_soft_wall);
  RUN_TEST(test_a_late_round_gets_only_the_time_left);
  RUN_TEST(test_rung_three_omits_only_the_turns_the_request_needs);
  RUN_TEST(test_a_size_refusal_may_omit_every_omissible_turn);
  RUN_TEST(test_outside_content_through_more_taints_the_turn);
  RUN_TEST(test_a_delete_through_more_rebuilds_the_recent_files);
  RUN_TEST(test_a_write_through_more_supersedes_the_earlier_read);
  RUN_TEST(test_an_empty_answer_gets_one_plain_line);
  RUN_TEST(test_a_refused_allocation_after_the_reply_keeps_the_reply);
  RUN_TEST(test_rung_three_sizes_turns_by_what_they_render);
  RUN_TEST(test_an_item_the_arena_refuses_ends_the_turn_with_a_true_line);
  RUN_TEST(test_a_long_argument_is_stored_as_its_stub);
  RUN_TEST(test_a_huge_argument_and_a_huge_reply_keep_the_turn);
  RUN_TEST(test_results_pair_by_the_calls_q_when_ids_repeat);
  RUN_TEST(test_the_gap_note_is_stored_once_storage_returns);
  RUN_TEST(test_a_held_reply_survives_a_failed_extra_round);
  RUN_TEST(test_a_seam_status_line_goes_out_and_a_cut_reaches_the_seam);
  RUN_TEST(test_more_resolution_edges);
  RUN_TEST(test_a_refused_send_still_delivers_the_reply_in_hand);
  RUN_TEST(test_rung_three_climbs_once_when_sized_and_again_when_the_body_grows);
  RUN_TEST(test_a_refusal_at_every_allocation_of_a_turn_never_leaves_runTurn);
  RUN_TEST(test_a_throwing_tool_is_one_failed_call);
  RUN_TEST(test_the_last_round_with_only_calls_ends_with_the_limit_line);
  RUN_TEST(test_a_held_reply_survives_a_cut_or_empty_extra_round);
  RUN_TEST(test_a_failed_write_receipt_is_never_replaced_by_a_weaker_line);
  RUN_TEST(test_rung_two_advances_before_history_is_omitted);
  RUN_TEST(test_a_refused_bulk_write_never_promises_view);
  RUN_TEST(test_nested_long_strings_are_stubbed_and_the_bulk_holds_the_whole);
  RUN_TEST(test_unparsable_long_arguments_are_one_stub);
  RUN_TEST(test_an_owner_message_over_the_line_limit_gets_its_own_line);
  RUN_TEST(test_the_not_stored_note_renders_once);
  RUN_TEST(test_a_full_arena_frees_its_oldest_closed_turn);
  RUN_TEST(test_view_fetches_a_long_argument_and_a_long_reply_back);
  RUN_TEST(test_a_long_owner_message_goes_to_the_inbox_as_a_file);
  RUN_TEST(test_signed_thinking_over_the_line_limit_keeps_the_turn);
  RUN_TEST(test_guard_inputs_measure_size_and_spend);
  RUN_TEST(test_a_routed_head_sends_the_deadline_header);
  RUN_TEST(test_the_log_reopens_when_storage_returns);
  RUN_TEST(test_an_empty_engine_answers_a_typed_outcome);
}

}  // namespace engine_tests
