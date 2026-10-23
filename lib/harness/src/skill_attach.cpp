#include "nimbus/harness/skill_attach.h"

#include <algorithm>
#include <cstring>

#include "nimbus/harness/compose.h"   // loopToolName

namespace agent {

namespace {

bool fits(const std::string& s) { return !s.empty() && s.size() <= kSkillAttachKeyMax; }

void copyKey(char* dst, const std::string& s) {
  std::memcpy(dst, s.data(), s.size());
  dst[s.size()] = 0;
}

}  // namespace

void AttachTable::expire(uint32_t nowMs) {
  for (Slot& s : slots_)
    if (s.used && (uint32_t)(nowMs - s.lastTurnMs) > kSkillStickyIdleMs) s = Slot{};
}

void AttachTable::noteTurn(const std::string& chat, uint32_t nowMs) {
  expire(nowMs);
  for (Slot& s : slots_)
    if (s.used && chat == s.chat) s.lastTurnMs = nowMs;
}

void AttachTable::noteUsed(const std::string& chat, const std::string& id, uint32_t nowMs) {
  if (!fits(chat) || !fits(id)) return;
  Slot* inChat[kSkillAttachSlots] = {};
  size_t n = 0;
  for (Slot& s : slots_) {
    if (!s.used || chat != s.chat) continue;
    if (id == s.id) {   // read again: refresh, no transition
      s.lastUseMs = s.lastTurnMs = nowMs;
      return;
    }
    inChat[n++] = &s;
  }
  if (n >= kSkillStickyPerChat) {   // this chat's least recently read goes
    Slot* lru = *std::max_element(inChat, inChat + n, [nowMs](const Slot* a, const Slot* b) {
      return (uint32_t)(nowMs - a->lastUseMs) < (uint32_t)(nowMs - b->lastUseMs);
    });
    *lru = Slot{};
  }
  Slot* free = nullptr;
  for (Slot& s : slots_)
    if (!s.used) { free = &s; break; }
  if (!free) {   // the table is full: the slot idle longest goes
    free = std::max_element(std::begin(slots_), std::end(slots_), [nowMs](const Slot& a, const Slot& b) {
      return (uint32_t)(nowMs - a.lastTurnMs) < (uint32_t)(nowMs - b.lastTurnMs);
    });
  }
  *free = Slot{};
  copyKey(free->chat, chat);
  copyKey(free->id, id);
  free->lastUseMs = free->lastTurnMs = nowMs;
  free->used = true;
}

void AttachTable::dropChat(const std::string& chat) {
  for (Slot& s : slots_)
    if (s.used && chat == s.chat) s = Slot{};
}

void AttachTable::dropId(const std::string& id) {
  for (Slot& s : slots_)
    if (s.used && id == s.id) s = Slot{};
}

std::vector<std::string> AttachTable::stickyFor(const std::string& chat) const {
  std::vector<std::string> out;
  for (const Slot& s : slots_)
    if (s.used && chat == s.chat) out.emplace_back(s.id);
  std::sort(out.begin(), out.end());
  return out;
}

size_t AttachTable::size() const {
  size_t n = 0;
  for (const Slot& s : slots_) n += s.used ? 1 : 0;
  return n;
}

std::vector<AttachPick> selectAttach(const std::vector<std::string>& sticky,
                                     const std::vector<SkillMeta>& metas) {
  std::vector<AttachPick> pinned, read;
  for (const SkillMeta& m : metas)
    if (!m.builtin() && m.approved && (m.inject == "context" || m.inject == "both"))
      pinned.push_back(AttachPick{m.id, m.rev, false, true});
  auto byId = [](const AttachPick& a, const AttachPick& b) { return a.id < b.id; };
  std::sort(pinned.begin(), pinned.end(), byId);
  for (const std::string& id : sticky) {
    if (std::any_of(pinned.begin(), pinned.end(), [&](const AttachPick& p) { return p.id == id; }))
      continue;
    auto it = std::find_if(metas.begin(), metas.end(), [&](const SkillMeta& m) { return m.id == id; });
    if (it == metas.end() || !(it->builtin() || it->approved)) continue;   // gone or not approved
    if (std::any_of(read.begin(), read.end(), [&](const AttachPick& p) { return p.id == id; })) continue;
    read.push_back(AttachPick{id, it->builtin() ? 0 : it->rev, it->builtin(), false});
  }
  std::sort(read.begin(), read.end(), byId);
  pinned.insert(pinned.end(), read.begin(), read.end());
  return pinned;
}

namespace {

std::string sectionHead(const AttachPick& p) {
  std::string h = "[SKILL " + p.id;
  if (!p.builtin) h += " rev " + std::to_string(p.rev);
  h += p.builtin ? " - built-in" : " - approved";
  h += p.pinned ? " - on every turn (inject " + std::string("context") + ")]\n"
                : " - attached because it was read in this chat]\n";
  return h;
}

std::string truncMark(const std::string& id) {
  return "\n[skill truncated at 4096 B - " + loopToolName("skill.get") + " " + id + " for the rest]";
}

std::string overflowLine(size_t n) {
  return "(+" + std::to_string(n) + " more attached skill(s) not shown: over the 4096-byte "
         "budget - read them with " + loopToolName("skill.get") + ")\n";
}

constexpr size_t kMinBody = 64;   // a section shows at least this much of its body

}  // namespace

std::string skillsAttachText(const std::vector<AttachPick>& picks,
                             const std::function<std::string(const std::string&)>& body,
                             size_t cap) {
  if (picks.empty() || !body) return std::string();
  const std::string header =
      "[ACTIVE SKILLS] Already loaded for this chat: follow them; do not call " +
      loopToolName("skill.get") + " for these again.\n";
  std::string out = header;
  const size_t reserve = overflowLine(9999).size();
  for (size_t i = 0; i < picks.size(); ++i) {
    const std::string b = body(picks[i].id);
    if (b.empty()) continue;   // vanished between the listing and now
    const std::string head = sectionHead(picks[i]);
    const std::string mark = truncMark(picks[i].id);
    const size_t need = (i + 1 < picks.size()) ? reserve : 0;
    const size_t room = cap > out.size() + need ? cap - out.size() - need : 0;
    if (head.size() + kMinBody + mark.size() + 1 > room) {
      if (out.size() + overflowLine(picks.size() - i).size() <= cap)
        out += overflowLine(picks.size() - i);
      break;
    }
    if (head.size() + b.size() + 1 <= room)
      out += head + b + "\n";
    else
      out += head + skillUtf8Prefix(b, room - head.size() - mark.size() - 1) + mark + "\n";
  }
  return out == header ? std::string() : out;
}

}  // namespace agent
