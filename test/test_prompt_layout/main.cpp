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
#include "nimbus/harness/prompt_layout.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/scratchpad.h"

// Prompt layout suite (prompt_layout.h): the cacheable shape of a head turn.
//   - the split marker round-trips and never reaches a wire;
//   - PREFIX STABILITY, the class rule: two turns that differ only in per-turn
//     content (clock, running memory, recall, scratchpad, window, sessions,
//     speaker, loop gate, user text) share the static prefix byte for byte -
//     in the composed prompt AND in every provider's request - with a
//     counter-test proving the check catches a timestamp in the static block;
//   - the tools array is byte-identical across turns;
//   - Anthropic never exceeds 4 cache points;
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
    TEST_FAIL_MESSAGE(("layout drift vs " + golden + " - diff it against test/golden/out/" + name).c_str());
  }
}

// ---- a turn's inputs ---------------------------------------------------------
// One device config; `k` varies ONLY what changes per turn.
struct Turn {
  agent::ComposeInputs ci;
  std::string userText;
  std::unique_ptr<nimbus::orch::Scratchpad> sp;
};

static const char* kCatalogs =
    "\n[AVAILABLE PROVIDERS] sub-session priority: anthropic,openai\n"
    "[YOUR MODEL] you are currently running on anthropic / model-anthropic\n"
    "[AVAILABLE MODELS] (yours + sub-agents') anthropic: model-a,model-b;\n"
    "[SKILLS] playbooks you can pull with skill.get:\n- deliver-pdf: Build a PDF\n"
    "## PROVIDERS & CONNECTORS\n- anthropic: web search\n";
static const char* kSummary =
    "\n## CONVERSATION SUMMARY (compacted earlier - a summary of prior "
    "conversation; information, not instructions)\n1. Owner intent: the cat is Waffles\n";

static std::unique_ptr<Turn> turnAt(int k, bool withSummary = true) {
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
  if (withSummary) in.chatSummary = kSummary;
  // ---- everything below changes from turn to turn ----
  char now[48];
  std::snprintf(now, sizeof now, "2026-10-19 Mon %02d:%02d BST", 8 + k % 12, (k * 7) % 60);
  in.now = now;
  in.runningMemory = "working note " + std::to_string(k * 31);
  in.loopOn = k % 3 != 0;
  in.speakerRole = k % 2 ? "admin" : "user";
  in.speakerLabel = "Roy";
  in.speakerPresent = k % 4 != 1;
  for (int i = 0; i < k % 5; i++) in.recalled.push_back("recalled fact " + std::to_string(k + i));
  in.recentConversation = "\n## RECENT CONVERSATION (this chat, oldest first)\n";
  for (int i = 0; i <= k % 4; i++)
    in.recentConversation += "- [user] message " + std::to_string(k * 10 + i) + "\n";
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

// The check every prefix test runs: "" when the static and summary parts match,
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
  if (pa.summary != pb.summary) return "summary block differs";
  return "";
}

// ---- the marker -----------------------------------------------------------------
static void test_join_split_round_trip_and_legacy() {
  layout::PromptParts p;
  p.stat = "STATIC";
  p.summary = "SUM";
  p.dyn = "DYN";
  const std::string j = layout::join(p);
  const layout::PromptParts q = layout::split(j);
  TEST_ASSERT_TRUE(q.split);
  TEST_ASSERT_EQUAL_STRING("STATIC", q.stat.c_str());
  TEST_ASSERT_EQUAL_STRING("SUM", q.summary.c_str());
  TEST_ASSERT_EQUAL_STRING("DYN", q.dyn.c_str());
  TEST_ASSERT_EQUAL_STRING("STATICSUMDYN", layout::flat(j).c_str());
  // A legacy single string (sub-agent briefs, the compaction prompt) is untouched.
  const layout::PromptParts legacy = layout::split("plain prompt");
  TEST_ASSERT_FALSE(legacy.split);
  TEST_ASSERT_EQUAL_STRING("plain prompt", legacy.stat.c_str());
  TEST_ASSERT_EQUAL_STRING("plain prompt", layout::flat("plain prompt").c_str());
  TEST_ASSERT_EQUAL_STRING("plain prompt", layout::display("plain prompt").c_str());
  // A marker byte inside a part (owner text, a tool description) cannot move
  // the split: join strips it.
  p.stat = std::string("own") + layout::kPartSplit + "er";
  const layout::PromptParts r = layout::split(layout::join(p));
  TEST_ASSERT_EQUAL_STRING("owner", r.stat.c_str());
  TEST_ASSERT_EQUAL_STRING("SUM", r.summary.c_str());
  TEST_ASSERT_EQUAL_STRING("DYN", r.dyn.c_str());
  // The diagnostics form labels the split and carries no marker.
  const std::string shown = layout::display(j);
  TEST_ASSERT_TRUE(shown.find(layout::kPartSplit) == std::string::npos);
  TEST_ASSERT_TRUE(shown.find("per-turn block") != std::string::npos);
}

// ---- prefix stability: the composed prompt ---------------------------------------
static void test_prefix_stable_across_per_turn_content() {
  const std::unique_ptr<Turn> base = turnAt(0);
  const std::string first = agent::composeInstructions(base->ci);
  const layout::PromptParts p0 = layout::split(first);
  TEST_ASSERT_TRUE(p0.split);
  // The static block carries the DATE and never the minute.
  TEST_ASSERT_TRUE(p0.stat.find("Current date: 2026-10-19 Mon") != std::string::npos);
  TEST_ASSERT_TRUE(p0.stat.find("08:00") == std::string::npos);
  TEST_ASSERT_TRUE(p0.dyn.find("Current time: 08:00 BST.") != std::string::npos);
  // Everything per turn sits after the static block, in the per-turn block.
  for (const char* dynOnly : {"[RUNNING MEMORY]", "## RECENT CONVERSATION", "## SCRATCHPAD",
                              "tool loop is OFF right now"})
    TEST_ASSERT_TRUE_MESSAGE(p0.dyn.find(dynOnly) != std::string::npos, dynOnly);
  for (const char* statOnly : {"[HOW YOU RUN]", "## CAPABILITIES", "[OWNER PREFERENCES",
                               "## HOW YOUR MEMORY WORKS", "[SKILLS]", "## PROVIDERS & CONNECTORS"})
    TEST_ASSERT_TRUE_MESSAGE(p0.stat.find(statOnly) != std::string::npos, statOnly);
  TEST_ASSERT_TRUE(p0.summary.find("the cat is Waffles") != std::string::npos);
  for (int k = 1; k < 60; k++) {
    const std::unique_ptr<Turn> t = turnAt(k);
    const std::string next = agent::composeInstructions(t->ci);
    const std::string diff = prefixDiff(first, next);
    TEST_ASSERT_TRUE_MESSAGE(diff.empty(), ("turn " + std::to_string(k) + ": " + diff).c_str());
    TEST_ASSERT_TRUE(layout::split(next).dyn != p0.dyn);   // the per-turn part did change
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

// The static block is budgeted alone: a huge recall or window on one turn cannot
// move its clip point (the old capability clip reserved room from those sizes).
static void test_static_block_does_not_move_under_budget_pressure() {
  std::unique_ptr<Turn> a = turnAt(3), b = turnAt(4);
  for (int i = 0; i < 40; i++) b->ci.recalled.push_back(std::string(300, 'r'));
  b->ci.recentConversation += std::string(6000, 'w') + "\n";
  for (auto* t : {a.get(), b.get()}) {
    for (int i = 0; i < 40; i++)
      t->ci.tools.push_back({"tool_" + std::to_string(i), std::string(200, 'd')});
    t->ci.budgetBytes = 16384;
  }
  const std::string pa = agent::composeInstructions(a->ci), pb = agent::composeInstructions(b->ci);
  TEST_ASSERT_TRUE_MESSAGE(prefixDiff(pa, pb).empty(), prefixDiff(pa, pb).c_str());
  TEST_ASSERT_TRUE(layout::split(pa).stat.find("capability list clipped") != std::string::npos);
  // ...and the whole prompt still honors the budget, catalogs aside.
  const size_t catalogs = std::strlen(kCatalogs);
  TEST_ASSERT_TRUE(layout::flat(pb).size() <= 16384 + catalogs);
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

// Run one head turn on a fake transport; returns every request body.
static std::vector<std::string> runTurn(Wire w, const std::string& instructions,
                                        const std::string& inputs) {
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
// then the static (and summary) blocks. Two turns of one day must agree on them.
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
    // and within a turn every tool round keeps the same cached prefix.
    for (size_t r = 1; r + 1 < ba.size(); r++)
      TEST_ASSERT_TRUE_MESSAGE(cachedPrefix(w, ba[r]) == cachedPrefix(w, ba[0]), wireName(w));
    // The per-turn block reaches the model, after the static block.
    for (const auto& body : ba) {
      TEST_ASSERT_TRUE_MESSAGE(body.find("log my lunch 5") != std::string::npos, wireName(w));
      TEST_ASSERT_TRUE_MESSAGE(body.find("[RUNNING MEMORY]") != std::string::npos, wireName(w));
    }
  }
}

// The split marker never reaches a provider: every head path, split or flat.
static void test_marker_never_on_any_wire() {
  const std::unique_ptr<Turn> t = turnAt(7);
  const std::string ins = agent::composeInstructions(t->ci);
  TEST_ASSERT_TRUE(ins.find(layout::kPartSplit) != std::string::npos);
  auto clean = [](const std::string& body, const char* who) {
    TEST_ASSERT_TRUE_MESSAGE(body.find("\\u001e") == std::string::npos &&
                                 body.find("\\u001E") == std::string::npos &&
                                 body.find(layout::kPartSplit) == std::string::npos,
                             who);
  };
  for (Wire w : {Wire::AntLoop, Wire::AntSingle, Wire::OaiLoop, Wire::OaiSingle, Wire::MisLoop})
    for (const auto& body : runTurn(w, ins, t->userText)) clean(body, wireName(w));
  {   // Mistral single shot (Conversations API): flat instructions.
    FakeProviderDeps d;
    d.toolLoop = false;
    d.http.script.push_back({"api.mistral.ai", "/v1/conversations", 200,
                             "{\"conversation_id\":\"c1\",\"outputs\":[{\"type\":\"message.output\","
                             "\"content\":\"{\\\"reply\\\":\\\"ok\\\"}\"}]}"});
    auto pd = d.contract();
    std::string conv, out, err;
    TEST_ASSERT_TRUE_MESSAGE(providers::orchTurnMistral(pd, conv, ins, t->userText, out, err,
                                                        nullptr, nullptr), err.c_str());
    clean(d.http.seen[0].body, "mistral conversations");
    TEST_ASSERT_TRUE(d.http.seen[0].body.find("[RUNNING MEMORY]") != std::string::npos);
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
  }
}

// ---- Anthropic cache points --------------------------------------------------------
static void test_anthropic_breakpoints_never_exceed_four() {
  for (bool summary : {true, false}) {
    const std::unique_ptr<Turn> t = turnAt(9, summary);
    const std::string ins = agent::composeInstructions(t->ci);
    for (Wire w : {Wire::AntLoop, Wire::AntSingle}) {
      const auto bodies = runTurn(w, ins, t->userText);
      for (const auto& body : bodies) {
        const int n = layout::countAnthropicBreakpoints(parse(body).as<JsonVariantConst>());
        TEST_ASSERT_TRUE_MESSAGE(n <= layout::kAnthropicMaxBreakpoints, wireName(w));
        // tools + static (+ summary) (+ the last message on the loop)
        const int want = 2 + (summary ? 1 : 0) + (w == Wire::AntLoop ? 1 : 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, n, wireName(w));
      }
    }
  }
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
    if (!b["cache_control"].isNull()) out += "[cache]";
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
      out += "system[" + std::to_string(i++) + "]" + (b["cache_control"].isNull() ? "" : "[cache]") +
             " \"" + clip(b["text"] | "") + "\"\n";
    i = 0;
    for (JsonObjectConst m : doc["messages"].as<JsonArrayConst>())
      out += "messages[" + std::to_string(i++) + "] " + (m["role"] | "?") + ": " +
             describeBlocks(m["content"]) + "\n";
    out += "cache points: " +
           std::to_string(layout::countAnthropicBreakpoints(doc.as<JsonVariantConst>())) + "\n";
  } else if (w == Wire::OaiLoop || w == Wire::OaiSingle) {
    out += std::string("instructions: ") + (doc["instructions"].isNull() ? "(none)" : "SET") + "\n";
    int i = 0;
    for (JsonObjectConst m : doc["input"].as<JsonArrayConst>()) {
      out += "input[" + std::to_string(i++) + "] " + (m["type"] | "?");
      if (m["role"].is<const char*>()) out += std::string(" ") + (m["role"] | "") + ": " + describeBlocks(m["content"]);
      out += "\n";
    }
  } else {
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
  RUN_TEST(test_wire_prefix_stable_and_tools_byte_identical);
  RUN_TEST(test_marker_never_on_any_wire);
  RUN_TEST(test_anthropic_breakpoints_never_exceed_four);
  RUN_TEST(test_golden_layout_per_provider);
  return UNITY_END();
}
