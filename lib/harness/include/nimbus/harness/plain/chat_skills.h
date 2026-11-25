#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "nimbus/harness/skill_md.h"

// plain/chat_skills.h - the owner's skills on engine 2 (lane E2-CHAT), from the SKILL.md files the
// old engine's store keeps on the card (/mem/skills/<id>/SKILL.md, parsed with skill_md: the format
// is engine-neutral; the store, its approvals and its index text stay engine 1's).
//   - Only approved skills count (an assistant-made skill waits for the owner on engine 1).
//   - inject: context (or both): the skill rides every request in the pinned block, after the
//     notes, as the line "skills/<id>.md:" and the body's lines. ChatBinding checks that it fits
//     the block whole; one that does not is left out of the block and listed in the index instead
//     (the device tells the owner once: never cut silently).
//   - Every other approved skill is one index line "skills/<id>.md: <when to use>" (PRD section 4),
//     read with read_file when the model needs it (FileToolsIo::skillBody: skillBody() below).
// The two forms the model reads (the block's first line and the index line) are PC-0179
// (coverage/PROMPT-CHANGES.md). Header only, host-tested in test_plain_chat_binding.
namespace agent {
namespace plain {

struct SkillFile {
  std::string id;   // the folder name
  std::string md;   // SKILL.md as stored
};

struct ContextSkill {
  std::string id;
  std::string block;       // "skills/<id>.md:" and the body's lines, as the pinned block shows them
  std::string indexLine;   // its index line, for when it does not fit
};

struct ChatSkills {
  std::string index;                   // one line per skill read on demand
  std::vector<ContextSkill> context;   // in id order
};

// The skill store's id rule (SkillStore::validId): letters, digits, '-' and '_', at most 23.
inline bool chatSkillIdOk(const std::string& id) {
  if (id.empty() || id.size() > 23) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// The lines of `text` as the request's notes section keeps them (plain_request.cpp linesOf):
// a control character reads as a space, a line holding only spaces is left out.
inline std::string pinnedLines(const std::string& text) {
  std::string out, line;
  auto flush = [&] {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    for (char& ch : line) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if (c < 0x20 || c == 0x7F) ch = ' ';
    }
    if (line.find_first_not_of(' ') != std::string::npos) out += (out.empty() ? "" : "\n") + line;
    line.clear();
  };
  for (char ch : text) {
    if (ch == '\n') flush();
    else line += ch;
  }
  flush();
  return out;
}

inline std::string skillPath(const std::string& id) { return "skills/" + id + ".md"; }

inline ChatSkills chatSkillsOf(std::vector<SkillFile> files) {
  std::sort(files.begin(), files.end(), [](const SkillFile& a, const SkillFile& b) { return a.id < b.id; });
  ChatSkills out;
  for (const SkillFile& f : files) {
    if (!chatSkillIdOk(f.id)) continue;
    const SkillMd s = parseSkillMd(f.md);
    if (!s.approved) continue;
    std::string when = pinnedLines(s.desc.empty() ? (s.title.empty() ? f.id : s.title) : s.desc);
    std::replace(when.begin(), when.end(), '\n', ' ');
    const std::string line = skillPath(f.id) + ": " + skillUtf8Prefix(when, kSkillIndexDescCap);
    const std::string body = pinnedLines(s.body);
    if ((s.inject == "context" || s.inject == "both") && !body.empty()) {
      out.context.push_back(ContextSkill{f.id, skillPath(f.id) + ":\n" + body, line});
      continue;
    }
    out.index += (out.index.empty() ? "" : "\n") + line;
  }
  return out;
}

// The approved body of skill `id` among `files` (read_file on skills/<id>.md); false when none.
inline bool skillBody(const std::vector<SkillFile>& files, const std::string& id, std::string& out) {
  for (const SkillFile& f : files) {
    if (f.id != id || !chatSkillIdOk(id)) continue;
    const SkillMd s = parseSkillMd(f.md);
    if (!s.approved) return false;
    out = s.body;
    return true;
  }
  return false;
}

// The context skills the owner was told are too large for the pinned block, one "<id>@<bytes>" line
// each, kept on flash by the device so a restart does not tell him again (lane E2-BFIX4, bench 24 Nov:
// the notice went out once per boot, and the bench's console resets the board). A skill whose size
// changed is told again. True when `id` at `bytes` is due: `told` then holds it, newest last, at most
// kSkillToldMax lines (the oldest go first).
inline constexpr size_t kSkillToldMax = 32;
inline bool skillNoticeDue(std::string& told, const std::string& id, size_t bytes) {
  const std::string key = id + "@" + std::to_string(bytes);
  std::vector<std::string> lines;
  size_t at = 0;
  while (at < told.size()) {
    size_t nl = told.find('\n', at);
    if (nl == std::string::npos) nl = told.size();
    const std::string l = told.substr(at, nl - at);
    if (l == key) return false;
    if (!l.empty()) lines.push_back(l);
    at = nl + 1;
  }
  lines.push_back(key);
  if (lines.size() > kSkillToldMax) lines.erase(lines.begin(), lines.end() - kSkillToldMax);
  told.clear();
  for (const std::string& l : lines) told += l + "\n";
  return true;
}

}  // namespace plain
}  // namespace agent
