#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/skill_md.h"

// skill_store - the PORTABLE rules of the skills the assistant uses and edits.
// Host-tested (test_harness_skills); the device (src/agent/skills.cpp) and the
// harness lab only supply a SkillFs over their storage.
//
// Files, under the store root (SD /mem/skills, or /data/skills on internal flash
// when the device has no card):
//   <id>/SKILL.md       the live version (approved, or waiting when it is new)
//   <id>/SKILL.next.md  a proposed edit of an approved skill (waiting)
//
// The rules (one table, every actor and state, pinned by
// test_store_actor_state_matrix):
//   - an assistant save of a new id writes SKILL.md, waiting for the owner;
//   - an assistant save of its own waiting skill replaces it (still waiting);
//   - an assistant save of an APPROVED skill (any author) writes SKILL.next.md:
//     the approved version keeps running until the owner approves the edit;
//   - with the owner's "the assistant may approve its own skill revisions"
//     switch on (default off), an assistant save from an attended admin turn
//     that read no outside content goes live at once and the owner is told;
//   - the owner's save replaces SKILL.md (approved, the owner's);
//   - approve promotes a waiting skill or a proposed edit (never a stale edit:
//     one written against a revision that is no longer live);
//   - deny discards a proposed edit, or deletes a waiting skill; it refuses on an
//     approved skill with nothing waiting (that is /skill delete);
//   - the owner-origin guard: an assistant op never changes who wrote a skill,
//     never deletes an owner's skill, and never approves anything.
namespace agent {

// The storage seam (the EpiFs precedent): five plain file operations. Paths are
// absolute strings under the store root.
class SkillFs {
 public:
  virtual ~SkillFs() = default;
  // Read at most `cap` bytes of `path`. False when absent or unreadable.
  virtual bool read(const std::string& path, size_t cap, std::string& out) = 0;
  // Replace `path` so a reader sees the old file or the new one, never a torn one
  // (tmp file then rename), creating the parent directories.
  virtual bool writeAtomic(const std::string& path, const std::string& text) = 0;
  // The bare names of the sub-directories of `dir`.
  virtual std::vector<std::string> listDirs(const std::string& dir) = 0;
  virtual bool exists(const std::string& path) = 0;
  virtual bool remove(const std::string& path) = 0;
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual void removeDir(const std::string& dir) = 0;   // best effort
};

// Who is acting. The owner is the web page, the owner's Telegram chat or the
// serial console; the assistant acts through the skill tools.
struct SkillCaller {
  bool owner = false;
  bool admin = false;      // assistant: the conversation is an admin's
  bool scheduled = false;  // assistant: an unattended turn (routine, synthesis)
  bool tainted = false;    // assistant: the turn read outside content before this call
  static SkillCaller ownerCall() {
    SkillCaller c;
    c.owner = true;
    return c;
  }
  static SkillCaller agentCall(bool admin, bool scheduled, bool tainted) {
    SkillCaller c;
    c.admin = admin;
    c.scheduled = scheduled;
    c.tainted = tainted;
    return c;
  }
};

// The calling turn's trust facts for an assistant skill op. The engine sets the
// scope around each head tool call (one turn at a time on the device, so a plain
// static is enough); the device's skill.save handler reads it.
class SkillTurnScope {
 public:
  explicit SkillTurnScope(bool tainted);
  ~SkillTurnScope();
  SkillTurnScope(const SkillTurnScope&) = delete;
  SkillTurnScope& operator=(const SkillTurnScope&) = delete;

 private:
  bool prev_;
};
bool skillTurnTainted();

enum class SkillStatus : uint8_t { Approved = 0, Pending, ApprovedEditPending, kCount };
const char* skillStatusSlug(SkillStatus s);   // "approved" | "pending" | "approved+edit-pending"

struct SkillMeta {
  std::string id, title, desc, version;
  std::string source;   // "builtin" | the store's slug ("sd" or "flash")
  std::string origin;   // "builtin" | "user" | "agent"
  std::string inject;   // "spawn" | "context" | "both" ("" for a built-in)
  int rev = 1;
  bool approved = true;
  bool pendingEdit = false;  // a SKILL.next.md waits for the owner
  int nextRev = 0;           // the proposed edit's revision (0 = none)
  bool nextStale = false;    // the live skill changed after the edit was proposed
  bool builtin() const { return source == "builtin"; }
  SkillStatus status() const {
    if (!approved) return SkillStatus::Pending;
    return pendingEdit ? SkillStatus::ApprovedEditPending : SkillStatus::Approved;
  }
};

enum class SkillOutcome : uint8_t {
  Refused = 0,    // nothing changed; err says why
  Created,        // a new skill (the owner's: approved; the assistant's: waiting)
  Replaced,       // the owner replaced the live text, or the assistant its own waiting skill
  EditProposed,   // SKILL.next.md written; the approved version keeps running
  WentLive,       // the switch is on: the assistant's change is live now
  Approved,       // a waiting skill is approved
  EditApplied,    // a proposed edit replaced the live version
  EditDiscarded,  // a proposed edit was dropped; the live version is unchanged
  Deleted,
  Unchanged,      // approving an approved skill with nothing waiting
  kCount
};
const char* skillOutcomeSlug(SkillOutcome o);

struct SkillResult {
  bool ok = false;
  SkillOutcome outcome = SkillOutcome::Refused;
  std::string err;
  int rev = 0;                        // the live revision after the op (proposed for an edit)
  std::vector<std::string> warnings;  // owner saves: lint findings (never a refusal)
  // A new assistant skill whose id looks like an existing one's (the 17 Oct
  // "add to your estimation skill" request made a sibling instead of an edit):
  // the result names the existing skill so the assistant can save there instead.
  std::string similar;
};

// The owner's "the assistant may approve its own skill revisions" switch: a tiny
// file on internal flash on every tier (no settings-store key). Absent, empty or
// anything but "1" reads as off, so the default is off.
inline constexpr char kSkillSelfApproveFile[] = "/data/skills_auto.txt";
bool skillSelfApproveFromFile(const std::string& text);

// Which side of a built-in body to serve (the turn's Telegram sample, CUM-523).
struct SkillSide {
  bool telegram = true;
  bool fileSend = true;
};

struct SkillStoreConfig {
  std::string root = "/mem/skills";
  std::string sourceSlug = "sd";   // what list() reports as a stored skill's source
  size_t fileCap = 8192;           // the largest SKILL.md the store reads or writes
  int pendingCap = 4;              // waiting assistant skills + proposed edits
  size_t maxStored = 0;            // stored skills on this tier (0 = no limit)
};

// The lint for a save (the assistant's are refused on any finding, the owner's
// carry them as warnings). `written` is the text as sent (its raw inject value
// and unknown keys); `merged` is the text after inheriting from the live version.
std::vector<std::string> lintSkillSave(const SkillMd& written, const SkillMd& merged);

// The front-matter keys an author writes, in the words the skill.save tool text
// uses ("title (a short title); desc (...); ..."). One source: kSkillMdKeys.
std::string skillAuthorKeysDoc();

class SkillStore {
 public:
  SkillStore(SkillFs& fs, SkillStoreConfig cfg, std::function<std::string()> clock);

  // a-z A-Z 0-9 - _ , at most 23 bytes (the spawn directive's skill field).
  static bool validId(const std::string& id);

  // Built-ins and stored skills, a stored one replacing a built-in of the same id.
  std::vector<SkillMeta> list();
  bool find(const std::string& id, SkillMeta& out);
  std::string indexText();

  // skill.get: a labelled header line, then the body. A waiting skill is labelled
  // as not active (never served as if it were); an approved skill with a proposed
  // edit says the approved version is what runs. `trusted` (optional) is set when
  // the text is an approved or built-in skill, the ones that may stay attached.
  // "" for an unknown id.
  std::string get(const std::string& id, const SkillSide& side, bool* trusted = nullptr);
  // The body that runs: an approved stored skill's or a built-in's; "" otherwise.
  std::string liveBody(const std::string& id, const SkillSide& side);
  // The capsule a sub-agent spawned with this skill receives: approved stored
  // skills whose inject is spawn or both; "" for anything else.
  std::string spawnCapsule(const std::string& id);
  // Raw files for the owner's editor ("" when absent).
  std::string raw(const std::string& id);
  std::string rawNext(const std::string& id);

  // Waiting assistant skills plus proposed edits (the pending cap counts these).
  int pendingCount();

  SkillResult save(const std::string& id, const std::string& md, const SkillCaller& who,
                   bool selfApprove);
  SkillResult approve(const std::string& id, const SkillCaller& who);
  SkillResult deny(const std::string& id, const SkillCaller& who);
  SkillResult discard(const std::string& id, const SkillCaller& who);
  SkillResult remove(const std::string& id, const SkillCaller& who);

 private:
  struct Stored {
    bool have = false;
    SkillMd live;
    bool haveNext = false;
    SkillMd next;
  };
  std::string dirOf(const std::string& id) const;
  std::string livePath(const std::string& id) const;
  std::string nextPath(const std::string& id) const;
  bool readDoc(const std::string& path, SkillMd& out, size_t cap);
  Stored load(const std::string& id, size_t cap);
  bool metaOf(const std::string& id, SkillMeta& out);
  size_t storedCount();
  bool write(const std::string& path, const SkillMd& doc, std::string& err);
  SkillResult saveOwner(const std::string& id, const SkillMd& in, const Stored& cur);
  SkillResult saveAgent(const std::string& id, const SkillMd& in, const Stored& cur,
                        const SkillCaller& who, bool selfApprove);
  SkillResult agentWrite(const std::string& id, SkillMd doc, const Stored& cur, bool live);
  std::string similarTo(const std::string& id);

  SkillFs& fs_;
  SkillStoreConfig cfg_;
  std::function<std::string()> clock_;
};

// The /skill sub-commands the owner sends over Telegram (deterministic, owner-only,
// never a model turn). One table: the dispatcher, its help line and the tests.
enum class SkillCmd : uint8_t { List = 0, Show, Approve, Deny, Delete, Auto, Help, kCount };
struct SkillCmdRow {
  SkillCmd cmd;
  const char* word;
  bool needsId;   // Auto takes on|off instead of an id
};
extern const SkillCmdRow kSkillCmds[];
extern const size_t kSkillCmdCount;
// Parse "/skill <args>": the sub-command and its argument (an id, or on|off).
// An empty or unknown sub-command reads as Help.
SkillCmd parseSkillCmd(const std::string& args, std::string& arg);
std::string skillCmdUsage();

// The owner-facing words for one result: plain, short, no ticket numbers.
std::string skillResultText(const std::string& id, const SkillResult& r);
// The notice the owner gets when the switch let the assistant's change go live.
std::string skillSelfApproveNotice(const std::string& id, int rev, bool created);
// The notice the owner gets when the assistant proposes a new skill or an edit.
std::string skillProposalNotice(const std::string& id, const SkillResult& r);
// /skill show: description, revision, status and the start of the text, or a
// short summary of a proposed edit.
std::string skillShowText(SkillStore& st, const std::string& id);
std::string skillListText(SkillStore& st, bool selfApprove);

}  // namespace agent
