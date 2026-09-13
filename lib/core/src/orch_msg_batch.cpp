#include "nimbus/orch/msg_batch.h"

#include <cstdio>

// CUM-398 rapid-message batcher. See msg_batch.h. Pure logic, host-tested; the
// device seam (src/agent/telegram.cpp) fills it on the tg_poll task and emits one
// turn per bucket. No concurrency, no I/O.
namespace nimbus {
namespace orch {

MessageBatcher::Bucket* MessageBatcher::find(const std::string& chatId) {
  for (auto& b : chats_)
    if (b.chatId.size() == chatId.size() &&
        std::char_traits<char>::compare(b.chatId.c_str(), chatId.data(), chatId.size()) == 0)
      return &b;
  return nullptr;
}

BatchAdd MessageBatcher::add(const std::string& chatId, const std::string& from,
                             const std::string& text) {
  const size_t n = text.size();
  Bucket* b = find(chatId);

  if (!b) {
    // A new chat. Two ceilings gate creation; either one means "re-serve this
    // message, it did not fit this drain" - never a silent drop. A lone message is
    // always accepted onto a fresh accumulator (a single Telegram/inject message is
    // smaller than the total cap), so a message can never be permanently stuck.
    if (chats_.size() >= kBatchMaxChats) return BatchAdd::Deferred;
    if (totalBytes_ + n > kBatchMaxTotalBytes) return BatchAdd::Deferred;
    chats_.emplace_back();
    b = &chats_.back();
    b->chatId.assign(chatId.data(), chatId.size());
    b->turnFrom.assign(from.data(), from.size());
  } else {
    // An existing chat: enforce the per-chat count and byte caps and the shared
    // total-bytes cap. A trip marks the bucket capped (its render says more are
    // waiting) and defers this message; the caller re-serves it next drain.
    if (b->msgs.size() >= kBatchMaxMsgsPerChat ||
        b->bytes + n > kBatchMaxBytesPerChat ||
        totalBytes_ + n > kBatchMaxTotalBytes) {
      b->capped = true;
      return BatchAdd::Deferred;
    }
  }

  b->msgs.emplace_back();
  Msg& m = b->msgs.back();
  m.chatFrom.assign(from.data(), from.size());
  m.text.assign(text.data(), text.size());
  b->bytes  += n;
  totalBytes_ += n;

  if (b->msgs.size() >= kBatchMaxMsgsPerChat || b->bytes >= kBatchMaxBytesPerChat)
    return BatchAdd::ChatFull;
  return BatchAdd::Accepted;
}

PsString MessageBatcher::render(size_t i) const {
  const Bucket& b = chats_[i];
  const size_t nmsg = b.msgs.size();
  PsString out;

  // The common case: one message, nothing shed. Emit the raw text unchanged so a
  // normal single message is byte-for-byte identical to the pre-batching turn.
  if (nmsg == 1 && !b.capped) {
    out.assign(b.msgs[0].text.data(), b.msgs[0].text.size());
    return out;
  }

  // Two or more (or one-with-more-waiting): render each as a delimited block, in
  // arrival order, so the model reads them as distinct messages with their sender.
  for (size_t k = 0; k < nmsg; ++k) {
    if (k) out += "\n\n";
    char hdr[48];
    std::snprintf(hdr, sizeof(hdr), "Message %zu of %zu", k + 1, nmsg);
    out += hdr;
    if (!b.msgs[k].chatFrom.empty()) {
      out += " from ";
      out += b.msgs[k].chatFrom;
    }
    out += ":\n";
    out += b.msgs[k].text;
  }
  if (b.capped)
    out += "\n\n(More messages from this chat are waiting and will be handled next.)";
  return out;
}

void MessageBatcher::clear() {
  chats_.clear();
  totalBytes_ = 0;
}

}  // namespace orch
}  // namespace nimbus
