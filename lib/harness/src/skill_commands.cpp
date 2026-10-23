// The owner's /skill sub-commands and the owner-facing words for skill changes
// (portable, host-tested). The device's handleMessage only parses and delivers.
#include <algorithm>
#include <cctype>
#include <map>

#include "nimbus/harness/builtin_skills.h"
#include "nimbus/harness/skill_store.h"

namespace agent {

const SkillCmdRow kSkillCmds[] = {
    {SkillCmd::List, "list", false},       {SkillCmd::Show, "show", true},
    {SkillCmd::Approve, "approve", true},  {SkillCmd::Deny, "deny", true},
    {SkillCmd::Delete, "delete", true},    {SkillCmd::Auto, "auto", true},
    {SkillCmd::Help, "help", false},
};
const size_t kSkillCmdCount = sizeof(kSkillCmds) / sizeof(kSkillCmds[0]);

namespace {

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  const size_t e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

const char* statusWords(const SkillMeta& m) {
  if (m.builtin()) return "built-in, in use";
  if (!m.approved) return "waiting for your approval, not in use";
  if (m.pendingEdit && m.nextStale) return "in use; an out-of-date edit is waiting";
  if (m.pendingEdit) return "in use; an edit is waiting for your approval";
  return "in use";
}

const char* authorWords(const SkillMeta& m) {
  if (m.builtin()) return "built in";
  return m.origin == "agent" ? "written by the assistant" : "written by you";
}

// How many lines an edit adds or removes (a multiset difference: enough for "12
// lines changed" without a diff engine on the device).
int linesChanged(const std::string& a, const std::string& b) {
  std::map<std::string, int> count;
  size_t p = 0;
  auto each = [&p](const std::string& s, std::string& line) {
    if (p >= s.size()) return false;
    size_t e = s.find('\n', p);
    if (e == std::string::npos) e = s.size();
    line = s.substr(p, e - p);
    p = e + 1;
    return true;
  };
  std::string line;
  for (p = 0; each(a, line);) count[line]++;
  for (p = 0; each(b, line);) count[line]--;
  int n = 0;
  for (const auto& kv : count) n += kv.second < 0 ? -kv.second : kv.second;
  return n;
}

}  // namespace

SkillCmd parseSkillCmd(const std::string& args, std::string& arg) {
  const std::string a = trimmed(args);
  const size_t sp = a.find_first_of(" \t");
  const std::string word = lower(sp == std::string::npos ? a : a.substr(0, sp));
  arg = sp == std::string::npos ? std::string() : trimmed(a.substr(sp + 1));
  for (size_t i = 0; i < kSkillCmdCount; ++i) {
    if (word != kSkillCmds[i].word) continue;
    if (kSkillCmds[i].needsId && arg.empty()) return SkillCmd::Help;
    if (kSkillCmds[i].cmd == SkillCmd::Auto) arg = lower(arg);
    return kSkillCmds[i].cmd;
  }
  arg.clear();
  return SkillCmd::Help;
}

std::string skillCmdUsage() {
  return "Usage: /skill list | show <id> | approve <id> | deny <id> | delete <id> | auto on|off";
}

std::string skillResultText(const std::string& id, const SkillResult& r) {
  if (!r.ok) return "Skill " + id + ": " + r.err;
  const std::string rev = std::to_string(r.rev);
  switch (r.outcome) {
    case SkillOutcome::Approved: return "Skill " + id + " is approved and in use.";
    case SkillOutcome::EditApplied:
      return "The edit to " + id + " is applied (revision " + rev + ") and in use.";
    case SkillOutcome::EditDiscarded:
      return "The proposed edit to " + id + " is discarded; the current version stays in use.";
    case SkillOutcome::Deleted: return "Skill " + id + " is deleted.";
    case SkillOutcome::Unchanged: return "Skill " + id + " is already approved; nothing is waiting.";
    case SkillOutcome::Created:
    case SkillOutcome::Replaced: return "Skill " + id + " is saved (revision " + rev + ").";
    default: break;
  }
  return "Skill " + id + ": done.";
}

std::string skillSelfApproveNotice(const std::string& id, int rev, bool created) {
  return std::string("The assistant ") + (created ? "added a new skill, " : "changed the skill ") +
         id + " (revision " + std::to_string(rev) +
         "), and it is in use now because you let it approve its own skill changes. "
         "Send /skill show " + id + " to read it, or /skill auto off to review changes first.";
}

std::string skillProposalNotice(const std::string& id, const SkillResult& r) {
  const std::string tail = " Send /skill show " + id + " to read it, /skill approve " + id +
                           " to use it, or /skill deny " + id + " to drop it.";
  if (r.outcome == SkillOutcome::EditProposed)
    return "The assistant proposes an edit to the skill " + id + " (revision " +
           std::to_string(r.rev - 1) + " to " + std::to_string(r.rev) +
           "). The current version stays in use until you decide." + tail;
  if (r.outcome == SkillOutcome::Replaced)
    return "The assistant updated its new skill " + id + " (revision " + std::to_string(r.rev) +
           "). It is still not in use." + tail;
  return "The assistant wrote a new skill, " + id + ". It is not in use until you approve it." +
         tail;
}

std::string skillShowText(SkillStore& st, const std::string& id) {
  SkillMeta m;
  if (!st.find(id, m)) return "No skill named " + id + ". Send /skill list to see them.";
  std::string out = id + " (revision " + std::to_string(m.rev) + ", " + statusWords(m) + ", " +
                    authorWords(m) + ")\n";
  if (!m.desc.empty()) out += "When: " + m.desc + "\n";
  const std::string body = m.approved ? st.liveBody(id, SkillSide{}) : parseSkillMd(st.raw(id)).body;
  out += "\n" + skillUtf8Prefix(body, 600) + (body.size() > 600 ? "..." : "") + "\n";
  if (m.pendingEdit) {
    const SkillMd next = parseSkillMd(st.rawNext(id));
    out += "\nProposed edit (revision " + std::to_string(m.nextRev) + "): " +
           std::to_string(linesChanged(body, next.body)) + " lines changed" +
           (m.nextStale ? "; it is out of date (the skill changed after it was proposed)" : "") +
           ".\n" + skillUtf8Prefix(next.body, 600) + (next.body.size() > 600 ? "..." : "") + "\n";
  }
  return out;
}

std::string skillListText(SkillStore& st, bool selfApprove) {
  std::string mine, builtins;
  for (const SkillMeta& m : st.list()) {
    if (m.builtin()) {
      builtins += (builtins.empty() ? "" : ", ") + m.id;
      continue;
    }
    mine += "- " + m.id + ": " + statusWords(m) + " (revision " + std::to_string(m.rev) + ", " +
            authorWords(m) + ")\n";
  }
  std::string out = mine.empty() ? std::string("No saved skills yet.\n") : "Skills:\n" + mine;
  if (!builtins.empty()) out += "Built in: " + builtins + "\n";
  out += std::string("The assistant may approve its own skill changes: ") +
         (selfApprove ? "on" : "off") + " (/skill auto on|off).";
  return out;
}

}  // namespace agent
