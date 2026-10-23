#include "nimbus/harness/skill_md.h"

#include <cstdlib>

#include "nimbus/harness/compose.h"   // loopToolName: the wire name the roster uses
#include "nimbus/mem_cap.h"           // utf8CapLen

namespace agent {

const SkillMdKey kSkillMdKeys[] = {
    {"title", true, "a short title"},
    {"desc", true,
     "REQUIRED for your saves: one line saying WHEN to read this skill (it is shown in "
     "[SKILLS] every turn)"},
    {"version", true, "optional free text"},
    {"inject", true,
     "spawn (sub-agents that name it get it), context (it rides every one of your turns "
     "once approved) or both"},
    {"created_by", false, "who wrote it (set by the device)"},
    {"created_at", false, "when it was created (set by the device)"},
    {"approved", false, "whether the owner approved it (set by the device)"},
    {"rev", false, "its revision number (set by the device)"},
    {"updated_at", false, "when it last changed (set by the device)"},
    {"base_rev", false, "the revision a proposed edit was written against (set by the device)"},
};
const size_t kSkillMdKeyCount = sizeof(kSkillMdKeys) / sizeof(kSkillMdKeys[0]);

bool isSkillMdKey(const std::string& key) {
  for (size_t i = 0; i < kSkillMdKeyCount; ++i)
    if (key == kSkillMdKeys[i].key) return true;
  return false;
}

bool validInject(const std::string& v) { return v == "spawn" || v == "context" || v == "both"; }

namespace {

void trim(std::string& s) {
  const char* ws = " \t\r\n";
  size_t b = s.find_first_not_of(ws);
  size_t e = s.find_last_not_of(ws);
  s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

// One "key: value" front-matter line -> (key, value); false if no ':'.
bool splitKv(const std::string& line, std::string& key, std::string& val) {
  size_t colon = line.find(':');
  if (colon == std::string::npos) return false;
  key = line.substr(0, colon);
  val = line.substr(colon + 1);
  trim(key);
  trim(val);
  return !key.empty();
}

int toRev(const std::string& v) {
  const long n = std::strtol(v.c_str(), nullptr, 10);
  return (n > 0 && n < 1000000) ? (int)n : 0;
}

void applyKey(SkillMd& out, const std::string& key, const std::string& val) {
  if (key == "title")           out.title = val;
  else if (key == "desc")       out.desc = val;
  else if (key == "version")    out.version = val;
  else if (key == "inject")     out.inject = out.injectRaw = val;
  else if (key == "created_by") out.createdBy = (val == "agent") ? "agent" : "user";
  else if (key == "created_at") out.createdAt = val;
  else if (key == "approved")   out.approved = !(val == "false" || val == "0" || val == "no");
  else if (key == "rev")        out.rev = toRev(val);
  else if (key == "updated_at") out.updatedAt = val;
  else if (key == "base_rev")   out.baseRev = toRev(val);
  else                          out.unknownKeys.push_back(key);
}

// The body after the closing delimiter line, leading blank lines dropped so
// "[SKILL: x]\n\n\nbody" never happens.
std::string bodyAfter(const std::string& md, size_t eol) {
  const size_t start = (eol == md.size()) ? md.size() : eol + 1;
  const size_t nb = md.find_first_not_of("\r\n", start);
  return nb == std::string::npos ? std::string() : md.substr(nb);
}

}  // namespace

SkillMd parseSkillMd(const std::string& md) {
  SkillMd out;
  // Front matter requires the file to OPEN with a '---' line (a mid-file '---' is
  // a markdown rule).
  const bool opens = md.compare(0, 4, "---\n") == 0 || md.compare(0, 5, "---\r\n") == 0;
  if (!opens) {
    out.body = md;
    return out;
  }
  size_t p = md.find('\n') + 1;   // past the opening delimiter line
  while (p < md.size()) {
    size_t eol = md.find('\n', p);
    if (eol == std::string::npos) eol = md.size();
    std::string line = md.substr(p, eol - p);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string probe = line;
    trim(probe);
    if (probe == "---") {   // closing delimiter -> body is the rest
      out.hadFrontMatter = true;
      out.body = bodyAfter(md, eol);
      if (!validInject(out.inject)) out.inject = "spawn";   // a known value, always
      return out;
    }
    std::string key, val;
    if (splitKv(line, key, val)) applyKey(out, key, val);
    p = (eol == md.size()) ? md.size() : eol + 1;
  }
  // Opened like front matter but never closed: the WHOLE file is the body (an
  // owner mid-edit must degrade to something visible, not vanish).
  out = SkillMd{};
  out.body = md;
  return out;
}

std::string composeSkillMd(const SkillMd& s) {
  std::string out = "---\n";
  if (!s.title.empty())   out += "title: " + s.title + "\n";
  if (!s.desc.empty())    out += "desc: " + s.desc + "\n";
  if (!s.version.empty()) out += "version: " + s.version + "\n";
  if (s.inject != "spawn" && validInject(s.inject)) out += "inject: " + s.inject + "\n";
  out += "created_by: " + std::string(s.createdBy == "agent" ? "agent" : "user") + "\n";
  if (!s.createdAt.empty()) out += "created_at: " + s.createdAt + "\n";
  if (!s.approved) out += "approved: false\n";   // approved is the default - omit
  if (s.rev > 0) out += "rev: " + std::to_string(s.rev) + "\n";
  if (!s.updatedAt.empty()) out += "updated_at: " + s.updatedAt + "\n";
  if (s.baseRev > 0) out += "base_rev: " + std::to_string(s.baseRev) + "\n";
  out += "---\n";
  out += s.body;
  return out;
}

std::string skillUtf8Prefix(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  const int keep = nimbus::utf8CapLen(s.data(), (int)s.size(), (int)maxBytes);
  return s.substr(0, keep > 0 ? (size_t)keep : 0);
}

std::string composeSkillInjection(const std::string& skillId, const std::string& capsule,
                                  const std::string& task) {
  if (capsule.empty()) return task;
  std::string cap = capsule;
  if (cap.size() > kSkillInjectCap)
    cap = skillUtf8Prefix(cap, kSkillInjectCap) + "\n[skill capsule truncated at 4096 bytes]";
  return "[SKILL: " + skillId + "]\n" + cap + "\n\n---\n" + task;
}

namespace {

// A description cut to the index cap: never inside a UTF-8 sequence (CJK text has
// no spaces to fall back on), at a word boundary when there is one.
std::string capDesc(std::string desc) {
  if (desc.size() <= kSkillIndexDescCap) return desc;
  desc = skillUtf8Prefix(desc, kSkillIndexDescCap);
  const size_t sp = desc.find_last_of(' ');
  if (sp != std::string::npos && sp > kSkillIndexDescCap / 2) desc.resize(sp);
  return desc + "...";
}

// "(waiting for the owner's approval, not active: a, b; edit proposed: c)": the
// model's own proposals stay visible, so it never reads a missing line as "saved
// and active" or "lost".
std::string pendingTail(const std::vector<SkillIndexEntry>& entries) {
  std::string pend, edits;
  for (const auto& e : entries) {
    if (e.pending) pend += (pend.empty() ? "" : ", ") + e.id;
    else if (e.pendingEdit) edits += (edits.empty() ? "" : ", ") + e.id;
  }
  if (pend.empty() && edits.empty()) return std::string();
  std::string out = "(waiting for the owner's approval";
  if (!pend.empty()) out += ", not active: " + pend;
  if (!edits.empty()) out += std::string(pend.empty() ? ", " : "; ") + "edit proposed, current version runs: " + edits;
  return out + ")\n";
}

}  // namespace

std::string skillsIndexText(const std::vector<SkillIndexEntry>& entries) {
  if (entries.empty()) return "";
  std::string out =
      "[SKILLS] Playbooks for tasks this device has a known-good way to do. "
      "BEFORE starting (or refusing) a task one of these covers, read it: " +
      loopToolName("skill.get") + " {id}.\n";
  const std::string tail = pendingTail(entries);
  size_t named = 0;
  for (size_t k = 0; k < entries.size(); ++k) {
    const SkillIndexEntry& e = entries[k];
    if (e.pending) continue;   // not approved: named in the tail line only
    const std::string line = "- " + e.id + ": " + capDesc(e.desc.empty() ? e.id : e.desc) + "\n";
    if (out.size() + line.size() + tail.size() > kSkillIndexByteCap) {
      // Over budget: NAME the remainder rather than hiding it.
      size_t remaining = 0;
      for (size_t r = k; r < entries.size(); r++)
        if (!entries[r].pending) remaining++;
      out += "(+" + std::to_string(remaining) + " more - " + loopToolName("skill.list") + ")\n";
      return out + tail;
    }
    out += line;
    named++;
  }
  if (!named && tail.empty()) return "";
  return out + tail;
}

}  // namespace agent
