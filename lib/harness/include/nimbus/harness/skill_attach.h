#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/skill_md.h"
#include "nimbus/harness/skill_store.h"

// skill_attach - approved skills ride the head's per-turn input once read, so the
// model follows them on the follow-up turns instead of re-reading (or skipping)
// them every turn. Portable and host-tested (test_harness_skills); RAM only (a
// small table, lost on restart by design; no NVS, no file).
//
// Two ways a skill is attached in a chat:
//   - sticky: the head read it with skill_get (an approved stored skill or a
//     built-in). It stays for that chat until the chat has no turn for 3 hours,
//     /clear, the skill stops being approved or is deleted, a third skill read
//     in the same chat (at most 2 per chat, least recently read goes), or a
//     restart;
//   - pinned: an approved stored skill whose inject is context or both rides
//     every head turn in every chat (built-ins are never pinned).
// The block is at most 4096 bytes, pinned skills first then sticky ones, each
// sorted by id (stable bytes for the prompt cache), cut on a UTF-8 boundary with
// a visible note. A proposed edit never changes what is attached: the approved
// version keeps running until the owner approves the edit.
namespace agent {

constexpr size_t kSkillAttachCap = kSkillInjectCap;
constexpr uint32_t kSkillStickyIdleMs = 3u * 60u * 60u * 1000u;
constexpr size_t kSkillStickyPerChat = 2;
constexpr size_t kSkillAttachSlots = 4;
constexpr size_t kSkillAttachKeyMax = 31;   // chat ids and skill ids longer than this are not tracked

class AttachTable {
 public:
  // The head read `id` in `chat` (the caller checked it is trusted).
  void noteUsed(const std::string& chat, const std::string& id, uint32_t nowMs);
  // A head turn starts in `chat`: drop every entry idle past the window, then
  // refresh this chat's entries.
  void noteTurn(const std::string& chat, uint32_t nowMs);
  void expire(uint32_t nowMs);
  void dropChat(const std::string& chat);
  void dropId(const std::string& id);
  // This chat's sticky ids, sorted.
  std::vector<std::string> stickyFor(const std::string& chat) const;
  size_t size() const;

 private:
  struct Slot {
    char chat[kSkillAttachKeyMax + 1] = {};
    char id[kSkillAttachKeyMax + 1] = {};
    uint32_t lastUseMs = 0;
    uint32_t lastTurnMs = 0;
    bool used = false;
  };
  Slot slots_[kSkillAttachSlots];
};

struct AttachPick {
  std::string id;
  int rev = 0;
  bool builtin = false;
  bool pinned = false;
};

// The skills to attach for one chat: pinned ones (approved stored skills with
// inject context or both) first, then the chat's sticky ids that are still
// trusted (approved stored or built-in); each part sorted by id, no duplicates.
std::vector<AttachPick> selectAttach(const std::vector<std::string>& sticky,
                                     const std::vector<SkillMeta>& metas);

// The [ACTIVE SKILLS] block for the picks ("" when there are none), at most
// `cap` bytes, valid UTF-8 when the bodies are. `body(id)` returns the text that
// runs (the approved version).
std::string skillsAttachText(const std::vector<AttachPick>& picks,
                             const std::function<std::string(const std::string&)>& body,
                             size_t cap = kSkillAttachCap);

}  // namespace agent
