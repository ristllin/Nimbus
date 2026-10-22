#pragma once
#include <cstddef>
#include <string>

#include "nimbus/orch/psram_alloc.h"

// tg_media_memo - the blob-hash dedupe of the receive path's paid description step.
//
// A photo that arrives again with the same bytes and the same caption context (the
// same picture sent twice, a page served again after its answer, a forward of a photo
// the chat already sent) needs no second paid vision call: its description is
// remembered by (chat, content hash, caption context) and reused. The content hash is
// the one the label's ref, the blob sidecar and the files store all use, so every store
// dedupes on the same key. Only a real description is remembered (a failed one is
// retried next time), and only for the same chat (one chat never reads a description
// another chat's caption asked for). A fixed ring of kMediaMemoSlots entries; the text
// rides the WorkingAllocator (PSRAM on the device). Single owner: the poll task.
// Portable and host-tested (test_media_label).
namespace nimbus::net {

constexpr size_t kMediaMemoSlots = 8;

class MediaMemo {
 public:
  // The remembered description of `hash` for `chat` under caption context `ctx`.
  bool find(const std::string& chat, const std::string& hash, const std::string& ctx, std::string& out) const {
    for (const Entry& e : ring_)
      if (e.used && e.chat == chat.c_str() && e.hash == hash.c_str() && e.ctx == ctx.c_str()) {
        out.assign(e.desc.data(), e.desc.size());
        return true;
      }
    return false;
  }

  // Remember a description (an empty one, a failure, is never kept).
  void put(const std::string& chat, const std::string& hash, const std::string& ctx, const std::string& desc) {
    if (desc.empty() || hash.empty()) return;
    Entry& e = ring_[next_];
    next_ = (next_ + 1) % kMediaMemoSlots;
    e.used = true;
    e.chat.assign(chat.data(), chat.size());
    e.hash.assign(hash.data(), hash.size());
    e.ctx.assign(ctx.data(), ctx.size());
    e.desc.assign(desc.data(), desc.size());
  }

 private:
  using Str = std::basic_string<char, std::char_traits<char>, nimbus::orch::WorkingAllocator<char>>;
  struct Entry {
    bool used = false;
    Str  chat, hash, ctx, desc;
  };
  Entry  ring_[kMediaMemoSlots];
  size_t next_ = 0;
};

}  // namespace nimbus::net
