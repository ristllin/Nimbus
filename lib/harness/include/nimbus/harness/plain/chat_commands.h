#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nimbus/harness/skill_store.h"   // the /skill table and words (lane E2-P-SKILL)
#include "nimbus/orch/command.h"
#include "nimbus/orch/notes.h"              // the held notes /prefs answers
#include "nimbus/orch/prefs_tools.h"        // prefsCommand: engine 1's /prefs
#include "nimbus/orch/tx/log.h"   // TxFiles, kTxRoot - /clear's generation file

// plain/chat_commands.h - where an owner's slash command goes on an engine-2 device (lane E2-CHAT).
// No command reaches the model. Each verb of nimbus::orch::kCommandVerbs has one route:
//   Shared      the handler engine 1 calls (it does not depend on the engine): /help, /start,
//               /update, /ota, /play, /fetch, /loops, /loop, /usage, /model, and /budget with
//               arguments (a limit change and its confirm); /remind (lane E2-SCHED: a reminder
//               made through the schedule tool's own code, as the owner's); /skill and /prefs
//               (lane E2-P-SKILL, plain/chat_skill_tools.h: engine 1's store and rules file; the
//               rules reach engine 2 through the notes, and /prefs answers the held notes); /mcp approve|deny
//               (lane E2-P-CONN1: engine 1's approval, connectors::mcp::approvalCommand);
//   Clear       a new conversation on engine 2 (the stored one is kept on the card);
//   Compact     a compaction of this chat now;
//   BudgetView  a bare /budget: engine 2's budget view (the system prompt in tokens), then the
//               spending text a bare /budget gives on engine 1;
//   NotYet      engine 1 only for now, answered with one plain line: none today (the route stays
//               for a verb engine 2 cannot answer yet);
//   Unknown     a verb the table does not know: the same "try /help" answer as engine 1.
// test_plain_chat_commands walks every verb of the table: a verb added there with no route here
// fails it.
namespace agent {
namespace plain {

enum class ChatCommandRoute : uint8_t { Shared = 0, Clear = 1, Compact = 2, BudgetView = 3, NotYet = 4, Unknown = 5 };

inline const char* chatCommandRouteSlug(ChatCommandRoute r) {
  switch (r) {
    case ChatCommandRoute::Shared: return "shared";
    case ChatCommandRoute::Clear: return "clear";
    case ChatCommandRoute::Compact: return "compact";
    case ChatCommandRoute::BudgetView: return "budget-view";
    case ChatCommandRoute::NotYet: return "not-yet";
    case ChatCommandRoute::Unknown: return "unknown";
  }
  return "unknown";
}

// The route of a parsed command (c.isCommand is the caller's check).
inline ChatCommandRoute chatCommandRoute(const nimbus::orch::Command& c) {
  const std::string& v = c.verb;
  if (!nimbus::orch::findCommandVerb(v)) return ChatCommandRoute::Unknown;
  if (v == "clear") return ChatCommandRoute::Clear;
  if (v == "compact") return ChatCommandRoute::Compact;
  if (v == "budget") return c.args.empty() ? ChatCommandRoute::BudgetView : ChatCommandRoute::Shared;
  return ChatCommandRoute::Shared;
}

// ---- lines engine 1's own would not make true on engine 2 (lane E2-FIX14) ---------------------------
// Device copy for the owner, never model text. Engine 2 binds no connectors and no sub-agents, so the
// switch "/loop grants on" is stored (it applies once they exist) and the reply says what routines can
// do now; "off" is engine 1's line, true on both.
inline const char* routineGrantsLine(bool on) {
  return on ? "Saved. Routines on the new engine have no connectors or sub-agents to use yet, so they still only "
              "read, set their follow-up and reply."
            : "Routines now only read, set their follow-up and reply.";
}
// The web settings' "Erase memory": engine 2 keeps no separate assistant memory (engine 1's running
// notes), so nothing is erased; the owner's notes and files are his and stay.
inline constexpr char kNoAssistantMemoryLine[] =
    "The new engine keeps no separate assistant memory, so nothing was erased. Your notes and files are kept.";

// ---- /clear's generation (lane E2-FIX2B, finding P7) ------------------------------------------
// A chat's conversation is the transcript of its generation: 0 is the chat id itself, n is
// "<chat>_c<n>". The number is kept in /mem/tx/<chat>.gen as one decimal line, written whole to
// <chat>.gen.tmp and renamed, so a cut leaves either number whole. Reading it has three answers: a
// value; Absent, only when /mem/tx listed whole and holds neither name (a chat never cleared:
// generation 0); Unknown otherwise (a refused read, a main file there that does not read as a
// number), which the device treats as an error for that turn, never as generation 0. Lane E2-FIX3
// (finding R2): a temp that does not read as a whole line is the leftover of a write that never
// committed (a cut inside it): it is removed (or, if the card refuses, ignored) and the main file,
// or its absence, decides. Only an unreadable main file cannot tell, and /clear still gets out of it
// (freeGeneration).
enum class GenRead : uint8_t { Value = 0, Absent = 1, Unknown = 2 };

inline std::string generationPath(const std::string& chatId) {
  return std::string(nimbus::orch::tx::kTxRoot) + "/" + chatId + ".gen";
}

// Digits, then the line end the writer puts there (`needEnd`), or digits alone (an older build's
// in-place write).
inline bool parseGeneration(const char* buf, size_t n, bool needEnd, uint32_t& g) {
  size_t digits = 0;
  uint64_t v = 0;
  while (digits < n && buf[digits] >= '0' && buf[digits] <= '9' && digits < 10) v = v * 10 + (uint64_t)(buf[digits++] - '0');
  if (!digits || v > UINT32_MAX) return false;
  const bool ended = digits + 1 == n && buf[digits] == '\n';
  if (!(ended || (!needEnd && digits == n))) return false;
  g = (uint32_t)v;
  return true;
}

inline GenRead readGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t& g) {
  g = 0;
  const std::string path = generationPath(chatId), tmp = path + ".tmp";
  char buf[16];
  size_t got = 0;
  // The temp holds a whole line only when a cut stopped its rename: then it is the newer number.
  const bool tmpRead = f.readAt(tmp, 0, buf, sizeof buf, got);
  if (tmpRead && parseGeneration(buf, got, true, g)) return GenRead::Value;
  got = 0;
  if (f.readAt(path, 0, buf, sizeof buf, got) && parseGeneration(buf, got, false, g)) return GenRead::Value;
  g = 0;
  std::vector<std::string> names;
  if (!f.list(nimbus::orch::tx::kTxRoot, names)) return GenRead::Unknown;
  const std::string base = chatId + ".gen", baseTmp = base + ".tmp";
  bool torn = false;
  for (const std::string& n : names) {
    if (n == base) return GenRead::Unknown;   // there, and not a number
    if (n == baseTmp && !tmpRead) return GenRead::Unknown;   // there, and not read: it may be whole
    torn = torn || n == baseTmp;
  }
  if (torn) (void)f.remove(tmp);   // read, not whole, never renamed: its number was never in force
  return GenRead::Absent;
}

// The number /clear moves a chat to (lane E2-FIX3, findings R2 and R4): the first n above `from`
// (the number read; 0 when it could not be read) whose transcript "<chat>_c<n>" holds no file, so
// no conversation is ever reused, also after a number that was lost or never stored. False when a
// folder cannot be told (the caller refuses the /clear and says so).
inline constexpr uint32_t kGenerationProbe = 256;
inline bool freeGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t from, uint32_t& next) {
  const uint64_t last = std::min<uint64_t>(UINT32_MAX, (uint64_t)from + kGenerationProbe);
  for (uint64_t n = (uint64_t)from + 1; n <= last; ++n) {
    std::vector<std::string> names;
    if (!f.list(std::string(nimbus::orch::tx::kTxRoot) + "/" + chatId + "_c" + std::to_string(n), names)) return false;
    if (names.empty()) {
      next = (uint32_t)n;
      return true;
    }
  }
  return false;
}

// Lane E2-FIX5 (finding L1): a whole temp is the newest number (a rename that was refused or cut,
// perhaps after the main file was already removed), so it is renamed into place before the temp is
// written again: a cut inside the new write then leaves the main file, never no number at all.
inline bool writeGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t g) {
  const std::string path = generationPath(chatId), tmp = path + ".tmp";
  char buf[16];
  size_t got = 0;
  uint32_t was = 0;
  if (f.readAt(tmp, 0, buf, sizeof buf, got) && parseGeneration(buf, got, true, was) && !f.rename(tmp, path))
    return false;
  return f.writeWhole(tmp, std::to_string(g) + "\n") && f.rename(tmp, path);
}

// /clear's write (lane E2-FIX3, finding R4): true only when the card holds `g` now. A refused
// rename can leave the whole temp, which reads as `g`: that counts. Anything else did not happen.
inline bool storeGeneration(nimbus::orch::tx::TxFiles& f, const std::string& chatId, uint32_t g) {
  if (writeGeneration(f, chatId, g)) return true;
  uint32_t back = 0;
  return readGeneration(f, chatId, back) == GenRead::Value && back == g;
}

// ---- /skill and /prefs (lane E2-P-SKILL) -----------------------------------------------------
// /skill: engine 1's sub-commands (parseSkillCmd, skillListText, skillShowText, skillResultText).
// /prefs: engine 1's command over the standing-rules file (prefsCommand); engine 2 reads the rules
// through the owner's notes, and the device copies the file into them again after every change
// (migratedev::importRulesOnce), so a rule applies to the next turn. /prefs also lists the notes held
// for approval (notes.h: a turn that read outside content, or none a person started) and answers
// them: /prefs approve note <id>, /prefs deny note <id>. Owner copy, never sent to a model.
// ---- /skill ----------------------------------------------------------------------------------
// Engine 1's /skill (orchestrator_e1.cpp), over the shared store. The caller holds the store lock.
inline std::string skillCommand(SkillStore& st, const std::string& args, bool selfApprove,
                                const std::function<bool(bool)>& setSelfApprove) {
  std::string arg;
  const SkillCmd sub = parseSkillCmd(args, arg);
  if (sub == SkillCmd::List) return skillListText(st, selfApprove);
  if (sub == SkillCmd::Show) return skillShowText(st, arg);
  if (sub == SkillCmd::Auto) {
    if (arg != "on" && arg != "off") return skillCmdUsage();
    if (!setSelfApprove || !setSelfApprove(arg == "on")) return "The switch could not be written.";
    return std::string("The assistant may approve its own skill changes: ") + arg + ".";
  }
  const SkillCaller owner = SkillCaller::ownerCall();
  if (sub == SkillCmd::Approve) return skillResultText(arg, st.approve(arg, owner));
  if (sub == SkillCmd::Deny) return skillResultText(arg, st.deny(arg, owner));
  if (sub == SkillCmd::Delete) return skillResultText(arg, st.remove(arg, owner));
  return skillCmdUsage();
}

// ---- the held notes --------------------------------------------------------------------------
// One held call as the owner reads it: "p1: set key [rule]: value (after outside content)".
inline std::string heldNoteLine(const nimbus::orch::notes::PendingNote& p) {
  std::string what = p.remove ? "remove " + p.note.key : "set " + nimbus::orch::notes::formatLine(p.note);
  return p.id + ": " + what + (p.why == "outside" ? " (after outside content)" : " (from a routine)");
}

// The owner's held notes as a short list; "" with none.
inline std::string heldNotesText(nimbus::orch::notes::Notebook& nb, const nimbus::orch::Principal& owner) {
  std::vector<nimbus::orch::notes::PendingNote> held;
  if (!nb.pending(owner, held) || held.empty()) return std::string();
  std::string out = "Notes waiting for your approval:";
  for (const auto& p : held) out += "\n" + heldNoteLine(p);
  return out + "\nApprove with /prefs approve note <id>, or deny with /prefs deny note <id>.";
}

// "approve note p1" or "deny note p1": the owner's answer. False: `args` is not one.
inline bool heldNoteAnswer(nimbus::orch::notes::Notebook& nb, const nimbus::orch::Principal& owner,
                           const std::string& args, std::string& reply) {
  const bool approve = args.rfind("approve note ", 0) == 0;
  if (!approve && args.rfind("deny note ", 0) != 0) return false;
  std::string id = args.substr(approve ? 13 : 10);
  while (!id.empty() && id.back() == ' ') id.pop_back();
  const nimbus::orch::ToolResult r = approve ? nb.approve(owner, id) : nb.reject(owner, id);
  if (!r.success) reply = "Not changed: " + r.error;
  else reply = approve ? "Approved " + id + ": " + r.output + " It applies from the next reply." : "Denied " + id + ".";
  return true;
}

// ---- /prefs ----------------------------------------------------------------------------------
// Engine 1's /prefs over the rules file, plus the held notes: listed with the rules, answered with
// approve note / deny note. The device copies the rules into the notes after a change.
inline std::string prefsCommandPlain(const nimbus::orch::PrefsToolContext& ctx, nimbus::orch::notes::Notebook& nb,
                                     const nimbus::orch::Principal& owner, const std::string& args) {
  std::string reply;
  if (heldNoteAnswer(nb, owner, args, reply)) return reply;
  reply = nimbus::orch::prefsCommand(ctx, args);
  if (!args.empty() && args != "list") return reply;
  const std::string held = heldNotesText(nb, owner);
  return held.empty() ? reply : reply + "\n\n" + held;
}

// ---- the held notes on the Rules page (lane E2-REP2) ------------------------------------------
// The page's rows for the notes held for the owner, the owner's own and each member's, and the owner's
// answers to them. An answer is queued (the page runs on another task) and applied by pump() on the
// notes' only writer; until then the row says the answer was sent, and a refusal stays on the row with
// the device's reason, so the page never says "Approved" before the device has applied it.
struct HeldMember {
  std::string chatId;
  std::string label;
};
struct HeldRow {
  std::string id;     // "note-p1" (the owner's), "note-<chat>/p1" (a member's)
  std::string text;
};

class HeldNoteWeb {
 public:
  static constexpr const char* kPrefix = "note-";
  static bool isRow(const std::string& id) { return id.rfind(kPrefix, 0) == 0; }

  std::vector<HeldRow> rows(nimbus::orch::notes::Notebook& nb, const nimbus::orch::Principal& owner,
                            const std::vector<HeldMember>& members) {
    std::vector<HeldRow> out;
    add(out, nb, owner, "", "");
    for (const HeldMember& m : members)
      add(out, nb, nimbus::orch::principalForRole(m.chatId, nimbus::orch::Role::User), m.chatId,
          m.label.empty() ? m.chatId : m.label);
    return out;
  }

  // Queue the owner's answer; false when too many wait.
  bool queue(bool approve, const std::string& rowId) {
    std::lock_guard<std::mutex> g(mu_);
    if (todo_.size() >= 16) return false;
    todo_.push_back({approve, rowId});
    sent_[rowId] = std::string();
    return true;
  }

  // Run the queued answers. Returns the refusals, one line each (the owner is told them).
  std::vector<std::string> pump(nimbus::orch::notes::Notebook& nb, const nimbus::orch::Principal& owner) {
    std::vector<Answer> run;
    {
      std::lock_guard<std::mutex> g(mu_);
      run.swap(todo_);
    }
    std::vector<std::string> refused;
    for (const Answer& a : run) {
      std::string chat, id;
      split(a.row, chat, id);
      const nimbus::orch::Principal who =
          chat.empty() ? owner : nimbus::orch::principalForRole(chat, nimbus::orch::Role::User);
      std::string reply;
      (void)heldNoteAnswer(nb, who, (a.approve ? "approve note " : "deny note ") + id, reply);
      std::lock_guard<std::mutex> g(mu_);
      if (reply.rfind("Not changed", 0) == 0) {
        sent_[a.row] = reply;   // the row stays and says why
        refused.push_back(reply);
      } else {
        sent_.erase(a.row);     // applied: the note left the shelf
      }
    }
    return refused;
  }

 private:
  struct Answer {
    bool approve;
    std::string row;
  };
  static void split(const std::string& row, std::string& chat, std::string& id) {
    const std::string rest = row.substr(std::string(kPrefix).size());
    const size_t slash = rest.find('/');
    chat = slash == std::string::npos ? std::string() : rest.substr(0, slash);
    id = slash == std::string::npos ? rest : rest.substr(slash + 1);
  }
  void add(std::vector<HeldRow>& out, nimbus::orch::notes::Notebook& nb, const nimbus::orch::Principal& who,
           const std::string& chat, const std::string& label) {
    std::vector<nimbus::orch::notes::PendingNote> held;
    if (!nb.pending(who, held)) return;
    for (const auto& p : held) {
      HeldRow r;
      r.id = std::string(kPrefix) + (chat.empty() ? "" : chat + "/") + p.id;
      r.text = "Note " + (label.empty() ? std::string() : "from " + label + " ") + heldNoteLine(p);
      std::lock_guard<std::mutex> g(mu_);
      auto it = sent_.find(r.id);
      if (it != sent_.end())
        r.text += it->second.empty() ? " (your answer is sent; the device has not applied it yet)"
                                     : " (" + it->second + ")";
      out.push_back(r);
    }
  }
  std::mutex mu_;
  std::vector<Answer> todo_;
  std::map<std::string, std::string> sent_;   // row id -> "" (sent) or the device's refusal
};

}  // namespace plain
}  // namespace agent
