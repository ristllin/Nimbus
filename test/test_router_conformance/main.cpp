// test_router_conformance - the battery over provider table rows (lane E2-R3a; router design section 11,
// layer L3, properties 1 to 7 and 10 to 11 that need no new engine code).
//
// Every property runs once per row of txb::tableRows() (each provider of the capability table, direct,
// and through the Cumulo router where the table says the router carries it). A row whose codec has not
// landed WAITS: the property prints the (property, row, lane) case and the last test prints how many
// cases wait. A waiting case is neither a failure nor a silent skip, and it runs by itself once its
// codec's sentinel answers (pending.h); -DTXB_FORCE_PENDING runs every row.
//
// Each checker in row_props.h is counter-tested once here on a deliberate break.
#include <unity.h>

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../support/scripted_provider/wire_fixture.h"
#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/render.h"
#include "../support/tx_battery/rows.h"
#include "row_props.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace caps = nimbus::orch::caps;

void setUp() {}
void tearDown() {}

namespace {

size_t g_ran = 0, g_ranAnthropic = 0;

// Run `body` on every row; a row still waiting for `extra` lanes or its codec is logged, not run.
// An engine turn on a routed row also waits for `routedLane` (the routed binding, design lane R8).
void forEachRow(const char* property, std::initializer_list<const char*> extra,
                const std::function<void(const txb::Row&)>& body, const char* routedLane = nullptr,
                bool routedOnly = false) {
  size_t waiting = 0, rows = 0;
  for (const txb::Row& r : txb::tableRows()) {
    if (routedOnly && !r.routed) continue;
    ++rows;
    std::string lane = txb::rowWaitsFor(r, extra);
    if (lane.empty() && r.routed && routedLane) lane = txb::rowWaitsFor(r, {routedLane});
    if (lane.rfind("ERROR ", 0) == 0) TEST_FAIL_MESSAGE(lane.c_str());
    if (!lane.empty()) {
      txb::waitLog().note(property, r, lane);
      std::printf("WAITING %s | %s | %s\n", property, r.name.c_str(), lane.c_str());
      ++waiting;
      continue;
    }
    std::printf("RUN %s | %s\n", property, r.name.c_str());
    body(r);
    ++g_ran;
    if (r.slot == caps::Slot::Anthropic) ++g_ranAnthropic;
  }
  std::printf("%s: %zu rows ran, %zu waiting\n", property, rows - waiting, waiting);
}

std::string msgOf(const txb::Row& r, const std::string& what) { return r.name + ": " + what; }

// The stored transcripts every render property reads: the four-turn day, snapshot by snapshot.
std::vector<txb::Snapshot> dayFor(const txb::Row& r) { return txb::dayOfFourTurns(r.dialect); }

// ---- answer fixtures per row ---------------------------------------------------------------------------------
// Anthropic's recorded bodies carry no header; their expectations are written here.
struct AnthropicExpect {
  const char* file;
  const char* expect;
};
const AnthropicExpect kAnthropic[] = {
    {"max_tokens_cut_call.json", "stop=cut complete=false calls=1"},
    {"thinking_over_the_line_cut.json", "stop=cut complete=false calls=0"},
    {"synthetic_refusal.json", "stop=refusal complete=false calls=1"},
    {"tool_use_one.json", "stop=tool complete=true calls=1"},
    {"tool_use_two.json", "stop=tool complete=true calls=2"},
    {"text_end_turn.json", "stop=end complete=true calls=0"},
    {"thinking_then_call.json", "stop=tool complete=true calls=1"},
    {"thinking_then_text.json", "stop=end complete=true calls=0"},
    {"input_transformations_dropped.json", "stop=tool complete=true calls=1"},
    {"error_not_found.json", "class=unknown_model retry=no"},
};

std::vector<sp::WireFixture> fixturesFor(const txb::Row& r) {
  std::vector<sp::WireFixture> out;
  std::string err;
  const char* dir = nullptr;
  switch (r.slot) {
    case caps::Slot::OpenAI: dir = "test/support/fixtures/openai_wire_synthetic"; break;
    case caps::Slot::Mistral: dir = "test/support/fixtures/mistral_wire_synthetic"; break;
    case caps::Slot::Zai: dir = "test/support/fixtures/zai_wire_synthetic"; break;
    case caps::Slot::Anthropic:
    case caps::Slot::CustomMessages:
      for (const AnthropicExpect& a : kAnthropic) {
        sp::WireFixture f;
        f.file = std::string("test/support/fixtures/anthropic_wire_2026-11/") + a.file;
        TEST_ASSERT_TRUE_MESSAGE(sp::readWhole(f.file, f.body), f.file.c_str());
        f.source = "recorded: anthropic_wire_2026-11, read 2026-11-24";
        f.expect = a.expect;
        f.status = f.expected("class").empty() ? 200 : 404;
        out.push_back(f);
      }
      break;
    default: break;   // custom chat: no provider of its own; covered by the shared chat fixtures' rows
  }
  if (dir) TEST_ASSERT_TRUE_MESSAGE(sp::loadWireFixtures(dir, out, err), err.c_str());
  if (r.routed) TEST_ASSERT_TRUE_MESSAGE(sp::loadWireFixtures("test/support/fixtures/router_wire_synthetic", out, err), err.c_str());
  return out;
}

plain::StopReason stopOf(const std::string& slug) {
  if (slug == "end") return plain::StopReason::EndTurn;
  if (slug == "tool") return plain::StopReason::ToolUse;
  if (slug == "cut") return plain::StopReason::MaxTokens;
  if (slug == "refusal") return plain::StopReason::Refusal;
  return plain::StopReason::Other;   // context: StopReason::Context arrives with lane R4a
}

tx::Status parseWith(const txb::Row& r, const sp::WireFixture& f, plain::ParsedStep& step) {
  const plain::WireCodec* w = nullptr;
  tx::Status st = plain::wireFor(r.dialect, w);
  if (!st.ok()) return st;
  agent::HttpResponse resp;
  resp.status = f.status;
  resp.body = f.body;
  return w->parse(resp, step);
}

}  // namespace

// ---- the rows ------------------------------------------------------------------------------------------------------
void test_rows_are_the_capability_table() {
  const std::vector<txb::Row> rows = txb::tableRows();
  TEST_ASSERT_EQUAL_MESSAGE((int)caps::builtinCaps().providerCount + 4, (int)rows.size(), "6 providers and 4 routed");
  for (const txb::Row& r : rows) {
    const caps::Caps c = caps::capsFor(r.slot, r.model.c_str(), r.routed);
    TEST_ASSERT_NOT_NULL_MESSAGE(c.provider, r.name.c_str());
    const bool custom = r.slot == caps::Slot::CustomChat || r.slot == caps::Slot::CustomMessages;
    TEST_ASSERT_TRUE_MESSAGE(custom || c.known, msgOf(r, "the table does not know " + r.model).c_str());
    TEST_ASSERT_TRUE_MESSAGE(txb::engineDialect(c.provider->dialect) == r.dialect, r.name.c_str());
  }
}

// ---- 1. same input, same bytes (also after reloading the transcript from its stored lines) ------------------------
void test_p1_same_input_same_bytes() {
  forEachRow("p1 same bytes", {"RB-T1"}, [](const txb::Row& r) {
    txb::RenderFixture fx(r);
    for (const txb::Snapshot& s : dayFor(r)) {
      txb::Rendered a, b, c;
      TXB_OK(txb::renderItems(fx, s.c, 0, a, s.openTurn));
      TXB_OK(txb::renderItems(fx, s.c, 0, b, s.openTurn));
      txb::Convo back = s.c;
      for (tx::Item& it : back.items) {
        std::string line;
        TXB_OK(tx::encodeLine(it, line));
        TXB_OK(tx::decodeLine(line.data(), line.size(), it));
      }
      TXB_OK(txb::renderItems(fx, back, 0, c, s.openTurn));
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(rc::sameBytes(a.body, b.body, s.name + " twice", why), msgOf(r, why).c_str());
      TEST_ASSERT_TRUE_MESSAGE(rc::sameBytes(a.body, c.body, s.name + " reloaded", why), msgOf(r, why).c_str());
    }
  });
}

// ---- 2. stable prefix after appending a turn; the tools array is identical every round, guard round too -----------
void test_p2_stable_prefix_and_tools() {
  forEachRow("p2 stable prefix", {"RB-T1"}, [](const txb::Row& r) {
    txb::RenderFixture fx(r);
    const std::vector<txb::Snapshot> snaps = dayFor(r);
    std::vector<txb::Rendered> out(snaps.size());
    for (size_t i = 0; i < snaps.size(); ++i) TXB_OK(txb::renderItems(fx, snaps[i].c, 0, out[i], snaps[i].openTurn));
    // The last snapshot is the outside change point (test_tx_prefix checks it apart).
    for (size_t i = 1; i + 1 < snaps.size(); ++i) {
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(sp::isPrefix(out[i - 1].view, out[i].view, why), msgOf(r, snaps[i].name + ": " + why).c_str());
      TEST_ASSERT_TRUE_MESSAGE(rc::toolsSame(out[0].view, out[i].view, why), msgOf(r, why).c_str());
    }
    // The guard round: tool choice none, the same tools array.
    const txb::Snapshot& last = snaps[snaps.size() - 2];
    txb::Rendered guard;
    TXB_OK(txb::renderItems(fx, last.c, 0, guard, last.openTurn, true));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("none", guard.view.toolChoice.c_str(), msgOf(r, "guard round").c_str());
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(rc::toolsSame(out[0].view, guard.view, why), msgOf(r, "guard round: " + why).c_str());
  });
}

// ---- 3. pairing: every call has exactly one result after it, at every stub pointer ----------------------------------
void test_p3_every_call_has_one_result() {
  forEachRow("p3 pairing", {"RB-T1"}, [](const txb::Row& r) {
    txb::RenderFixture fx(r);
    for (const txb::Snapshot& s : dayFor(r))
      for (uint64_t p = 0; p <= s.c.q; ++p) {
        txb::Rendered out;
        TXB_OK(txb::renderItems(fx, s.c, p, out, s.openTurn));
        std::string why;
        TEST_ASSERT_TRUE_MESSAGE(txb::viewPaired(out.view, why),
                                 msgOf(r, s.name + " pointer " + std::to_string(p) + ": " + why).c_str());
      }
  });
}

// ---- 4. tool round trip through the engine ------------------------------------------------------------------------------
void test_p4_tool_round_trip() {
  forEachRow("p4 round trip", {"RB-E1a"}, [](const txb::Row& r) {
    txb::Battery b(r);
    b.world.files["notes/a.md"] = "alpha";
    b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
    b.model.push(sp::say("It says alpha."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("What is in notes/a.md?", out));
    TEST_ASSERT_EQUAL_MESSAGE(2, (int)b.model.requests().size(), r.name.c_str());
    const sp::RequestView& next = b.model.requests()[1].view;
    TEST_ASSERT_TRUE_MESSAGE(next.parsed, msgOf(r, next.parseError).c_str());
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(rc::roundTripHolds(b.world.dispatched, "read_file", next, why), msgOf(r, why).c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("It says alpha.", out.text.c_str(), r.name.c_str());
    TEST_ASSERT_TRUE_MESSAGE(b.model.verify(why), msgOf(r, why).c_str());
  }, "RB-R8");
}

// ---- 5. a not-complete answer's calls never run (in parse and after the exchange) --------------------------------------
void test_p5_cut_answer_calls_never_run() {
  forEachRow("p5 cut calls never run", {"RB-E1a"}, [](const txb::Row& r) {
    // Scripted: a cut answer and a context answer that each call an effect.
    for (sp::Finish fin : {sp::Finish::MaxTokens, sp::Finish::Context}) {
      txb::Battery b(r);
      sp::Step s = sp::call("ledger_write", R"({"day":"2026-11-24"})", "Logging");
      s.finish = fin;
      b.model.push(s);
      b.model.respondWith([](const sp::Recorded&, size_t) { return sp::say("Done."); });
      plain::TurnOutcome out;
      TXB_OK(b.owner("log it", out));
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(rc::nothingRan(b.world.dispatched, "ledger_write", why), msgOf(r, why).c_str());
    }
    // Every recorded or synthetic answer that is not complete: no call complete in parse, none run after it.
    for (const sp::WireFixture& f : fixturesFor(r)) {
      if (f.expected("complete") != "false") continue;
      plain::ParsedStep step;
      TXB_OK(parseWith(r, f, step));
      for (const plain::ToolCall& c : step.calls)
        TEST_ASSERT_FALSE_MESSAGE(c.complete, msgOf(r, f.file + ": a call reads as complete").c_str());
      if (f.expected("stop") != "context")
        TEST_ASSERT_EQUAL_STRING_MESSAGE(plain::stopSlug(stopOf(f.expected("stop"))), plain::stopSlug(step.stop),
                                         msgOf(r, f.file).c_str());
      txb::Battery b(r);
      b.model.push(sp::replay(f));
      b.model.respondWith([](const sp::Recorded&, size_t) { return sp::say("Done."); });
      plain::TurnOutcome out;
      (void)b.owner("go on", out);
      TEST_ASSERT_TRUE_MESSAGE(b.world.dispatched.empty(), msgOf(r, f.file + ": a call of a cut answer ran").c_str());
    }
  }, "RB-R8");
}

// ---- 6. errors that must not retry: billing, keys, an unknown model, the router's park classes -------------------------
void test_p6_no_retry_on_billing_auth_or_unknown_model() {
  forEachRow("p6 no retry", {"RB-E1a"}, [](const txb::Row& r) {
    std::vector<sp::WireFixture> fs = fixturesFor(r);
    if (r.dialect == plain::Dialect::AnthropicMessages && !r.routed)
      for (int status : {401, 402}) {
        sp::WireFixture f;
        f.file = "scripted anthropic " + std::to_string(status);
        f.status = status;
        f.body = sp::renderError(sp::Dialect::Anthropic, status);
        f.expect = status == 401 ? "class=key_auth retry=no" : "class=billing retry=no";
        fs.push_back(f);
      }
    size_t checked = 0;
    for (const sp::WireFixture& f : fs) {
      const std::string cls = f.expected("class");
      if (f.expected("retry") != "no" || !(cls == "billing" || cls == "key_auth" || cls == "park_billing" ||
                                           cls == "park_auth" || cls == "unknown_model"))
        continue;
      txb::Battery b(r);
      b.model.push(sp::replay(f));
      b.model.respondWith([](const sp::Recorded&, size_t) { return sp::say("retried"); });
      plain::TurnOutcome out;
      (void)b.owner("hello", out);
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)b.model.requests().size(), msgOf(r, f.file + ": retried").c_str());
      TEST_ASSERT_TRUE_MESSAGE(out.text != "retried", msgOf(r, f.file).c_str());
      ++checked;
    }
    TEST_ASSERT_TRUE_MESSAGE(checked > 0, msgOf(r, "no error fixture checked").c_str());
  }, "RB-R8");
}

// ---- 7. usage arithmetic: uncached + read + write by the row's usage mode ----------------------------------------------
uint64_t rawRequestIn(plain::Dialect d, const std::string& body) {
  JsonDocument doc, filter;
  filter["usage"] = true;   // only the usage: a recorded body may carry a 150 KB thinking block
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return 0;
  JsonObjectConst u = doc["usage"];
  if (d == plain::Dialect::AnthropicMessages)
    return (uint64_t)(u["input_tokens"] | 0) + (u["cache_read_input_tokens"] | 0) + (u["cache_creation_input_tokens"] | 0);
  if (d == plain::Dialect::OpenAIResponses) return u["input_tokens"] | 0;
  return u["prompt_tokens"] | 0;
}

void test_p7_usage_arithmetic() {
  forEachRow("p7 usage", {}, [](const txb::Row& r) {
    const caps::UsageMode mode = caps::capsFor(r.slot, r.model.c_str(), r.routed).row.usageMode;
    size_t checked = 0;
    for (const sp::WireFixture& f : fixturesFor(r)) {
      if (f.status != 200 || f.body.find("\"usage\"") == std::string::npos || !f.expected("class").empty()) continue;
      plain::ParsedStep step;
      TXB_OK(parseWith(r, f, step));
      const uint64_t want = rawRequestIn(r.dialect, f.body);
      TEST_ASSERT_TRUE_MESSAGE(want > 0, msgOf(r, f.file).c_str());
      TEST_ASSERT_EQUAL_UINT64_MESSAGE(want, rc::requestIn(step.usage, mode), msgOf(r, f.file).c_str());
      ++checked;
    }
    if (r.slot != caps::Slot::CustomChat) TEST_ASSERT_TRUE_MESSAGE(checked > 0, msgOf(r, "no usage fixture").c_str());
  });
}

// ---- 10. never reduce history: a later request carries every stored turn the earlier one did -----------------------
void test_p10_history_never_shrinks() {
  forEachRow("p10 history kept", {"RB-T1"}, [](const txb::Row& r) {
    txb::RenderFixture fx(r);
    const std::vector<txb::Snapshot> snaps = dayFor(r);
    txb::Rendered prev;
    for (size_t i = 0; i < snaps.size(); ++i) {
      txb::Rendered cur;
      TXB_OK(txb::renderItems(fx, snaps[i].c, 0, cur, snaps[i].openTurn));
      std::string why;
      if (i) TEST_ASSERT_TRUE_MESSAGE(rc::historyKept(prev.view, cur.view, why), msgOf(r, snaps[i].name + ": " + why).c_str());
      prev = cur;
    }
  });
}

// ---- 11. limits: string size, UTF-8, nesting on every render; a routed request declares its deadline and cap ----------
void test_p11_limits() {
  forEachRow("p11 limits", {"RB-T1"}, [](const txb::Row& r) {
    txb::RenderFixture fx(r);
    for (const txb::Snapshot& s : dayFor(r))
      for (uint64_t p : {(uint64_t)0, s.c.q}) {
        txb::Rendered out;
        TXB_OK(txb::renderItems(fx, s.c, p, out, s.openTurn));
        std::string why;
        TEST_ASSERT_TRUE_MESSAGE(rc::limitsHold(out.body, why), msgOf(r, s.name + ": " + why).c_str());
        TEST_ASSERT_TRUE_MESSAGE(out.view.maxTokens > 0, msgOf(r, s.name + ": no output cap").c_str());
      }
  });
}

void test_p11_routed_request_declares_deadline_and_cap() {
  forEachRow("p11 routed deadline", {"RB-E1a", "RB-R8"}, [](const txb::Row& r) {
    txb::Battery b(r);
    b.model.push(sp::say("Hello."));
    plain::TurnOutcome out;
    TXB_OK(b.owner("hi", out));
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(rc::routedDeclares(b.model.last().req, b.model.last().view, why), msgOf(r, why).c_str());
  }, nullptr, true);
}

// ---- counter-tests: each checker goes red on a deliberate break -----------------------------------------------------------
void test_counter_each_property_catches_its_break() {
  std::string why;
  // 1: one byte moved.
  TEST_ASSERT_FALSE(rc::sameBytes("{\"a\":1}", "{\"a\":2}", "x", why));
  TEST_ASSERT_TRUE(why.find("offset 5") != std::string::npos);
  // 2: a tool description changed between rounds.
  sp::RequestView a, b;
  a.toolsJson = {"{\"name\":\"read_file\"}"};
  b.toolsJson = {"{\"name\":\"read_file\",\"description\":\"x\"}"};
  TEST_ASSERT_FALSE(rc::toolsSame(a, b, why));
  // 4: the call ran twice; the result is missing.
  sp::RequestView next;
  next.parsed = true;
  next.messages = {{"user", {sp::detail::textBlock("go")}},
                   {"assistant", {sp::Block{sp::BlockKind::ToolCall, "", "c1", "read_file", "{}", false, ""}}}};
  TEST_ASSERT_FALSE(rc::roundTripHolds({"read_file", "read_file"}, "read_file", next, why));
  TEST_ASSERT_FALSE(rc::roundTripHolds({"read_file"}, "read_file", next, why));
  TEST_ASSERT_TRUE(why.find("no result") != std::string::npos);
  next.messages.push_back({"user", {sp::Block{sp::BlockKind::ToolResult, "alpha", "c1", "", "", false, ""}}});
  TEST_ASSERT_TRUE_MESSAGE(rc::roundTripHolds({"read_file"}, "read_file", next, why), why.c_str());
  // 5: the cut call ran.
  TEST_ASSERT_FALSE(rc::nothingRan({"ledger_write"}, "ledger_write", why));
  // 7: the wrong usage mode counts the cache twice or not at all.
  nimbus::orch::TokenUsage u;
  u.promptTokens = 100;
  u.cacheReadTokens = 900;
  u.cacheWriteTokens = 50;
  TEST_ASSERT_EQUAL_UINT64(1050, rc::requestIn(u, caps::UsageMode::InputExcludesCache));
  TEST_ASSERT_EQUAL_UINT64(150, rc::requestIn(u, caps::UsageMode::InputIncludesCache));
  // 10: a later request with one owner message fewer.
  sp::RequestView more, fewer;
  more.messages = {{"user", {sp::detail::textBlock("one")}}, {"assistant", {sp::detail::textBlock("a")}}, {"user", {sp::detail::textBlock("two")}}};
  fewer.messages = {{"user", {sp::detail::textBlock("two")}}};
  TEST_ASSERT_FALSE(rc::historyKept(more, fewer, why));
  // 11: a long string, invalid UTF-8, deep nesting; a routed request without the header or the cap.
  TEST_ASSERT_FALSE(rc::limitsHold("{\"x\":\"" + std::string(60001, 'a') + "\"}", why));
  TEST_ASSERT_TRUE(rc::limitsHold("{\"x\":\"" + std::string(60000, 'a') + "\"}", why));
  TEST_ASSERT_FALSE(rc::limitsHold(std::string("{\"x\":\"\xC3\x28\"}"), why));
  TEST_ASSERT_FALSE(rc::limitsHold(std::string(33, '[') + std::string(33, ']'), why));
  TEST_ASSERT_TRUE(rc::limitsHold(std::string(32, '[') + std::string(32, ']'), why));
  TEST_ASSERT_TRUE(rc::limitsHold("{\"x\":\"caf\xC3\xA9 \\\" [[[\"}", why));   // brackets in a string do not nest
  agent::HttpRequest req;
  sp::RequestView v;
  v.maxTokens = 4096;
  TEST_ASSERT_FALSE(rc::routedDeclares(req, v, why));
  req.headers.emplace_back("x-cumulo-deadline-ms", "85000");
  TEST_ASSERT_TRUE(rc::routedDeclares(req, v, why));
  v.maxTokens = 0;
  TEST_ASSERT_FALSE(rc::routedDeclares(req, v, why));
}

// Every synthetic fixture loads with its source line, and the replay serves it byte for byte.
void test_fixtures_carry_their_source_and_replay_verbatim() {
  size_t n = 0;
  for (const char* dir : {"test/support/fixtures/openai_wire_synthetic", "test/support/fixtures/mistral_wire_synthetic",
                          "test/support/fixtures/zai_wire_synthetic", "test/support/fixtures/router_wire_synthetic"}) {
    std::vector<sp::WireFixture> fs;
    std::string err;
    TEST_ASSERT_TRUE_MESSAGE(sp::loadWireFixtures(dir, fs, err), err.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.size() >= 9, dir);
    for (const sp::WireFixture& f : fs) {
      TEST_ASSERT_TRUE_MESSAGE(f.source.rfind("synthetic: ", 0) == 0 && f.source.find(", read 2026-11-24") != std::string::npos,
                               f.file.c_str());
      TEST_ASSERT_TRUE_MESSAGE(!f.expected("stop").empty() || !f.expected("class").empty(), f.file.c_str());
      JsonDocument doc;
      TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, f.body), f.file.c_str());
      sp::ScriptedProvider p;
      p.push(sp::replay(f));
      agent::HttpRequest req;
      req.path = "/v1/chat/completions";
      req.body = "{}";
      agent::HttpResponse resp;
      TEST_ASSERT_TRUE(p.exec(req, resp, err));
      TEST_ASSERT_EQUAL(f.status, resp.status);
      TEST_ASSERT_TRUE_MESSAGE(resp.body == f.body, f.file.c_str());
      ++n;
    }
  }
  // A body without its source line does not load.
  sp::WireFixture bad;
  std::string err;
  TEST_ASSERT_FALSE(sp::parseWireFixture("status: 200\nexpect: stop=end\n{}\n", bad, err));
  TEST_ASSERT_FALSE(sp::parseWireFixture("synthetic: https://x, read 2026-11-24\nexpect: stop=end\n{}\n", bad, err));
  std::printf("synthetic fixtures: %zu\n", n);
}

void test_zz_waiting_cases_are_counted() {
  const auto& lines = txb::waitLog().lines;
  std::printf("ROUTER CONFORMANCE: %zu row cases ran (%zu on Anthropic rows), %zu waiting\n", g_ran, g_ranAnthropic,
              lines.size());
  for (const std::string& l : lines) std::printf("  waiting: %s\n", l.c_str());
  TEST_ASSERT_TRUE_MESSAGE(g_ranAnthropic > 0, "no property ran on an Anthropic row");
  // The count also rides the case's own Unity result line (pio test -v, and the raw native log).
  static char summary[96];
  std::snprintf(summary, sizeof summary, "%zu row cases ran, %zu waiting", g_ran, lines.size());
  TEST_PASS_MESSAGE(summary);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_rows_are_the_capability_table);
  RUN_TEST(test_counter_each_property_catches_its_break);
  RUN_TEST(test_fixtures_carry_their_source_and_replay_verbatim);
  RUN_TEST(test_p1_same_input_same_bytes);
  RUN_TEST(test_p2_stable_prefix_and_tools);
  RUN_TEST(test_p3_every_call_has_one_result);
  RUN_TEST(test_p4_tool_round_trip);
  RUN_TEST(test_p5_cut_answer_calls_never_run);
  RUN_TEST(test_p6_no_retry_on_billing_auth_or_unknown_model);
  RUN_TEST(test_p7_usage_arithmetic);
  RUN_TEST(test_p10_history_never_shrinks);
  RUN_TEST(test_p11_limits);
  RUN_TEST(test_p11_routed_request_declares_deadline_and_cap);
  RUN_TEST(test_zz_waiting_cases_are_counted);
  return UNITY_END();
}
