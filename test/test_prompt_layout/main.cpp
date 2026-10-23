#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../support/fake_platform.h"
#include "../support/fake_provider_deps.h"
#include "nimbus/harness/compose.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/wire_limits.h"  // kJsonStrGuard (prism row 9)

// Prompt layout suite (prompt_layout.h): the cacheable shape of a head turn.
//   - the split markers round-trip and never reach a wire;
//   - PREFIX STABILITY, the class rule: two turns that differ only in per-turn
//     content (clock, running memory, recall, scratchpad, sessions, speaker,
//     loop gate, user text) share the static and shelf blocks byte for byte -
//     in the composed prompt AND in every provider's request - with a
//     counter-test proving the check catches a timestamp in the static block;
//   - the history rides as real messages (the summary pair first) and the
//     turns of one chat nest: turn N+1's history starts with turn N's;
//   - the tools array is byte-identical across turns and rounds (CUM-641: the
//     forced final round included);
//   - Anthropic never exceeds 4 cache points, and a 1-hour point never
//     follows a 5-minute one; OpenAI carries at most 3 explicit breakpoints;
//   - image parts never carry a cache point;
//   - the TTL table (1 hour when the chat's median gap is 5-60 minutes) and
//     the per-chat prompt_cache_key;
//   - one golden layout per provider (GOLDEN_UPDATE=1 re-blesses; look at the
//     diff in test/golden/out before blessing).

using harness_test::FakeProviderDeps;
using harness_test::LogCapture;
namespace layout = agent::layout;
namespace providers = agent::providers;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

// ---- golden helpers (same contract as test_harness_compose) -----------------
static bool blessMode() {
  const char* e = std::getenv("GOLDEN_UPDATE");
  return e && std::strcmp(e, "1") == 0;
}
static bool readFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
static void writeFile(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}
static void checkTextGolden(const char* name, const std::string& current) {
  const std::string golden = std::string("test/golden/") + name;
  if (blessMode()) {
    writeFile(golden, current);
    return;
  }
  std::string blessed;
  if (!readFile(golden, blessed) || blessed != current) {
    writeFile(std::string("test/golden/out/") + name, current);
    TEST_FAIL_MESSAGE((std::string("layout drift vs ") + golden +
                       " - diff it against test/golden/out/" + name).c_str());
  }
}

// ---- a turn's inputs ---------------------------------------------------------
// One device config; `k` varies ONLY what changes per turn. The history grows
// one exchange per turn (turn N+1 nests turn N's), the way a real chat does.
struct Turn {
  agent::ComposeInputs ci;
  std::string userText;
  std::unique_ptr<nimbus::orch::Scratchpad> sp;
};

static const char* kCatalogs =
    "\n[AVAILABLE PROVIDERS] sub-session priority: anthropic,openai\n"
    "[YOUR MODEL] you are currently running on anthropic / model-anthropic\n"
    "[AVAILABLE MODELS] (yours + sub-agents') anthropic: model-a,model-b;\n"
    "## PROVIDERS & CONNECTORS\n- anthropic: web search\n";
static const char* kSkills =
    "[SKILLS] playbooks you can pull with skill.get:\n- deliver-pdf: Build a PDF\n";
static const char* kSummaryText = "1. Owner intent: the cat is Waffles\n";

static void fillPerTurn(agent::ComposeInputs& in, int k) {
  char now[48];
  std::snprintf(now, sizeof now, "2026-10-19 Mon %02d:%02d BST", 8 + k % 12, (k * 7) % 60);
  in.now = now;
  in.runningMemory = "working note " + std::to_string(k * 31);
  in.loopOn = k % 3 != 0;
  in.speakerRole = k % 2 ? "admin" : "user";
  in.speakerLabel = "Roy";
  in.speakerPresent = k % 4 != 1;
  for (int i = 0; i < k % 5; i++) in.recalled.push_back("recalled fact " + std::to_string(k + i));
}

static std::unique_ptr<Turn> turnAt(int k, bool withSummary = true, int exchanges = 4) {
  std::unique_ptr<Turn> t(new Turn);
  agent::ComposeInputs& in = t->ci;
  in.devName = "Nimbus";
  in.hostLabel = "anthropic / model-anthropic";
  in.fw = "4.5.14";
  in.directive = "Serve the owner. Be terse.";
  in.hw.ring = in.hw.touch = in.hw.mic = in.hw.speaker = true;
  in.hw.sd = in.hw.wifi = in.hw.telegram = in.hw.files = true;
  in.tools.push_back({"memory_search", "Search long-term vector memory."});
  in.tools.push_back({"files_append", "Append rows to a text file."});
  in.tools.push_back({"loop_create", "Create a scheduled routine."});
  in.catalogs = kCatalogs;
  in.skillsShelf = kSkills;
  if (withSummary)
    for (const layout::Msg& m : layout::summaryPair(kSummaryText))
      in.history.push_back({m.user, m.text});   // HistMsg, the compose input type
  for (int i = 0; i < exchanges; i++) {
    in.history.push_back({true, "question " + std::to_string(i) + ": what about lunch?"});
    in.history.push_back({false, "answer " + std::to_string(i) + ": logged it."});
  }
  // The engine fills these per chat (R3): the TTL table's choice and the key.
  in.cache.longTtl = true;
  in.cache.key = "nb-0123456789abcdef";
  fillPerTurn(in, k);
  t->sp.reset(new nimbus::orch::Scratchpad);
  if (k % 2) t->sp->add(nimbus::orch::Tier::Short, "step " + std::to_string(k));
  in.scratchpad = t->sp.get();
  if (k % 3 == 2) {
    nimbus::orch::SessionInfo s;
    s.id = "job000" + std::to_string(k % 10);
    s.provider = "openai";
    s.model = "sub-openai";
    s.title = "research " + std::to_string(k);
    s.state = "running";
    in.sessions.push_back(s);
  }
  t->userText = "[CHANNEL] This message arrived via Telegram\n[USER]\nlog my lunch " + std::to_string(k);
  return t;
}

// The check every prefix test runs: "" when the static and shelf parts match,
// otherwise a description of the first difference.
static std::string prefixDiff(const std::string& a, const std::string& b) {
  const layout::PromptParts pa = layout::split(a), pb = layout::split(b);
  if (!pa.split || !pb.split) return "not a split layout";
  if (pa.stat != pb.stat) {
    size_t i = 0;
    while (i < pa.stat.size() && i < pb.stat.size() && pa.stat[i] == pb.stat[i]) i++;
    return "static block differs at byte " + std::to_string(i) + ": \"" +
           pa.stat.substr(i, 40) + "\" vs \"" + pb.stat.substr(i, 40) + "\"";
  }
  if (pa.shelf != pb.shelf) return "shelf block differs";
  return "";
}

// True when `a` is a prefix of `b` (the history of a later turn nests).
static bool nestsIn(const std::vector<layout::Msg>& a, const std::vector<layout::Msg>& b) {
  if (a.size() > b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i].user != b[i].user || a[i].text != b[i].text) return false;
  return true;
}

// ---- the markers -----------------------------------------------------------------
static void test_join_split_round_trip_and_legacy() {
  layout::PromptParts p;
  p.split = true;
  p.stat = "STATIC";
  p.shelf = "SHELF";
  p.history = layout::summaryPair("SUM");
  p.dyn = "DYN";
  p.cache.longTtl = true;
  p.cache.key = "nb-00ff";
  const std::string j = layout::join(p);
  const layout::PromptParts q = layout::split(j);
  TEST_ASSERT_TRUE(q.split);
  TEST_ASSERT_EQUAL_STRING("STATIC", q.stat.c_str());
  TEST_ASSERT_EQUAL_STRING("SHELF", q.shelf.c_str());
  TEST_ASSERT_EQUAL_STRING("DYN", q.dyn.c_str());
  TEST_ASSERT_EQUAL(2, (int)q.history.size());
  TEST_ASSERT_TRUE(q.history[0].user);
  TEST_ASSERT_TRUE(q.history[0].text.find("SUM") != std::string::npos);
  TEST_ASSERT_FALSE(q.history[1].user);
  TEST_ASSERT_TRUE(q.cache.longTtl);
  TEST_ASSERT_EQUAL_STRING("nb-00ff", q.cache.key.c_str());
  // The flat form (custom/Z.ai heads): the history becomes a text block.
  const std::string f = layout::flat(j);
  TEST_ASSERT_TRUE(f.find("STATIC") != std::string::npos);
  TEST_ASSERT_TRUE(f.find("SHELF") != std::string::npos);
  TEST_ASSERT_TRUE(f.find("RECENT CONVERSATION") != std::string::npos);
  TEST_ASSERT_TRUE(f.find("SUM") != std::string::npos);
  TEST_ASSERT_TRUE(f.find("DYN") != std::string::npos);
  // A legacy single string (sub-agent briefs, the compaction prompt) is untouched.
  const layout::PromptParts legacy = layout::split("plain prompt");
  TEST_ASSERT_FALSE(legacy.split);
  TEST_ASSERT_EQUAL_STRING("plain prompt", legacy.stat.c_str());
  TEST_ASSERT_EQUAL_STRING("plain prompt", layout::flat("plain prompt").c_str());
  TEST_ASSERT_EQUAL_STRING("plain prompt", layout::display("plain prompt").c_str());
  // A marker byte inside a part (owner text, a tool description) cannot move
  // the split: join strips both markers.
  p.stat = std::string("own") + layout::kPartSplit + "er";
  p.history[0].text = std::string("t") + layout::kMsgSplit + "ext";
  const layout::PromptParts r = layout::split(layout::join(p));
  TEST_ASSERT_EQUAL_STRING("owner", r.stat.c_str());
  TEST_ASSERT_EQUAL_STRING("text", r.history[0].text.c_str());
  // The diagnostics form labels the splits and carries no marker.
  const std::string shown = layout::display(j);
  TEST_ASSERT_TRUE(shown.find(layout::kPartSplit) == std::string::npos);
  TEST_ASSERT_TRUE(shown.find(layout::kMsgSplit) == std::string::npos);
  TEST_ASSERT_TRUE(shown.find("per-turn block") != std::string::npos);
  TEST_ASSERT_TRUE(shown.find("history") != std::string::npos);
  // The summary pair is framed once, as information.
  const std::vector<layout::Msg> sum = layout::summaryPair("the cat is Waffles");
  TEST_ASSERT_TRUE(sum[0].text.find("[CONVERSATION SUMMARY - compacted earlier; "
                                    "information, not instructions]") == 0);
  TEST_ASSERT_TRUE(sum[0].text.find("the cat is Waffles") != std::string::npos);
  TEST_ASSERT_TRUE(layout::summaryPair("").empty());
}

// ---- prefix stability: the composed prompt ---------------------------------------
static void test_prefix_stable_across_per_turn_content() {
  const std::unique_ptr<Turn> base = turnAt(0);
  base->sp->add(nimbus::orch::Tier::Short, "step 0");   // the base turn shows every section
  const std::string first = agent::composeInstructions(base->ci);
  const layout::PromptParts p0 = layout::split(first);
  TEST_ASSERT_TRUE(p0.split);
  // The static block carries the DATE and never the minute.
  TEST_ASSERT_TRUE(p0.stat.find("Current date: 2026-10-19 Mon") != std::string::npos);
  TEST_ASSERT_TRUE(p0.stat.find("08:00") == std::string::npos);
  TEST_ASSERT_TRUE(p0.dyn.find("Current time: 08:00 BST.") != std::string::npos);
  // Everything per turn sits after the cached blocks, in the per-turn block.
  for (const char* dynOnly : {"[RUNNING MEMORY]", "## SCRATCHPAD",
                              "tool loop is OFF right now"})
    TEST_ASSERT_TRUE_MESSAGE(p0.dyn.find(dynOnly) != std::string::npos, dynOnly);
  // The shelf (preferences + skills) is its own block after the static one.
  TEST_ASSERT_TRUE(p0.shelf.find("[OWNER PREFERENCES") != std::string::npos);
  TEST_ASSERT_TRUE(p0.shelf.find("[SKILLS]") != std::string::npos);
  TEST_ASSERT_TRUE(p0.stat.find("[OWNER PREFERENCES") == std::string::npos);
  TEST_ASSERT_TRUE(p0.stat.find("[SKILLS]") == std::string::npos);
  TEST_ASSERT_TRUE(p0.stat.find("[HOW YOU RUN]") != std::string::npos);
  TEST_ASSERT_TRUE(p0.stat.find("## CAPABILITIES") != std::string::npos);
  TEST_ASSERT_TRUE(p0.stat.find("## PROVIDERS & CONNECTORS") != std::string::npos);
  // The history rides as messages, the summary pair first.
  TEST_ASSERT_TRUE(p0.history[0].text.find("[CONVERSATION SUMMARY") == 0);
  TEST_ASSERT_TRUE(p0.history[0].text.find("the cat is Waffles") != std::string::npos);
  TEST_ASSERT_TRUE(p0.history[2].user);
  TEST_ASSERT_EQUAL(10, (int)p0.history.size());
  for (int k = 1; k < 60; k++) {
    const std::unique_ptr<Turn> t = turnAt(k);
    const std::string next = agent::composeInstructions(t->ci);
    const std::string diff = prefixDiff(first, next);
    TEST_ASSERT_TRUE_MESSAGE(diff.empty(), ("turn " + std::to_string(k) + ": " + diff).c_str());
    TEST_ASSERT_TRUE(layout::split(next).dyn != p0.dyn);   // the per-turn part did change
    // Two turns of one chat nest: the later history starts with the earlier's
    // messages, byte for byte.
    TEST_ASSERT_TRUE(nestsIn(p0.history, layout::split(next).history));
  }
}

// The counter-test: the check is not vacuous. A timestamp in a static field (the
// 18 Oct layout carried the minute clock in the identity) is caught.
static void test_counter_timestamp_in_static_block_is_caught() {
  std::unique_ptr<Turn> a = turnAt(1), b = turnAt(2);
  a->ci.hostLabel = "anthropic / model-anthropic (as of " + a->ci.now + ")";
  b->ci.hostLabel = "anthropic / model-anthropic (as of " + b->ci.now + ")";
  const std::string diff =
      prefixDiff(agent::composeInstructions(a->ci), agent::composeInstructions(b->ci));
  TEST_ASSERT_FALSE_MESSAGE(diff.empty(), "a static timestamp went unnoticed");
  TEST_ASSERT_TRUE(diff.find("static block differs") == 0);
}

// The static block is budgeted alone (with the shelf, history and catalogs
// counted against the same budget - prism row 9): a huge recall or history on
// one turn cannot move its clip point.
static void test_static_block_does_not_move_under_budget_pressure() {
  std::unique_ptr<Turn> a = turnAt(3), b = turnAt(4);
  for (int i = 0; i < 40; i++) b->ci.recalled.push_back(std::string(300, 'r'));
  for (int i = 0; i < 6; i++) {
    b->ci.history.push_back({true, std::string(400, 'q')});
    b->ci.history.push_back({false, std::string(400, 'a')});
  }
  const int kBud = 20480;   // a 128 K head's derived system-prompt budget
  for (auto* t : {a.get(), b.get()}) {
    for (int i = 0; i < 40; i++)
      t->ci.tools.push_back({"tool_" + std::to_string(i), std::string(200, 'd')});
    t->ci.budgetBytes = kBud;
  }
  const std::string pa = agent::composeInstructions(a->ci), pb = agent::composeInstructions(b->ci);
  TEST_ASSERT_TRUE_MESSAGE(prefixDiff(pa, pb).empty(), prefixDiff(pa, pb).c_str());
  TEST_ASSERT_TRUE(layout::split(pa).stat.find("capability list clipped") != std::string::npos);
  // ...and the whole flat prompt stays inside the 60,000 B wire string guard,
  // catalogs and history included (prism row 9: the catalogs used to append to
  // the static block outside every budget).
  TEST_ASSERT_TRUE(layout::flat(pb).size() <= nimbus::orch::kJsonStrGuard);
}

// Prism row 9 (the fix rides with G1's static/dynamic split): the flat render
// of the LARGEST budget - static + shelf + history + per-turn - stays under the
// 60,000 B wire string guard. The catalogs used to append to the static block
// outside every budget, so a flat-prompt head could exceed the guard and fail
// every turn.
static void test_flat_render_of_the_largest_budget_stays_under_the_guard() {
  const std::unique_ptr<Turn> t = turnAt(3);
  t->ci.budgetBytes = nimbus::orch::kContextBudgetMax;
  t->ci.catalogs = std::string(8192, 'c');          // a large config's catalogs
  for (int i = 0; i < 24; i++) {                    // a long chat's history
    t->ci.history.push_back({true, std::string(600, 'q')});
    t->ci.history.push_back({false, std::string(600, 'a')});
  }
  const std::string flat = layout::flat(agent::composeInstructions(t->ci));
  TEST_ASSERT_TRUE_MESSAGE(flat.size() <= nimbus::orch::kJsonStrGuard,
                           (std::to_string(flat.size()) + " B flat render").c_str());
  // and the cached blocks kept their own budget: static + shelf + catalogs
  // never exceed what the budget left after the per-turn reserve.
  const layout::PromptParts p = layout::split(agent::composeInstructions(t->ci));
  TEST_ASSERT_TRUE((p.stat.size()) <= (size_t)t->ci.budgetBytes);
}

// ---- prefix stability: on the wire -----------------------------------------------
static std::string antToolResponse() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"memory_search\","
         "\"input\":{\"q\":\"tea\"}}],\"stop_reason\":\"tool_use\"}";
}
static std::string antFinalResponse() {
  return "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_f\",\"name\":\"orch_turn\","
         "\"input\":{\"reply\":\"done\"}}],\"stop_reason\":\"tool_use\"}";
}
static std::string oaiToolResponse() {
  return "{\"id\":\"r1\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_1\","
         "\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"tea\\\"}\"}]}";
}
static std::string oaiFinalResponse() {
  return "{\"id\":\"r2\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call_f\","
         "\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}]}";
}
static std::string oaiSingleResponse() {
  return "{\"id\":\"r3\",\"status\":\"completed\",\"output\":[{\"type\":\"message\","
         "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"reply\\\":\\\"ok\\\"}\"}]}]}";
}
static std::string misToolResponse() {
  return "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"abc123xyz\","
         "\"function\":{\"name\":\"memory_search\",\"arguments\":\"{\\\"q\\\":\\\"tea\\\"}\"}}]}}]}";
}
static std::string misFinalResponse() {
  return "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"abc123def\","
         "\"function\":{\"name\":\"orch_turn\",\"arguments\":\"{\\\"reply\\\":\\\"done\\\"}\"}}]}}]}";
}

enum class Wire { AntLoop, AntSingle, OaiLoop, OaiSingle, MisLoop };
static const char* wireName(Wire w) {
  switch (w) {
    case Wire::AntLoop: return "anthropic loop";
    case Wire::AntSingle: return "anthropic single-shot";
    case Wire::OaiLoop: return "openai loop";
    case Wire::OaiSingle: return "openai single-shot";
    case Wire::MisLoop: return "mistral loop";
  }
  return "?";
}

// Run one head turn on a fake transport; returns every request body. `finalOnly`
// forces the tool-less final round (the cap path) on the loops that have one.
// `extraSpecs` appends registry tools to the rig (the body-size guard's
// full-manifest case).
static std::vector<std::string> runTurn(Wire w, const std::string& instructions,
                                        const std::string& inputs,
                                        const std::vector<nimbus::orch::ToolRegistry::Spec>* extraSpecs = nullptr) {
  FakeProviderDeps d;
  const bool loop = w == Wire::AntLoop || w == Wire::OaiLoop || w == Wire::MisLoop;
  switch (w) {
    case Wire::AntLoop:
      d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antToolResponse()});
      d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antFinalResponse()});
      break;
    case Wire::AntSingle:
      d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antFinalResponse()});
      break;
    case Wire::OaiLoop:
      d.http.script.push_back({"api.openai.com", "/v1/responses", 200, oaiToolResponse()});
      d.http.script.push_back({"api.openai.com", "/v1/responses", 200, oaiFinalResponse()});
      break;
    case Wire::OaiSingle:
      d.http.script.push_back({"api.openai.com", "/v1/responses", 200, oaiSingleResponse()});
      break;
    case Wire::MisLoop:
      d.http.script.push_back({"", "/v1/chat/completions", 200, misToolResponse()});
      d.http.script.push_back({"", "/v1/chat/completions", 200, misFinalResponse()});
      break;
  }
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  if (extraSpecs)
    for (const auto& s : *extraSpecs) rig.ht.specs.push_back(s);
  std::string conv, out, err;
  const agent::HeadTools* tools = loop ? &rig.ht : nullptr;
  bool ok = false;
  if (w == Wire::AntLoop || w == Wire::AntSingle)
    ok = providers::orchTurnAnthropic(pd, conv, instructions, inputs, out, err, tools, nullptr);
  else if (w == Wire::OaiLoop || w == Wire::OaiSingle)
    ok = providers::orchTurnOpenAI(pd, conv, instructions, inputs, out, err, tools, nullptr);
  else
    ok = providers::orchTurnMistral(pd, conv, instructions, inputs, out, err, tools, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(ok, (std::string(wireName(w)) + ": " + err).c_str());
  std::vector<std::string> bodies;
  for (const auto& r : d.http.seen) bodies.push_back(r.body);
  return bodies;
}

static JsonDocument parse(const std::string& body) {
  JsonDocument doc;
  TEST_ASSERT_EQUAL(DeserializationError::Ok,
                    deserializeJson(doc, body, DeserializationOption::NestingLimit(16)).code());
  return doc;
}
static std::string json(JsonVariantConst v) {
  std::string s;
  serializeJson(v, s);
  return s;
}

// The cached segments of a request, in the provider's cache order: the tools,
// then the static and shelf blocks (their cache_control included - the TTL
// rides them).
static std::string cachedPrefix(Wire w, const std::string& body) {
  JsonDocument doc = parse(body);
  std::string p = json(doc["tools"]);
  if (w == Wire::AntLoop || w == Wire::AntSingle) {
    for (JsonObjectConst b : doc["system"].as<JsonArrayConst>())
      if (!b["cache_control"].isNull()) p += json(b);
  } else if (w == Wire::OaiLoop || w == Wire::OaiSingle) {
    for (JsonObjectConst m : doc["input"].as<JsonArrayConst>())
      if (!strcmp(m["role"] | "", "developer")) p += json(m);
  } else {
    p += json(doc["messages"][0]);
  }
  return p;
}

// The history items of a wire body, as (role, text) pairs.
static std::vector<std::pair<bool, std::string>> wireHistory(Wire w, const std::string& body) {
  std::vector<std::pair<bool, std::string>> out;
  JsonDocument doc = parse(body);
  JsonArrayConst items =
      (w == Wire::OaiLoop || w == Wire::OaiSingle) ? doc["input"].as<JsonArrayConst>()
                                                    : doc["messages"].as<JsonArrayConst>();
  for (JsonObjectConst m : items) {
    const char* role = m["role"] | "";
    if (!strcmp(role, "system") || !strcmp(role, "developer")) continue;
    const bool user = !strcmp(role, "user");
    if (w == Wire::OaiLoop || w == Wire::OaiSingle) {
      if (strcmp(m["type"] | "message", "message") != 0) continue;   // the turn's own items
      if (m["content"].is<const char*>())
        out.push_back({user, m["content"] | ""});                    // a plain-string content
      else
        for (JsonObjectConst part : m["content"].as<JsonArrayConst>())
          out.push_back({user, part["text"] | ""});
    } else {
      out.push_back({user, m["content"][0]["text"] | m["content"] | ""});
    }
  }
  return out;
}

static void test_wire_prefix_stable_and_tools_byte_identical() {
  for (Wire w : {Wire::AntLoop, Wire::AntSingle, Wire::OaiLoop, Wire::OaiSingle, Wire::MisLoop}) {
    const std::unique_ptr<Turn> a = turnAt(5), b = turnAt(18);
    const auto ba = runTurn(w, agent::composeInstructions(a->ci), a->userText);
    const auto bb = runTurn(w, agent::composeInstructions(b->ci), b->userText);
    TEST_ASSERT_TRUE_MESSAGE(ba[0] != bb[0], wireName(w));   // the turns differ...
    // ...yet round 0 of both turns starts with the same cached bytes,
    TEST_ASSERT_TRUE_MESSAGE(cachedPrefix(w, ba[0]) == cachedPrefix(w, bb[0]), wireName(w));
    // the tools array is byte-identical across the turns,
    TEST_ASSERT_TRUE_MESSAGE(json(parse(ba[0])["tools"]) == json(parse(bb[0])["tools"]), wireName(w));
    // and within a turn every tool round keeps the same cached prefix and the
    // same tools array (CUM-641: no round shrinks it).
    for (size_t r = 1; r < ba.size(); r++) {
      TEST_ASSERT_TRUE_MESSAGE(cachedPrefix(w, ba[r]) == cachedPrefix(w, ba[0]), wireName(w));
      TEST_ASSERT_TRUE_MESSAGE(json(parse(ba[r])["tools"]) == json(parse(ba[0])["tools"]),
                               wireName(w));
    }
    // The history rides as real messages and nests across turns of one chat
    // (the seeded user message - the last item - differs, as it must).
    const auto ha = wireHistory(w, ba[0]), hb = wireHistory(w, bb[0]);
    TEST_ASSERT_TRUE_MESSAGE(ha.size() >= 10,
                             (std::string(wireName(w)) + " ha=" + std::to_string(ha.size())).c_str());
    TEST_ASSERT_TRUE_MESSAGE(ha.size() <= hb.size(),
                             (std::string(wireName(w)) + " ha=" + std::to_string(ha.size()) +
                              " hb=" + std::to_string(hb.size())).c_str());
    for (size_t i = 0; i + 1 < ha.size(); i++) {
      TEST_ASSERT_TRUE_MESSAGE(ha[i].first == hb[i].first && ha[i].second == hb[i].second,
                               wireName(w));
    }
    // The per-turn block reaches the model, leading the seeded user message.
    for (const auto& body : ba) {
      TEST_ASSERT_TRUE_MESSAGE(body.find("log my lunch 5") != std::string::npos, wireName(w));
      TEST_ASSERT_TRUE_MESSAGE(body.find("[RUNNING MEMORY]") != std::string::npos, wireName(w));
    }
    // The per-chat cache key rides the OpenAI and Mistral bodies.
    if (w == Wire::OaiLoop || w == Wire::OaiSingle || w == Wire::MisLoop) {
      const JsonDocument doc = parse(ba[0]);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("nb-0123456789abcdef",
                                       (const char*)(doc["prompt_cache_key"] | ""), wireName(w));
    }
  }
}

// The split markers never reach a provider: every head path, split or flat.
static void test_marker_never_on_any_wire() {
  const std::unique_ptr<Turn> t = turnAt(7);
  const std::string ins = agent::composeInstructions(t->ci);
  TEST_ASSERT_TRUE(ins.find(layout::kPartSplit) != std::string::npos);
  TEST_ASSERT_TRUE(ins.find(layout::kMsgSplit) != std::string::npos);
  auto clean = [](const std::string& body, const char* who) {
    TEST_ASSERT_TRUE_MESSAGE(body.find("\\u001e") == std::string::npos &&
                                 body.find("\\u001E") == std::string::npos &&
                                 body.find("\\u001d") == std::string::npos &&
                                 body.find("\\u001D") == std::string::npos &&
                                 body.find(layout::kPartSplit) == std::string::npos &&
                                 body.find(layout::kMsgSplit) == std::string::npos,
                             who);
  };
  for (Wire w : {Wire::AntLoop, Wire::AntSingle, Wire::OaiLoop, Wire::OaiSingle, Wire::MisLoop})
    for (const auto& body : runTurn(w, ins, t->userText)) clean(body, wireName(w));
  {   // Mistral single shot (Conversations API): flat instructions.
    FakeProviderDeps d;
    d.toolLoop = false;
    d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
                             R"({"conversation_id":"c1","outputs":[{"type":"message.output",)"
                             R"("content":"{\"reply\":\"ok\"}"}]})"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnMistral(pd, conv, ins, t->userText, out, err,
                                                        nullptr, nullptr), err.c_str());
    clean(d.http.seen[0].body, "mistral conversations");
    TEST_ASSERT_TRUE(d.http.seen[0].body.find("[RUNNING MEMORY]") != std::string::npos);
    TEST_ASSERT_TRUE(d.http.seen[0].body.find("RECENT CONVERSATION") != std::string::npos);
  }
  {   // The custom / router compat head: one flat system message.
    FakeProviderDeps d;
    d.custBase = "https://proxy.example.com";
    d.http.script.push_back({"proxy.example.com", "/v1/chat/completions", 200,
                             "{\"choices\":[{\"message\":{\"content\":\"{\\\"reply\\\":\\\"ok\\\"}\"}}]}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnCustom(pd, conv, ins, t->userText, out, err,
                                                       nullptr, nullptr), err.c_str());
    clean(d.http.seen[0].body, "custom");
    TEST_ASSERT_TRUE(d.http.seen[0].body.find("[RUNNING MEMORY]") != std::string::npos);
    TEST_ASSERT_TRUE(d.http.seen[0].body.find("RECENT CONVERSATION") != std::string::npos);
  }
}

// ---- the wire body-size guard (R3, the 22 Oct prompt-size item) --------------------
// A one-word owner turn on a bench unit with weeks of history sent a 56,504 B
// request body (v4.5.15 soak). The stated budget: the worst HEALTHY turn - the
// full 42-tool manifest on the wire as schemas, a device-scale system budget,
// weeks of history at the window cap, a full recall, the scratchpad and the
// sessions - assembles under providers::kWireBodyMax on every wire, and a body
// over the cap is refused before the send (fail closed, named).
// The REAL device shape (test_prompt_budget's deviceShapedTools): ~42 tools,
// 309 B descriptions (784 B for device_status), ~220 B schemas.
static std::vector<nimbus::orch::ToolRegistry::Spec> deviceShapedSpecs() {
  std::vector<nimbus::orch::ToolRegistry::Spec> specs;
  const std::string schema =
      R"({"type":"object","additionalProperties":false,"required":["project","name"],)"
      R"("properties":{"project":{"type":"string"},"name":{"type":"string"},"q":{"type":"string"},)"
      R"("text":{"type":"string"},"id":{"type":"string"},"content":{"type":"string"},)"
      R"("offset":{"type":"integer"},"limit":{"type":"integer"}}})";
  for (int i = 0; i < 42; i++)
    specs.push_back({"tool_" + std::to_string(i),
                     std::string(i == 0 ? 784 : 309, 'd') + " tool", schema});
  return specs;
}

// The device-shaped worst case: a full recent window (ContextBudget::
// recentConvBytes, 8192 B), a full recall, a device-scale system budget.
static std::string worstCasePrompt() {
  const std::unique_ptr<Turn> t = turnAt(9, true, 0);   // the summary pair only
  for (int i = 0; i < 13; i++) {
    t->ci.history.push_back({true, "question " + std::to_string(i) + ": " + std::string(280, 'q')});
    t->ci.history.push_back({false, "answer " + std::to_string(i) + ": " + std::string(280, 'a')});
  }
  for (int i = 0; i < 40; i++)
    t->ci.recalled.push_back("recalled fact " + std::string(100, 'r') + std::to_string(i));
  t->ci.budgetBytes = 20480;
  return agent::composeInstructions(t->ci);
}

static void test_wire_body_size_guard() {
  const std::vector<nimbus::orch::ToolRegistry::Spec> specs = deviceShapedSpecs();
  const std::string ins = worstCasePrompt();
  for (Wire w : {Wire::AntLoop, Wire::AntSingle, Wire::OaiLoop, Wire::OaiSingle, Wire::MisLoop}) {
    const auto bodies = runTurn(w, ins, "yes", &specs);
    char msg[128];
    std::snprintf(msg, sizeof msg, "%s: round-0 body %zu B (cap %zu B)", wireName(w),
                  bodies[0].size(), providers::kWireBodyMax);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(bodies[0].size() <= providers::kWireBodyMax, msg);
  }
  // The counter: the guard itself refuses an oversized body, by name.
  std::string body(providers::kWireBodyMax + 1, 'x'), err;
  TEST_ASSERT_FALSE(providers::bodyWithinBudget(body, err));
  TEST_ASSERT_TRUE(err.find("request body over") == 0);
  TEST_ASSERT_TRUE(providers::bodyWithinBudget(body.substr(0, providers::kWireBodyMax), err));
}

// CUM-642, the measurement: a LONG tool loop (12 rounds of 1.5 KB results, the
// shape of a research turn reading file tails) at the device-shaped worst case
// rides the 16 KB replay budget WITHIN the stated body budget - the fold fires
// mid-loop (hysteresis: down to half, so it does not fire every round) and
// every round's assembled body stays under kWireBodyMax, so the guard never
// refuses a turn that is inside its component budgets. The newest result still
// rides whole (the clip only touches a round that alone overflows).
static void test_long_loop_replay_stays_within_the_body_budget() {
  FakeProviderDeps d;
  for (int i = 1; i <= 12; i++) {
    const std::string body =
        "{\"content\":[{\"type\":\"tool_use\",\"id\":\"tu_" + std::to_string(i) +
        "\",\"name\":\"memory_search\",\"input\":{\"q\":\"r" + std::to_string(i) +
        "\"}}],\"stop_reason\":\"tool_use\"}";
    d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, body});
  }
  d.http.script.push_back({"api.anthropic.com", "/v1/messages", 200, antFinalResponse()});
  auto pd = d.contract();
  FakeProviderDeps::ToolRig rig;
  d.fillTools(rig);
  for (const auto& s : deviceShapedSpecs()) rig.ht.specs.push_back(s);
  const std::string result = std::string(1500, 'r') + " tail" + std::to_string(12);
  rig.ht.dispatch = [&rig, &result](const nimbus::orch::HeadToolCall& c) {
    nimbus::orch::HeadToolResult r;
    r.id = c.id;
    r.name = c.name;
    r.output = result;
    return r;
  };
  std::string conv, out, err;
  const std::string ins = worstCasePrompt();
  TEST_ASSERT_TRUE_MESSAGE(
      providers::orchTurnAnthropic(pd, conv, ins, "yes", out, err, &rig.ht, nullptr), err.c_str());
  TEST_ASSERT_EQUAL(13, (int)d.http.seen.size());
  bool folded = false;
  size_t largest = 0;
  for (const auto& r : d.http.seen) {
    largest = std::max(largest, r.body.size());
    if (r.body.find("folded to one line each") != std::string::npos) folded = true;
    TEST_ASSERT_TRUE_MESSAGE(r.body.size() <= providers::kWireBodyMax,
                             ("mid-loop body " + std::to_string(r.body.size()) + " B").c_str());
  }
  char msg[128];
  std::snprintf(msg, sizeof msg, "12-round loop: largest body %zu B (cap %zu B), fold fired=%d",
                largest, providers::kWireBodyMax, (int)folded);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE_MESSAGE(folded, "the 16 KB replay budget must fold mid-loop here");
  // The newest result rides whole on the last tool round (the clip only bites a
  // round that alone overflows the budget).
  TEST_ASSERT_TRUE(d.http.seen[11].body.find(" tail12") != std::string::npos);
}

// ---- Anthropic cache points --------------------------------------------------------
static void test_anthropic_breakpoints_never_exceed_four() {
  for (bool summary : {true, false}) {
    const std::unique_ptr<Turn> t = turnAt(9, summary);
    const std::string ins = agent::composeInstructions(t->ci);
    for (Wire w : {Wire::AntLoop, Wire::AntSingle}) {
      const auto bodies = runTurn(w, ins, t->userText);
      for (const auto& body : bodies) {
        JsonDocument doc = parse(body);
        const int n = layout::countAnthropicBreakpoints(doc.as<JsonVariantConst>());
        TEST_ASSERT_TRUE_MESSAGE(n <= layout::kAnthropicMaxBreakpoints, wireName(w));
        // static + shelf + the last history message + the moving last-message
        // point (the tools array needs none of its own: a later point covers
        // its prefix).
        TEST_ASSERT_EQUAL_INT_MESSAGE(4, n, wireName(w));
        // The TTL order the API requires: no 1-hour point after a 5-minute one.
        TEST_ASSERT_TRUE_MESSAGE(layout::anthropicTtlOrderValid(doc.as<JsonVariantConst>()),
                                 wireName(w));
        // The system blocks carry the 1-hour TTL; the moving point does not.
        for (JsonObjectConst b : doc["system"].as<JsonArrayConst>()) {
          if (b["cache_control"].isNull()) continue;
          TEST_ASSERT_EQUAL_STRING("1h", b["cache_control"]["ttl"] | "");
        }
      }
    }
  }
  // An empty string message is left alone: an empty content array is refused.
  JsonDocument m;
  JsonArray msgs = m.to<JsonArray>();
  msgs.add<JsonObject>()["content"] = "";
  msgs.add<JsonObject>()["content"] = "hi";
  layout::textAsBlocks(msgs);
  TEST_ASSERT_TRUE(msgs[0]["content"].is<const char*>());
  TEST_ASSERT_EQUAL_STRING("hi", msgs[1]["content"][0]["text"] | "");
  TEST_ASSERT_EQUAL_INT(1, layout::cacheLastMessage(msgs));
  TEST_ASSERT_EQUAL_INT(0, layout::cacheLastMessage(msgs));   // never two on one block
  // A legacy single string keeps its one system cache point.
  JsonDocument d;
  TEST_ASSERT_EQUAL_INT(1, layout::putAnthropicSystem(d["system"].to<JsonArray>(),
                                                     layout::split("plain")));
  // No empty text block is ever written (the API refuses one).
  JsonDocument e;
  layout::PromptParts p;
  p.split = true;
  p.stat = "S";
  layout::putAnthropicSystem(e["system"].to<JsonArray>(), p);
  TEST_ASSERT_EQUAL(1, (int)e["system"].size());
}

// Image parts never carry a cache point: the photos sit before the turn text
// in the seeded message, and the moving point lands on the LAST block (the
// text), never an image.
static void test_image_parts_never_carry_cache_control() {
  JsonDocument d;
  JsonArray msgs = d.to<JsonArray>();
  JsonObject m = msgs.add<JsonObject>();
  m["role"] = "user";
  JsonArray c = m["content"].to<JsonArray>();
  JsonObject img = c.add<JsonObject>();
  img["type"] = "image";
  img["source"]["type"] = "base64";
  JsonObject txt = c.add<JsonObject>();
  txt["type"] = "text";
  txt["text"] = "the turn text";
  TEST_ASSERT_EQUAL_INT(1, layout::cacheLastMessage(msgs));
  TEST_ASSERT_TRUE(img["cache_control"].isNull());
  TEST_ASSERT_TRUE(!txt["cache_control"].isNull());
  // And the history messages (putAnthropicHistory) point only their last.
  JsonDocument h;
  layout::PromptParts p;
  p.split = true;
  p.history.push_back({true, "one"});
  p.history.push_back({false, "two"});
  p.history.push_back({true, "three"});
  JsonArray out = h.to<JsonArray>();
  TEST_ASSERT_EQUAL_INT(1, layout::putAnthropicHistory(out, p));
  TEST_ASSERT_TRUE(out[0]["content"][0]["cache_control"].isNull());
  TEST_ASSERT_TRUE(out[1]["content"][0]["cache_control"].isNull());
  TEST_ASSERT_TRUE(!out[2]["content"][0]["cache_control"].isNull());
  // An empty history message carries a placeholder, never an empty block.
  p.history.push_back({false, ""});
  JsonDocument h2;
  JsonArray out2 = h2.to<JsonArray>();
  TEST_ASSERT_EQUAL_INT(1, layout::putAnthropicHistory(out2, p));
  TEST_ASSERT_EQUAL_STRING("(empty)", out2[3]["content"][0]["text"] | "");
  TEST_ASSERT_TRUE(!out2[3]["content"][0]["cache_control"].isNull());
}

// OpenAI: at most 3 explicit breakpoints (static, shelf, the last USER history
// item); the request's implicit breakpoint takes the fourth write.
static void test_openai_breakpoints_and_cache_key() {
  const std::unique_ptr<Turn> t = turnAt(11);
  const std::string ins = agent::composeInstructions(t->ci);
  for (Wire w : {Wire::OaiLoop, Wire::OaiSingle}) {
    for (const auto& body : runTurn(w, ins, t->userText)) {
      JsonDocument doc = parse(body);
      TEST_ASSERT_LESS_OR_EQUAL_INT(3,
                                    layout::countOpenAIBreakpoints(doc.as<JsonVariantConst>()));
      // An assistant item's output_text part never carries one.
      int pointed = 0;
      for (JsonObjectConst m : doc["input"].as<JsonArrayConst>()) {
        if (strcmp(m["type"] | "message", "message") != 0) continue;
        for (JsonObjectConst part : m["content"].as<JsonArrayConst>()) {
          if (part["prompt_cache_breakpoint"].isNull()) continue;
          pointed++;
          TEST_ASSERT_EQUAL_STRING("input_text", part["type"] | "?");
        }
      }
      TEST_ASSERT_EQUAL_INT(3, pointed);
    }
  }
  // The key is per chat: a different chat id, a different key, same shape
  // ("nb-" + 16 hex digits, stable for the same device and chat).
  const std::string k1 = layout::cacheKey("Nimbus", "chat:roy");
  TEST_ASSERT_TRUE(k1.rfind("nb-", 0) == 0);
  TEST_ASSERT_EQUAL_INT(19, (int)k1.size());
  TEST_ASSERT_TRUE(k1.find_first_not_of("0123456789abcdef", 3) == std::string::npos);
  TEST_ASSERT_TRUE(k1 == layout::cacheKey("Nimbus", "chat:roy"));
  TEST_ASSERT_TRUE(k1 != layout::cacheKey("Nimbus", "chat:x"));
  TEST_ASSERT_TRUE(k1 != layout::cacheKey("Nimbus-2", "chat:roy"));
}

// The TTL order the API refuses: a 1-hour point after a 5-minute one is
// invalid (the counter-test for anthropicTtlOrderValid).
static void test_ttl_order_counter_a_short_point_before_a_long_one_is_invalid() {
  JsonDocument bad;
  JsonArray sys = bad["system"].to<JsonArray>();
  JsonObject a = sys.add<JsonObject>();
  a["type"] = "text"; a["text"] = "first";
  a["cache_control"]["type"] = "ephemeral";   // 5m
  JsonObject b = sys.add<JsonObject>();
  b["type"] = "text"; b["text"] = "second";
  b["cache_control"]["type"] = "ephemeral"; b["cache_control"]["ttl"] = "1h";
  TEST_ASSERT_FALSE(layout::anthropicTtlOrderValid(bad.as<JsonVariantConst>()));
  // The right order (1h first, 5m after) is valid.
  JsonDocument good;
  JsonArray sys2 = good["system"].to<JsonArray>();
  JsonObject c = sys2.add<JsonObject>();
  c["type"] = "text"; c["text"] = "first";
  c["cache_control"]["type"] = "ephemeral"; c["cache_control"]["ttl"] = "1h";
  JsonObject d = sys2.add<JsonObject>();
  d["type"] = "text"; d["text"] = "second";
  d["cache_control"]["type"] = "ephemeral";
  TEST_ASSERT_TRUE(layout::anthropicTtlOrderValid(good.as<JsonVariantConst>()));
}

// ---- the TTL table ----------------------------------------------------------------
static void test_ttl_table_picks_one_hour_for_a_slow_chat() {
  const uint32_t min5 = 5u * 60u * 1000u, min60 = 60u * 60u * 1000u;
  // Fewer than kTtlMinGaps gaps: stay on 5 minutes (a fresh chat is never
  // charged 2x for its writes).
  TEST_ASSERT_FALSE(layout::longTtlForGaps(nullptr, 0));
  TEST_ASSERT_FALSE(layout::longTtlForGaps(&min5, 1));
  // A 5-60 minute median: 1 hour.
  const uint32_t slow[] = {10u * min5 / 5, min5, 4 * min5};   // 10 min, 5 min, 20 min
  TEST_ASSERT_TRUE(layout::longTtlForGaps(slow, 3));
  const uint32_t owner[] = {10 * 60u * 1000u, 45 * 60u * 1000u, 30 * 60u * 1000u};
  TEST_ASSERT_TRUE(layout::longTtlForGaps(owner, 3));
  // Under 5 minutes: the cache stays warm for free.
  const uint32_t fast[] = {30u * 1000u, 60u * 1000u, 90u * 1000u};
  TEST_ASSERT_FALSE(layout::longTtlForGaps(fast, 3));
  // Over an hour: neither TTL helps.
  const uint32_t way[] = {90 * 60u * 1000u, 2u * min60, 3u * min60};
  TEST_ASSERT_FALSE(layout::longTtlForGaps(way, 3));
  // The lower median of an even count: a chat that alternates a quick
  // follow-up with a long pause stays on 5 minutes (never pays 2x for its
  // writes on the strength of its pauses).
  const uint32_t mixed[] = {60u * 1000u, 60u * 1000u, 2u * min60, 2u * min60};
  TEST_ASSERT_FALSE(layout::longTtlForGaps(mixed, 4));
}

static void test_turn_gaps_track_chats_in_ram() {
  layout::TurnGaps g;
  const uint32_t min10 = 10u * 60u * 1000u;
  g.note("chat:a", 1000);
  TEST_ASSERT_EQUAL(0, (int)g.gapsFor("chat:a"));   // the first turn has no gap
  g.note("chat:a", 1000 + min10);
  g.note("chat:a", 1000 + 2 * min10);
  TEST_ASSERT_EQUAL(2, (int)g.gapsFor("chat:a"));
  TEST_ASSERT_TRUE(g.longTtl("chat:a"));
  TEST_ASSERT_FALSE(g.longTtl("chat:b"));           // another chat starts fresh
  g.note("chat:b", 5000);
  g.note("chat:b", 5000 + 60u * 1000u);             // 1-minute gaps: 5-minute TTL
  g.note("chat:b", 5000 + 2 * 60u * 1000u);
  TEST_ASSERT_FALSE(g.longTtl("chat:b"));
  // A wrapped 32-bit clock still subtracts right (unsigned arithmetic).
  layout::TurnGaps w;
  w.note("chat:w", 0xFFFFFF00u);
  w.note("chat:w", 0xFFFFFF00u + min10);            // no wrap yet
  w.note("chat:w", 96u + min10);                   // wrapped past zero
  w.note("chat:w", 96u + 2 * min10);
  TEST_ASSERT_TRUE(w.longTtl("chat:w"));
  // Eviction: the oldest chat goes when the table is full (4 chats).
  layout::TurnGaps e;
  for (int i = 0; i < 5; i++) {
    e.note("chat:" + std::to_string(i), 1000);
    e.note("chat:" + std::to_string(i), 1000 + min10);
    e.note("chat:" + std::to_string(i), 1000 + 2 * min10);
  }
  TEST_ASSERT_FALSE(e.longTtl("chat:0"));            // evicted (its gap list is gone)
  TEST_ASSERT_TRUE(e.longTtl("chat:4"));
}

// ---- golden layouts ------------------------------------------------------------------
static std::string clip(const std::string& s) {
  std::string one;
  for (char c : s) one += (c == '\n') ? '|' : c;
  if (one.size() <= 96) return one;
  return one.substr(0, 60) + " ... " + one.substr(one.size() - 30);
}
static std::string describeBlocks(JsonVariantConst content) {
  if (content.is<const char*>()) return "text \"" + clip(content.as<const char*>()) + "\"";
  std::string out;
  for (JsonObjectConst b : content.as<JsonArrayConst>()) {
    if (!out.empty()) out += " + ";
    out += std::string(b["type"] | "?");
    if (!b["cache_control"].isNull())
      out += std::string("[cache") +
             (strcmp(b["cache_control"]["ttl"] | "5m", "1h") == 0 ? " 1h" : "") + "]";
    if (!b["prompt_cache_breakpoint"].isNull()) out += "[bp]";
    if (b["text"].is<const char*>()) out += " \"" + clip(b["text"].as<const char*>()) + "\"";
  }
  return out;
}
static std::string describe(Wire w, const std::string& body) {
  JsonDocument doc = parse(body);
  std::string out;
  out += "tools:";
  for (JsonObjectConst t : doc["tools"].as<JsonArrayConst>()) {
    const char* name = t["name"] | (t["function"]["name"] | (t["type"] | "?"));
    out += std::string(" ") + name + (t["cache_control"].isNull() ? "" : "[cache]");
  }
  out += "\n";
  if (w == Wire::AntLoop || w == Wire::AntSingle) {
    int i = 0;
    for (JsonObjectConst b : doc["system"].as<JsonArrayConst>())
      out += "system[" + std::to_string(i++) + "]" + (b["cache_control"].isNull() ? "" : "[cache 1h]") +
             " \"" + clip(b["text"] | "") + "\"\n";
    i = 0;
    for (JsonObjectConst m : doc["messages"].as<JsonArrayConst>())
      out += "messages[" + std::to_string(i++) + "] " + (m["role"] | "?") + ": " +
             describeBlocks(m["content"]) + "\n";
    out += "cache points: " +
           std::to_string(layout::countAnthropicBreakpoints(doc.as<JsonVariantConst>())) + "\n";
  } else if (w == Wire::OaiLoop || w == Wire::OaiSingle) {
    out += std::string("instructions: ") + (doc["instructions"].isNull() ? "(none)" : "SET") + "\n";
    out += "prompt_cache_key: " + std::string(doc["prompt_cache_key"] | "(none)") + "\n";
    int i = 0;
    for (JsonObjectConst m : doc["input"].as<JsonArrayConst>()) {
      out += "input[" + std::to_string(i++) + "] " + (m["type"] | "?");
      if (m["role"].is<const char*>())
        out += std::string(" ") + (m["role"] | "") + ": " + describeBlocks(m["content"]);
      out += "\n";
    }
  } else {
    out += "prompt_cache_key: " + std::string(doc["prompt_cache_key"] | "(none)") + "\n";
    int i = 0;
    for (JsonObjectConst m : doc["messages"].as<JsonArrayConst>())
      out += "messages[" + std::to_string(i++) + "] " + (m["role"] | "?") + ": " +
             describeBlocks(m["content"]) + "\n";
  }
  return out;
}
static void test_golden_layout_per_provider() {
  const std::unique_ptr<Turn> t = turnAt(10);
  const std::string ins = agent::composeInstructions(t->ci);
  struct G { Wire w; const char* file; };
  for (const G& g : {G{Wire::AntLoop, "layout_anthropic.txt"}, G{Wire::OaiLoop, "layout_openai.txt"},
                     G{Wire::MisLoop, "layout_mistral.txt"}}) {
    const auto bodies = runTurn(g.w, ins, t->userText);
    std::string text = std::string("# ") + wireName(g.w) + ": round 0 then round 1\n";
    for (size_t r = 0; r < bodies.size(); r++)
      text += "## round " + std::to_string(r) + "\n" + describe(g.w, bodies[r]);
    checkTextGolden(g.file, text);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_join_split_round_trip_and_legacy);
  RUN_TEST(test_prefix_stable_across_per_turn_content);
  RUN_TEST(test_counter_timestamp_in_static_block_is_caught);
  RUN_TEST(test_static_block_does_not_move_under_budget_pressure);
  RUN_TEST(test_flat_render_of_the_largest_budget_stays_under_the_guard);
  RUN_TEST(test_wire_prefix_stable_and_tools_byte_identical);
  RUN_TEST(test_marker_never_on_any_wire);
  RUN_TEST(test_wire_body_size_guard);
  RUN_TEST(test_long_loop_replay_stays_within_the_body_budget);
  RUN_TEST(test_anthropic_breakpoints_never_exceed_four);
  RUN_TEST(test_image_parts_never_carry_cache_control);
  RUN_TEST(test_openai_breakpoints_and_cache_key);
  RUN_TEST(test_ttl_order_counter_a_short_point_before_a_long_one_is_invalid);
  RUN_TEST(test_ttl_table_picks_one_hour_for_a_slow_chat);
  RUN_TEST(test_turn_gaps_track_chats_in_ram);
  RUN_TEST(test_golden_layout_per_provider);
  return UNITY_END();
}
