// Request composition (plain/request.h, PRD section 4): the system text in its three
// cache blocks and the always-on tool manifest. Every model-facing word comes from
// plain/texts.h; this file only joins, clips and measures. A pure function of its
// facts: the same facts give the same bytes (test_plain_request).
#include "nimbus/harness/plain/request.h"

#include <set>

#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tool_registry.h"

namespace agent {
namespace plain {

namespace {

namespace core = texts::core;
namespace td = texts::tool_descriptions;
using nimbus::orch::Principal;
using nimbus::orch::ToolRegistry;

constexpr char kPara[] = "\n\n";
constexpr char kLine[] = "\n";

// Cut at most `max` bytes, never inside a UTF-8 character.
std::string clipUtf8(const std::string& s, size_t max) {
  if (s.size() <= max) return s;
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
  return s.substr(0, n);
}

// A device-supplied value shown inside a line (a time zone, a model id): one line,
// clipped, so it can never open a paragraph of its own.
std::string oneLine(const std::string& s, size_t max) {
  std::string out = clipUtf8(s, max);
  for (char& c : out)
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  return out;
}

// The non-empty lines of a block, control characters inside a line made spaces.
std::vector<std::string> linesOf(const std::string& block) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= block.size()) {
    size_t end = block.find('\n', at);
    if (end == std::string::npos) end = block.size();
    std::string line = block.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) out.push_back(oneLine(line, line.size()));
    at = end + 1;
  }
  return out;
}

void addPara(std::string& out, const std::string& para) {
  if (para.empty()) return;
  if (!out.empty()) out += kPara;
  out += para;
}

// Listed and callable for this caller: the same test RB-N4's index and listing use
// (registered, not a router, gate open, no policy refusal, admin-only only for an
// admin, the access rules allow a call with no arguments).
bool reachable(const ToolRegistry& reg, const std::string& name, const Principal& who) {
  if (name.empty() || !reg.has(name) || reg.isRouter(name) || !reg.gateOpen(name)) return false;
  if (reg.isAdminOnly(name) && !who.perms().manageTenants) return false;
  if (reg.policyFor(name).kind != ToolRegistry::Verdict::Allow) return false;
  return reg.accessFor(name, ArduinoJson::JsonObjectConst(), who).kind == ToolRegistry::Verdict::Allow;
}

// `more` is a router: it is offered when registered and not refused by policy or gate.
bool offered(const ToolRegistry& reg, const std::string& name, const Principal& who) {
  if (reg.has(name) && reg.isRouter(name)) return reg.gateOpen(name) && reg.policyFor(name).kind == ToolRegistry::Verdict::Allow;
  return reachable(reg, name, who);
}

bool inPack(const char* name, bool food) {
  const std::string n = name;
  if (n == "ledger_write" || n == "ledger_day") return food;
  if (n == "agent_start" || n == "delete_file") return !food;
  return true;
}

// Sentences joined into one paragraph; a sentence naming a tool this caller cannot call
// is left out (uncallableNames), so the text never offers what the caller cannot do.
struct Para {
  const RequestFacts& f;
  const std::vector<ToolSpec>& tools;
  std::string text;
  void add(const std::string& sentence) {
    if (sentence.empty() || !uncallableNames(sentence, tools, *f.reg, *f.turn->who).empty()) return;
    if (!text.empty()) text += ' ';
    text += sentence;
  }
};

std::string coreText(const RequestFacts& f, const std::vector<ToolSpec>& tools) {
  std::string out;
  auto para = [&](std::initializer_list<std::string> sentences) {
    Para p{f, tools, std::string()};
    for (const std::string& s : sentences) p.add(s);
    addPara(out, p.text);
  };
  para({f.admin ? core::kIdentityOwner : core::kIdentityMember, core::kReplyForm});
  std::string tz;
  if (!f.timeZone.empty()) tz = core::kTimeZone + oneLine(f.timeZone, kTimeZoneMax) + ".";
  para({tz, core::kStamped});
  para({core::kHowToWork, core::kNoLater, core::kOneQuestion});
  para({core::kStubs, core::kOlderDays, core::kDeviceLines});
  para({core::kOutside});
  para({core::kRecall});
  if (f.food) para({core::kFood});
  if (!f.turn->model.empty()) para({core::kModel + oneLine(f.turn->model, kModelNameMax) + "."});
  const std::string idx = nimbus::orch::toolsIndex(*f.reg, *f.turn->who);
  if (!idx.empty()) addPara(out, std::string(core::kToolsIndex) + kLine + idx);
  return out;
}

// The skills index: whole lines newest used first while they fit kSkillsIndexCeiling
// with the header, and a true count of the rest.
std::string skillsSection(const std::string& index, const RequestFacts& f, const std::vector<ToolSpec>& tools) {
  const std::vector<std::string> lines = linesOf(index);
  auto callable = [&](const std::string& t) { return uncallableNames(t, tools, *f.reg, *f.turn->who).empty(); };
  if (lines.empty() || !callable(core::kSkills)) return std::string();
  const std::string more = callable(core::kSkillsMore) ? core::kSkillsMore : "";
  std::string out = core::kSkills;
  size_t shown = 0;
  for (const std::string& l : lines) {
    const size_t left = lines.size() - shown - 1;
    const std::string tail = left ? std::string(kLine) + "+" + std::to_string(left) + more : "";
    if (out.size() + 1 + l.size() + tail.size() > kSkillsIndexCeiling) break;
    out += kLine + l;
    shown++;
  }
  if (shown < lines.size()) out += std::string(kLine) + "+" + std::to_string(lines.size() - shown) + more;
  return out;
}

// Pinned notes: whole lines in order while the notes stay within `budget` bytes.
std::string notesSection(const std::string& notes, size_t budget) {
  std::string body;
  for (const std::string& l : linesOf(notes)) {
    if (body.size() + (body.empty() ? 0 : 1) + l.size() > budget) break;
    body += (body.empty() ? "" : kLine) + l;
  }
  return body.empty() ? body : std::string(core::kNotes) + kLine + body;
}

bool validFacts(const RequestFacts& f) { return f.turn && f.turn->who && f.reg; }

void jsonString(std::string& out, const std::string& s) {
  static const char kHex[] = "0123456789abcdef";
  out += '"';
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += ch;
    } else if (c < 0x20) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    } else {
      out += ch;
    }
  }
  out += '"';
}

// Words are scanned with capitals so "Search" is one word; a tool name has none.
bool wordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
bool lowerName(const std::string& w) {
  for (char c : w)
    if (c >= 'A' && c <= 'Z') return false;
  return !w.empty() && w[0] >= 'a' && w[0] <= 'z';
}

}  // namespace

Status composeSystem(const RequestFacts& facts, SystemBlocks& out) {
  out = SystemBlocks{};
  if (!validFacts(facts)) return Status::fail(Err::InvalidArgument, "plain::composeSystem");
  std::vector<ToolSpec> tools;
  const Status st = toolManifest(facts, tools);
  if (!st.ok()) return Status::fail(st.err, "plain::composeSystem");
  const std::string coreBlock = coreText(facts, tools);
  if (coreBlock.size() > kCoreCeiling) return Status::fail(Err::TooLarge, "plain::composeSystem");

  const std::string skills = skillsSection(facts.skillsIndex, facts, tools);
  // What is left of the system ceiling for the notes' lines, after the core, the skills
  // index, the notes header and the joins between them.
  size_t used = coreBlock.size() + skills.size() + std::string(core::kNotes).size() + 1;
  if (!skills.empty()) used += 2;
  const size_t room = used < kSystemBytesCeiling ? kSystemBytesCeiling - used : 0;
  const std::string notes = notesSection(facts.pinnedNotes, room < kPinnedNotesCeiling ? room : kPinnedNotesCeiling);

  out.core = coreBlock;
  addPara(out.notes, skills);
  addPara(out.notes, notes);
  if (!facts.digestBlock.empty()) addPara(out.digests, std::string(core::kDigests) + kLine + facts.digestBlock);
  if (!facts.recentFiles.empty()) addPara(out.digests, std::string(core::kRecentFiles) + kLine + facts.recentFiles);
  return Status::success();
}

Status toolManifest(const RequestFacts& facts, std::vector<ToolSpec>& out) {
  out.clear();
  if (!validFacts(facts)) return Status::fail(Err::InvalidArgument, "plain::toolManifest");
  for (const td::ToolDecl& d : td::kTools) {
    if (!inPack(d.name, facts.food) || !offered(*facts.reg, d.name, *facts.turn->who)) continue;
    out.push_back(ToolSpec{d.name, d.description, declSchema(d, facts.shortHost)});
  }
  if (out.size() > kMaxAlwaysOnTools || toolsBytes(out) > kToolsBytesCeiling) {
    out.clear();
    return Status::fail(Err::TooLarge, "plain::toolManifest");
  }
  return Status::success();
}

const td::ToolDecl* toolDecl(const std::string& name) {
  for (const td::ToolDecl& d : td::kTools)
    if (name == d.name) return &d;
  return nullptr;
}

std::string declSchema(const td::ToolDecl& d, bool shortHost) {
  std::string s = d.schema;
  if (d.schemaTail) s += std::to_string(shortHost ? kWriteContentMaxShort : kWriteContentMax) + d.schemaTail;
  return s;
}

void applyToolPack(ToolRegistry& reg, bool food) {
  for (const char* name : {"agent_start", "delete_file"}) {
    nimbus::orch::ToolClass c = nimbus::orch::toolClassFor(name);
    if (!food) c.group = "";
    reg.setClass(name, c);
  }
}

size_t toolsBytes(const std::vector<ToolSpec>& tools) {
  std::string out = "[";
  for (size_t i = 0; i < tools.size(); i++) {
    if (i) out += ',';
    out += "{\"name\":";
    jsonString(out, tools[i].name);
    out += ",\"description\":";
    jsonString(out, tools[i].description);
    out += ",\"input_schema\":";
    out += tools[i].schemaJson;
    out += '}';
  }
  out += ']';
  return out.size();
}

namespace {

// Every name a tool goes by for this caller: the class table (planned and old names) and
// whatever the registry holds.
std::set<std::string> knownToolNames(const ToolRegistry& reg, const Principal& who) {
  std::set<std::string> known;
  size_t n = 0;
  const nimbus::orch::ToolClassRow* rows = nimbus::orch::toolClassTable(n);
  for (size_t i = 0; i < n; i++) known.insert(nimbus::orch::spelledFor(who, rows[i].name));
  for (const ToolRegistry::Spec& s : reg.toolSpecs()) known.insert(nimbus::orch::spelledFor(who, s.name));
  return known;
}

// The next whole word at or after `at` that does not continue a word or a file
// extension; `at` moves past it. False at the end of the text.
bool nextWord(const std::string& text, size_t& at, std::string& word, bool& call) {
  while (at < text.size()) {
    const size_t i = at;
    size_t j = i;
    while (j < text.size() && wordChar(text[j])) j++;
    at = j > i ? j : i + 1;
    if (j == i || (i > 0 && text[i - 1] == '.')) continue;
    word = text.substr(i, j - i);
    call = j < text.size() && text[j] == '(';
    return true;
  }
  return false;
}

}  // namespace

std::vector<std::string> uncallableNames(const std::string& text, const std::vector<ToolSpec>& tools,
                                         const ToolRegistry& reg, const Principal& who) {
  const std::set<std::string> known = knownToolNames(reg, who);
  std::set<std::string> callable, bad;
  for (const ToolSpec& t : tools) callable.insert(t.name);
  std::vector<std::string> out;
  std::string word;
  bool call = false;
  for (size_t at = 0; nextWord(text, at, word, call);) {
    if (!lowerName(word) || (!call && !known.count(word)) || callable.count(word)) continue;
    const std::string regName = reg.resolveName(word);
    if (!reg.classOf(regName).alwaysOn() && reachable(reg, regName, who)) continue;
    if (bad.insert(word).second) out.push_back(word);
  }
  return out;
}

}  // namespace plain
}  // namespace agent
