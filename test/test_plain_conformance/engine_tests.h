#pragma once
// The loop core's own cases (lane RB-E1a), run from main.cpp beside the battery's: scripted
// turns through the plain engine with the stand-in wire of each dialect (txb::StubWire, or
// the real wire once it lands). A tool-less reply, tool rounds, a cut answer, an exchange
// error of each class, a store that answers not stored or has no card, a restart in the
// middle of a turn, the round budget and the wall clock, byte-identical requests, every
// seam at its point, a refused allocation.
#include <unity.h>

#include <new>
#include <string>
#include <vector>

#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "nimbus/harness/plain/texts.h"

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
      {{sp::call("read_file", rf), sp::call("ledger_day", R"({"day":"2026-11-17"})"),
        sp::call("read_file", R"({"path":"notes/b.md"})"), sp::say("Three rounds.")},
       {{1, ""}, {1, ""}, {1, ""}, {0, "Three rounds."}}},
      {{sp::calls({{"read_file", rf, ""}, {"ledger_day", "{}", ""}}), sp::say("Both.")}, {{2, ""}, {0, "Both."}}},
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
    if (c.call->name == "ledger_day") {
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
                          {"ledger_day", "{}", ""},
                          {"device_control", R"({"action":"restart"})", ""}}));
  b.model.push(sp::say("Done, I think."));
  b.model.push(sp::say("Read alpha; the restart was refused."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("do three things", out));
  plain::IdleOutcome idle;
  TXB_OK(b.idle(idle));
  const std::vector<std::string> want = {
      "beforeDispatch read_file pure",   "afterResult read_file stored",
      "beforeDispatch ledger_day pure",  "afterResult ledger_day stored",
      "beforeDispatch device_control effect", "afterResult device_control stored error",
      "beforeDeliver Done, I think.",   // after the one hold the registry asks no handler again
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
  b.hooks->clear();
  b.model.push(sp::say("It may not have saved."));
  TXB_OK(b.owner("did it?", out));
  TEST_ASSERT_TRUE(bodyHas(b.model.last(), "outcome unknown"));
  expectPaired(b);
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
  RUN_TEST(test_an_empty_engine_answers_a_typed_outcome);
}

}  // namespace engine_tests
