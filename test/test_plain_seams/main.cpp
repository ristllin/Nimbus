// The engine-2 seams, lane RB-B0a: every header of nimbus/orch/tx and
// nimbus/harness/plain compiles and links in one binary; every stub answers the typed
// Err::NotImplemented naming itself (so a pending suite can say which seam is missing);
// the hooks registry runs the six seams in slot order; the frozen slug tables are whole.
//
// When an implementing lane writes a body, it removes that body's row from kStubs: the
// row would now fail, which is the point (a stub cannot quietly become a fake success).
#include <unity.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../support/scripted_provider/scripted_provider.h"
#include "nimbus/harness/plain/agents.h"
#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/docs.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/media.h"
#include "nimbus/harness/plain/rails.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/resume.h"
#include "nimbus/harness/plain/truth.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/harness/plain/unattended.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/migrate.h"
#include "nimbus/orch/tx/rules.h"
#include "nimbus/orch/tx/status.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

// Storage that refuses everything: enough for a stub's signature.
struct NoFiles : tx::TxFiles {
  bool append(const std::string&, const char*, size_t) override { return false; }
  bool readAt(const std::string&, size_t, char*, size_t, size_t& got) override {
    got = 0;
    return false;
  }
  bool size(const std::string&, size_t&) override { return false; }
  bool writeWhole(const std::string&, const std::string&) override { return false; }
  bool rename(const std::string&, const std::string&) override { return false; }
  bool remove(const std::string&) override { return false; }
  bool list(const std::string&, std::vector<std::string>&) override { return false; }
};

NoFiles gFiles;
tx::TxLog gLog(gFiles, "1001");
tx::TxArena gArena;
plain::PlainHooks gHooks;
plain::TurnCtx gTurn;
plain::ToolCall gCall;

struct StubRow {
  const char* where;
  std::function<tx::Status()> call;
};

// Every stub body, called once. Keep one row per function the stub files define.
std::vector<StubRow> stubs() {
  using std::string;
  static string s;
  static tx::Item item;
  static tx::HeadState head;
  static std::vector<tx::Item> items;
  static std::vector<tx::DayRow> rows;
  static bool flag;
  return {
      {"tx::profileMarks", [] { tx::ProfileMarks m; return tx::profileMarks(tx::Profile::Standard, m); }},
      {"tx::budgetFor", [] { tx::HeadBudget b; return tx::budgetFor(tx::HeadKey{}, tx::Profile::Lean, b); }},
      {"tx::dryRun", [] { tx::MigrationReport r; return tx::dryRun(gFiles, "1001", rows, r); }},
      {"tx::migrateStep", [] { tx::MigrationState m; return tx::migrateStep(gFiles, "1001", rows, m); }},
      {"tx::isReentry", [] { return tx::isReentry(gFiles, "1001", flag); }},
      {"tx::reentryItems", [] { return tx::reentryItems(head, rows, items); }},
      {"plain::wireFor anthropic", [] { const plain::WireCodec* w; return plain::wireFor(plain::Dialect::AnthropicMessages, w); }},
      {"plain::wireFor responses", [] { const plain::WireCodec* w; return plain::wireFor(plain::Dialect::OpenAIResponses, w); }},
      {"plain::wireFor chat", [] { const plain::WireCodec* w; return plain::wireFor(plain::Dialect::ChatCompletions, w); }},
      {"plain::wireFor flat", [] { const plain::WireCodec* w; return plain::wireFor(plain::Dialect::Flat, w); }},
      {"plain::PlainEngine::runTurn", [] { plain::PlainEngine e(plain::EngineDeps{}); plain::TurnOutcome o; return e.runTurn({}, o); }},
      {"plain::PlainEngine::idle", [] { plain::PlainEngine e(plain::EngineDeps{}); plain::IdleOutcome o; return e.idle({}, o); }},
      {"plain::PlainEngine::boot", [] { plain::PlainEngine e(plain::EngineDeps{}); return e.boot({}); }},
      {"plain::guardLimitsFor", [] { plain::GuardLimits l; return plain::guardLimitsFor(8, {}, l); }},
      {"plain::checkGuards", [] { plain::GuardTrip t; return plain::checkGuards({}, {}, t); }},
      {"plain::isPureTool", [] { return plain::isPureTool("read_file", flag); }},
      {"plain::installSuppression", [] { return plain::installSuppression(gHooks); }},
      {"plain::deliverTurn", [] { plain::DeliverOutcome o; return plain::deliverTurn({}, {}, o); }},
      {"plain::turnEndItem", [] { return plain::turnEndItem(gTurn, {}, item); }},
      {"plain::installResume", [] { return plain::installResume(gHooks, {}); }},
      {"plain::railFor", [] { plain::RailVerdict v; return plain::railFor(gTurn, gCall, v); }},
      {"plain::installRails", [] { return plain::installRails(gHooks, {}); }},
      {"plain::photoAllowed", [] { return plain::photoAllowed(gTurn, {}, flag); }},
      {"plain::installMedia", [] { return plain::installMedia(gHooks, {}); }},
      {"plain::installUnattended", [] { return plain::installUnattended(gHooks, {}); }},
      {"plain::routineChatKey", [] { return plain::routineChatKey("r1", s); }},
      {"plain::agentReportPath", [] { return plain::agentReportPath("a7", "trip", s); }},
      {"plain::installAgents", [] { return plain::installAgents(gHooks, {}); }},
      {"plain::installTruth", [] { return plain::installTruth(gHooks, {}); }},
      {"plain::documentRoute", [] { plain::DocRoute r; return plain::documentRoute(gTurn, "inbox/1-a.pdf", "application/pdf", r); }},
  };
}

}  // namespace

// Every stub answers NotImplemented with its own name, and no stub registers a hook.
static void test_every_stub_answers_not_implemented() {
  std::set<std::string> names;
  for (const StubRow& row : stubs()) {
    const tx::Status st = row.call();
    TEST_ASSERT_TRUE_MESSAGE(st.err == tx::Err::NotImplemented, row.where);
    TEST_ASSERT_TRUE_MESSAGE(st.notImplemented() && !st.ok(), row.where);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(row.where, st.where, row.where);
    TEST_ASSERT_TRUE_MESSAGE(names.insert(row.where).second, row.where);
  }
  for (plain::Seam seam : plain::kAllSeams) TEST_ASSERT_EQUAL(0, (int)gHooks.registered(seam));
  // A log over refusing storage never claims it stored an item.
  tx::StoreState state = tx::StoreState::Stored;
  (void)gLog.append(tx::Item{}, state);
  TEST_ASSERT_TRUE(state == tx::StoreState::NotStored);
  const plain::WireCodec* w = reinterpret_cast<const plain::WireCodec*>(&state);
  (void)plain::wireFor(plain::Dialect::Flat, w);
  TEST_ASSERT_NULL(w);
}

// The engine stub wired the way the engine lanes will wire it: a scripted model behind
// the transport. Until RB-E1a lands, a turn answers NotImplemented and sends nothing.
static void test_engine_stub_sends_nothing() {
  sp::ScriptedProvider model;
  model.push(sp::say("hello"));
  plain::EngineDeps deps;
  deps.transport = &model;
  deps.hooks = &gHooks;
  deps.log = &gLog;
  deps.arena = &gArena;
  plain::PlainEngine engine(deps);
  plain::TurnInput in;
  in.ctx.chatKey = "1001";
  in.text = "hi";
  plain::TurnOutcome out;
  const tx::Status st = engine.runTurn(in, out);
  TEST_ASSERT_TRUE(st.notImplemented());
  TEST_ASSERT_FALSE(out.delivered);
  TEST_ASSERT_EQUAL(0, (int)model.requests().size());
}

// ---- the hooks registry ------------------------------------------------------------

static void test_hooks_declares_the_six_seams() {
  const char* want[] = {"beforeDispatch", "afterResult", "beforeDeliver", "afterDeliver", "idleStep", "bootResume"};
  TEST_ASSERT_EQUAL(6, (int)plain::kSeamCount);
  for (size_t i = 0; i < plain::kSeamCount; ++i) TEST_ASSERT_EQUAL_STRING(want[i], plain::seamName(plain::kAllSeams[i]));
}

// One handler per slot per seam; a second one is Busy; a null one is refused.
static void test_registration_rules() {
  plain::PlainHooks h;
  auto run = [](const plain::DispatchCtx&) { return plain::DispatchDecision{}; };
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Rails, run).ok());
  const tx::Status again = h.beforeDispatch(plain::HookSlot::Rails, run);
  TEST_ASSERT_TRUE(again.err == tx::Err::Busy);
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Intents, run).ok());
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Media, nullptr).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(h.beforeDispatch(static_cast<plain::HookSlot>(plain::kHookSlotCount), run).err ==
                   tx::Err::InvalidArgument);
  // The Observer slot never decides: refused on the deciding seams, allowed elsewhere.
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Observer, run).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(h.beforeDeliver(plain::HookSlot::Observer, [](const plain::DeliverCtx&) {
                     return plain::DeliverDecision{};
                   }).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(h.idleStep(plain::HookSlot::Observer, [](const plain::IdleCtx&) { return plain::IdleOutcome{}; }).err ==
                   tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(h.afterDeliver(plain::HookSlot::Observer, [](const plain::DeliveredCtx&) {}).ok());
  // Nobody decided reads as no slot, not as some slot.
  TEST_ASSERT_TRUE(h.runBeforeDispatch({}).by == plain::HookSlot::None);
  // The same slot on another seam is free.
  TEST_ASSERT_TRUE(h.afterResult(plain::HookSlot::Rails, [](const plain::ResultCtx&) {}).ok());
  TEST_ASSERT_EQUAL(2, (int)h.registered(plain::Seam::BeforeDispatch));
  TEST_ASSERT_EQUAL(1, (int)h.registered(plain::Seam::AfterResult));
  TEST_ASSERT_EQUAL(1, (int)h.registered(plain::Seam::AfterDeliver));
  TEST_ASSERT_TRUE(h.has(plain::Seam::BeforeDispatch, plain::HookSlot::Intents));
  TEST_ASSERT_FALSE(h.has(plain::Seam::BeforeDispatch, plain::HookSlot::Media));
  h.clear();
  for (plain::Seam seam : plain::kAllSeams) TEST_ASSERT_EQUAL(0, (int)h.registered(seam));
}

// Order is slot order whatever the registration order; the first verdict that is not
// Run decides and names its slot; afterResult reaches every handler in slot order.
static void test_dispatch_runs_in_slot_order() {
  plain::PlainHooks h;
  std::vector<int> order;
  auto runner = [&order](int tag) {
    return [&order, tag](const plain::DispatchCtx&) {
      order.push_back(tag);
      return plain::DispatchDecision{};
    };
  };
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Intents, runner(3)).ok());
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Policy, runner(0)).ok());
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Suppression, runner(2)).ok());
  plain::DispatchDecision d = h.runBeforeDispatch({});
  TEST_ASSERT_TRUE(d.verdict == plain::DispatchVerdict::Run);
  TEST_ASSERT_EQUAL(3, (int)order.size());
  TEST_ASSERT_EQUAL(0, order[0]);
  TEST_ASSERT_EQUAL(2, order[1]);
  TEST_ASSERT_EQUAL(3, order[2]);
  // Rails refuses: later slots (suppression, intents) never see the call.
  TEST_ASSERT_TRUE(h.beforeDispatch(plain::HookSlot::Rails, [&order](const plain::DispatchCtx&) {
                     order.push_back(1);
                     plain::DispatchDecision r;
                     r.verdict = plain::DispatchVerdict::Refuse;
                     r.text = "refused: tainted turn";
                     return r;
                   }).ok());
  order.clear();
  d = h.runBeforeDispatch({});
  TEST_ASSERT_TRUE(d.verdict == plain::DispatchVerdict::Refuse);
  TEST_ASSERT_TRUE(d.by == plain::HookSlot::Rails);
  TEST_ASSERT_EQUAL_STRING("refused: tainted turn", d.text.c_str());
  TEST_ASSERT_EQUAL(2, (int)order.size());
  std::vector<int> seen;
  TEST_ASSERT_TRUE(h.afterResult(plain::HookSlot::Observer, [&seen](const plain::ResultCtx&) { seen.push_back(11); }).ok());
  TEST_ASSERT_TRUE(h.afterResult(plain::HookSlot::Truth, [&seen](const plain::ResultCtx&) { seen.push_back(5); }).ok());
  h.runAfterResult({});
  TEST_ASSERT_EQUAL(2, (int)seen.size());
  TEST_ASSERT_EQUAL(5, seen[0]);
  TEST_ASSERT_EQUAL(11, seen[1]);
}

// At most one hold a turn: a held turn delivers whatever the seam would say.
static void test_deliver_holds_at_most_once() {
  plain::PlainHooks h;
  int asked = 0;
  TEST_ASSERT_TRUE(h.beforeDeliver(plain::HookSlot::Truth, [&asked](const plain::DeliverCtx&) {
                     ++asked;
                     plain::DeliverDecision d;
                     d.verdict = plain::DeliverVerdict::Hold;
                     d.deviceNote = "[device] effects this turn: none.";
                     return d;
                   }).ok());
  plain::DeliverCtx ctx;
  plain::DeliverDecision d = h.runBeforeDeliver(ctx);
  TEST_ASSERT_TRUE(d.verdict == plain::DeliverVerdict::Hold);
  TEST_ASSERT_TRUE(d.by == plain::HookSlot::Truth);
  ctx.held = true;
  d = h.runBeforeDeliver(ctx);
  TEST_ASSERT_TRUE(d.verdict == plain::DeliverVerdict::Deliver);
  TEST_ASSERT_EQUAL(1, asked);
  int delivered = 0;
  TEST_ASSERT_TRUE(h.afterDeliver(plain::HookSlot::Observer, [&delivered](const plain::DeliveredCtx&) { ++delivered; }).ok());
  h.runAfterDeliver({});
  TEST_ASSERT_EQUAL(1, delivered);
}

// Idle work: one unit per step in the fixed order, nothing while the owner waits.
static void test_idle_order_and_owner_first() {
  plain::PlainHooks h;
  std::vector<plain::HookSlot> ran;
  auto unit = [&ran](plain::HookSlot slot, bool works) {
    return [&ran, slot, works](const plain::IdleCtx&) {
      ran.push_back(slot);
      plain::IdleOutcome o;
      o.worked = works;
      return o;
    };
  };
  TEST_ASSERT_TRUE(h.idleStep(plain::HookSlot::ShadowProbe, unit(plain::HookSlot::ShadowProbe, true)).ok());
  TEST_ASSERT_TRUE(h.idleStep(plain::HookSlot::Compaction, unit(plain::HookSlot::Compaction, true)).ok());
  TEST_ASSERT_TRUE(h.idleStep(plain::HookSlot::Agents, unit(plain::HookSlot::Agents, false)).ok());
  plain::IdleCtx ctx;
  ctx.ownerWaiting = true;
  TEST_ASSERT_FALSE(h.runIdleStep(ctx).worked);
  TEST_ASSERT_EQUAL(0, (int)ran.size());
  ctx.ownerWaiting = false;
  const plain::IdleOutcome o = h.runIdleStep(ctx);
  TEST_ASSERT_TRUE(o.worked);
  TEST_ASSERT_TRUE(o.by == plain::HookSlot::Compaction);
  TEST_ASSERT_EQUAL(2, (int)ran.size());   // agents (nothing to do), then compaction; the probe waits
  TEST_ASSERT_TRUE(ran[0] == plain::HookSlot::Agents);
  // The PRD's idle order is the slot order.
  TEST_ASSERT_TRUE(plain::HookSlot::Agents < plain::HookSlot::Routines);
  TEST_ASSERT_TRUE(plain::HookSlot::Routines < plain::HookSlot::Compaction);
  TEST_ASSERT_TRUE(plain::HookSlot::Compaction < plain::HookSlot::ShadowProbe);
}

// Boot resume: every handler runs; the first failure is the answer.
static void test_boot_resume_runs_all() {
  plain::PlainHooks h;
  int calls = 0;
  TEST_ASSERT_TRUE(h.bootResume(plain::HookSlot::Resume, [&calls](const plain::BootCtx&) {
                     ++calls;
                     return tx::Status::fail(tx::Err::Io, "resume");
                   }).ok());
  TEST_ASSERT_TRUE(h.bootResume(plain::HookSlot::Observer, [&calls](const plain::BootCtx&) {
                     ++calls;
                     return tx::Status::fail(tx::Err::NotFound, "observer");
                   }).ok());
  const tx::Status st = h.runBootResume({});
  TEST_ASSERT_EQUAL(2, calls);
  TEST_ASSERT_TRUE(st.err == tx::Err::Io);
  TEST_ASSERT_TRUE(plain::PlainHooks{}.runBootResume({}).ok());
}

// ---- the frozen slug tables are whole and unique --------------------------------------

template <class E, size_t N>
static void assertUniqueSlugs(const E (&all)[N], const char* (*slug)(E), const char* what) {
  std::set<std::string> seen;
  for (E e : all) {
    const std::string s = slug(e);
    TEST_ASSERT_FALSE_MESSAGE(s.empty(), what);
    TEST_ASSERT_TRUE_MESSAGE(seen.insert(s).second, (std::string(what) + " " + s).c_str());
  }
}

static void test_slug_tables_are_whole() {
  std::set<char> letters;
  for (tx::ItemKind k : tx::kAllItemKinds) {
    tx::ItemKind back;
    TEST_ASSERT_TRUE(tx::kindFromLetter(tx::kindLetter(k), back));
    TEST_ASSERT_TRUE(back == k);
    TEST_ASSERT_TRUE(letters.insert(tx::kindLetter(k)).second);
  }
  TEST_ASSERT_EQUAL(12, (int)tx::kItemKindCount);
  tx::ItemKind none;
  TEST_ASSERT_FALSE(tx::kindFromLetter('z', none));   // z is the crc key, never a kind
  assertUniqueSlugs(plain::kAllStatusLines, plain::statusLineSlug, "status line");
  TEST_ASSERT_TRUE(plain::personTrigger(plain::Trigger::Resume));
  TEST_ASSERT_FALSE(plain::personTrigger(plain::Trigger::Routine));
  assertUniqueSlugs(plain::kAllDialects, plain::dialectSlug, "dialect");
  const plain::Trigger triggers[] = {plain::Trigger::Owner,       plain::Trigger::Console, plain::Trigger::Routine,
                                     plain::Trigger::AgentResult, plain::Trigger::Notice,  plain::Trigger::Resume};
  assertUniqueSlugs(triggers, plain::triggerSlug, "trigger");
  const tx::Profile profiles[] = {tx::Profile::Tight, tx::Profile::Lean, tx::Profile::Standard, tx::Profile::Wide};
  assertUniqueSlugs(profiles, tx::profileSlug, "profile");
  const tx::Err errs[] = {tx::Err::None,     tx::Err::NotImplemented, tx::Err::InvalidArgument,
                          tx::Err::NotStored, tx::Err::NotFound,      tx::Err::Torn,
                          tx::Err::TooLarge,  tx::Err::Io,            tx::Err::Busy};
  assertUniqueSlugs(errs, tx::errSlug, "err");
  const plain::GuardTrip trips[] = {plain::GuardTrip::None, plain::GuardTrip::Rounds, plain::GuardTrip::Time,
                                    plain::GuardTrip::Spend, plain::GuardTrip::Size};
  assertUniqueSlugs(trips, plain::guardTripSlug, "guard trip");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_stub_answers_not_implemented);
  RUN_TEST(test_engine_stub_sends_nothing);
  RUN_TEST(test_hooks_declares_the_six_seams);
  RUN_TEST(test_registration_rules);
  RUN_TEST(test_dispatch_runs_in_slot_order);
  RUN_TEST(test_deliver_holds_at_most_once);
  RUN_TEST(test_idle_order_and_owner_first);
  RUN_TEST(test_boot_resume_runs_all);
  RUN_TEST(test_slug_tables_are_whole);
  return UNITY_END();
}
