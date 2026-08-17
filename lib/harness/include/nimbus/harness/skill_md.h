#pragma once
#include <string>
#include <vector>

// skill_md - the PORTABLE half of dynamic skills (roadmap P2: owner-authored
// SD capsules, "attached per sub-agent at spawn"). Host-tested
// (test_harness_skills); the device's src/agent/skills.cpp uses this parser
// over files read from /mem/skills/<id>/SKILL.md, and the portable JobEngine
// uses the injection composer at dispatch time.
//
// SKILL.md format: an optional leading front-matter block
//
//   ---
//   title: One-line owner-facing title
//   version: 3            (optional, free text)
//   inject: spawn         (optional: spawn | context | both; default spawn)
//   ---
//   ...the capsule body (markdown)...
//
// No front matter => the whole file is the body (title empty; the device falls
// back to the id). Unknown front-matter keys are ignored, never an error - an
// owner-edited file must always parse to SOMETHING usable.
namespace agent {

struct SkillMd {
  std::string title;
  // One-line "when to reach for this" description (W15, owner contract): it
  // rides the ambient [SKILLS] index in every turn, so the model can PULL the
  // full body with skill.get instead of the recipe bloating every prompt.
  // Optional - a capsule without one falls back to its title in the index.
  std::string desc;
  std::string version;
  std::string inject = "spawn";   // "spawn" | "context" | "both"
  // Origin + approval (v4.0.0 skills authoring). createdBy: "user" | "agent" -
  // who authored this capsule. approved: an AGENT-authored capsule starts
  // false and is INERT for spawn injection until the owner approves (the loops
  // precedent - a persistent instruction blob is a prompt-injection channel).
  // Defaults keep every pre-existing file a user-approved capsule.
  std::string createdBy = "user";
  std::string createdAt;          // free text (device stamps an ISO-ish time)
  bool approved = true;
  std::string body;
  bool hadFrontMatter = false;
};

// Parse a SKILL.md text. Never fails: worst case the whole input is the body.
SkillMd parseSkillMd(const std::string& md);

// Serialize the CANONICAL front matter + body. This is the sanitization
// primitive: the device's save() parses whatever it was handed, keeps only the
// known fields, SERVER-STAMPS createdBy/createdAt/approved, and re-emits
// through this - a model-supplied `approved: true` or `created_by: user`
// never survives to disk.
std::string composeSkillMd(const SkillMd& s);

// Per-spawn injection cap: a capsule longer than this is truncated (with a
// visible note) before it rides the sub-agent instruction. Keeps a runaway
// owner file from blowing the dispatch body.
constexpr size_t kSkillInjectCap = 4096;

// Compose the spawn instruction for a task with a resolved skill capsule:
//   [SKILL: <id>]\n<capsule (capped)>\n\n---\n<task>
// An empty capsule returns the task unchanged (hint-passthrough semantics).
std::string composeSkillInjection(const std::string& skillId, const std::string& capsule,
                                  const std::string& task);

// ---- W15: the ambient [SKILLS] index -----------------------------------------
// The harness contract (owner 2026-08-09): the model must be able to PULL a
// skill's detail from a one-line description it always sees - a skill only the
// model already knows about is a skill it will never reach for. This block
// rides every turn's dynamic context (like [PROVIDERS & CONNECTORS]); the
// bodies stay behind skill.get.
struct SkillIndexEntry {
  std::string id;
  std::string desc;      // one line; falls back to title, then id, at the caller
  bool pending = false;  // agent-authored, not yet owner-approved
};

// Per-entry description cap and whole-block byte budget. The block is INPUT-side
// (not the byte-budgeted system prompt), but it repeats every turn, so it stays
// tight: entries past the budget collapse to a "+N more (skill.list)" line
// rather than silently vanishing - a skill the index doesn't NAME is invisible,
// which is the exact failure class this block exists to remove.
constexpr size_t kSkillIndexDescCap  = 140;
constexpr size_t kSkillIndexByteCap  = 1400;

// Render the block ("" when there are no entries):
//   [SKILLS] Playbooks - read the matching one with skill.get(id) BEFORE ...
//   - <id>: <desc>
//   (+N more - skill.list)
std::string skillsIndexText(const std::vector<SkillIndexEntry>& entries);

}  // namespace agent
