#pragma once
// recall_warm - the turn's recall embedding made during the settle hold (lane RB-TS1).
//
// On the old engine every owner turn opens with one embedding request for its recall
// (a fresh TLS session to the embeddings provider): about 1.2 s of handshake plus the
// request, after the hold, while the owner waits (coverage/LINK-2026-11.md section 5).
// The hold itself is quiet time on the same task. So while a page is held, the device
// embeds the held text, and the turn's recall takes that vector instead of asking
// again, but only when nothing can differ:
//   - the page is exactly one plain text from an allowed chat that is not a command,
//     not forwarded and not late (the turn's text is then that text byte for byte:
//     MessageBatcher::render of one on-time message is its text);
//   - the query is that exact text, the embedder (provider, model, width) is the same,
//     and the vector is at most kRecallWarmMaxAgeMs old;
//   - a vector is used once.
// Anything else (a joined burst, a photo, a voice note, an edit) embeds as before.
// Portable and host-tested (test_recall_warm).

#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/tg_updates.h"

namespace nimbus {
namespace orch {

constexpr uint32_t kRecallWarmMaxAgeMs = 30000;

// The held text to embed now, or nullptr. `allowed(chat)` is the allowlist; `late(date)`
// is true when the turn would carry the message's send time.
template <class Allowed, class Late>
const tg::Update* warmableHeldText(const std::vector<tg::Update>& page, Allowed&& allowed, Late&& late) {
  if (page.size() != 1) return nullptr;
  const tg::Update& u = page[0];
  if (u.edited || u.forwarded || u.textIsCaption || u.text.empty() || u.chatId.empty() || u.text[0] == '/')
    return nullptr;
  if (u.attachment.kind != tg::Attachment::Kind::None) return nullptr;
  if (!allowed(u.chatId) || late(u.date)) return nullptr;
  return &u;
}

// One slot: the text, its vector and the embedder that made it.
class RecallWarm {
 public:
  bool has(const std::string& text) const { return !v_.empty() && text_ == text; }

  void put(const std::string& text, const std::string& embedder, std::vector<int8_t>&& v, uint32_t nowMs) {
    text_ = text;
    embedder_ = embedder;
    v_.swap(v);
    atMs_ = nowMs;
  }

  // The vector for exactly this query, made by this embedder, still fresh. The slot is
  // emptied either way: the next turn never sees a vector made for an earlier one.
  bool take(const std::string& query, const std::string& embedder, uint32_t nowMs, std::vector<int8_t>& out) {
    if (v_.empty()) return false;
    const bool fits = text_ == query && embedder_ == embedder && nowMs - atMs_ <= kRecallWarmMaxAgeMs;
    if (fits) out.swap(v_);
    clear();
    return fits;
  }

  void clear() {
    text_.clear();
    embedder_.clear();
    std::vector<int8_t>().swap(v_);
    atMs_ = 0;
  }

 private:
  std::string text_;
  std::string embedder_;
  std::vector<int8_t> v_;
  uint32_t atMs_ = 0;
};

}  // namespace orch
}  // namespace nimbus
