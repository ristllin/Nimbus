#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/skill_attach.h"   // AttachTable / AttachPick
#include "nimbus/harness/skill_store.h"   // SkillStore / SkillCaller / SkillResult

// skills - the device's skills the assistant uses and can edit (lane I).
//
// All rules live in the PORTABLE, host-tested SkillStore (lib/harness): staged
// SKILL.next.md revisions, the owner-origin guard, the save lint, the pending
// cap, revision counters and the self-approve switch. This adapter only binds
// the store to the device's storage and surfaces:
//   - the [SKILLS] index and the per-chat attach table (the engine deps);
//   - the skill.* tool surface (memory_subsystem);
//   - the owner surfaces: /skill commands (orchestrator) and the web routes.
//
// Files, under the store root on the ACTIVE tier (SD /mem/skills when a card is
// mounted, else LittleFS /data/skills):
//   <id>/SKILL.md       the live version (approved, or waiting when new)
//   <id>/SKILL.next.md  a proposed edit of an approved skill (waiting)
// The owner's "the assistant may approve its own skill revisions" switch is a
// tiny file on internal flash on every tier (/data/skills_auto.txt; absent or
// anything but "1" reads as off, so the default is off).
//
// Memory discipline: file reads stage through PSRAM (8 KB file cap); the RAM
// attach table is 4 slots (~256 B); nothing here holds steady-state
// internal-SRAM beyond that.
namespace agent::skills {

// The active tier is the SD card (store root /mem/skills).
bool sdAvailable();

// The one store over the active tier. begin() (memory_subsystem's, or the
// first call) constructs it; safe to call from any task afterwards.
agent::SkillStore& store();

// The owner's self-approve switch (D-4): "the assistant may approve its own
// skill revisions" - when on, an attended, untainted admin save goes live and
// the owner is told; default off.
bool selfApproveOn();
bool setSelfApprove(bool on);

// ---- the engine's attach surface ---------------------------------------------
// The [SKILLS] index block (the portable skillsIndexText over the store).
std::string indexText();
// The skills attached to one chat's turns: pinned context skills plus what the
// chat read (the RAM AttachTable + selectAttach, both portable).
std::vector<agent::AttachPick> picksFor(const std::string& chatId);
// The body that runs for one attached id (the approved version, "" otherwise).
std::string attachBody(const std::string& id);
// A head turn read this skill in this chat (RAM only; sticky attach).
void noteUsed(const std::string& chat, const std::string& id);
// /clear in a chat drops its sticky skills (the CUM-511 boundary).
void dropChat(const std::string& chat);

// ---- the tool and web surface ------------------------------------------------
// Built-ins and stored skills, a stored one replacing a built-in of the id.
std::vector<agent::SkillMeta> list();
// skill.get: the labelled body (a waiting skill is labelled not-active; "" for
// an unknown id). `trusted` (optional) is set for an approved or built-in skill
// - the ones that may attach. A built-in follows the turn's CUM-523 sides.
std::string get(const std::string& id, bool* trusted = nullptr);
// The capsule a sub-agent spawned with this skill receives ("" unless an
// approved stored skill with inject spawn|both).
std::string spawnCapsule(const std::string& id);
// Raw texts for the owner's editor ("" when absent).
std::string raw(const std::string& id);
std::string rawNext(const std::string& id);
// Waiting assistant skills plus proposed edits (the pending cap counts these).
int pendingCount();

// One whole-store operation under the storage lock, with the caller resolved by
// the caller (the tool handler passes the turn's admin/scheduled/tainted facts
// via SkillCaller; the web routes pass ownerCall()). Refusals leave the store
// byte-identical; the result carries the plain owner words (skillResultText).
agent::SkillResult save(const std::string& id, const std::string& md,
                        const agent::SkillCaller& who);
agent::SkillResult approve(const std::string& id, const agent::SkillCaller& who);
agent::SkillResult deny(const std::string& id, const agent::SkillCaller& who);
agent::SkillResult discard(const std::string& id, const agent::SkillCaller& who);
agent::SkillResult remove(const std::string& id, const agent::SkillCaller& who);

}  // namespace agent::skills
