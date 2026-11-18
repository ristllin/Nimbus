// Request composition of the plain engine (plain/request.h, PRD section 4, lane RB-E2).
//
// Class rules proven here, over every role, pack and host rather than one instance:
//   - ceilings on a synthetic Lumi-shaped state with notes at their cap: core at most
//     3,072 B including the 512 B tools index, the system (core + skills index + pinned
//     notes) at most 8,192 B, at most 14 tools in 10,240 B, descriptions at most 240 B;
//   - every tool-like name in the system text and the descriptions is callable by that
//     caller (counter-test: the checker catches a name that is not);
//   - advertised == callable: a tool is in the array only when registered and reachable,
//     the registry carries the declared bytes, and the tools index never repeats an
//     array tool;
//   - byte stability: the same facts give the same bytes over 300 random states, nothing
//     but the facts is read (turn number, round, attempt, clock, taint, trigger, dialect),
//     and each fact moves only its own block (a notes edit never moves the core);
//   - the golden test/golden/plain_prompt_core.txt records the measured figures for every
//     role, pack and host plus the owner's full request text (GOLDEN_UPDATE=1 re-blesses;
//     tools/release_gate/check_plain_ceilings.py gates the figures).
#include <unity.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tool_registry.h"

using namespace nimbus::orch;
namespace plain = agent::plain;
namespace td = agent::plain::texts::tool_descriptions;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

void setUp() {}
void tearDown() {}

namespace {

// ---- the fixture: a registry shaped like engine 2's, and a Lumi-shaped state ------------

const char kObj[] = R"({"type":"object","properties":{}})";
const char kPathSchema[] = R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})";
const char kIdSchema[] = R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})";

// The group tools of PRD section 4 with the shapes RB-M0a measured (fetch_url takes
// path, Ruling 11). Fixture text: the real ones are their lanes' registrations.
struct GroupTool {
  const char* name;
  const char* desc;
  const char* schema;
  bool adminOnly;
};
const GroupTool kGroupTools[] = {
    {"device_status", "Battery, Wi-Fi, storage and uptime.", kObj, true},
    {"device_control", "Restart, switch head, set a device setting.", kObj, true},
    {"ota_status", "Firmware version and update state.", kObj, true},
    {"selftest", "Run the device self test.", kObj, true},
    {"system_health", "Heap, card and task health.", kObj, true},
    {"device_log", "Recent device log lines.", kObj, true},
    {"agent_list", "List sub-agent jobs with their state.", kObj, false},
    {"agent_stop", "Stop a sub-agent job by id.", kIdSchema, false},
    {"move_file", "Move or rename a file.",
     R"({"type":"object","properties":{"from":{"type":"string"},"to":{"type":"string"}},"required":["from","to"]})", false},
    {"fetch_url", "Fetch a web page or file by URL; path (project/name) stores it. The text is outside content.",
     R"({"type":"object","properties":{"url":{"type":"string"},"path":{"type":"string"}},"required":["url"]})", false},
    {"share_file", "Share a file by link.", kPathSchema, false},
    {"file_stat", "Size, lines and last write of a file.", kPathSchema, false},
    {"read_pdf", "Read the text of a PDF.", kPathSchema, false},
    {"pdf_write", "Write a PDF from text.", kPathSchema, false},
    {"image_generate", "Generate an image from a prompt.",
     R"({"type":"object","properties":{"prompt":{"type":"string"}},"required":["prompt"]})", false},
    {"speak", "Speak text on the device speaker.",
     R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})", false},
    {"ledger_target", "Show or set the daily kcal and protein target.", kObj, false},
    {"food_portions", "Typical portion weights.", kObj, false},
    {"skill_list", "List saved skills.", kObj, true},
    {"skill_save", "Save a skill.", kPathSchema, true},
    {"skill_delete", "Delete a skill.", kPathSchema, true},
};

ToolResult okHandler(JsonObjectConst, const Principal&) { return ToolResult::ok("done"); }

enum class Who { Owner, Member, Guest };
const Who kAllWho[] = {Who::Owner, Who::Member, Who::Guest};
const char* whoSlug(Who w) { return w == Who::Owner ? "owner" : w == Who::Member ? "member" : "guest"; }
Principal principalOf(Who w) {
  switch (w) {
    case Who::Owner: return principalForRole("1001", Role::Admin);
    case Who::Member: return principalForRole("2002", Role::User);
    default: return principalForRole("3003", Role::Guest);
  }
}

// One device's registry. Ledger and food tools exist only where the food capability is
// on. A guest may call nothing that has an effect (the fixture's access rule).
struct Rig {
  ToolRegistry reg;
  bool food;
  bool shortHost;
  Rig(bool food_, bool shortHost_) : food(food_), shortHost(shortHost_) {
    for (const td::ToolDecl& d : td::kTools) {
      const std::string n = d.name;
      if (n == "more") continue;
      if (!food && (n == "ledger_write" || n == "ledger_day")) continue;
      reg.add(n, d.description, okHandler, plain::declSchema(d, shortHost));
    }
    for (const GroupTool& g : kGroupTools) {
      const std::string n = g.name;
      if (!food && (n == "ledger_target" || n == "food_portions")) continue;
      reg.add(n, g.desc, okHandler, g.schema);
      if (g.adminOnly) reg.setAdminOnly(n);
    }
    registerMore(reg);
    plain::applyToolPack(reg, food);
    reg.setAccessResolver([this](const std::string& name, JsonObjectConst, const Principal& who) {
      if (who.role == Role::Guest && reg.classOf(name).effect == EffectClass::Effect)
        return ToolRegistry::Verdict::deny("guest");
      return ToolRegistry::Verdict::allow();
    });
  }
};

std::string repeatTo(const std::string& unit, size_t bytes) {
  std::string out;
  while (out.size() < bytes) out += unit;
  return out.substr(0, bytes);
}

// Pinned notes of exactly `cap` bytes: 'key: value' lines.
std::string notesAtCap(size_t cap, unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::string out;
  for (int i = 0;; i++) {
    std::string line = "fact-" + std::to_string(i) + ": " + repeatTo("synthetic value " + std::to_string(rng() % 97) + " ", 20 + rng() % 120);
    const size_t need = (out.empty() ? 0 : 1) + line.size();
    if (out.size() + need > cap) {
      const size_t left = cap - out.size();
      if (left > 8) out += (out.empty() ? "" : "\n") + std::string("last: ") + repeatTo("x", left - (out.empty() ? 6 : 7));
      break;
    }
    out += (out.empty() ? "" : "\n") + line;
  }
  return out;
}

std::string skillsLines(int n) {
  std::string out;
  for (int i = 0; i < n; i++)
    out += "skills/skill-" + std::to_string(i) + ".md: when the person plans thing number " + std::to_string(i) + "\n";
  return out;
}

std::string digestsAt(size_t bytes) {
  std::string out;
  for (int d = 10; out.size() + 768 <= bytes; d++)
    out += "2026-11-" + std::to_string(d) + ": " + repeatTo("Owner and Nimbus talked about plans. ", 750) + "\n";
  return out;
}
std::string recentFilesAt(size_t bytes) {
  std::string out;
  for (int i = 0; out.size() + 40 <= bytes; i++) out += "notes/file-" + std::to_string(i) + ".md 2026-11-17 13:02\n";
  return out;
}

struct State {
  plain::TurnCtx turn;
  Principal who;
  plain::RequestFacts facts;
};

// A Lumi-shaped state: notes at their cap (owner 4,096 B, members 1,024 B), an
// overfull skills index, a full standard digest block and recent-files list.
std::unique_ptr<State> lumiState(const Rig& rig, Who w, const std::string& model = "claude-sonnet-5-5",
                                 const std::string& tz = "Europe/London") {
  std::unique_ptr<State> s(new State());
  s->who = principalOf(w);
  s->turn.model = model;
  s->turn.who = &s->who;
  s->facts.turn = &s->turn;
  s->facts.reg = &rig.reg;
  s->facts.pinnedNotes = notesAtCap(w == Who::Owner ? plain::kPinnedNotesCeiling : 1024);
  s->facts.skillsIndex = w == Who::Owner ? skillsLines(40) : std::string();
  s->facts.digestBlock = digestsAt(6144);
  s->facts.recentFiles = recentFilesAt(600);
  s->facts.timeZone = tz;
  s->facts.food = rig.food;
  s->facts.admin = (w == Who::Owner);
  s->facts.shortHost = rig.shortHost;
  return s;
}

struct Composed {
  plain::SystemBlocks sys;
  std::vector<plain::ToolSpec> tools;
};
Composed compose(const plain::RequestFacts& f) {
  Composed c;
  TEST_ASSERT_TRUE_MESSAGE(plain::composeSystem(f, c.sys).ok(), "composeSystem");
  TEST_ASSERT_TRUE_MESSAGE(plain::toolManifest(f, c.tools).ok(), "toolManifest");
  return c;
}

std::string label(Who w, bool food, bool shortHost) {
  return std::string(whoSlug(w)) + (food ? " food" : " nofood") + (shortHost ? " 60s" : " 120s");
}

template <typename F>
void forEveryConfig(F fn) {
  for (Who w : kAllWho)
    for (bool food : {true, false})
      for (bool shortHost : {false, true}) {
        Rig rig(food, shortHost);
        fn(rig, w, label(w, food, shortHost));
      }
}

bool hasWord(const std::string& text, const std::string& word) {
  for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
    const bool l = at == 0 || !(std::isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_');
    const size_t e = at + word.size();
    const bool r = e >= text.size() || !(std::isalnum((unsigned char)text[e]) || text[e] == '_');
    if (l && r) return true;
  }
  return false;
}

}  // namespace

// ---- ceilings ------------------------------------------------------------------------

static void test_every_role_pack_and_host_fits_the_ceilings() {
  const std::string longModel(plain::kModelNameMax + 30, 'm');
  const std::string longTz(plain::kTimeZoneMax + 30, 'z');
  forEveryConfig([&](Rig& rig, Who w, const std::string& at) {
    for (int variant = 0; variant < 2; variant++) {
      auto s = variant ? lumiState(rig, w, longModel, longTz) : lumiState(rig, w);
      const Composed c = compose(s->facts);
      const std::string idx = toolsIndex(rig.reg, s->who);
      TEST_ASSERT_TRUE_MESSAGE(idx.size() <= kToolsIndexMax, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(c.sys.core.size() <= plain::kCoreCeiling, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(c.sys.core.size() + c.sys.notes.size() <= plain::kSystemBytesCeiling, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(c.tools.size() <= plain::kMaxAlwaysOnTools, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(plain::toolsBytes(c.tools) <= plain::kToolsBytesCeiling, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(!c.tools.empty() && c.tools.back().name == "more", at.c_str());
      for (const plain::ToolSpec& t : c.tools) {
        TEST_ASSERT_TRUE_MESSAGE(t.description.size() <= plain::kToolDescriptionMax, t.name.c_str());
        JsonDocument d;
        TEST_ASSERT_FALSE_MESSAGE(deserializeJson(d, t.schemaJson), t.name.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE("object", d["type"].as<const char*>(), t.name.c_str());
      }
      // The pinned notes are all there when they are at their cap (nothing dropped).
      if (variant == 0) TEST_ASSERT_TRUE_MESSAGE(c.sys.notes.find(s->facts.pinnedNotes) != std::string::npos, at.c_str());
    }
  });
}

// Every declaration fits on its own, and the whole catalog of declarations (both packs)
// fits the tool_descriptions class.
static void test_every_declaration_fits() {
  size_t total = 0;
  for (const td::ToolDecl& d : td::kTools) {
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(d.description) <= plain::kToolDescriptionMax, d.name);
    for (bool shortHost : {false, true}) {
      JsonDocument doc;
      TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, plain::declSchema(d, shortHost)), d.name);
    }
    total += std::strlen(d.name) + std::strlen(d.description) + std::strlen(d.schema) +
             (d.schemaTail ? std::strlen(d.schemaTail) : 0);
    TEST_ASSERT_TRUE_MESSAGE(toolClassFor(d.name).group != nullptr, d.name);
  }
  TEST_ASSERT_TRUE(total <= plain::kToolsBytesCeiling);
  // write_file states its host's cap in the schema.
  const td::ToolDecl* w = plain::toolDecl("write_file");
  TEST_ASSERT_NOT_NULL(w);
  TEST_ASSERT_TRUE(plain::declSchema(*w, false).find("\"maxLength\":12000") != std::string::npos);
  TEST_ASSERT_TRUE(plain::declSchema(*w, true).find("\"maxLength\":4096") != std::string::npos);
  TEST_ASSERT_NULL(plain::toolDecl("fetch_url"));
}

// ---- callable --------------------------------------------------------------------------

static void test_every_tool_like_name_is_callable() {
  forEveryConfig([&](Rig& rig, Who w, const std::string& at) {
    auto s = lumiState(rig, w);
    const Composed c = compose(s->facts);
    std::string all = c.sys.core + "\n" + c.sys.notes + "\n" + c.sys.digests;
    for (const plain::ToolSpec& t : c.tools) all += "\n" + t.description;
    const std::vector<std::string> bad = plain::uncallableNames(all, c.tools, rig.reg, s->who);
    std::string msg = at + ":";
    for (const std::string& b : bad) msg += " " + b;
    TEST_ASSERT_TRUE_MESSAGE(bad.empty(), msg.c_str());
    // Advertised == callable: each array tool dispatches for this caller.
    for (const plain::ToolSpec& t : c.tools) {
      if (t.name == "more") continue;
      JsonDocument args;
      args.to<ArduinoJson::JsonObject>();
      const ToolResult r = rig.reg.dispatch(t.name, args.as<JsonObjectConst>(), s->who);
      TEST_ASSERT_TRUE_MESSAGE(r.success, (at + " " + t.name + ": " + r.error).c_str());
    }
    // And each declared tool left out is out of the pack or not callable by this caller.
    for (const td::ToolDecl& d : td::kTools) {
      bool listed = false;
      for (const plain::ToolSpec& t : c.tools) listed = listed || t.name == d.name;
      if (listed || std::string(d.name) == "more") continue;
      JsonDocument args;
      args.to<ArduinoJson::JsonObject>();
      const ToolResult r = rig.reg.dispatch(d.name, args.as<JsonObjectConst>(), s->who);
      const bool packOut = (std::string(d.name).rfind("ledger_", 0) == 0) != rig.food;
      TEST_ASSERT_TRUE_MESSAGE(packOut || !r.success, (at + " left out but callable: " + d.name).c_str());
    }
  });
}

// Counter-test: the checker sees what it must, and a sentence naming a tool the caller
// cannot call is left out of the core (a guest cannot schedule).
static void test_the_checker_catches_an_uncallable_name() {
  Rig rig(false, false);
  auto s = lumiState(rig, Who::Owner);
  const Composed c = compose(s->facts);
  auto bad = [&](const std::string& t) { return plain::uncallableNames(t, c.tools, rig.reg, s->who); };
  TEST_ASSERT_EQUAL(1, (int)bad("then call fetch_page(url) to read it").size());
  TEST_ASSERT_EQUAL_STRING("ledger_write", bad("log it with ledger_write").at(0).c_str());   // no food pack
  TEST_ASSERT_EQUAL_STRING("files_read", bad("use files_read").at(0).c_str());              // an old-engine name
  TEST_ASSERT_EQUAL(0, (int)bad("agent_list through more, then view r12").size());           // reachable, advertised
  TEST_ASSERT_EQUAL(0, (int)bad("Search the web. Send a note. Schedule it.").size());         // capitals are words
  TEST_ASSERT_EQUAL(0, (int)bad("notes/ledger.ledger_write").size());                       // an extension
  // A seeded mention in the composed core turns the property red.
  TEST_ASSERT_EQUAL(1, (int)bad(c.sys.core + " Use ledger_day for totals.").size());

  Rig food(true, false);
  auto g = lumiState(food, Who::Guest);
  auto o = lumiState(food, Who::Owner);
  const Composed gc = compose(g->facts), oc = compose(o->facts);
  TEST_ASSERT_TRUE(hasWord(oc.sys.core, "schedule"));
  TEST_ASSERT_FALSE(hasWord(gc.sys.core, "schedule"));
  TEST_ASSERT_FALSE(hasWord(gc.sys.core, "ledger_write"));   // the guest cannot write the ledger
  TEST_ASSERT_TRUE(hasWord(oc.sys.core, "ledger_write"));
  for (const plain::ToolSpec& t : gc.tools) TEST_ASSERT_TRUE_MESSAGE(food.reg.classOf(t.name).effect != EffectClass::Effect || t.name == "more", t.name.c_str());
}

// The registry carries the declared bytes, so a listing through more, the MCP surface and
// the array show one text; more's declaration is RB-N4's.
static void test_registry_and_manifest_agree() {
  forEveryConfig([&](Rig& rig, Who w, const std::string& at) {
    auto s = lumiState(rig, w);
    const Composed c = compose(s->facts);
    for (const plain::ToolSpec& t : c.tools) {
      bool found = false;
      for (const ToolRegistry::Spec& r : rig.reg.toolSpecs()) {
        if (r.name != t.name) continue;
        found = true;
        TEST_ASSERT_EQUAL_STRING_MESSAGE(r.description.c_str(), t.description.c_str(), (at + " " + t.name).c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(r.schemaJson.c_str(), t.schemaJson.c_str(), (at + " " + t.name).c_str());
      }
      TEST_ASSERT_TRUE_MESSAGE(found, t.name.c_str());
    }
  });
  const td::ToolDecl* m = plain::toolDecl("more");
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_EQUAL_STRING(kMoreDescription, m->description);
  TEST_ASSERT_EQUAL_STRING(kMoreSchema, m->schema);
}

static void test_fixed_order_and_packs() {
  const char* food[] = {"read_file", "write_file", "append_file", "edit_file", "list_files", "note", "search",
                        "web_search", "view", "send", "schedule", "ledger_write", "ledger_day", "more"};
  const char* nofood[] = {"read_file", "write_file", "append_file", "edit_file", "list_files", "note", "search",
                          "web_search", "view", "send", "schedule", "agent_start", "delete_file", "more"};
  for (bool f : {true, false}) {
    Rig rig(f, false);
    auto s = lumiState(rig, Who::Owner);
    const Composed c = compose(s->facts);
    TEST_ASSERT_EQUAL(14, (int)c.tools.size());
    for (size_t i = 0; i < 14; i++) TEST_ASSERT_EQUAL_STRING((f ? food : nofood)[i], c.tools[i].name.c_str());
    // The tools index never repeats a tool of the array.
    const std::string idx = toolsIndex(rig.reg, s->who);
    TEST_ASSERT_TRUE(idx.size() > 0);
    for (const plain::ToolSpec& t : c.tools) TEST_ASSERT_FALSE_MESSAGE(hasWord(idx, t.name), t.name.c_str());
    // Turning food on again puts the pair back behind more.
    plain::applyToolPack(rig.reg, true);
    TEST_ASSERT_FALSE(rig.reg.classOf("agent_start").alwaysOn());
    TEST_ASSERT_EQUAL_STRING("agents", rig.reg.classOf("agent_start").group);
  }
  // The pack follows the capability, not what happens to be registered: with food off,
  // registered ledger tools are not in the array and the core has no food paragraph.
  {
    Rig rig(true, false);
    auto s = lumiState(rig, Who::Owner);
    s->facts.food = false;
    const Composed c = compose(s->facts);
    for (const plain::ToolSpec& t : c.tools) TEST_ASSERT_TRUE_MESSAGE(t.name.rfind("ledger_", 0) != 0, t.name.c_str());
    TEST_ASSERT_FALSE(hasWord(c.sys.core, "ledger_write"));
  }
  // An unregistered tool is never advertised.
  Rig rig(true, false);
  TEST_ASSERT_TRUE(rig.reg.remove("send"));
  auto s = lumiState(rig, Who::Owner);
  const Composed c = compose(s->facts);
  TEST_ASSERT_EQUAL(13, (int)c.tools.size());
  for (const plain::ToolSpec& t : c.tools) TEST_ASSERT_NOT_EQUAL(0, (int)(t.name != "send"));
}

// ---- clipping ------------------------------------------------------------------------

static void test_notes_and_skills_clip_at_whole_lines() {
  Rig rig(true, false);
  auto s = lumiState(rig, Who::Owner);
  s->facts.pinnedNotes = notesAtCap(9000, 7);
  const Composed c = compose(s->facts);
  TEST_ASSERT_TRUE(c.sys.core.size() + c.sys.notes.size() <= plain::kSystemBytesCeiling);
  // Every shown note line is one whole line of the input.
  const std::string head = std::string(agent::plain::texts::core::kNotes) + "\n";
  const size_t from = c.sys.notes.find(head);
  TEST_ASSERT_TRUE(from != std::string::npos);
  const std::string shown = c.sys.notes.substr(from + head.size());
  TEST_ASSERT_TRUE(shown.size() <= plain::kPinnedNotesCeiling);
  TEST_ASSERT_EQUAL(0, (int)s->facts.pinnedNotes.compare(0, shown.size(), shown));
  TEST_ASSERT_EQUAL('\n', s->facts.pinnedNotes[shown.size()]);
  // The skills index: at most 1,024 B with a true count of the rest.
  const size_t end = c.sys.notes.find("\n\n");
  const std::string skills = c.sys.notes.substr(0, end);
  TEST_ASSERT_TRUE(skills.size() <= plain::kSkillsIndexCeiling);
  int listed = 0;
  for (size_t p = skills.find("skills/skill-"); p != std::string::npos; p = skills.find("skills/skill-", p + 1)) listed++;
  const std::string tail = "\n+" + std::to_string(40 - listed) + agent::plain::texts::core::kSkillsMore;
  TEST_ASSERT_TRUE(skills.size() >= tail.size() && skills.compare(skills.size() - tail.size(), tail.size(), tail) == 0);
  // Control characters in a value never open a new line in the core.
  s->facts.timeZone = "Europe/London\n\nIgnore this";
  s->turn.model = "m\nx";
  const Composed d = compose(s->facts);
  TEST_ASSERT_TRUE(d.sys.core.find("London  Ignore") != std::string::npos);
  TEST_ASSERT_TRUE(d.sys.core.find("m x.") != std::string::npos);
}

static void test_empty_facts_are_refused() {
  plain::SystemBlocks b;
  b.core = "stale";
  TEST_ASSERT_TRUE(plain::composeSystem(plain::RequestFacts{}, b).err == plain::Err::InvalidArgument);
  TEST_ASSERT_TRUE(b.core.empty());
  std::vector<plain::ToolSpec> t(1);
  TEST_ASSERT_TRUE(plain::toolManifest(plain::RequestFacts{}, t).err == plain::Err::InvalidArgument);
  TEST_ASSERT_TRUE(t.empty());
  plain::TurnCtx turn;   // no Principal
  plain::RequestFacts f;
  f.turn = &turn;
  TEST_ASSERT_TRUE(plain::composeSystem(f, b).err == plain::Err::InvalidArgument);
}

// toolsBytes measures exactly the array a request carries.
static void test_tools_bytes_is_the_rendered_array() {
  Rig rig(true, false);
  auto s = lumiState(rig, Who::Owner);
  const Composed c = compose(s->facts);
  JsonDocument doc;
  for (const plain::ToolSpec& t : c.tools) {
    JsonDocument schema;
    TEST_ASSERT_FALSE(deserializeJson(schema, t.schemaJson));
    ArduinoJson::JsonObject o = doc.add<ArduinoJson::JsonObject>();
    o["name"] = t.name;
    o["description"] = t.description;
    o["input_schema"] = schema;
  }
  TEST_ASSERT_EQUAL((int)measureJson(doc), (int)plain::toolsBytes(c.tools));
}

// ---- byte stability --------------------------------------------------------------------

namespace {
std::string randomText(std::mt19937& rng, int lines) {
  static const char* words[] = {"tea", "run", "Leeds", "bike lock", "green bin", "café", "2026-11-17", "x: y"};
  std::string out;
  for (int i = 0; i < lines; i++) {
    out += "k" + std::to_string(rng() % 1000) + ":";
    for (int w = 0; w < 1 + (int)(rng() % 8); w++) out += std::string(" ") + words[rng() % 8];
    out += "\n";
  }
  return out;
}
}  // namespace

static void test_same_facts_same_bytes() {
  std::mt19937 rng(20261118);
  for (int i = 0; i < 300; i++) {
    const bool food = rng() % 2, shortHost = rng() % 2;
    const Who w = kAllWho[rng() % 3];
    Rig a(food, shortHost), b(food, shortHost);
    auto sa = lumiState(a, w), sb = lumiState(b, w);
    const std::string notes = randomText(rng, rng() % 80), skills = randomText(rng, rng() % 40);
    const std::string tz = rng() % 4 ? "Europe/London" : "", model = rng() % 2 ? "claude-haiku-5-5" : "gpt-5.6-luna";
    for (State* s : {sa.get(), sb.get()}) {
      s->facts.pinnedNotes = notes;
      s->facts.skillsIndex = skills;
      s->facts.timeZone = tz;
      s->turn.model = model;
    }
    // Everything composition must not read differs between the two.
    sb->turn.turn = 1000 + i;
    sb->turn.roundNo = 9;
    sb->turn.attempt = 2;
    sb->turn.startedAt = 1794920520 + i * 3600;
    sb->turn.startedMs = 77;
    sb->turn.tainted = true;
    sb->turn.trigger = plain::Trigger::Routine;
    sb->turn.dialect = plain::Dialect::ChatCompletions;
    sb->turn.profile = nimbus::orch::tx::Profile::Wide;
    sb->turn.chatKey = "other";
    sb->turn.host = "elsewhere";
    const Composed ca = compose(sa->facts), cb = compose(sb->facts), ca2 = compose(sa->facts);
    for (const Composed* x : {&cb, &ca2}) {
      TEST_ASSERT_TRUE(ca.sys.core == x->sys.core);
      TEST_ASSERT_TRUE(ca.sys.notes == x->sys.notes);
      TEST_ASSERT_TRUE(ca.sys.digests == x->sys.digests);
      TEST_ASSERT_EQUAL((int)ca.tools.size(), (int)x->tools.size());
      for (size_t t = 0; t < ca.tools.size(); t++) {
        TEST_ASSERT_TRUE(ca.tools[t].name == x->tools[t].name);
        TEST_ASSERT_TRUE(ca.tools[t].description == x->tools[t].description);
        TEST_ASSERT_TRUE(ca.tools[t].schemaJson == x->tools[t].schemaJson);
      }
    }
    // Each fact moves only its own block: notes and skills never the core or the
    // tools; digests and recent files never the core or the notes.
    sb->facts.pinnedNotes = notes + "added: one more\n";
    sb->facts.skillsIndex = "skills/new.md: new\n" + skills;
    sb->facts.digestBlock += "2026-11-18: more\n";
    sb->facts.recentFiles = "a/b.md 2026-11-18 09:00\n";
    const Composed cc = compose(sb->facts);
    TEST_ASSERT_TRUE(ca.sys.core == cc.sys.core);
    TEST_ASSERT_TRUE(ca.sys.digests != cc.sys.digests);
    TEST_ASSERT_EQUAL((int)ca.tools.size(), (int)cc.tools.size());
  }
}

// Counter-test: a clock in the core is caught by the same comparison.
static void test_a_clock_in_the_core_would_be_caught() {
  Rig rig(true, false);
  auto a = lumiState(rig, Who::Owner), b = lumiState(rig, Who::Owner);
  b->turn.startedAt = a->turn.startedAt + 300;
  auto withClock = [](const State& s) {
    plain::SystemBlocks out;
    TEST_ASSERT_TRUE(plain::composeSystem(s.facts, out).ok());
    return out.core + std::to_string(s.turn.startedAt);   // what a leaked clock would do
  };
  TEST_ASSERT_TRUE(withClock(*a) != withClock(*b));
}

// ---- the golden: measured figures and the owner's full text ---------------------------

namespace {
bool blessMode() {
  const char* e = std::getenv("GOLDEN_UPDATE");
  return e && std::strcmp(e, "1") == 0;
}
bool readAll(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
}  // namespace

static void test_golden_records_every_role_pack_and_host() {
  std::string g =
      "# Engine-2 request composition on the synthetic Lumi-state fixture (test/test_plain_request).\n"
      "# Generated: GOLDEN_UPDATE=1 pio test -e native -f test_plain_request; do not edit.\n"
      "# Gate: tools/release_gate/check_plain_ceilings.py.\n";
  forEveryConfig([&](Rig& rig, Who w, const std::string&) {
    auto s = lumiState(rig, w);
    const Composed c = compose(s->facts);
    size_t longest = 0;
    for (const plain::ToolSpec& t : c.tools) longest = t.description.size() > longest ? t.description.size() : longest;
    char line[400];
    std::snprintf(line, sizeof(line),
                  "measure role=%s pack=%s host=%s core=%zu index=%zu notes=%zu system=%zu digests=%zu tools=%zu "
                  "tools_bytes=%zu longest_description=%zu\n",
                  whoSlug(w), rig.food ? "food" : "nofood", rig.shortHost ? "60s" : "120s", c.sys.core.size(),
                  toolsIndex(rig.reg, s->who).size(), c.sys.notes.size(), c.sys.core.size() + c.sys.notes.size(),
                  c.sys.digests.size(), c.tools.size(), plain::toolsBytes(c.tools), longest);
    g += line;
  });
  Rig rig(true, false);
  auto s = lumiState(rig, Who::Owner);
  const Composed c = compose(s->facts);
  g += "== core (owner, food, 120s, claude-sonnet-5-5, Europe/London)\n" + c.sys.core + "\n";
  g += "== notes (first 400 B)\n" + c.sys.notes.substr(0, 400) + "\n";
  g += "== tools\n";
  for (const plain::ToolSpec& t : c.tools) {
    char line[64];
    std::snprintf(line, sizeof(line), "%zu %zu ", t.description.size(), t.schemaJson.size());
    g += line + t.name + ": " + t.description + "\n  " + t.schemaJson + "\n";
  }
  if (const char* dump = std::getenv("PLAIN_REQUEST_DUMP")) {   // the provider acceptance check reads it
    JsonDocument d;
    for (bool f : {true, false}) {
      Rig r(f, false);
      auto st = lumiState(r, Who::Owner);
      const Composed x = compose(st->facts);
      ArduinoJson::JsonObject o = d[f ? "food" : "nofood"].to<ArduinoJson::JsonObject>();
      o["core"] = x.sys.core;
      o["notes"] = x.sys.notes;
      o["digests"] = x.sys.digests;
      ArduinoJson::JsonArray ts = o["tools"].to<ArduinoJson::JsonArray>();
      for (const plain::ToolSpec& t : x.tools) {
        ArduinoJson::JsonObject j = ts.add<ArduinoJson::JsonObject>();
        j["name"] = t.name;
        j["description"] = t.description;
        j["schema"] = t.schemaJson;
      }
    }
    std::string out;
    serializeJson(d, out);
    FILE* f = std::fopen(dump, "wb");
    TEST_ASSERT_NOT_NULL(f);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
  const std::string path = "test/golden/plain_prompt_core.txt";
  if (blessMode()) {
    FILE* f = std::fopen(path.c_str(), "wb");
    TEST_ASSERT_NOT_NULL(f);
    std::fwrite(g.data(), 1, g.size(), f);
    std::fclose(f);
    return;
  }
  std::string cur;
  TEST_ASSERT_TRUE_MESSAGE(readAll(path, cur), "missing golden: GOLDEN_UPDATE=1 pio test -e native -f test_plain_request");
  if (cur != g) {
    FILE* f = std::fopen("test/golden/plain_prompt_core.txt.out", "wb");
    if (f) {
      std::fwrite(g.data(), 1, g.size(), f);
      std::fclose(f);
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(cur == g, "plain_prompt_core.txt is stale (see .out); read the diff, then GOLDEN_UPDATE=1");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_role_pack_and_host_fits_the_ceilings);
  RUN_TEST(test_every_declaration_fits);
  RUN_TEST(test_every_tool_like_name_is_callable);
  RUN_TEST(test_the_checker_catches_an_uncallable_name);
  RUN_TEST(test_registry_and_manifest_agree);
  RUN_TEST(test_fixed_order_and_packs);
  RUN_TEST(test_notes_and_skills_clip_at_whole_lines);
  RUN_TEST(test_empty_facts_are_refused);
  RUN_TEST(test_tools_bytes_is_the_rendered_array);
  RUN_TEST(test_same_facts_same_bytes);
  RUN_TEST(test_a_clock_in_the_core_would_be_caught);
  RUN_TEST(test_golden_records_every_role_pack_and_host);
  return UNITY_END();
}
