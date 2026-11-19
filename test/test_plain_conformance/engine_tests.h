#pragma once
// The loop core's own cases (lane RB-E1a), run from main.cpp beside the battery's: scripted
// turns through the plain engine with the stand-in wire of each dialect (txb::StubWire, or
// the real wire once it lands). A tool-less reply, tool rounds, a cut answer, an exchange
// error of each class, a store that answers not stored or has no card, a restart in the
// middle of a turn, the round budget and the wall clock, byte-identical requests, every
// seam at its point, a refused allocation.
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <functional>
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
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "[2026-11-17 Tue 13:02] hello"));
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
  // Compaction holds its own slot now (RB-C2); the idle seam's later slot is free.
  TXB_OK(h.idleStep(plain::HookSlot::ShadowProbe, [&](const plain::IdleCtx&) {
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
  TEST_ASSERT_TRUE(idle.worked && idle.by == plain::HookSlot::ShadowProbe);
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

// Rung 3's estimate is never above what the stand-in wire renders, for every kind of item a
// closed turn holds (the file tools' receipts, an error result, an outside result's stub, a call
// whose long argument is stored as its stub, a plain chat): sized by it, the omission frees what
// the body is over in one climb. An estimate above the real bytes (stored lines: keys, a stub
// copy beside a receipt) omits too few turns and needs a second climb (rb-main 6180ffd8d).
inline void test_rung_three_estimate_never_exceeds_the_rendered_bytes_of_any_item_kind() {
  struct Kind {
    const char* name;
    std::function<void(txb::Battery&, int)> turn;
  };
  const std::string body(600, 's');
  const std::vector<Kind> kinds = {
      {"chat", [&](txb::Battery& b, int i) { b.model.push(sp::say("ok " + std::to_string(i))); }},
      {"a file receipt",
       [&](txb::Battery& b, int i) {
         b.model.push(sp::call("read_file", R"({"path":"notes/r.md"})"));
         b.model.push(sp::say("read " + std::to_string(i)));
       }},
      {"an error result",
       [&](txb::Battery& b, int i) {
         b.model.push(sp::call("read_file", "{\"path\":\"notes/missing-" + std::to_string(i) + ".md\"}"));
         b.model.push(sp::say("missing " + std::to_string(i)));
       }},
      {"an outside result",
       [&](txb::Battery& b, int i) {
         b.model.push(sp::call("web_search", "{\"q\":\"w" + std::to_string(i) + "\",\"bytes\":3000}"));
         b.model.push(sp::say("searched " + std::to_string(i)));
       }},
      {"a long argument",
       [&](txb::Battery& b, int i) {
         b.model.push(sp::call("write_file", "{\"path\":\"notes/w" + std::to_string(i) + ".md\",\"text\":\"" +
                                                 std::string(1500, 'w') + "\"}"));
         b.model.push(sp::say("wrote " + std::to_string(i)));
       }},
  };
  for (const Kind& k : kinds) {
    txb::Battery b;
    b.world.files["notes/r.md"] = std::string(900, 'r');
    // History up to just under the cap, so no request before the big read climbs.
    for (int i = 0; i < 400 && (b.model.requests().empty() || b.model.last().body.size() < 80000); ++i) {
      k.turn(b, i);
      b.advance(60);
      plain::TurnOutcome out;
      TXB_OK(b.owner("turn " + std::to_string(i) + " " + body, out));
      TEST_ASSERT_EQUAL_MESSAGE(0, (int)ladderEvents(b), k.name);
    }
    b.model.push(sp::call("big_read", R"({"tag":"one","bytes":40000})"));
    b.model.push(sp::say("Read it."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read the big one", out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Read it.", out.text.c_str(), k.name);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)ladderEvents(b), k.name);
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
inline bool from = false;      // refuse refuseAt and every allocation after it
inline size_t minBytes = 0;    // refuse every allocation of minBytes or more (0: off)
inline int paused = 0;         // the test's own bookkeeping (the recording channel) is not refused
inline void arm(long at) {
  count = 0;
  refuseAt = at;
  from = false;
  minBytes = 0;
  armed = true;
}
inline void armFrom(long at) {
  arm(at);
  from = true;
}
inline void armSize(size_t bytes) {
  arm(-1);
  minBytes = bytes;
}
inline void disarm() { armed = false; }
inline bool refuse(size_t n) {
  if (!armed || paused) return false;
  const long i = count++;
  if (minBytes) return n >= minBytes;
  return from ? refuseAt >= 0 && i >= refuseAt : i == refuseAt;
}
struct Pause {
  Pause() { ++paused; }
  ~Pause() { --paused; }
};
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
    TEST_ASSERT_TRUE_MESSAGE(countKind(storedOf(b), tx::ItemKind::Delivery) <= 1, msg);   // the d item never twice
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
    // The held reply went out: the turn's cause says so (ruling 59 item 5), never "empty".
    TEST_ASSERT_EQUAL_STRING(mode == 0 ? "cut" : "reply", lastEnd(storedOf(b))->end.cause.c_str());
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

// ---- ruling 59: what the narrow check of round three left open (lane RB-E1b) -----------------
// 1. A refused allocation is a designed state that always ends with the reply the model produced
// or one plain memory line. The hook refuses "allocation k and every one after it" and "every
// allocation of S bytes or more" over five turn shapes: a reply past the small-string size after
// a tool round, a reply past 12,288 B, a held reply, a cut reply, a seam's status line. In every
// run: nothing leaves runTurn, the owner got the model's reply or exactly one memory line, no
// message went out twice, and afterDeliver ran once whenever the reply went out.
struct RefusalShape {
  const char* name;
  std::function<void(txb::Battery&)> setup;   // after boot: the scripted answers and the seams
  std::vector<std::string> replies;           // what the model produced that may go out
};

inline std::vector<RefusalShape> refusalShapes() {
  const std::string over = "Past the small-string size: " + std::string(200, 'a');
  const std::string huge = std::string(13000, 'b') + " END";
  const std::string held = "Held reply " + std::string(100, 'h');
  const std::string fin = "Final reply " + std::string(100, 'f');
  const std::string cut = "Cut reply " + std::string(100, 'c');
  return {
      {"a tool round and a reply past the small-string size",
       [over](txb::Battery& b) {
         b.world.files["notes/a.md"] = "alpha";
         b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
         b.model.push(sp::say(over));
       },
       {over}},
      {"a reply past 12,288 B", [huge](txb::Battery& b) { b.model.push(sp::say(huge)); }, {huge}},
      {"a held reply",
       [held, fin](txb::Battery& b) {
         TXB_OK(b.hooks->beforeDeliver(plain::HookSlot::Truth, [](const plain::DeliverCtx& c) {
           plain::DeliverDecision d;
           if (!c.held && !c.cut) {
             d.verdict = plain::DeliverVerdict::Hold;
             d.deviceNote = "[device] effects this turn: none, and a note past the small-string size.";
           }
           return d;
         }));
         b.model.push(sp::say(held));
         b.model.push(sp::say(fin));
       },
       {held, fin}},
      {"a cut reply", [cut](txb::Battery& b) { b.model.push(sp::cut(cut)); }, {cut}},
      {"a seam's status line",
       [over](txb::Battery& b) {
         TXB_OK(b.hooks->beforeDeliver(plain::HookSlot::Truth, [](const plain::DeliverCtx&) {
           plain::DeliverDecision d;
           d.status = plain::StatusLine::FailedWrite;
           d.statusText = "Note: the file change in this reply did not go through (a receipt past the small-string size).";
           return d;
         }));
         b.model.push(sp::say(over));
       },
       {over}},
  };
}

struct RefusalRun {
  bool escaped = false;
  long allocations = 0;     // the allocations the hook saw (a counting run: all of the turn's)
  size_t replies = 0, memory = 0, dupes = 0, messages = 0, dItems = 0;
  int afterDeliverSent = 0, afterDeliverCalls = 0;
};

// mode 0 counts, 1 refuses allocation k and every one after it, 2 every allocation of k bytes or more.
inline RefusalRun runRefusal(const RefusalShape& shape, int mode, long k, bool nextTurn) {
  RefusalRun r;
  txb::Battery b;
  TXB_OK(b.boot());
  int calls = 0, sent = 0;
  TXB_OK(b.hooks->afterDeliver(plain::HookSlot::Observer, [&calls, &sent](const plain::DeliveredCtx& c) {
    ++calls;
    sent += c.sent;
  }));
  b.channel.send = [&b](const std::string& chat, const std::string& text) {
    alloc_hook::Pause p;   // the transport's own memory, not the engine's
    b.sent.push_back(txb::SentMessage{chat, text});
    return true;
  };
  shape.setup(b);
  plain::TurnInput in;
  in.ctx.chatKey = in.ctx.chatId = b.chatKey;
  in.ctx.who = &b.who;
  in.ctx.host = "api.anthropic.com";
  in.ctx.model = "claude-sonnet-5-5";
  in.text = "go";
  plain::TurnOutcome out;
  if (mode == 0) alloc_hook::arm(-1);
  else if (mode == 1) alloc_hook::armFrom(k);
  else alloc_hook::armSize((size_t)k);
  hookTurn(b, out, in, r.escaped);
  r.allocations = alloc_hook::count;
  alloc_hook::disarm();
  r.dItems = countKind(storedOf(b), tx::ItemKind::Delivery);
  r.afterDeliverCalls = calls;
  r.afterDeliverSent = sent;
  r.messages = b.sent.size();
  for (size_t i = 0; i < b.sent.size(); ++i) {
    const std::string& m = b.sent[i].text;
    r.replies += std::find(shape.replies.begin(), shape.replies.end(), m) != shape.replies.end();
    r.memory += m == texts::device_notes::kNoMemory;
    for (size_t j = 0; j < i; ++j) r.dupes += b.sent[j].text == m;
  }
  if (nextTurn) {   // the chat goes on: the next turn answers, its request paired
    b.model.push(sp::say("ok"));
    in.text = "next";
    in.ctx.turn = 0;
    plain::TurnOutcome next;
    TXB_OK(b.engine->runTurn(in, next));
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(b.model.last().view, why), why.c_str());
  }
  return r;
}

inline void expectOwnerHeard(const RefusalShape& shape, const RefusalRun& r, const char* mode, long k) {
  char msg[200];
  std::snprintf(msg, sizeof msg, "%s, refusing %s %ld: replies %u, memory lines %u, messages %u, afterDeliver %d/%d",
                shape.name, mode, k, (unsigned)r.replies, (unsigned)r.memory, (unsigned)r.messages,
                r.afterDeliverSent, r.afterDeliverCalls);
  TEST_ASSERT_FALSE_MESSAGE(r.escaped, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.replies <= 1 && r.memory <= 1, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.replies == 1 || r.memory == 1, msg);   // never silent
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)r.dupes, msg);                    // nothing sent twice
  TEST_ASSERT_TRUE_MESSAGE(r.messages <= 2, msg);
  if (r.replies) TEST_ASSERT_EQUAL_MESSAGE(1, r.afterDeliverSent, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.afterDeliverCalls <= 1, msg);
  TEST_ASSERT_TRUE_MESSAGE(r.dItems <= 1, msg);   // the d item is never stored twice
}

inline void test_a_refused_allocation_from_any_point_on_still_reaches_the_owner() {
  for (const RefusalShape& shape : refusalShapes()) {
    const RefusalRun all = runRefusal(shape, 0, -1, false);
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)all.replies, shape.name);
    TEST_ASSERT_TRUE(all.allocations > 50);
    for (long k = 0; k < all.allocations; ++k) expectOwnerHeard(shape, runRefusal(shape, 1, k, k % 97 == 0), "from", k);
  }
}

inline void test_a_refused_large_allocation_still_reaches_the_owner() {
  for (const RefusalShape& shape : refusalShapes())
    for (long s : {8L, 16L, 24L, 32L, 48L, 64L, 96L, 128L, 192L, 256L, 384L, 512L, 1024L, 2048L, 4096L, 8192L, 13000L,
                   16384L, 32768L, 65536L, 131072L})
      expectOwnerHeard(shape, runRefusal(shape, 2, s, true), "at or over", s);
}

// 2 and 3. The deliver seam: a refusal after the first part went out sends the rest and the
// memory line, never the first part again; afterDeliver runs once.
inline void test_a_refusal_inside_delivery_never_sends_a_part_twice() {
  txb::Battery b;
  int calls = 0;
  b.deliverFn = [&](const plain::DeliverInput& in, plain::DeliverOutcome& out) {
    const bool first = calls++ == 0;
    if (in.text && !in.text->empty() && !out.replyDone) {
      const size_t half = in.text->size() / 2;
      for (uint16_t part = in.fromPart; part < 2; ++part) {
        if (first && part == 1) throw std::bad_alloc();   // after the first part went out
        b.sent.push_back(txb::SentMessage{b.chatKey, part ? in.text->substr(half) : in.text->substr(0, half)});
        out.sent = true;
        out.parts = (uint16_t)(part + 1);
      }
      out.replyDone = true;
    }
    if (in.statusText && !in.statusText->empty() && !out.statusSent) {
      b.sent.push_back(txb::SentMessage{b.chatKey, *in.statusText});
      out.statusSent = true;
    }
    return tx::Status::success();
  };
  TXB_OK(b.boot());
  int after = 0;
  TXB_OK(b.hooks->afterDeliver(plain::HookSlot::Observer, [&after](const plain::DeliveredCtx& c) { after += c.sent; }));
  b.model.push(sp::say("first half|second half"));
  plain::TurnOutcome out;
  (void)b.owner("q", out);
  TEST_ASSERT_EQUAL(3, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("first half|", b.sent[0].text.substr(0, 11).c_str());
  TEST_ASSERT_EQUAL_STRING("second half", b.sent[1].text.substr(b.sent[1].text.size() - 11).c_str());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kNoMemory, b.sent[2].text.c_str());
  TEST_ASSERT_EQUAL(1, after);
  TEST_ASSERT_TRUE(out.delivered);
}

// 2. afterDeliver runs for a reply that went out even when the d item's store is refused.
inline void test_after_deliver_runs_when_the_d_item_is_refused() {
  txb::Battery b;
  TXB_OK(b.boot());
  int after = 0;
  TXB_OK(b.hooks->afterDeliver(plain::HookSlot::Observer, [&after](const plain::DeliveredCtx& c) { after += c.sent; }));
  bool armedAtSend = false;
  b.channel.send = [&](const std::string& chat, const std::string& text) {
    {
      alloc_hook::Pause p;
      b.sent.push_back(txb::SentMessage{chat, text});
    }
    if (!armedAtSend) {
      armedAtSend = true;
      alloc_hook::armFrom(0);   // everything after the send is refused: the d item first
    }
    return true;
  };
  b.model.push(sp::say("Sent before the d item."));
  plain::TurnOutcome out;
  bool escaped = false;
  plain::TurnInput in;
  in.ctx.chatKey = in.ctx.chatId = b.chatKey;
  in.ctx.who = &b.who;
  in.text = "hi";
  hookTurn(b, out, in, escaped);
  alloc_hook::disarm();
  TEST_ASSERT_FALSE(escaped);
  TEST_ASSERT_EQUAL(1, after);
  TEST_ASSERT_EQUAL_STRING("Sent before the d item.", b.sent[0].text.c_str());
}

// 4. An owner message with more parts than one stored line holds has its own true line.
inline void test_a_message_with_too_many_parts_gets_its_own_line() {
  txb::Battery b;
  std::vector<tx::Part> parts;
  for (int i = 0; i < 257; ++i) {
    tx::Part p;
    p.kind = tx::PartKind::Image;
    p.ref = "p" + std::to_string(1000 + i);
    parts.push_back(p);
  }
  plain::TurnOutcome out;
  const tx::Status st = b.owner("look at these", out, parts);
  TEST_ASSERT_FALSE(st.ok());
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kTooManyParts, b.sent[0].text.c_str());
  TEST_ASSERT_TRUE(b.sent[0].text != texts::device_notes::kNoMemory);
}

// The full arena (rulings 56 and 60): at turn open and between rounds the loop drops the oldest
// closed resident turns from RAM, names them with the omission line and raises omittedThru;
// storage keeps them, and the chat keeps answering.
inline void test_a_chat_that_filled_its_arena_keeps_answering_between_rounds() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.arena->init(16 * 1024, b.platform.allocLarge, b.platform.freeLarge));
  shortTurns(b, 4);
  TEST_ASSERT_EQUAL(0, (int)b.engine->omittedThru());
  for (int i = 0; i < 6; ++i) b.model.push(sp::call("big_read", "{\"tag\":\"m" + std::to_string(i) + "\",\"bytes\":1900}"));
  b.model.push(sp::say("Read six."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read six", out));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Read six.", out.text.c_str(), out.statusText.c_str());
  TEST_ASSERT_TRUE_MESSAGE(b.engine->omittedThru() > 0, "the dropped turns are not named to the compaction driver");
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), "earlier turns") && bodyHas(b.model.last(), "not shown"),
                           "the dropped turns are not named in the request");
  size_t owners = 0;
  for (const tx::Item& it : storedOf(b)) owners += it.kind == tx::ItemKind::User;
  TEST_ASSERT_EQUAL_MESSAGE(5, (int)owners, "storage lost a dropped turn");
  expectPaired(b);
}

// ---- the hostile review of part one (two fresh-context reviewers, lane RB-E1b) -----------------
// A turn that filled the arena and left a call without its result: the next turn answers the
// call ("outcome unknown") before a full arena drops anything, so storage keeps it paired.
inline void test_a_dangling_call_is_answered_before_a_full_arena_drops_its_turn() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.arena->init(8 * 1024, b.platform.allocLarge, b.platform.freeLarge));
  // Sized so the arena refuses a result, not a call: the turn leaves its last call dangling.
  for (const char* tag : {"a", "b", "c", "d", "e"})
    b.model.push(sp::call("big_read", std::string("{\"tag\":\"") + tag + "\",\"bytes\":1500}"));
  plain::TurnOutcome out;
  (void)b.owner("read until the arena is full", out);
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kNoMemory, b.sent.back().text.c_str());
  b.model.push(sp::say("Next."));
  b.advance(30);
  TXB_OK(b.owner("next", out));
  TEST_ASSERT_EQUAL_STRING("Next.", out.text.c_str());
  size_t unknown = 0;   // stored: the request may no longer show that turn (it left RAM)
  for (const tx::Item& it : storedOf(b)) unknown += it.kind == tx::ItemKind::Result && it.x == texts::tool_results::kOutcomeUnknown;
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)unknown, "the dangling call was never answered");
  expectPaired(b);
}

// A refused allocation in one afterDeliver handler does not keep the next from running.
inline void test_every_after_deliver_handler_runs_when_one_is_refused() {
  txb::Battery b;
  TXB_OK(b.boot());
  int second = 0;
  TXB_OK(b.hooks->afterDeliver(plain::HookSlot::Agents, [](const plain::DeliveredCtx&) { throw std::bad_alloc(); }));
  TXB_OK(b.hooks->afterDeliver(plain::HookSlot::Observer, [&second](const plain::DeliveredCtx& c) { second += c.sent; }));
  b.model.push(sp::say("Here."));
  plain::TurnOutcome out;
  (void)b.owner("hi", out);
  TEST_ASSERT_EQUAL(1, second);
  TEST_ASSERT_EQUAL_STRING("Here.", b.sent[0].text.c_str());
}

// The reply's send is refused twice: a failed-write receipt is not replaced by the memory line.
inline void test_a_receipt_survives_when_the_reply_cannot_be_sent() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDeliver(plain::HookSlot::Truth, [](const plain::DeliverCtx&) {
    plain::DeliverDecision d;
    d.status = plain::StatusLine::FailedWrite;
    d.statusText = "receipt line";
    return d;
  }));
  b.channel.send = [&b](const std::string& chat, const std::string& text) {
    if (text == "Saved it.") throw std::bad_alloc();
    b.sent.push_back(txb::SentMessage{chat, text});
    return true;
  };
  b.model.push(sp::say("Saved it."));
  plain::TurnOutcome out;
  (void)b.owner("save", out);
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING("receipt line", b.sent[0].text.c_str());
  // With no receipt, a reply that can never be sent leaves the memory line alone.
  b.hooks->clear();
  b.sent.clear();
  b.model.push(sp::say("Saved it."));
  b.advance(30);
  (void)b.owner("save again", out);
  TEST_ASSERT_EQUAL(1, (int)b.sent.size());
  TEST_ASSERT_EQUAL_STRING(texts::device_notes::kNoMemory, b.sent[0].text.c_str());
}

// At turn open a full arena makes room before the first request (free bytes under the reserve):
// the first request already names what left RAM.
inline void test_a_turn_opens_with_room_when_the_arena_is_nearly_full() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.arena->init(64 * 1024, b.platform.allocLarge, b.platform.freeLarge));
  int n = 0;
  // Each turn opens with at least the 32 KB reserve free; the last one ends just under it.
  for (; b.arena->freeBytes() >= 32 * 1024 && n < 200; ++n) shortTurns(b, 1);
  TEST_ASSERT_EQUAL(0, (int)b.engine->omittedThru());
  const size_t first = b.model.requests().size();
  b.model.push(sp::say("Room."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("one more", out));
  TEST_ASSERT_EQUAL_STRING("Room.", out.text.c_str());
  TEST_ASSERT_TRUE_MESSAGE(b.engine->omittedThru() > 0, "the turn opened without making room");
  TEST_ASSERT_TRUE(bodyHas(b.model.requests()[first], "earlier turns"));
  TEST_ASSERT_TRUE(n > 10);
}

// A refused allocation in the boot's resume seam is a status, never an escape.
inline void test_a_refused_allocation_in_the_resume_seam_is_a_status() {
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->bootResume(plain::HookSlot::Resume, [](const plain::BootCtx&) -> tx::Status { throw std::bad_alloc(); }));
  tx::Status st = tx::Status::success();
  bool escaped = false;
  try {
    st = b.engine->boot(plain::BootCtx{b.epoch, false});
  } catch (const std::bad_alloc&) {
    escaped = true;
  }
  TEST_ASSERT_FALSE(escaped);
  TEST_ASSERT_TRUE(st.err == tx::Err::TooLarge);
}


// ---- PART TWO (lane RB-E1b): guards scaled to the head, identical-call suppression, deferred effects ----
// What each head class costs, as a binding fills TurnSetup::cost from its budget and price row.
// Figures for the test only (PRD section 3's table: request size in tokens at the profile; rates
// in micro-USD per 1M tokens); warm and cold request costs follow from the same rates.
struct HeadClass {
  const char* name;
  plain::Dialect dialect;
  uint32_t requestTokens;       // P: the request at the profile's trigger
  uint64_t in, out, cacheR;     // cacheR 0: 0.1 x in; a router with no cache discount: = in
  bool routed;
};
inline std::vector<HeadClass> headClasses() {
  return {
      {"claude-sonnet-5-5 direct, standard", plain::Dialect::AnthropicMessages, 28300, 3000000, 15000000, 0, false},
      {"claude-haiku-5-5 direct, standard", plain::Dialect::AnthropicMessages, 28300, 1000000, 5000000, 0, false},
      {"gpt-5.6 direct, standard", plain::Dialect::OpenAIResponses, 28300, 5000000, 40000000, 0, false},
      {"gpt-5.6-luna direct, standard", plain::Dialect::OpenAIResponses, 28300, 250000, 2000000, 0, false},
      {"glm-5.3 direct, lean", plain::Dialect::ChatCompletions, 21480, 1000000, 4000000, 0, false},
      {"router gpt-5.6, lean, no cache discount", plain::Dialect::ChatCompletions, 21480, 5000000, 40000000, 5000000, true},
      {"router claude-sonnet-5-5, lean", plain::Dialect::ChatCompletions, 21480, 3000000, 15000000, 3000000, true},
      {"mistral-large-4-0 direct, lean, assumed", plain::Dialect::ChatCompletions, 21480, 2000000, 6000000, 2000000, false},
      {"paid custom at the highest price row, lean", plain::Dialect::Flat, 21480, 15000000, 75000000, 15000000, false},
  };
}
inline plain::HeadCost costOf(const HeadClass& h) {
  plain::HeadCost c;
  c.targetTokens = h.requestTokens;
  c.requestTokensAtTrigger = h.requestTokens;
  c.inMicroPerM = h.in;
  c.outMicroPerM = h.out;
  c.cacheReadMicroPerM = h.cacheR;
  plain::RequestTokens warm{h.requestTokens / 10, 740, h.requestTokens - h.requestTokens / 10, 0};
  plain::RequestTokens cold{h.requestTokens, 740, 0, 0};
  c.warmRequestMicroUsd = plain::requestMicroUsd(c, warm);
  c.coldRequestMicroUsd = plain::requestMicroUsd(c, cold);
  return c;
}
// A round's usage as the head reports it: the request grows by about 600 tokens a round, 90%
// of it read from the cache after the first round.
inline sp::Usage usageAt(const HeadClass& h, size_t round) {
  sp::Usage u;
  const uint32_t size = h.requestTokens + (uint32_t)round * 600;
  u.input = round ? size / 10 : size;
  u.cacheRead = round ? size - size / 10 : 0;
  u.output = 740;
  return u;
}

// A scripted turn of `rounds` tool rounds, then the reply, on one head class.
inline void scriptedTurn(txb::Battery& b, const HeadClass& h, size_t rounds, const char* tool) {
  for (size_t i = 0; i < rounds; ++i) {
    sp::Step s = std::string(tool) == "write_file"
                     ? sp::call("write_file", "{\"path\":\"notes/fix.md\",\"text\":\"row " + std::to_string(i) + "\"}")
                     : sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(i) + "}");
    s.usage = usageAt(h, i);
    b.model.push(s);
  }
  sp::Step done = sp::say("Done.");
  done.usage = usageAt(h, rounds);
  b.model.push(done);
}

// The definition of done: a scripted 8-round turn completes on every head class, and a
// 12-call correction (twelve writes, one round each) finishes in one turn.
inline void test_an_8_round_turn_and_a_12_call_correction_complete_on_every_head_class() {
  for (const HeadClass& h : headClasses())
    for (size_t rounds : {(size_t)8, (size_t)12}) {
      txb::Battery b(h.dialect);
      b.cost = costOf(h);
      b.routed = h.routed;
      b.world.files["notes/a.md"] = "alpha";
      scriptedTurn(b, h, rounds, rounds == 12 ? "write_file" : "read_file");
      plain::TurnOutcome out;
      TXB_OK(b.owner("go", out));
      char msg[160];
      std::snprintf(msg, sizeof msg, "%s, %u rounds: trip %s, %u USD micro", h.name, (unsigned)rounds,
                    plain::guardTripSlug(out.trip), (unsigned)out.end.microUsd);
      TEST_ASSERT_TRUE_MESSAGE(out.trip == plain::GuardTrip::None, msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("Done.", out.text.c_str(), msg);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)b.sent.size(), msg);
      TEST_ASSERT_EQUAL_MESSAGE((int)rounds + 1, (int)b.model.requests().size(), msg);
      TEST_ASSERT_TRUE_MESSAGE(out.end.microUsd > 0, msg);   // priced
    }
}

// Each guard: exactly one device item, exactly one last round (tool choice none), the last
// round's text delivered, and one status line naming the guard.
inline void expectOneTrip(txb::Battery& b, const plain::TurnOutcome& out, plain::GuardTrip g, const char* who) {
  const std::string slug = plain::guardTripSlug(g);
  char msg[160];
  std::snprintf(msg, sizeof msg, "%s: the %s guard (tripped %s)", who, slug.c_str(), plain::guardTripSlug(out.trip));
  TEST_ASSERT_TRUE_MESSAGE(out.trip == g, msg);
  const std::vector<tx::Item> items = storedOf(b);
  size_t notes = 0;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::DeviceNote) {
      ++notes;
      TEST_ASSERT_EQUAL_STRING_MESSAGE(("[device] limit reached (" + slug + ")").c_str(), it.x.c_str(), msg);
    }
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)notes, msg);
  size_t none = 0;
  for (const auto& r : b.model.requests()) none += r.view.toolChoice == "none";
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)none, msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("none", b.model.last().view.toolChoice.c_str(), msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Stopped at the limit.", out.text.c_str(), msg);
  TEST_ASSERT_EQUAL_MESSAGE(2, (int)b.sent.size(), msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Stopped at the limit.", b.sent[0].text.c_str(), msg);
  const std::string line = std::string(texts::device_notes::kLimitStopped) + slug + texts::device_notes::kLimitStoppedEnd;
  TEST_ASSERT_EQUAL_STRING_MESSAGE(line.c_str(), b.sent[1].text.c_str(), msg);
  TEST_ASSERT_TRUE_MESSAGE(out.status == plain::StatusLine::LimitReached, msg);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(("limit:" + slug).c_str(), lastEnd(items)->end.cause.c_str(), msg);
}

inline sp::Step heavyUntilNone(const sp::Recorded& r, const sp::Usage& u) {
  sp::Step s = untilNone(r, 0);
  s.usage = u;
  return s;
}

inline void test_each_guard_gives_one_item_and_one_last_round() {
  for (plain::Dialect d : plain::kAllDialects) {
    {   // rounds
      txb::Battery b(d);
      b.world.files["notes/a.md"] = "alpha";
      b.model.respondWith([](const sp::Recorded& r, size_t) { return untilNone(r, 0); });
      plain::TurnOutcome out;
      TXB_OK(b.owner("keep reading", out));
      expectOneTrip(b, out, plain::GuardTrip::Rounds, plain::dialectSlug(d));
      TEST_ASSERT_EQUAL(plain::kDefaultRounds + 1, (int)b.model.requests().size());
    }
    {   // time: 70 s an answer, the 240 s soft wall
      txb::Battery b(d);
      b.world.files["notes/a.md"] = "alpha";
      b.model.respondWith([](const sp::Recorded& r, size_t) { return untilNone(r, 70000); });
      plain::TurnOutcome out;
      TXB_OK(b.owner("keep reading slowly", out));
      expectOneTrip(b, out, plain::GuardTrip::Time, plain::dialectSlug(d));
    }
    {   // spend: 0.20 USD a request at a 0.50 USD cap
      txb::Battery b(d);
      b.world.files["notes/a.md"] = "alpha";
      b.cost.inMicroPerM = 10000000;   // 10 USD per 1M
      sp::Usage u;
      u.input = 20000;
      b.model.respondWith([u](const sp::Recorded& r, size_t) { return heavyUntilNone(r, u); });
      plain::TurnOutcome out;
      TXB_OK(b.owner("keep reading", out));
      expectOneTrip(b, out, plain::GuardTrip::Spend, plain::dialectSlug(d));
      TEST_ASSERT_EQUAL(4, (int)b.model.requests().size());   // 0.20, 0.40, 0.60: trip, then the last round
    }
    {   // size: cached tokens count (a request of 100,000 tokens, 99% from the cache)
      txb::Battery b(d);
      b.world.files["notes/a.md"] = "alpha";
      b.cost.targetTokens = b.cost.requestTokensAtTrigger = 20000;   // cap 320,000 tokens
      sp::Usage u;
      u.input = 1000;
      u.cacheRead = 99000;
      b.model.respondWith([u](const sp::Recorded& r, size_t) { return heavyUntilNone(r, u); });
      plain::TurnOutcome out;
      TXB_OK(b.owner("keep reading", out));
      expectOneTrip(b, out, plain::GuardTrip::Size, plain::dialectSlug(d));
      TEST_ASSERT_EQUAL(5, (int)b.model.requests().size());
    }
  }
}

// The limits themselves (PRD section 1 step 5).
inline void test_guard_limits_scale_with_the_head() {
  plain::GuardLimits l;
  plain::HeadCost none;
  TXB_OK(plain::guardLimitsFor(8, none, l));
  TEST_ASSERT_EQUAL(16, l.rounds);
  TXB_OK(plain::guardLimitsFor(0, none, l));
  TEST_ASSERT_EQUAL(16, l.rounds);
  TXB_OK(plain::guardLimitsFor(9, none, l));
  TEST_ASSERT_EQUAL(9, l.rounds);
  TXB_OK(plain::guardLimitsFor(32, none, l));
  TEST_ASSERT_EQUAL(32, l.rounds);
  TXB_OK(plain::guardLimitsFor(40, none, l));
  TEST_ASSERT_EQUAL(32, l.rounds);
  TEST_ASSERT_EQUAL(plain::kSoftWallMs, l.softWallMs);
  TEST_ASSERT_EQUAL(16u * plain::kDefaultRequestTokens, l.maxPromptTokens);
  TEST_ASSERT_EQUAL(plain::kMinSpendCapMicroUsd, l.maxSpendMicroUsd);
  plain::HeadCost c;
  c.targetTokens = 200000;            // 4 x target wins
  c.requestTokensAtTrigger = 30000;
  c.warmRequestMicroUsd = 50000;      // 16 x 0.05 + 0.20 = 1.00 USD wins over 0.50
  c.coldRequestMicroUsd = 200000;
  TXB_OK(plain::guardLimitsFor(8, c, l));
  TEST_ASSERT_EQUAL(800000u, l.maxPromptTokens);
  TEST_ASSERT_EQUAL(1000000u, l.maxSpendMicroUsd);
  c.targetTokens = 1000;              // 16 x request wins
  TXB_OK(plain::guardLimitsFor(8, c, l));
  TEST_ASSERT_EQUAL(480000u, l.maxPromptTokens);
  // The order: rounds, time, spend, size; each at its limit, not before.
  plain::GuardState s;
  plain::GuardTrip t;
  TXB_OK(plain::checkGuards(l, s, t));
  TEST_ASSERT_TRUE(t == plain::GuardTrip::None);
  s.promptTokens = l.maxPromptTokens - 1;
  s.spendMicroUsd = l.maxSpendMicroUsd - 1;
  s.rounds = (uint16_t)(l.rounds - 1);
  s.elapsedMs = l.softWallMs - plain::kMinRequestMs - 1;
  TXB_OK(plain::checkGuards(l, s, t));
  TEST_ASSERT_TRUE(t == plain::GuardTrip::None);
  s.promptTokens = l.maxPromptTokens;
  TXB_OK(plain::checkGuards(l, s, t));
  TEST_ASSERT_TRUE(t == plain::GuardTrip::Size);
  s.spendMicroUsd = l.maxSpendMicroUsd;
  TXB_OK(plain::checkGuards(l, s, t));
  TEST_ASSERT_TRUE(t == plain::GuardTrip::Spend);
  s.elapsedMs += 1;
  TXB_OK(plain::checkGuards(l, s, t));
  TEST_ASSERT_TRUE(t == plain::GuardTrip::Time);
  s.rounds = l.rounds;
  TXB_OK(plain::checkGuards(l, s, t));
  TEST_ASSERT_TRUE(t == plain::GuardTrip::Rounds);
  // The spend of one request: cache rates default to 0.1x and 1.25x of the input rate.
  plain::HeadCost p;
  p.inMicroPerM = 3000000;
  p.outMicroPerM = 15000000;
  TEST_ASSERT_EQUAL(3000u + 15000u + 300u + 3750u, plain::requestMicroUsd(p, plain::RequestTokens{1000, 1000, 1000, 1000}));
  TEST_ASSERT_EQUAL(0u, plain::requestMicroUsd(plain::HeadCost{}, plain::RequestTokens{1000000, 1000000, 0, 0}));
}

// ---- identical-call suppression: pure tools only, one turn, the earlier result whole ----------
inline size_t dispatchedOf(const txb::Battery& b, const std::string& name) {
  size_t n = 0;
  for (const std::string& d : b.world.dispatched) n += d == name;
  return n;
}

inline void test_a_repeated_pure_call_is_answered_from_the_earlier_result() {
  for (int viaMore = 0; viaMore < 2; ++viaMore) {   // ruling 52 item 3: through more too
    txb::Battery b(plain::Dialect::ChatCompletions);   // arguments as the model wrote them, spaces and all
    if (viaMore) withMore(b);
    b.world.files["notes/a.md"] = "alpha";
    const std::string direct = R"({"path":"notes/a.md"})", routed = R"({"name":"read_file","args":{"path":"notes/a.md"}})";
    b.model.push(sp::call("read_file", direct));
    b.model.push(viaMore ? sp::call("more", routed) : sp::call("read_file", "{ \"path\" : \"notes/a.md\" }"));
    b.model.push(sp::say("It says alpha."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read it twice", out));
    TEST_ASSERT_EQUAL_MESSAGE(1, (int)dispatchedOf(b, "read_file"), viaMore ? "through more" : "direct");
    TEST_ASSERT_TRUE(bodyHas(b.model.last(), texts::tool_results::kSameAsEarlier));
    TEST_ASSERT_EQUAL_STRING("It says alpha.", out.text.c_str());
    expectPaired(b);
    // Not across turns: the next turn reads again.
    b.model.push(sp::call("read_file", direct));
    b.model.push(sp::say("Still alpha."));
    b.advance(30);
    TXB_OK(b.owner("and now?", out));
    TEST_ASSERT_EQUAL(2, (int)dispatchedOf(b, "read_file"));
  }
}

inline void test_a_write_between_two_reads_lets_the_second_run() {
  for (int viaMore = 0; viaMore < 2; ++viaMore) {
    txb::Battery b;
    withMore(b);
    b.world.files["notes/a.md"] = "alpha";
    const std::string rf = R"({"path":"notes/a.md"})";
    b.model.push(sp::call("read_file", rf));
    b.model.push(viaMore ? sp::call("more", R"({"name":"write_file","args":{"path":"notes/a.md","text":"beta"}})")
                         : sp::call("write_file", R"({"path":"notes/a.md","text":"beta"})"));
    b.model.push(sp::call("more", R"({"name":"read_file","args":{"path":"notes/a.md"}})"));
    b.model.push(sp::say("Now beta."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read, write, read", out));
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)dispatchedOf(b, "read_file"), viaMore ? "write through more" : "direct write");
    TEST_ASSERT_FALSE(bodyHas(b.model.last(), texts::tool_results::kSameAsEarlier));
  }
}

inline void test_a_repeat_of_a_tool_that_is_not_pure_always_runs() {
  for (int viaMore = 0; viaMore < 2; ++viaMore) {   // an internal read whose repeat may differ
    txb::Battery b;
    withMore(b);
    b.model.push(viaMore ? sp::call("more", R"({"name":"clock_now","args":{}})") : sp::call("clock_now", "{}"));
    b.model.push(viaMore ? sp::call("more", R"({"name":"clock_now","args":{}})") : sp::call("clock_now", "{}"));
    b.model.push(sp::say("Asked twice."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("the time twice", out));
    TEST_ASSERT_EQUAL(2, (int)dispatchedOf(b, "clock_now"));
  }
  {   // an earlier error result answers nothing
    txb::Battery b;
    b.model.push(sp::call("read_file", R"({"path":"notes/none.md"})"));
    b.model.push(sp::call("read_file", R"({"path":"notes/none.md"})"));
    b.model.push(sp::say("Not there."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("read a missing file twice", out));
    TEST_ASSERT_EQUAL(2, (int)dispatchedOf(b, "read_file"));
  }
  for (int viaMore = 0; viaMore < 2; ++viaMore) {
    txb::Battery b;
    withMore(b);
    const std::string args = R"({"query":"weather"})";
    const std::string routed = R"({"name":"web_search","args":{"query":"weather"}})";
    b.model.push(viaMore ? sp::call("more", routed) : sp::call("web_search", args));
    b.model.push(viaMore ? sp::call("more", routed) : sp::call("web_search", args));
    b.model.push(sp::say("Searched twice."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("search twice", out));
    TEST_ASSERT_EQUAL(2, (int)dispatchedOf(b, "web_search"));
  }
}

// The earlier result is no longer whole (ladder rung 2 stubbed it): the repeat runs.
inline void test_a_repeat_runs_when_the_earlier_result_is_a_stub() {
  txb::Battery b;
  shortTurns(b, 8);
  const std::string first = R"({"tag":"r0","bytes":20000})";
  b.model.push(sp::call("big_read", first));
  for (int i = 1; i < 6; ++i) b.model.push(sp::call("big_read", "{\"tag\":\"r" + std::to_string(i) + "\",\"bytes\":20000}"));
  b.model.push(sp::call("big_read", first));
  b.model.push(sp::say("Read seven."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read seven big ones", out));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Read seven.", out.text.c_str(), out.statusText.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(7, (int)dispatchedOf(b, "big_read"), "a repeat was answered from a result shown as a stub");
}

// ---- deferred effects --------------------------------------------------------------------
// A restart or a head switch asked for by a tool runs after the turn: the reply goes out and
// the d and e items are stored first; the binding reads TurnOutcome::deferred. Through more too.
inline void test_a_deferred_effect_runs_after_the_turn_is_closed() {
  for (int viaMore = 0; viaMore < 2; ++viaMore) {
    txb::Battery b;
    withMore(b);
    const std::string restart = R"({"action":"restart"})", head = R"({"action":"head","to":"claude-haiku-5-5"})";
    if (viaMore)
      b.model.push(sp::calls({{"more", R"({"name":"device_control","args":{"action":"restart"}})", ""},
                              {"more", R"({"name":"device_control","args":{"action":"head","to":"claude-haiku-5-5"}})", ""}}));
    else
      b.model.push(sp::calls({{"device_control", restart, ""}, {"device_control", head, ""}}));
    b.model.push(sp::say("Switching and restarting after this reply."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("switch to haiku and restart", out));
    TEST_ASSERT_TRUE(out.deferred.has(plain::Deferred::Restart));
    TEST_ASSERT_TRUE(out.deferred.has(plain::Deferred::HeadSwitch));
    TEST_ASSERT_FALSE(out.deferred.has(plain::Deferred::Install));
    TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5", out.deferred.arg[(size_t)plain::Deferred::HeadSwitch].c_str());
    TEST_ASSERT_EQUAL(1, (int)b.sent.size());   // the reply went out before the binding acts
    const std::vector<tx::Item> items = storedOf(b);
    TEST_ASSERT_EQUAL(1, (int)countKind(items, tx::ItemKind::TurnEnd));
    TEST_ASSERT_EQUAL(1, (int)countKind(items, tx::ItemKind::Delivery));
    expectPaired(b);
  }
  {   // a failed call asks for nothing
    txb::Battery f;
    f.model.push(sp::call("device_control", R"({"action":"restart","fail":true})"));
    f.model.push(sp::say("It failed."));
    plain::TurnOutcome out;
    TXB_OK(f.owner("restart", out));
    TEST_ASSERT_EQUAL(0, (int)out.deferred.mask);
  }
  // A refused call asks for nothing.
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.hooks->beforeDispatch(plain::HookSlot::Rails, [](const plain::DispatchCtx&) {
    plain::DispatchDecision d;
    d.verdict = plain::DispatchVerdict::Refuse;
    d.text = "refused";
    return d;
  }));
  b.model.push(sp::call("device_control", R"({"action":"restart"})"));
  b.model.push(sp::say("It was refused."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("restart", out));
  TEST_ASSERT_EQUAL(0, (int)out.deferred.mask);
}

// ---- red cases for other lanes (ruling 56; on test/pending_roster.txt with ruling:56) ---------
inline void test_a_full_arena_frees_its_oldest_closed_turn() {
  txb::Battery b;
  shortTurns(b, 3);
  TXB_OK(b.arena->init(4096, b.platform.allocLarge, b.platform.freeLarge));
  shortTurns(b, 6);   // each turn must still answer: the oldest closed turns give way
  TEST_ASSERT_EQUAL(9, (int)b.sent.size());
  for (const txb::SentMessage& m : b.sent) TEST_ASSERT_TRUE(m.text != texts::device_notes::kNoMemory);
  TEST_ASSERT_TRUE_MESSAGE(b.engine->omittedThru() > 0, "the dropped turns are not named to the compaction driver");
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), "earlier turns"), "the dropped turns are not named in the request");
}

inline void test_view_fetches_a_long_argument_and_a_long_reply_back() {
  TXB_WAITS_FOR("PENDING RB-N3", "RB-E1a");
  txb::Battery b;
  const std::string arg = std::string(5000, 'l') + "-ARG-END", reply = std::string(13000, 'r') + "-REPLY-END";
  b.model.push(sp::call("write_file", "{\"path\":\"notes/l.md\",\"text\":\"" + arg + "\"}"));
  b.model.push(sp::say(reply));
  plain::TurnOutcome out;
  TXB_OK(b.owner("save", out));
  // The stubs name what view fetches (ruling 59 item 7: the round trip, not only the handle).
  std::vector<std::string> ids;
  for (const tx::Item& it : storedOf(b)) {
    if (it.kind != tx::ItemKind::Call && it.kind != tx::ItemKind::Assistant) continue;
    const std::string& x = it.kind == tx::ItemKind::Call ? it.args : it.x;
    const size_t at = x.find("view id=");
    if (at == std::string::npos) continue;
    size_t end = at + 8;
    while (end < x.size() && x[end] != ']' && x[end] != ' ' && x[end] != '"') ++end;
    ids.push_back(x.substr(at + 8, end - at - 8));
  }
  TEST_ASSERT_EQUAL_MESSAGE(2, (int)ids.size(), "a stubbed argument or reply names nothing view can fetch");
  b.model.push(sp::call("view", "{\"id\":\"" + ids[0] + "\"}"));
  b.model.push(sp::call("view", "{\"id\":\"" + ids[1] + "\"}"));
  b.model.push(sp::say("ok"));
  TXB_OK(b.owner("show both back", out));
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), arg), "view did not fetch the long argument back whole");
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(b.model.last(), reply), "view did not fetch the long reply back whole");
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

// Ruling 56 and 59 item 7: the guard inputs measure size and spend, asserted on the guard
// itself: cached prompt tokens count toward the size guard (the e item's promptTokens stays the
// uncached count), and a priced head's spend trips the spend guard.
inline void test_guard_inputs_measure_size_and_spend() {
  {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.cost.targetTokens = b.cost.requestTokensAtTrigger = 500;   // a size cap of 8,000 tokens
    sp::Step s = sp::call("read_file", R"({"path":"notes/a.md"})");
    s.usage.input = 100;
    s.usage.cacheRead = 9000;
    b.model.push(s);
    b.model.push(sp::say("Stopped at the limit."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("hello", out));
    TEST_ASSERT_TRUE_MESSAGE(out.trip == plain::GuardTrip::Size, "the size guard does not count cached prompt tokens");
    TEST_ASSERT_EQUAL_STRING("limit:size", lastEnd(storedOf(b))->end.cause.c_str());
    TEST_ASSERT_TRUE_MESSAGE(out.end.promptTokens < 9000, "the e item's uncached count took the cached tokens");
  }
  {
    txb::Battery b;
    b.world.files["notes/a.md"] = "alpha";
    b.cost.inMicroPerM = 3000000;
    b.cost.outMicroPerM = 15000000;
    sp::Step s = sp::call("read_file", R"({"path":"notes/a.md"})");
    s.usage.input = 200000;   // 0.60 USD over a 0.50 USD cap
    b.model.push(s);
    b.model.push(sp::say("Stopped at the limit."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("hello", out));
    TEST_ASSERT_TRUE_MESSAGE(out.trip == plain::GuardTrip::Spend, "the spend guard has no price");
    TEST_ASSERT_TRUE(out.end.microUsd >= 600000);
    TEST_ASSERT_EQUAL_STRING("limit:spend", lastEnd(storedOf(b))->end.cause.c_str());
  }
}

// Ruling 59 item 7: the head is routed (the router host, HeadEndpoint::routed), so only a
// correct fix turns it green: a routed request carries the deadline header, a custom endpoint's
// request does not.
inline void test_a_routed_head_sends_the_deadline_header() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-E1a");
  txb::Battery b(plain::Dialect::ChatCompletions);
  b.routed = true;
  b.hostOverride = "router.cumulo-nimbus.ai";
  b.modelOverride = "anthropic/claude-sonnet-5-5";
  b.model.push(sp::say("hi"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello", out));
  bool header = false;
  for (const auto& h : b.model.last().req.headers) header |= h.first.find("eadline") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(header, "a routed request carries no deadline header");
  txb::Battery c(plain::Dialect::Flat);
  c.model.push(sp::say("hi"));
  TXB_OK(c.owner("hello", out));
  for (const auto& h : c.model.last().req.headers)
    TEST_ASSERT_TRUE_MESSAGE(h.first.find("eadline") == std::string::npos, "a custom endpoint got the router's header");
}

// Ruling 54 item 4 and 59 item 7: one place splits a routed head's model. TurnCtx::model is the
// router's selector ('<upstream>/<model>'); the request sends that selector to the router (the
// router picks the upstream), and the exchange sizes the answer for the upstream it names.
inline void test_a_routed_head_sends_the_router_model_selector() {
  TXB_WAITS_FOR("PENDING RB-W4", "RB-E1a");
  txb::Battery b(plain::Dialect::ChatCompletions);
  b.routed = true;
  b.hostOverride = "router.cumulo-nimbus.ai";
  b.modelOverride = "anthropic/claude-sonnet-5-5";
  b.model.push(sp::say("hi"));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello", out));
  const sp::Recorded& r = b.model.last();
  TEST_ASSERT_TRUE_MESSAGE(r.req.path.rfind("/router/", 0) == 0, "a routed head's request does not go to the router path");
  TEST_ASSERT_TRUE_MESSAGE(bodyHas(r, "\"model\":\"anthropic/claude-sonnet-5-5\""), "the router's selector is not the model sent");
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

// The day start rides the turn (RB-REC5): a message sent at 00:30 belongs to the day before when
// the day starts at 04:00, and the stamp says so; with the day starting at midnight it does not.
inline void test_the_stamp_carries_the_turns_day_start() {
  for (uint16_t start : {(uint16_t)240, (uint16_t)0}) {
    txb::Battery b;
    b.dayStartMin = start;
    b.model.push(sp::say("Late."));
    b.advance(11 * 3600 + 28 * 60);   // kT0 is 13:02 UTC: now 00:30 the next day
    plain::TurnOutcome out;
    TXB_OK(b.owner("still up", out));
    const bool clause = bodyHas(b.model.last(), "[2026-11-18 Wed 00:30 (day 2026-11-17 until 04:00)] still up");
    TEST_ASSERT_EQUAL_MESSAGE(start == 240, clause, "the stamp does not follow the turn's day start");
    if (start == 0) TEST_ASSERT_TRUE(bodyHas(b.model.last(), "[2026-11-18 Wed 00:30] still up"));
  }
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
  RUN_TEST(test_rung_three_estimate_never_exceeds_the_rendered_bytes_of_any_item_kind);
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
  RUN_TEST(test_a_refused_allocation_from_any_point_on_still_reaches_the_owner);
  RUN_TEST(test_a_refused_large_allocation_still_reaches_the_owner);
  RUN_TEST(test_a_refusal_inside_delivery_never_sends_a_part_twice);
  RUN_TEST(test_after_deliver_runs_when_the_d_item_is_refused);
  RUN_TEST(test_a_message_with_too_many_parts_gets_its_own_line);
  RUN_TEST(test_a_chat_that_filled_its_arena_keeps_answering_between_rounds);
  RUN_TEST(test_a_full_arena_frees_its_oldest_closed_turn);
  RUN_TEST(test_an_8_round_turn_and_a_12_call_correction_complete_on_every_head_class);
  RUN_TEST(test_each_guard_gives_one_item_and_one_last_round);
  RUN_TEST(test_guard_limits_scale_with_the_head);
  RUN_TEST(test_a_repeated_pure_call_is_answered_from_the_earlier_result);
  RUN_TEST(test_a_write_between_two_reads_lets_the_second_run);
  RUN_TEST(test_a_repeat_of_a_tool_that_is_not_pure_always_runs);
  RUN_TEST(test_a_repeat_runs_when_the_earlier_result_is_a_stub);
  RUN_TEST(test_a_deferred_effect_runs_after_the_turn_is_closed);
  RUN_TEST(test_a_dangling_call_is_answered_before_a_full_arena_drops_its_turn);
  RUN_TEST(test_every_after_deliver_handler_runs_when_one_is_refused);
  RUN_TEST(test_a_receipt_survives_when_the_reply_cannot_be_sent);
  RUN_TEST(test_a_refused_allocation_in_the_resume_seam_is_a_status);
  RUN_TEST(test_a_turn_opens_with_room_when_the_arena_is_nearly_full);
  RUN_TEST(test_view_fetches_a_long_argument_and_a_long_reply_back);
  RUN_TEST(test_a_long_owner_message_goes_to_the_inbox_as_a_file);
  RUN_TEST(test_signed_thinking_over_the_line_limit_keeps_the_turn);
  RUN_TEST(test_guard_inputs_measure_size_and_spend);
  RUN_TEST(test_a_routed_head_sends_the_deadline_header);
  RUN_TEST(test_a_routed_head_sends_the_router_model_selector);
  RUN_TEST(test_the_log_reopens_when_storage_returns);
  RUN_TEST(test_the_stamp_carries_the_turns_day_start);
  RUN_TEST(test_an_empty_engine_answers_a_typed_outcome);
}

}  // namespace engine_tests
