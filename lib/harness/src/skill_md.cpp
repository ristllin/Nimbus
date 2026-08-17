#include "nimbus/harness/skill_md.h"

namespace agent {

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

}  // namespace

SkillMd parseSkillMd(const std::string& md) {
  SkillMd out;
  // Front matter requires the file to OPEN with a '---' line (tolerate a BOM-less
  // leading "---\n" or "---\r\n" only - a mid-file '---' is a markdown rule).
  size_t p = 0;
  bool opens = md.compare(0, 4, "---\n") == 0 || md.compare(0, 5, "---\r\n") == 0;
  if (!opens) {
    out.body = md;
    return out;
  }
  p = md.find('\n') + 1;   // past the opening delimiter line

  // Walk lines until the closing '---' line; collect keys.
  while (p < md.size()) {
    size_t eol = md.find('\n', p);
    if (eol == std::string::npos) eol = md.size();
    std::string line = md.substr(p, eol - p);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string probe = line;
    trim(probe);
    if (probe == "---") {   // closing delimiter -> body is the rest
      out.hadFrontMatter = true;
      size_t bodyStart = (eol == md.size()) ? md.size() : eol + 1;
      out.body = md.substr(bodyStart);
      // Drop leading blank lines so "[SKILL: x]\n\n\nbody" never happens.
      size_t nb = out.body.find_first_not_of("\r\n");
      out.body = (nb == std::string::npos) ? std::string() : out.body.substr(nb);
      // Normalize inject to a known value; anything else falls back to spawn.
      if (out.inject != "spawn" && out.inject != "context" && out.inject != "both")
        out.inject = "spawn";
      return out;
    }
    std::string key, val;
    if (splitKv(line, key, val)) {
      if (key == "title")      out.title = val;
      if (key == "desc")       out.desc = val;
      if (key == "version")    out.version = val;
      if (key == "inject")     out.inject = val;
      if (key == "created_by") out.createdBy = (val == "agent") ? "agent" : "user";
      if (key == "created_at") out.createdAt = val;
      if (key == "approved")   out.approved = !(val == "false" || val == "0" || val == "no");
    }
    p = (eol == md.size()) ? md.size() : eol + 1;
  }

  // Opened like front matter but never closed: treat the WHOLE file as body
  // (an owner mid-edit must degrade to something visible, not vanish).
  out = SkillMd{};
  out.body = md;
  return out;
}

std::string composeSkillMd(const SkillMd& s) {
  std::string out = "---\n";
  if (!s.title.empty())   out += "title: " + s.title + "\n";
  if (!s.desc.empty())    out += "desc: " + s.desc + "\n";
  if (!s.version.empty()) out += "version: " + s.version + "\n";
  if (s.inject != "spawn") out += "inject: " + s.inject + "\n";
  out += "created_by: " + std::string(s.createdBy == "agent" ? "agent" : "user") + "\n";
  if (!s.createdAt.empty()) out += "created_at: " + s.createdAt + "\n";
  if (!s.approved) out += "approved: false\n";   // approved is the default - omit
  out += "---\n";
  out += s.body;
  return out;
}

std::string composeSkillInjection(const std::string& skillId, const std::string& capsule,
                                  const std::string& task) {
  if (capsule.empty()) return task;
  std::string cap = capsule;
  if (cap.size() > kSkillInjectCap) {
    cap.resize(kSkillInjectCap);
    cap += "\n[skill capsule truncated at 4096 bytes]";
  }
  return "[SKILL: " + skillId + "]\n" + cap + "\n\n---\n" + task;
}

std::string skillsIndexText(const std::vector<SkillIndexEntry>& entries) {
  if (entries.empty()) return "";
  std::string out =
      "[SKILLS] Playbooks for tasks this device has a known-good way to do. "
      "BEFORE starting (or refusing) a task one of these covers, read it: "
      "skill.get {id}.\n";
  size_t named = 0;
  for (const auto& e : entries) {
    if (e.pending) continue;   // unapproved agent capsules stay out of the index
    std::string desc = e.desc.empty() ? e.id : e.desc;
    if (desc.size() > kSkillIndexDescCap) {
      desc.resize(kSkillIndexDescCap);
      // ⚠ prism: a spaceless >cap desc (CJK text has NO spaces) used to keep a
      // mid-UTF-8 cut - invalid bytes in the [SKILLS] block 400 EVERY turn's
      // request until the skill is edited. Strip a straddled sequence first
      // (continuation bytes 10xxxxxx, then the orphaned lead), THEN prefer a
      // word boundary when one exists.
      while (!desc.empty() && ((unsigned char)desc.back() & 0xC0) == 0x80) desc.pop_back();
      if (!desc.empty() && (unsigned char)desc.back() >= 0xC0) desc.pop_back();
      const size_t sp = desc.find_last_of(' ');
      if (sp != std::string::npos && sp > kSkillIndexDescCap / 2) desc.resize(sp);
      desc += "...";
    }
    const std::string line = "- " + e.id + ": " + desc + "\n";
    if (out.size() + line.size() > kSkillIndexByteCap) {
      // Over budget: NAME the remainder rather than hiding it - an unnamed
      // skill is an invisible one.
      size_t remaining = 0;
      for (size_t k = (size_t)(&e - &entries[0]); k < entries.size(); k++)
        if (!entries[k].pending) remaining++;
      out += "(+" + std::to_string(remaining) + " more - skill.list)\n";
      return out;
    }
    out += line;
    named++;
  }
  return named ? out : "";
}

}  // namespace agent
