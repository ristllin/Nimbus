// test_plain_truth - the truth check bound into the plain engine (PRD section 8.8; lane RB-G1).
// The engine runs through txb::Battery (a scripted head at the transport, storage in RAM, a virtual
// clock); the probe is a scripted ProbeFn, so nothing here spends. The classes under test:
//   1. the probe reads recent receipts from the transcript (and never changes a stored item);
//   2. enforce holds before delivery and never after;
//   3. a held turn shows a status line and delivers one honest answer;
//   4. shadow runs from the idle step and changes nothing the owner sees;
//   5. a probe request is never conversation, nor counted against the conversation's marks.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "../support/tx_battery/driver.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/truth.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/truth.h"
#include "nimbus/orch/tx/arena.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace truth = nimbus::orch::truth;

void setUp() {}
void tearDown() {}

namespace {

// The probe as a script: each request is recorded with its user message and what the owner had
// been sent when it was asked; the answers are given in order (No once they run out).
struct Probe {
  std::vector<truth::Verdict> answers;
  std::vector<std::string> users;
  std::vector<size_t> sentAtAsk;
  const txb::Battery* b = nullptr;
  uint32_t takesMs = 0;
  uint32_t* clock = nullptr;
  truth::ProbeFn fn() {
    return [this](const truth::ProbeRequest& r) {
      ArduinoJson::JsonDocument d;
      deserializeJson(d, r.body);
      users.push_back(d["messages"][0]["content"] | "");
      sentAtAsk.push_back(b ? b->sent.size() : 0);
      if (clock) *clock += takesMs;
      const size_t i = users.size() - 1;
      return truth::ProbeResult{i < answers.size() ? answers[i] : truth::Verdict::No, 10};
    };
  }
};

struct Rig {
  txb::Battery b;
  Probe probe;
  truth::State state;
  std::vector<plain::TruthReport> reports;
  plain::TruthMode mode = plain::TruthMode::Enforce;
  plain::RecentSource older;
  plain::ProgressReporter* progress = nullptr;
  bool installed = true;

  Rig() {
    probe.b = &b;
    probe.clock = &b.ms;
    b.moreHooks = [this](plain::PlainHooks& h) {
      if (!installed) return;
      plain::TruthDeps d;
      d.mode = mode;
      d.arena = b.arena.get();
      d.probe = probe.fn();
      d.state = &state;
      d.older = older;
      d.now = [this] { return (int64_t)b.epoch; };
      d.progress = progress;
      d.nowMs = [this] { return b.ms; };
      d.done = [this](const plain::TruthReport& r) { reports.push_back(r); };
      TEST_ASSERT_TRUE(plain::installTruth(h, d).ok());
    };
  }
  std::vector<tx::Item> stored() {
    std::vector<tx::Item> items;
    TEST_ASSERT_TRUE(b.stored(items).ok());
    return items;
  }
};

size_t countKind(const std::vector<tx::Item>& items, tx::ItemKind k) {
  size_t n = 0;
  for (const tx::Item& it : items) n += it.kind == k;
  return n;
}
const tx::Item* lastEnd(const std::vector<tx::Item>& items) {
  for (size_t i = items.size(); i-- > 0;)
    if (items[i].kind == tx::ItemKind::TurnEnd) return &items[i];
  return nullptr;
}
void* alloc(size_t n) { return std::malloc(n); }
void release(void* p) { std::free(p); }
bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
bool anyRequestHas(const txb::Battery& b, const std::string& part) {
  for (const auto& r : b.model.requests())
    if (contains(r.body, part)) return true;
  return false;
}

// ---- 1. the probe reads recent receipts from the transcript ------------------------------------

void test_the_probe_reads_recent_receipts_from_the_transcript() {
  Rig r;
  r.b.model.push(sp::calls({{"append_file", R"({"path":"notes/b.md","text":"x"})", ""},
                            {"write_file", R"({"path":"notes/a.md","text":"milk"})", ""}}));
  r.b.model.push(sp::say("Saved it to notes/a.md."));
  r.b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  r.b.model.push(sp::say("Yes, I saved it to notes/a.md earlier."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("save milk to my notes", out).ok());
  r.b.advance(3600);
  TEST_ASSERT_TRUE(r.b.owner("did you save it?", out).ok());
  TEST_ASSERT_EQUAL(2, (int)r.probe.users.size());
  // Turn 1: its own effect is this turn's, from the stored c and r items.
  TEST_ASSERT_TRUE(contains(r.probe.users[0], "<effects_this_turn>\n- append_file FAILED: unknown tool append_file\n- write_file ok: wrote notes/a.md"));
  TEST_ASSERT_TRUE(contains(r.probe.users[0], "<recent_effects>\n(none)"));
  // Turn 2: the same receipt is a recent one, with its time, and this turn has none.
  TEST_ASSERT_TRUE(contains(r.probe.users[1], "<effects_this_turn>\n(none)"));
  TEST_ASSERT_TRUE_MESSAGE(contains(r.probe.users[1], "write_file: wrote notes/a.md"), r.probe.users[1].c_str());
  // A read is no effect; a failed effect backs nothing, so it is never a recent receipt.
  TEST_ASSERT_FALSE(contains(r.probe.users[1], "read_file"));
  TEST_ASSERT_FALSE(contains(r.probe.users[1], "append_file"));
  TEST_ASSERT_TRUE(contains(r.probe.users[1], "<owner_message>\ndid you save it?\n</owner_message>"));
  TEST_ASSERT_TRUE(r.reports.back().ringKnown);
  TEST_ASSERT_TRUE(r.reports.back().mode == truth::Truth::Enforce);
}

// Counter: a true reply is never held, and the check changes no stored item: the same script with
// and without the check stores the same lines (the e item's probe column apart).
void test_a_true_reply_is_never_held_and_nothing_stored_changes() {
  std::vector<std::vector<tx::Item>> runs;
  for (int with = 0; with < 2; ++with) {
    Rig r;
    r.installed = with == 1;
    r.b.model.push(sp::call("write_file", R"({"path":"notes/a.md","text":"milk"})"));
    r.b.model.push(sp::say("Saved it."));
    r.b.model.push(sp::say("It is in notes/a.md."));
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
    TEST_ASSERT_TRUE(r.b.owner("where is it?", out).ok());
    TEST_ASSERT_EQUAL(2, (int)r.b.sent.size());
    TEST_ASSERT_EQUAL_STRING("It is in notes/a.md.", r.b.sent[1].text.c_str());
    TEST_ASSERT_EQUAL(with ? 2 : 0, (int)r.probe.users.size());
    TEST_ASSERT_EQUAL(3, (int)r.b.model.requests().size());   // no extra round
    runs.push_back(r.stored());
  }
  TEST_ASSERT_EQUAL(runs[0].size(), runs[1].size());
  for (size_t i = 0; i < runs[0].size(); ++i) {
    tx::Item a = runs[0][i], c = runs[1][i];
    TEST_ASSERT_EQUAL(0, (int)a.end.probeRequests);
    if (c.kind == tx::ItemKind::TurnEnd) TEST_ASSERT_EQUAL(1, (int)c.end.probeRequests);
    c.end.probeRequests = 0;
    std::string la, lc;
    TEST_ASSERT_TRUE(tx::encodeLine(a, la).ok());
    TEST_ASSERT_TRUE(tx::encodeLine(c, lc).ok());
    TEST_ASSERT_EQUAL_STRING(la.c_str(), lc.c_str());
  }
}

// The recent receipts are the chat's whole record only when they reach its first item, number the
// newest 50, or the binding's source says so; otherwise the turn is receipt-only (RB-M0c: without
// them the probe flags about a third of true replies).
void test_recent_receipts_unknown_means_receipt_only() {
  for (int withOlder = 0; withOlder < 2; ++withOlder) {
    tx::TxArena arena;
    TEST_ASSERT_TRUE(arena.init(64 * 1024, alloc, release).ok());
    tx::Item u;
    u.q = 100;   // the chat was compacted before: items 1 to 99 are not resident
    u.turn = 7;
    u.kind = tx::ItemKind::User;
    u.x = "did you save it?";
    TEST_ASSERT_TRUE(arena.append(u).ok());
    plain::PlainHooks hooks;
    Probe probe;
    truth::State state;
    plain::TruthDeps d;
    d.arena = &arena;
    d.probe = probe.fn();
    d.state = &state;
    if (withOlder)
      d.older = [](uint64_t beforeQ, size_t, std::vector<plain::Receipt>& out, bool& whole) {
        plain::Receipt r;
        r.q = beforeQ - 10;
        r.turn = 6;
        r.tool = "write_file";
        r.ok = true;
        r.text = "wrote notes/a.md";
        out.push_back(r);
        whole = true;
        return true;
      };
    TEST_ASSERT_TRUE(plain::installTruth(hooks, d).ok());
    plain::TurnCtx t;
    t.turn = 7;
    t.chatId = "1001";
    t.host = "api.anthropic.com";
    t.model = "claude-sonnet-5-5";
    const std::string reply = "Yes, saved earlier.";
    plain::DeliverCtx c;
    c.turn = &t;
    c.text = &reply;
    const plain::DeliverDecision dd = hooks.runBeforeDeliver(c);
    TEST_ASSERT_TRUE(dd.verdict == plain::DeliverVerdict::Deliver);
    TEST_ASSERT_EQUAL(withOlder ? 1 : 0, (int)probe.users.size());
    TEST_ASSERT_EQUAL(withOlder ? 1 : 0, (int)dd.probes);
    if (withOlder) TEST_ASSERT_TRUE(contains(probe.users[0], "write_file: wrote notes/a.md"));
  }
}

// recentFromLog reads the stored segments: the receipts of every stored turn, whole from the first.
void test_recent_receipts_from_the_card() {
  Rig r;
  r.b.model.push(sp::call("write_file", R"({"path":"notes/a.md","text":"milk"})"));
  r.b.model.push(sp::say("Saved."));
  r.b.model.push(sp::call("append_file", R"({"path":"notes/a.md","text":"eggs"})"));
  r.b.model.push(sp::say("That did not work."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
  TEST_ASSERT_TRUE(r.b.owner("add eggs", out).ok());
  std::vector<plain::Receipt> got;
  bool whole = false;
  TEST_ASSERT_TRUE(plain::recentFromLog(r.b.files, r.b.chatKey)(1u << 30, 50, got, whole));
  TEST_ASSERT_TRUE(whole);
  TEST_ASSERT_EQUAL(2, (int)got.size());
  TEST_ASSERT_EQUAL_STRING("write_file", got[0].tool.c_str());
  TEST_ASSERT_TRUE(got[0].ok);
  TEST_ASSERT_FALSE(got[1].ok);
  // Only what is stored before the asked q, and at most `want`, the newest.
  TEST_ASSERT_TRUE(plain::recentFromLog(r.b.files, r.b.chatKey)(got[1].q, 50, got, whole));
  TEST_ASSERT_EQUAL(1, (int)got.size());
  TEST_ASSERT_TRUE(plain::recentFromLog(r.b.files, r.b.chatKey)(1u << 30, 1, got, whole));
  TEST_ASSERT_EQUAL(1, (int)got.size());
  TEST_ASSERT_FALSE(got[0].ok);
  // A second segment: reading one segment back does not reach the first, so the list is not whole.
  std::string seg2, line;
  TEST_ASSERT_TRUE(tx::segmentPath(r.b.chatKey, 2, seg2).ok());
  tx::Item c;
  c.kind = tx::ItemKind::Call;
  c.q = 900;
  c.turn = 9;
  c.n = "write_file";
  c.eff = true;
  TEST_ASSERT_TRUE(tx::encodeLine(c, line).ok());
  r.b.files.files[seg2] = line;
  TEST_ASSERT_TRUE(plain::recentFromLog(r.b.files, r.b.chatKey, 1)(1u << 30, 50, got, whole));
  TEST_ASSERT_FALSE(whole);
  TEST_ASSERT_TRUE(plain::recentFromLog(r.b.files, r.b.chatKey, 2)(1u << 30, 50, got, whole));
  TEST_ASSERT_TRUE(whole);
  TEST_ASSERT_EQUAL(2, (int)got.size());
}

// Every tool of the class table: an effect tool has a receipt kind (speech changes nothing
// stored), a pure or read tool has none; a router call is classed by the tool it reaches.
void test_every_effect_tool_has_a_receipt_kind() {
  size_t n = 0;
  const nimbus::orch::ToolClassRow* rows = nimbus::orch::toolClassTable(n);
  for (size_t i = 0; i < n; ++i) {
    const std::string name = rows[i].name;
    const bool effect = rows[i].effect == nimbus::orch::EffectClass::Effect;
    const truth::Kind k = plain::receiptKind(name, effect);
    if (!effect || name == "speak" || name == "reply.speak") TEST_ASSERT_TRUE_MESSAGE(k == truth::Kind::Read || effect, name.c_str());
    else TEST_ASSERT_TRUE_MESSAGE(k != truth::Kind::Read, name.c_str());
    if (!effect) TEST_ASSERT_TRUE_MESSAGE(k == truth::Kind::Read, name.c_str());
  }
  TEST_ASSERT_TRUE(plain::receiptKind("a_connector_write", true) == truth::Kind::Other);
  plain::ReceiptScan scan;
  std::vector<plain::Receipt> out;
  tx::Item c;
  c.kind = tx::ItemKind::Call;
  c.turn = 1;
  c.n = "more";
  c.args = R"({"name":"write_file","args":{"path":"a"}})";
  c.eff = true;
  tx::Item res;
  res.kind = tx::ItemKind::Result;
  res.turn = 1;
  res.x = "wrote a\nmore lines";
  scan.add(c, out);
  scan.add(res, out);
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL_STRING("write_file", out[0].tool.c_str());
  TEST_ASSERT_EQUAL_STRING("wrote a", out[0].text.c_str());
}

// ---- 2. enforce holds before delivery and never after -------------------------------------------

void test_enforce_holds_before_delivery() {
  Rig r;
  r.probe.answers = {truth::Verdict::Yes};
  r.b.model.push(sp::say("Saved it."));
  r.b.model.push(sp::say("I have not saved it yet. Shall I?"));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
  TEST_ASSERT_EQUAL(1, (int)r.probe.users.size());
  TEST_ASSERT_EQUAL(0, (int)r.probe.sentAtAsk[0]);   // asked before anything went out
  TEST_ASSERT_EQUAL(1, (int)r.b.sent.size());        // one answer, the round's
  TEST_ASSERT_EQUAL_STRING("I have not saved it yet. Shall I?", r.b.sent[0].text.c_str());
  TEST_ASSERT_EQUAL(2, (int)r.b.model.requests().size());
  TEST_ASSERT_TRUE(anyRequestHas(r.b, "[device] effects this turn: none. Recent: none."));
  const std::vector<tx::Item> items = r.stored();
  TEST_ASSERT_EQUAL(1, (int)countKind(items, tx::ItemKind::DeviceNote));
  TEST_ASSERT_TRUE(r.reports.back().held && r.reports.back().retried);
  const tx::Item* e = lastEnd(items);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL(1, (int)e->end.probeRequests);
  TEST_ASSERT_EQUAL(1, (int)r.state.heads.size());
  TEST_ASSERT_EQUAL(1, (int)r.state.heads[0].c.holds);
}

// Counter: at most one hold a turn; none on a cut answer or a guard's last round; nothing the
// check does after delivery holds or sends.
void test_no_second_hold_no_hold_on_a_cut_or_last_round() {
  {
    Rig r;   // a failed write: the held round's reply is probed once more, never held again
    r.probe.answers = {truth::Verdict::Yes, truth::Verdict::Yes};
    r.b.model.push(sp::call("append_file", R"({"path":"notes/a.md","text":"eggs"})"));
    r.b.model.push(sp::say("Added eggs."));
    r.b.model.push(sp::say("Added them."));
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(r.b.owner("add eggs", out).ok());
    TEST_ASSERT_EQUAL(2, (int)r.probe.users.size());
    TEST_ASSERT_EQUAL(3, (int)r.b.model.requests().size());
    TEST_ASSERT_EQUAL(2, (int)r.b.sent.size());
    TEST_ASSERT_EQUAL_STRING("Added them.", r.b.sent[0].text.c_str());
    TEST_ASSERT_TRUE(contains(r.b.sent[1].text, "did not go through"));
    TEST_ASSERT_TRUE(out.status == plain::StatusLine::FailedWrite);
    TEST_ASSERT_EQUAL(2, (int)lastEnd(r.stored())->end.probeRequests);
  }
  {
    Rig r;   // a cut answer cannot be held
    r.probe.answers = {truth::Verdict::Yes};
    r.b.model.respondWith([](const sp::Recorded&, size_t) { return sp::cut("Saved it and"); });
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
    TEST_ASSERT_EQUAL(0, (int)countKind(r.stored(), tx::ItemKind::DeviceNote));
    TEST_ASSERT_FALSE(r.reports.back().held);
    for (const tx::Item& it : r.stored())
      for (const std::string& ev : it.end.events) TEST_ASSERT_TRUE(ev != "hold");
  }
  {
    plain::PlainHooks hooks;   // a guard's last round cannot be held
    tx::TxArena arena;
    TEST_ASSERT_TRUE(arena.init(16 * 1024, alloc, release).ok());
    Probe probe;
    probe.answers = {truth::Verdict::Yes};
    truth::State state;
    plain::TruthDeps d;
    d.arena = &arena;
    d.probe = probe.fn();
    d.state = &state;
    TEST_ASSERT_TRUE(plain::installTruth(hooks, d).ok());
    plain::TurnCtx t;
    t.turn = 1;
    t.host = "api.anthropic.com";
    t.model = "claude-sonnet-5-5";
    const std::string reply = "Saved it.";
    plain::DeliverCtx c;
    c.turn = &t;
    c.text = &reply;
    c.last = true;
    TEST_ASSERT_TRUE(hooks.runBeforeDeliver(c).verdict == plain::DeliverVerdict::Deliver);
    // after delivery: nothing holds or sends, and idle has no work
    plain::DeliveredCtx dc;
    dc.turn = &t;
    dc.text = &reply;
    hooks.runAfterDeliver(dc);
    plain::IdleCtx ic;
    TEST_ASSERT_FALSE(hooks.runIdleStep(ic).worked);
  }
}

// A guard's last round cannot be held by the engine, and the check knows it (DeliverCtx::last): it
// counts no hold and reports none.
void test_the_last_round_is_never_held_and_never_counted_as_held() {
  Rig r;
  r.b.world.files["notes/a.md"] = "alpha";
  r.probe.answers = std::vector<truth::Verdict>(4, truth::Verdict::Yes);
  r.b.model.respondWith([](const sp::Recorded& q, size_t) {
    return q.view.toolChoice == "none" ? sp::say("Saved it.")
                                       : sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(q.step) + "}");
  });
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("keep reading", out).ok());
  TEST_ASSERT_EQUAL_STRING("Saved it.", out.text.c_str());
  TEST_ASSERT_EQUAL(1, (int)r.probe.users.size());
  TEST_ASSERT_FALSE(r.reports.back().held);
  TEST_ASSERT_EQUAL(0, (int)r.state.heads[0].c.holds);
  TEST_ASSERT_EQUAL(0, (int)r.state.heads[0].c.retryFailed);
}

// The held round's own effects join this turn's once; the effects before the hold are not noted again.
void test_held_round_effects_join_once() {
  Rig r;
  r.probe.answers = {truth::Verdict::Yes, truth::Verdict::Yes};
  r.b.model.push(sp::call("write_file", R"({"path":"notes/a.md","text":"milk"})"));
  r.b.model.push(sp::say("Saved milk and eggs."));
  r.b.model.push(sp::call("append_file", R"({"path":"notes/a.md","text":"eggs"})"));
  r.b.model.push(sp::say("Saved both."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("save milk and eggs", out).ok());
  TEST_ASSERT_EQUAL(2, (int)r.probe.users.size());
  const std::string& u = r.probe.users[1];
  TEST_ASSERT_TRUE_MESSAGE(contains(u, "<effects_this_turn>\n- write_file ok: wrote notes/a.md\n- append_file FAILED: "
                                       "unknown tool append_file\n</effects_this_turn>"), u.c_str());
  TEST_ASSERT_TRUE(r.reports.back().receipt);
}

// ---- 3. a held turn shows a status line and delivers one honest answer ---------------------------

struct Lines {
  std::vector<std::string> sent, edits;
  plain::ProgressDeps deps() {
    plain::ProgressDeps d;
    d.send = [this](const std::string&, const std::string& t, int64_t& id) {
      sent.push_back(t);
      id = (int64_t)sent.size();
      return true;
    };
    d.edit = [this](const std::string&, int64_t, const std::string& t) {
      edits.push_back(t);
      return true;
    };
    return d;
  }
};

void test_a_held_turn_shows_a_status_line_and_one_answer() {
  for (int longTurn = 0; longTurn < 2; ++longTurn) {
    Lines lines;
    plain::ProgressReporter rep(lines.deps());
    Rig r;
    r.progress = &rep;
    r.probe.answers = {truth::Verdict::Yes};
    r.probe.takesMs = longTurn ? 21000 : 1000;
    r.b.model.push(sp::say("Saved it."));
    r.b.model.push(sp::say("Nothing is saved yet. Shall I save it?"));
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
    TEST_ASSERT_EQUAL(1, (int)r.b.sent.size());   // one answer
    TEST_ASSERT_EQUAL_STRING("Nothing is saved yet. Shall I save it?", r.b.sent[0].text.c_str());
    TEST_ASSERT_EQUAL(longTurn ? 1 : 0, (int)lines.sent.size());   // the status line, its own class
    if (longTurn) {
      TEST_ASSERT_TRUE_MESSAGE(contains(lines.sent[0], "waiting for the model's answer."), lines.sent[0].c_str());
      TEST_ASSERT_EQUAL(1, (int)lines.edits.size());   // its final form when the turn ended
    }
    TEST_ASSERT_FALSE(rep.isOpen());
  }
}

// A held reply whose extra round fails goes out: never silence.
void test_a_held_reply_goes_out_when_the_round_fails() {
  Rig r;
  r.probe.answers = {truth::Verdict::Yes};
  r.b.model.push(sp::say("Saved it."));
  r.b.model.respondWith([](const sp::Recorded&, size_t) { return sp::httpError(500); });
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
  TEST_ASSERT_TRUE(r.b.sent.size() >= 1);
  TEST_ASSERT_EQUAL_STRING("Saved it.", r.b.sent[0].text.c_str());
  TEST_ASSERT_TRUE(r.reports.back().held);
  TEST_ASSERT_FALSE(r.reports.back().retried);
  TEST_ASSERT_EQUAL(1, (int)r.state.heads[0].c.retryFailed);   // the check closed after delivery
}

// A failed write whose held round then fails: the held reply goes out with the receipt it owes, never
// poorer than the same turn with no hold (the receipt outranks the provider's line).
void test_a_failed_held_round_still_sends_the_receipt() {
  Rig r;
  r.probe.answers = {truth::Verdict::Yes};
  r.b.model.push(sp::call("append_file", R"({"path":"notes/a.md","text":"eggs"})"));
  r.b.model.push(sp::say("Added eggs."));
  r.b.model.respondWith([](const sp::Recorded&, size_t) { return sp::httpError(500); });
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("add eggs", out).ok());
  TEST_ASSERT_EQUAL(2, (int)r.b.sent.size());
  TEST_ASSERT_EQUAL_STRING("Added eggs.", r.b.sent[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("Note: the file change in this reply did not go through (unknown tool append_file)",
                           r.b.sent[1].text.c_str());
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::FailedWrite);
  TEST_ASSERT_TRUE(r.reports.back().receipt);
  TEST_ASSERT_EQUAL(1, (int)r.probe.users.size());   // the probe already said the reply claims success
}

// A `more` call that reached no tool ran nothing: a refused name never earns a receipt beside the
// true reply of the write that followed it.
void test_a_refused_router_call_is_no_failed_write() {
  Rig r;
  r.mode = plain::TruthMode::ReceiptOnly;
  nimbus::orch::registerMore(r.b.reg);
  r.b.model.push(sp::call("more", R"({"name":"writ_file","args":{}})"));
  r.b.model.push(sp::call("write_file", R"({"path":"notes/a.md","text":"milk"})"));
  r.b.model.push(sp::say("Saved it."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
  TEST_ASSERT_EQUAL(1, (int)r.b.sent.size());
  TEST_ASSERT_FALSE(r.reports.back().receipt);
}

// The held round read outside content: from then on the turn is receipt-only (ruling 9: a forged
// effect line fooled the probe), so the failed write's receipt is sent without asking the probe again.
void test_a_held_round_that_reads_outside_content_is_receipt_only() {
  Rig r;
  r.probe.answers = {truth::Verdict::Yes, truth::Verdict::No};
  r.b.model.push(sp::call("append_file", R"({"path":"notes/a.md","text":"eggs"})"));
  r.b.model.push(sp::say("Added eggs."));
  r.b.model.push(sp::call("web_search", R"({"q":"eggs","bytes":200})"));
  r.b.model.push(sp::say("Added them, and here is a recipe."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("add eggs", out).ok());
  TEST_ASSERT_EQUAL(1, (int)r.probe.users.size());
  TEST_ASSERT_EQUAL(2, (int)r.b.sent.size());
  TEST_ASSERT_TRUE(contains(r.b.sent[1].text, "did not go through"));
}

// Counter: a turn that is not held shows no line; a failed write on a receipt-only head sends one
// receipt after the reply, never instead of it, and asks no probe.
void test_receipt_only_failed_write_sends_one_receipt_after_the_reply() {
  Lines lines;
  plain::ProgressReporter rep(lines.deps());
  Rig r;
  r.progress = &rep;
  r.mode = plain::TruthMode::ReceiptOnly;
  r.b.model.push(sp::call("append_file", R"({"path":"notes/a.md","text":"eggs"})"));
  r.b.model.push(sp::say("Added eggs."));
  r.b.model.push(sp::call("write_file", R"({"path":"notes/b.md","text":"x"})"));
  r.b.model.push(sp::say("Wrote it."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("add eggs", out).ok());
  TEST_ASSERT_EQUAL(0, (int)r.probe.users.size());
  TEST_ASSERT_EQUAL(2, (int)r.b.sent.size());
  TEST_ASSERT_EQUAL_STRING("Added eggs.", r.b.sent[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("Note: the file change in this reply did not go through (unknown tool append_file)",
                           r.b.sent[1].text.c_str());
  TEST_ASSERT_TRUE(r.reports.back().receipt);
  TEST_ASSERT_TRUE(r.reports.back().mode == truth::Truth::ReceiptOnly);
  TEST_ASSERT_TRUE(r.b.owner("write b", out).ok());   // a write that landed: no line
  TEST_ASSERT_EQUAL(3, (int)r.b.sent.size());
  TEST_ASSERT_EQUAL(0, (int)lines.sent.size());
}

// ---- 4. shadow runs from the idle step -----------------------------------------------------------

void test_shadow_probes_from_the_idle_step_only() {
  Rig r;
  r.mode = plain::TruthMode::Shadow;
  r.probe.answers = {truth::Verdict::Yes};
  r.b.model.push(sp::say("Saved it."));
  plain::TurnOutcome out;
  TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
  TEST_ASSERT_EQUAL(0, (int)r.probe.users.size());   // nothing before delivery
  TEST_ASSERT_EQUAL(1, (int)r.b.sent.size());
  TEST_ASSERT_EQUAL_STRING("Saved it.", r.b.sent[0].text.c_str());
  TEST_ASSERT_TRUE(r.reports.back().shadowDue);
  const std::vector<tx::Item> before = r.stored();
  plain::IdleOutcome io;
  TEST_ASSERT_TRUE(r.b.idle(io).ok());
  TEST_ASSERT_TRUE(io.worked);
  TEST_ASSERT_TRUE(io.by == plain::HookSlot::ShadowProbe);
  TEST_ASSERT_EQUAL(1, (int)r.probe.users.size());
  TEST_ASSERT_TRUE(contains(r.probe.users[0], "<reply>\nSaved it.\n</reply>"));
  // Nothing the owner sees changed: no message, no stored item.
  TEST_ASSERT_EQUAL(1, (int)r.b.sent.size());
  TEST_ASSERT_EQUAL(before.size(), r.stored().size());
  TEST_ASSERT_EQUAL(1, (int)r.state.heads[0].c.shadowProbes);
  TEST_ASSERT_EQUAL(1, (int)r.state.heads[0].c.shadowHits);
  TEST_ASSERT_EQUAL(0, (int)r.state.heads[0].c.holds);
  // One probe per shadow turn: the next idle step has nothing to do for the check.
  TEST_ASSERT_TRUE(r.b.idle(io).ok());
  TEST_ASSERT_EQUAL(1, (int)r.probe.users.size());
}

// ---- 5. a probe request is not conversation --------------------------------------------------------

void test_a_probe_request_is_never_conversation() {
  std::vector<tx::TurnEndFacts> ends;
  std::vector<plain::TurnOutcome> outs;
  for (int with = 0; with < 2; ++with) {
    Rig r;
    r.installed = with == 1;
    r.b.model.push(sp::call("write_file", R"({"path":"notes/a.md","text":"milk"})"));
    r.b.model.push(sp::say("Saved it."));
    plain::TurnOutcome out;
    TEST_ASSERT_TRUE(r.b.owner("save milk", out).ok());
    ends.push_back(lastEnd(r.stored())->end);
    outs.push_back(out);
    // The head never sees the probe: its requests carry no probe prompt and no probe frame.
    TEST_ASSERT_FALSE(anyRequestHas(r.b, "claims_unbacked_action"));
    TEST_ASSERT_FALSE(anyRequestHas(r.b, "<effects_this_turn>"));
  }
  TEST_ASSERT_EQUAL(0, (int)ends[0].probeRequests);
  TEST_ASSERT_EQUAL(1, (int)ends[1].probeRequests);
  TEST_ASSERT_EQUAL(ends[0].headRequests, ends[1].headRequests);
  TEST_ASSERT_EQUAL(ends[0].promptTokens, ends[1].promptTokens);
  TEST_ASSERT_EQUAL(ends[0].lastPromptTokens, ends[1].lastPromptTokens);
  TEST_ASSERT_EQUAL(ends[0].outputTokens, ends[1].outputTokens);
  TEST_ASSERT_EQUAL(ends[0].microUsd, ends[1].microUsd);
  TEST_ASSERT_EQUAL(outs[0].headRequests, outs[1].headRequests);
}

// Installation: Off registers nothing; no arena is refused; a taken slot is Busy.
void test_install_rules() {
  plain::PlainHooks hooks;
  plain::TruthDeps d;
  TEST_ASSERT_TRUE(plain::installTruth(hooks, d).err == tx::Err::InvalidArgument);
  d.mode = plain::TruthMode::Off;
  TEST_ASSERT_TRUE(plain::installTruth(hooks, d).ok());
  for (plain::Seam s : plain::kAllSeams) TEST_ASSERT_EQUAL(0, (int)hooks.registered(s));
  tx::TxArena arena;
  d.mode = plain::TruthMode::Enforce;
  d.arena = &arena;
  TEST_ASSERT_TRUE(plain::installTruth(hooks, d).ok());
  TEST_ASSERT_TRUE(hooks.has(plain::Seam::BeforeDeliver, plain::HookSlot::Truth));
  TEST_ASSERT_TRUE(hooks.has(plain::Seam::AfterDeliver, plain::HookSlot::Truth));
  TEST_ASSERT_TRUE(hooks.has(plain::Seam::IdleStep, plain::HookSlot::ShadowProbe));
  TEST_ASSERT_TRUE(plain::installTruth(hooks, d).err == tx::Err::Busy);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_probe_reads_recent_receipts_from_the_transcript);
  RUN_TEST(test_a_true_reply_is_never_held_and_nothing_stored_changes);
  RUN_TEST(test_recent_receipts_unknown_means_receipt_only);
  RUN_TEST(test_recent_receipts_from_the_card);
  RUN_TEST(test_every_effect_tool_has_a_receipt_kind);
  RUN_TEST(test_enforce_holds_before_delivery);
  RUN_TEST(test_no_second_hold_no_hold_on_a_cut_or_last_round);
  RUN_TEST(test_the_last_round_is_never_held_and_never_counted_as_held);
  RUN_TEST(test_held_round_effects_join_once);
  RUN_TEST(test_a_held_turn_shows_a_status_line_and_one_answer);
  RUN_TEST(test_a_held_reply_goes_out_when_the_round_fails);
  RUN_TEST(test_a_failed_held_round_still_sends_the_receipt);
  RUN_TEST(test_a_refused_router_call_is_no_failed_write);
  RUN_TEST(test_a_held_round_that_reads_outside_content_is_receipt_only);
  RUN_TEST(test_receipt_only_failed_write_sends_one_receipt_after_the_reply);
  RUN_TEST(test_shadow_probes_from_the_idle_step_only);
  RUN_TEST(test_a_probe_request_is_never_conversation);
  RUN_TEST(test_install_rules);
  return UNITY_END();
}
