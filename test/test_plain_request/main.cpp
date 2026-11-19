// Request composition of the plain engine (plain/request.h, PRD section 4, lane RB-E2).
//
// Class rules proven here, over every role and host rather than one instance:
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
//   - one fixed pack (ruling 41): no fact, registration or person changes which tools the
//     array may hold or adds a paragraph to the core; no use-case word reaches any text a
//     model sees (the core literals, the declarations, the composed blocks, the tools index
//     and every group listing), with a counter-test that the matcher sees a seeded word;
//   - a refused allocation at every allocation of composition fails the call with outputs
//     empty, never a partial array or a partial system text (ruling 30);
//   - the golden test/golden/plain_prompt_core.txt records the measured figures for every
//     role and host plus the owner's full request text (GOLDEN_UPDATE=1 re-blesses;
//     tools/release_gate/check_plain_ceilings.py gates the figures).
#include <unity.h>

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
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

// One device's registry: every declared tool registered with its declared bytes (the
// always-on ones and those declared for a group behind more), then the group tools. A
// guest may call nothing that has an effect (the fixture's access rule).
struct Rig {
  ToolRegistry reg;
  bool shortHost;
  explicit Rig(bool shortHost_) : shortHost(shortHost_) {
    for (const td::ToolDecl& d : td::kTools) {
      if (std::string(d.name) == "more") continue;
      reg.add(d.name, d.description, okHandler, plain::declSchema(d, shortHost));
    }
    for (const td::ToolDecl& d : td::kBehindMore) reg.add(d.name, d.description, okHandler, plain::declSchema(d, shortHost));
    for (const GroupTool& g : kGroupTools) {
      reg.add(g.name, g.desc, okHandler, g.schema);
      if (g.adminOnly) reg.setAdminOnly(g.name);
    }
    registerMore(reg);
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
  s->facts.admin = (w == Who::Owner);
  s->facts.shortHost = rig.shortHost;
  return s;
}

// Blocks 1 and 2 as a wire that joins them carries them (a paragraph break between).
size_t systemBytes(const plain::SystemBlocks& b) { return b.core.size() + (b.notes.empty() ? 0 : 2) + b.notes.size(); }

bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
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

std::string label(Who w, bool shortHost) { return std::string(whoSlug(w)) + (shortHost ? " 60s" : " 120s"); }

template <typename F>
void forEveryConfig(F fn) {
  for (Who w : kAllWho)
    for (bool shortHost : {false, true}) {
      Rig rig(shortHost);
      fn(rig, w, label(w, shortHost));
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

static void test_every_role_and_host_fits_the_ceilings() {
  const std::string longModel(plain::kModelNameMax + 30, 'm');
  const std::string longTz(plain::kTimeZoneMax + 30, 'z');
  forEveryConfig([&](Rig& rig, Who w, const std::string& at) {
    for (int variant = 0; variant < 2; variant++) {
      auto s = variant ? lumiState(rig, w, longModel, longTz) : lumiState(rig, w);
      const Composed c = compose(s->facts);
      const std::string idx = toolsIndex(rig.reg, s->who);
      TEST_ASSERT_TRUE_MESSAGE(idx.size() <= kToolsIndexMax, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(c.sys.core.size() <= plain::kCoreCeiling, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(systemBytes(c.sys) <= plain::kSystemBytesCeiling, at.c_str());
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

// Every declaration fits on its own, and the whole catalog of declarations (the array's and
// those behind more) fits the tool_descriptions class.
static void test_every_declaration_fits() {
  size_t total = 0;
  std::vector<td::ToolDecl> all(std::begin(td::kTools), std::end(td::kTools));
  all.insert(all.end(), std::begin(td::kBehindMore), std::end(td::kBehindMore));
  for (const td::ToolDecl& d : all) {
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(d.description) <= plain::kToolDescriptionMax, d.name);
    for (bool shortHost : {false, true}) {
      JsonDocument doc;
      TEST_ASSERT_FALSE_MESSAGE(deserializeJson(doc, plain::declSchema(d, shortHost)), d.name);
    }
    total += std::strlen(d.name) + std::strlen(d.description) + std::strlen(d.schema) +
             (d.schemaTail ? std::strlen(d.schemaTail) : 0);
    TEST_ASSERT_TRUE_MESSAGE(toolClassFor(d.name).group != nullptr, d.name);
    TEST_ASSERT_TRUE_MESSAGE(plain::toolDecl(d.name) != nullptr, d.name);
  }
  // The array's declarations are always on by the class table; those behind more are not.
  for (const td::ToolDecl& d : td::kTools) TEST_ASSERT_TRUE_MESSAGE(toolClassFor(d.name).alwaysOn(), d.name);
  for (const td::ToolDecl& d : td::kBehindMore) TEST_ASSERT_FALSE_MESSAGE(toolClassFor(d.name).alwaysOn(), d.name);
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
    // And each declared tool left out is not callable by this caller.
    for (const td::ToolDecl& d : td::kTools) {
      bool listed = false;
      for (const plain::ToolSpec& t : c.tools) listed = listed || t.name == d.name;
      if (listed || std::string(d.name) == "more") continue;
      JsonDocument args;
      args.to<ArduinoJson::JsonObject>();
      const ToolResult r = rig.reg.dispatch(d.name, args.as<JsonObjectConst>(), s->who);
      TEST_ASSERT_FALSE_MESSAGE(r.success, (at + " left out but callable: " + d.name).c_str());
    }
  });
}

// Counter-test: the checker sees what it must, and a sentence naming a tool the caller
// cannot call is left out of the core (a guest cannot schedule).
static void test_the_checker_catches_an_uncallable_name() {
  Rig rig(false);
  auto s = lumiState(rig, Who::Owner);
  const Composed c = compose(s->facts);
  auto bad = [&](const std::string& t) { return plain::uncallableNames(t, c.tools, rig.reg, s->who); };
  TEST_ASSERT_EQUAL(1, (int)bad("then call fetch_page(url) to read it").size());
  TEST_ASSERT_EQUAL_STRING("records_write", bad("log it with records_write(rows)").at(0).c_str());   // no such tool
  TEST_ASSERT_EQUAL_STRING("files_read", bad("use files_read").at(0).c_str());              // an old-engine name
  TEST_ASSERT_EQUAL(0, (int)bad("agent_list through more, then view r12").size());           // reachable, advertised
  TEST_ASSERT_EQUAL(0, (int)bad("delete_file through more").size());                        // behind more, reachable
  TEST_ASSERT_EQUAL(0, (int)bad("Search the web. Send a note. Schedule it.").size());         // capitals are words
  TEST_ASSERT_EQUAL(0, (int)bad("notes/list.files_read").size());                           // an extension
  // A seeded mention in the composed core turns the property red.
  TEST_ASSERT_EQUAL(1, (int)bad(c.sys.core + " Use files_read for totals.").size());

  auto g = lumiState(rig, Who::Guest);
  const Composed gc = compose(g->facts);
  TEST_ASSERT_TRUE(hasWord(c.sys.core, "schedule"));
  TEST_ASSERT_FALSE(hasWord(gc.sys.core, "schedule"));
  for (const plain::ToolSpec& t : gc.tools) TEST_ASSERT_TRUE_MESSAGE(rig.reg.classOf(t.name).effect != EffectClass::Effect || t.name == "more", t.name.c_str());
  // The guest's array holds what it may call: the reads, calc and more.
  const char* guest[] = {"read_file", "list_files", "search", "web_search", "view", "calc", "more"};
  TEST_ASSERT_EQUAL(7, (int)gc.tools.size());
  for (size_t i = 0; i < 7; i++) TEST_ASSERT_EQUAL_STRING(guest[i], gc.tools[i].name.c_str());
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

// One fixed pack: 13 always-on tools plus more, the same order on every host, whatever the
// registry holds behind more (ruling 41: nothing swaps a slot for a use case).
static void test_one_fixed_pack_in_a_fixed_order() {
  const char* want[] = {"read_file", "write_file", "append_file", "edit_file", "list_files", "note", "search",
                        "web_search", "view", "send", "schedule", "calc", "agent_start", "more"};
  for (bool shortHost : {false, true}) {
    Rig rig(shortHost);
    auto s = lumiState(rig, Who::Owner);
    const Composed c = compose(s->facts);
    TEST_ASSERT_EQUAL(14, (int)c.tools.size());
    for (size_t i = 0; i < 14; i++) TEST_ASSERT_EQUAL_STRING(want[i], c.tools[i].name.c_str());
    TEST_ASSERT_EQUAL((int)td::kToolCount, (int)c.tools.size());
    // The tools index never repeats a tool of the array, and lists what is behind more.
    const std::string idx = toolsIndex(rig.reg, s->who);
    TEST_ASSERT_TRUE(idx.size() > 0);
    for (const plain::ToolSpec& t : c.tools) TEST_ASSERT_FALSE_MESSAGE(hasWord(idx, t.name), t.name.c_str());
    TEST_ASSERT_TRUE_MESSAGE(hasWord(idx, "delete_file"), idx.c_str());
    // A tool registered behind more never enters the array, and taking every group tool out
    // leaves the array and the core above the index as they were.
    Rig bare(shortHost);
    for (const GroupTool& g : kGroupTools) TEST_ASSERT_TRUE(bare.reg.remove(g.name));
    for (const td::ToolDecl& d : td::kBehindMore) TEST_ASSERT_TRUE(bare.reg.remove(d.name));
    auto b = lumiState(bare, Who::Owner);
    const Composed cb = compose(b->facts);
    TEST_ASSERT_EQUAL((int)c.tools.size(), (int)cb.tools.size());
    for (size_t i = 0; i < c.tools.size(); i++) {
      TEST_ASSERT_TRUE(c.tools[i].name == cb.tools[i].name && c.tools[i].description == cb.tools[i].description &&
                       c.tools[i].schemaJson == cb.tools[i].schemaJson);
    }
    // With nothing behind more there is no index paragraph; everything above it is the same.
    const size_t idxAt = c.sys.core.find(std::string("\n\n") + agent::plain::texts::core::kToolsIndex);
    TEST_ASSERT_TRUE(idxAt != std::string::npos);
    TEST_ASSERT_TRUE(cb.sys.core == c.sys.core.substr(0, idxAt));
    // A binding that moves an array tool into a group shows it there, never twice.
    Rig moved(shortHost);
    moved.reg.setClass("agent_start", ToolClass{"agents", Taint::Internal, EffectClass::Effect});
    auto m = lumiState(moved, Who::Owner);
    const Composed cm = compose(m->facts);
    TEST_ASSERT_EQUAL(13, (int)cm.tools.size());
    for (const plain::ToolSpec& t : cm.tools) TEST_ASSERT_TRUE(t.name != "agent_start");
    TEST_ASSERT_TRUE(hasWord(toolsIndex(moved.reg, m->who), "agent_start"));
  }
  // An unregistered tool is never advertised.
  Rig rig(false);
  TEST_ASSERT_TRUE(rig.reg.remove("send"));
  auto s = lumiState(rig, Who::Owner);
  const Composed c = compose(s->facts);
  TEST_ASSERT_EQUAL(13, (int)c.tools.size());
  for (const plain::ToolSpec& t : c.tools) TEST_ASSERT_NOT_EQUAL(0, (int)(t.name != "send"));
}

// ---- no use case in any model-facing text (ruling 41) ----------------------------------

namespace {
// The domain words of the lint (tools/check_no_domain_words.py, RB-REC1) and their close
// kin, as whole tokens of letters, any case: a tool, a schema, an index line or a core
// sentence that names one of them gives a use case built-in support.
const char* const kDomainWords[] = {"food",  "foods",     "meal",   "meals",  "kcal",     "calorie",
                                    "calories", "protein", "macro", "macros", "nutrition", "kj",
                                    "ledger", "diet",     "carb",   "carbs",  "fibre",    "fiber",
                                    "portion", "portions", "eaten",  "intake"};
std::string domainWordIn(const std::string& text) {
  std::string tok;
  for (size_t i = 0; i <= text.size(); i++) {
    const char ch = i < text.size() ? text[i] : ' ';
    if (std::isalpha((unsigned char)ch)) {
      tok += (char)std::tolower((unsigned char)ch);
      continue;
    }
    for (const char* w : kDomainWords)
      if (tok == w) return tok;
    tok.clear();
  }
  return "";
}
}  // namespace

static void test_no_domain_word_reaches_a_model() {
  // Counter-test: the matcher sees a seeded word as a token, in any case, and only as one.
  TEST_ASSERT_EQUAL_STRING("kcal", domainWordIn("Totals in KCAL come from the device.").c_str());
  TEST_ASSERT_EQUAL_STRING("ledger", domainWordIn("ledger_write(ops)").c_str());
  TEST_ASSERT_EQUAL_STRING("", domainWordIn("themeAlertHue centsPerKCalls TickCallback mealtimes").c_str());
  // Every literal a model can be shown, whether or not this build composes it.
  const char* core[] = {agent::plain::texts::core::kIdentityOwner, agent::plain::texts::core::kIdentityMember,
                        agent::plain::texts::core::kReplyForm,     agent::plain::texts::core::kTimeZone,
                        agent::plain::texts::core::kStamped,       agent::plain::texts::core::kHowToWork,
                        agent::plain::texts::core::kNoLater,       agent::plain::texts::core::kOneQuestion,
                        agent::plain::texts::core::kStubs,         agent::plain::texts::core::kOlderDays,
                        agent::plain::texts::core::kDeviceLines,   agent::plain::texts::core::kOutside,
                        agent::plain::texts::core::kRecall,        agent::plain::texts::core::kModel,
                        agent::plain::texts::core::kToolsIndex,    agent::plain::texts::core::kSkills,
                        agent::plain::texts::core::kSkillsMore,    agent::plain::texts::core::kNotes,
                        agent::plain::texts::core::kDigests,       agent::plain::texts::core::kRecentFiles};
  for (const char* t : core) TEST_ASSERT_EQUAL_STRING_MESSAGE("", domainWordIn(t).c_str(), t);
  for (const td::ToolDecl* list : {td::kTools, td::kBehindMore}) {
    const size_t n = list == td::kTools ? td::kToolCount : sizeof(td::kBehindMore) / sizeof(td::kBehindMore[0]);
    for (size_t i = 0; i < n; i++) {
      const std::string all = std::string(list[i].name) + " " + list[i].description + " " +
                              plain::declSchema(list[i], false);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", domainWordIn(all).c_str(), list[i].name);
    }
  }
  // The groups behind more and every table name.
  size_t gc = 0;
  const ToolGroup* g = toolGroupTable(gc);
  for (size_t i = 0; i < gc; i++) {
    const std::string line = std::string(g[i].name) + " " + g[i].about;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", domainWordIn(line).c_str(), g[i].name);
  }
  size_t rc = 0;
  const ToolClassRow* rows = toolClassTable(rc);
  for (size_t i = 0; i < rc; i++) {
    if (std::strchr(rows[i].name, '.')) continue;   // an engine-1 name (RB-REC1 removes those)
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", domainWordIn(rows[i].name).c_str(), rows[i].name);
  }
  // What every caller is shown: the composed blocks (the fixture's notes and digests are
  // synthetic), the array, the tools index and each group's listing.
  forEveryConfig([&](Rig& rig, Who w, const std::string& at) {
    auto s = lumiState(rig, w);
    const Composed c = compose(s->facts);
    std::string all = c.sys.core + "\n" + c.sys.notes + "\n" + c.sys.digests;
    for (const plain::ToolSpec& t : c.tools) all += "\n" + t.name + " " + t.description + " " + t.schemaJson;
    all += "\n" + toolsIndex(rig.reg, s->who);
    for (size_t i = 0; i < gc; i++) all += "\n" + groupListing(rig.reg, g[i].name, s->who);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", domainWordIn(all).c_str(), at.c_str());
  });
}

// Every catalogued text engine 2 can show, not only what composition reads: the whole e2
// catalog (tool results and errors, device notes, stubs, probe, summarizer) and the e1 rows
// of the sources engine 2 shares (the truth check's probe, facts item and receipt line; the
// groups behind more). The goldens are fresh by their own gate (gen_model_text_catalog.py).
// The truth probe's prompt is held byte for byte to tests/evals/probe/prompt.txt (ruling 9);
// its neutral wording was measured by RB-REC6, so no phrase is deferred.
namespace {
bool readAll(const std::string& path, std::string& out);   // below, with the golden test
}  // namespace

static void test_no_domain_word_in_any_catalogued_text() {
  const std::string shared[] = {"lib/core/src/orch_truth.cpp", "lib/core/include/nimbus/orch/truth.h",
                                "lib/core/src/orch_tool_groups.cpp"};
  int scanned = 0;
  for (const char* golden : {"test/golden/model_text_catalog.txt", "test/golden/model_text_catalog_e1.txt"}) {
    std::string all;
    TEST_ASSERT_TRUE_MESSAGE(readAll(golden, all), golden);
    const bool e1 = std::strstr(golden, "_e1") != nullptr;
    size_t at = 0;
    while (at < all.size()) {
      size_t end = all.find('\n', at);
      if (end == std::string::npos) end = all.size();
      const std::string line = all.substr(at, end - at);
      at = end + 1;
      if (line.empty() || line[0] == '#') continue;
      std::vector<std::string> f;
      for (size_t a = 0, t; f.size() < 4 && (t = line.find('\t', a)) != std::string::npos; a = t + 1) {
        f.push_back(line.substr(a, t - a));
        if (f.size() == 4) f.push_back(line.substr(t + 1));
      }
      TEST_ASSERT_EQUAL_MESSAGE(5, (int)f.size(), line.c_str());
      if (e1 && std::find(std::begin(shared), std::end(shared), f[1]) == std::end(shared)) continue;
      JsonDocument d;
      TEST_ASSERT_FALSE_MESSAGE(deserializeJson(d, f[4]), line.c_str());
      std::string text = d.as<std::string>();
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", domainWordIn(text).c_str(), (f[1] + ": " + text.substr(0, 80)).c_str());
      scanned++;
    }
  }
  TEST_ASSERT_TRUE(scanned > 250);
  TEST_MESSAGE(("property: " + std::to_string(scanned) + " catalogued texts engine 2 can show, no domain word").c_str());
}

// ---- a refused allocation (ruling 30) ------------------------------------------------------

namespace {
// The refusing allocator: while armed, the Nth allocation from now and every one after it
// is refused (std::bad_alloc, as the host's operator new does when memory runs out).
long g_allocsLeft = -1;   // -1: not armed
}  // namespace

void* operator new(std::size_t n) {
  if (g_allocsLeft == 0) throw std::bad_alloc();
  if (g_allocsLeft > 0) g_allocsLeft--;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

// Composition refused at its first, second, ... allocation, until it needs no more: every
// refusal answers TooLarge with every output empty (no partial system text, no partial
// tools array), and the run that completes gives the unconstrained bytes.
static void test_a_refused_allocation_leaves_no_partial_request() {
  forEveryConfig([&](Rig& rig, Who w, const std::string& at) {
    auto s = lumiState(rig, w);
    const Composed want = compose(s->facts);
    int refusedSystem = 0, refusedTools = 0;
    for (long k = 0;; k++) {
      TEST_ASSERT_TRUE_MESSAGE(k < 200000, at.c_str());
      plain::SystemBlocks b;
      b.core = b.notes = b.digests = "stale";
      g_allocsLeft = k;
      const plain::Status st = plain::composeSystem(s->facts, b);
      g_allocsLeft = -1;
      if (st.ok()) {
        TEST_ASSERT_TRUE_MESSAGE(b.core == want.sys.core && b.notes == want.sys.notes && b.digests == want.sys.digests,
                                 at.c_str());
        break;
      }
      refusedSystem++;
      TEST_ASSERT_TRUE_MESSAGE(st.err == plain::Err::TooLarge, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(b.core.empty() && b.notes.empty() && b.digests.empty(), at.c_str());
    }
    for (long k = 0;; k++) {
      TEST_ASSERT_TRUE_MESSAGE(k < 200000, at.c_str());
      std::vector<plain::ToolSpec> t(3);
      g_allocsLeft = k;
      const plain::Status st = plain::toolManifest(s->facts, t);
      g_allocsLeft = -1;
      if (st.ok()) {
        TEST_ASSERT_EQUAL_MESSAGE((int)want.tools.size(), (int)t.size(), at.c_str());
        for (size_t i = 0; i < t.size(); i++) TEST_ASSERT_TRUE(t[i].schemaJson == want.tools[i].schemaJson);
        break;
      }
      refusedTools++;
      TEST_ASSERT_TRUE_MESSAGE(st.err == plain::Err::TooLarge, at.c_str());
      TEST_ASSERT_TRUE_MESSAGE(t.empty(), at.c_str());
    }
    TEST_ASSERT_TRUE_MESSAGE(refusedSystem > 50 && refusedTools > 10, at.c_str());
  });
}

}  // namespace

// ---- clipping ------------------------------------------------------------------------

static void test_notes_and_skills_clip_at_whole_lines() {
  Rig rig(false);
  auto s = lumiState(rig, Who::Owner);
  s->facts.pinnedNotes = notesAtCap(9000, 7);
  const Composed c = compose(s->facts);
  TEST_ASSERT_TRUE(systemBytes(c.sys) <= plain::kSystemBytesCeiling);
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
  Rig rig(false);
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

// Policy, gates and the class table decide the array exactly as dispatch does; a refused
// `more` takes the tools index and every sentence that names it along.
static void test_policy_gates_and_pack_decide_the_array() {
  Rig rig(false);
  rig.reg.setPolicy("send", ToolRegistry::Verdict::deny("off"));
  rig.reg.setGate("web_search", [] { return false; }, "no key");
  auto s = lumiState(rig, Who::Owner);
  Composed c = compose(s->facts);
  for (const plain::ToolSpec& t : c.tools) {
    TEST_ASSERT_TRUE(t.name != "send");
    TEST_ASSERT_TRUE(t.name != "web_search");
  }
  TEST_ASSERT_EQUAL(12, (int)c.tools.size());
  rig.reg.setGate("more", [] { return false; }, "off");
  c = compose(s->facts);
  TEST_ASSERT_TRUE(c.tools.back().name != "more");
  TEST_ASSERT_FALSE(hasWord(c.sys.core, "more"));
  TEST_ASSERT_FALSE(hasWord(c.sys.notes, "more"));
  TEST_ASSERT_TRUE(c.sys.notes.find("\n+") != std::string::npos);   // the count stays, without the hint
  std::string all = c.sys.core + "\n" + c.sys.notes;
  TEST_ASSERT_EQUAL(0, (int)plain::uncallableNames(all, c.tools, rig.reg, s->who).size());
  TEST_ASSERT_EQUAL_STRING("agent_list", plain::uncallableNames("agent_list", c.tools, rig.reg, s->who).at(0).c_str());
  // The owner's identity needs the owner's Principal, whatever the flag says.
  Rig r(false);
  auto m = lumiState(r, Who::Member);
  m->facts.admin = true;
  TEST_ASSERT_TRUE(compose(m->facts).sys.core.rfind(agent::plain::texts::core::kIdentityMember, 0) == 0);
}

// Device-supplied text is normalised: whole UTF-8 characters, no blank or whitespace-only
// lines, no CR or Unicode line break, one oversized note skipped and the rest kept.
static void test_device_text_is_normalised() {
  Rig rig(false);
  auto s = lumiState(rig, Who::Owner);
  std::string tz;
  while (tz.size() < plain::kTimeZoneMax + 10) tz += "\xC3\xBC";   // u-umlaut, 2 bytes each
  s->facts.timeZone = "A" + tz;                                     // the clip falls mid-character
  s->turn.model = std::string(plain::kModelNameMax - 1, 'm') + "\xE2\x82\xAC";   // the euro sign, cut
  s->facts.pinnedNotes = "a: one\r\n   \n\t\nb: " + std::string(5000, 'x') + "\nc: three\xE2\x80\xA8" "d: four\n";
  s->facts.skillsIndex = "skills/a.md: a\r\n \n";
  s->facts.digestBlock = "2026-11-16: one\r\n\n\n\x01" "2026-11-17: two\xC2\x85x\n";
  s->facts.recentFiles = "\n\nnotes/a.md 2026-11-17 13:02\r\n";
  const Composed c = compose(s->facts);
  for (const std::string* b : {&c.sys.core, &c.sys.notes, &c.sys.digests}) {
    TEST_ASSERT_TRUE(validUtf8(*b));
    TEST_ASSERT_TRUE(b->find('\r') == std::string::npos);
    TEST_ASSERT_TRUE(b->find("\xE2\x80\xA8") == std::string::npos);
    TEST_ASSERT_TRUE(b->find("\xC2\x85") == std::string::npos);
    TEST_ASSERT_TRUE(b->find("\n \n") == std::string::npos);
  }
  TEST_ASSERT_TRUE(c.sys.notes.find("a: one\nc: three d: four") != std::string::npos);
  TEST_ASSERT_TRUE(c.sys.notes.find("xxxxx") == std::string::npos);
  TEST_ASSERT_TRUE(c.sys.digests.find("2026-11-16: one\n 2026-11-17: two x") != std::string::npos);
  TEST_ASSERT_TRUE(c.sys.digests.find("\n\n\n") == std::string::npos);
  TEST_ASSERT_TRUE(c.sys.digests.find(std::string(agent::plain::texts::core::kRecentFiles) + "\nnotes/a.md") != std::string::npos);
}

// Each fact moves only its own block (the provider's cache prefix: tools, core, notes,
// digests): a digests change leaves tools, core and notes; a notes or skills change
// leaves tools, core and digests.
static void test_each_fact_moves_only_its_block() {
  Rig rig(false);
  auto a = lumiState(rig, Who::Owner), b = lumiState(rig, Who::Owner), c = lumiState(rig, Who::Owner);
  b->facts.digestBlock += "2026-11-18: one more day\n";
  b->facts.recentFiles += "notes/new.md 2026-11-18 09:00\n";
  c->facts.pinnedNotes = "new-fact: a value\n" + c->facts.pinnedNotes.substr(0, 3000);
  c->facts.skillsIndex = "skills/new.md: new\n" + c->facts.skillsIndex;
  const Composed x = compose(a->facts), y = compose(b->facts), z = compose(c->facts);
  auto sameTools = [](const Composed& p, const Composed& q) {
    if (p.tools.size() != q.tools.size()) return false;
    for (size_t i = 0; i < p.tools.size(); i++)
      if (p.tools[i].name != q.tools[i].name || p.tools[i].description != q.tools[i].description ||
          p.tools[i].schemaJson != q.tools[i].schemaJson)
        return false;
    return true;
  };
  TEST_ASSERT_TRUE(sameTools(x, y) && sameTools(x, z));
  TEST_ASSERT_TRUE(x.sys.core == y.sys.core && x.sys.notes == y.sys.notes && x.sys.digests != y.sys.digests);
  TEST_ASSERT_TRUE(x.sys.core == z.sys.core && x.sys.digests == z.sys.digests && x.sys.notes != z.sys.notes);
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
    const bool shortHost = rng() % 2;
    const Who w = kAllWho[rng() % 3];
    Rig a(shortHost), b(shortHost);
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
  Rig rig(false);
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

static void test_golden_records_every_role_and_host() {
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
                  "measure role=%s host=%s core=%zu index=%zu notes=%zu system=%zu digests=%zu tools=%zu "
                  "tools_bytes=%zu longest_description=%zu\n",
                  whoSlug(w), rig.shortHost ? "60s" : "120s", c.sys.core.size(),
                  toolsIndex(rig.reg, s->who).size(), c.sys.notes.size(), systemBytes(c.sys),
                  c.sys.digests.size(), c.tools.size(), plain::toolsBytes(c.tools), longest);
    g += line;
  });
  Rig rig(false);
  auto s = lumiState(rig, Who::Owner);
  const Composed c = compose(s->facts);
  g += "== core (owner, 120s, claude-sonnet-5-5, Europe/London)\n" + c.sys.core + "\n";
  g += "== notes (first 400 B)\n" + c.sys.notes.substr(0, 400) + "\n";
  g += "== tools\n";
  for (const plain::ToolSpec& t : c.tools) {
    char line[64];
    std::snprintf(line, sizeof(line), "%zu %zu ", t.description.size(), t.schemaJson.size());
    g += line + t.name + ": " + t.description + "\n  " + t.schemaJson + "\n";
  }
  if (const char* dump = std::getenv("PLAIN_REQUEST_DUMP")) {   // the provider acceptance check reads it
    JsonDocument d;
    {
      Rig r(false);
      auto st = lumiState(r, Who::Owner);
      const Composed x = compose(st->facts);
      ArduinoJson::JsonObject o = d["owner"].to<ArduinoJson::JsonObject>();
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
  RUN_TEST(test_every_role_and_host_fits_the_ceilings);
  RUN_TEST(test_every_declaration_fits);
  RUN_TEST(test_every_tool_like_name_is_callable);
  RUN_TEST(test_the_checker_catches_an_uncallable_name);
  RUN_TEST(test_registry_and_manifest_agree);
  RUN_TEST(test_one_fixed_pack_in_a_fixed_order);
  RUN_TEST(test_no_domain_word_reaches_a_model);
  RUN_TEST(test_no_domain_word_in_any_catalogued_text);
  RUN_TEST(test_a_refused_allocation_leaves_no_partial_request);
  RUN_TEST(test_notes_and_skills_clip_at_whole_lines);
  RUN_TEST(test_policy_gates_and_pack_decide_the_array);
  RUN_TEST(test_device_text_is_normalised);
  RUN_TEST(test_each_fact_moves_only_its_block);
  RUN_TEST(test_empty_facts_are_refused);
  RUN_TEST(test_tools_bytes_is_the_rendered_array);
  RUN_TEST(test_same_facts_same_bytes);
  RUN_TEST(test_a_clock_in_the_core_would_be_caught);
  RUN_TEST(test_golden_records_every_role_and_host);
  return UNITY_END();
}
