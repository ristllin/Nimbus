#pragma once
#include <cstddef>
#include <string>
#include <vector>

// skill_md - the PORTABLE text format of a skill (a task playbook or knowledge
// capsule). Host-tested (test_harness_skills); the portable SkillStore
// (skill_store.h) reads and writes it, the JobEngine uses the injection
// composer at dispatch time, and the attach block (skill_attach.h) carries the
// approved bodies into the head's per-turn input.
//
// SKILL.md format: an optional leading front-matter block
//
//   ---
//   title: One-line owner-facing title
//   desc: One line saying WHEN to read this skill (shown in [SKILLS] every turn)
//   version: 3            (optional, free text)
//   inject: spawn         (optional: spawn | context | both; default spawn)
//   ---
//   ...the capsule body (markdown)...
//
// The server owns created_by / created_at / approved / rev / updated_at /
// base_rev: whatever an author writes there is replaced on save. No front
// matter => the whole file is the body. Unknown keys never break a parse (an
// owner-edited file must always parse to something usable); the save lint
// (skill_store.h) refuses them on the assistant's saves.
namespace agent {

struct SkillMd {
  std::string title;
  // One-line "when to reach for this" description: it rides the [SKILLS] index
  // every turn, so the model can pull the body with skill_get instead of the
  // recipe riding every prompt. A capsule without one falls back to its title.
  std::string desc;
  std::string version;
  std::string inject = "spawn";   // "spawn" | "context" | "both" (normalized)
  // Origin and approval. createdBy: "user" | "agent". approved: an assistant-made
  // capsule starts false and is inactive until the owner approves it (or the
  // owner lets the assistant approve its own revisions). Defaults keep every
  // pre-existing file a user-approved capsule.
  std::string createdBy = "user";
  std::string createdAt;          // written once, at creation
  bool approved = true;
  // Server-owned revision counter: 1 at creation, +1 per accepted change. A
  // legacy file has no rev and reads as revision 1 (skillRev).
  int rev = 0;
  std::string updatedAt;
  // SKILL.next.md only: the live revision this proposed edit was written
  // against. An approve promotes the edit only while the live file is still at
  // that revision (a stale proposal is never promoted).
  int baseRev = 0;
  std::string body;
  bool hadFrontMatter = false;
  // What the author wrote, for the save lint (never re-emitted): the raw inject
  // value ("" when absent) and every front-matter key outside kSkillMdKeys.
  std::string injectRaw;
  std::vector<std::string> unknownKeys;
};

// One front-matter key the format knows. `author` keys are the ones an author
// writes (title, desc, version, inject); the rest are server-owned. The
// skill.save tool text and the lint read this ONE table, so a key the composer
// writes can never go undocumented (the drift guard in test_harness_skills).
struct SkillMdKey {
  const char* key;
  bool author;
  const char* doc;   // what the key means, in the words the tool text uses
};
extern const SkillMdKey kSkillMdKeys[];
extern const size_t kSkillMdKeyCount;
bool isSkillMdKey(const std::string& key);

// The valid inject values and what each one means for the head and sub-agents.
bool validInject(const std::string& v);

// A capsule's revision (a legacy file without rev reads as 1).
inline int skillRev(const SkillMd& s) { return s.rev > 0 ? s.rev : 1; }

// Parse a SKILL.md text. Never fails: worst case the whole input is the body.
SkillMd parseSkillMd(const std::string& md);

// Serialize the CANONICAL front matter + body. This is the sanitization
// primitive: the store parses whatever it was handed, keeps only the known
// fields, SERVER-STAMPS the origin, approval and revision fields, and re-emits
// through this - a model-supplied `approved: true` or `created_by: user` never
// survives to disk.
std::string composeSkillMd(const SkillMd& s);

// Per-spawn injection cap: a capsule longer than this is cut on a UTF-8
// character boundary (with a visible note) before it rides the sub-agent
// instruction. The attach block (skill_attach.h) has the same cap.
constexpr size_t kSkillInjectCap = 4096;

// The longest prefix of `s` that fits `maxBytes` and ends on a UTF-8 character
// boundary (a cut never leaves a torn multi-byte sequence for a provider to 400).
std::string skillUtf8Prefix(const std::string& s, size_t maxBytes);

// Compose the spawn instruction for a task with a resolved skill capsule:
//   [SKILL: <id>]\n<capsule (capped)>\n\n---\n<task>
// An empty capsule returns the task unchanged (hint-passthrough semantics).
std::string composeSkillInjection(const std::string& skillId, const std::string& capsule,
                                  const std::string& task);

// ---- the ambient [SKILLS] index ----------------------------------------------
// The model must be able to PULL a skill's detail from a one-line description it
// always sees. The block rides the stable catalogs; the bodies stay behind
// skill_get (or ride [ACTIVE SKILLS] once read).
struct SkillIndexEntry {
  std::string id;
  std::string desc;          // one line; falls back to title, then id, at the caller
  bool pending = false;      // not approved yet: named only in the pending tail line
  bool pendingEdit = false;  // approved, with a proposed edit waiting for the owner
};

// Per-entry description cap and whole-block byte budget. Entries past the budget
// collapse to a "+N more" line rather than silently vanishing - a skill the
// index doesn't NAME is invisible.
constexpr size_t kSkillIndexDescCap  = 140;
constexpr size_t kSkillIndexByteCap  = 1400;

// Render the block ("" when there are no entries):
//   [SKILLS] Playbooks ... read the matching one with skill_get {id} BEFORE ...
//   - <id>: <desc>
//   (+N more - skill_list)
//   (waiting for the owner's approval, not active: a, b; edit proposed: c)
std::string skillsIndexText(const std::vector<SkillIndexEntry>& entries);

}  // namespace agent
