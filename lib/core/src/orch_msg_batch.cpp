#include "nimbus/orch/msg_batch.h"

#include <cstdio>

// CUM-398 rapid-message batcher, with the command boundary. See msg_batch.h. Pure
// logic, host-tested; the device seam (src/agent/telegram.cpp) fills it on the tg_poll
// task and emits one turn per bucket. No concurrency, no I/O.
namespace nimbus {
namespace orch {

static bool sameChat(const PsString& a, const std::string& b) {
  return a.size() == b.size() && std::char_traits<char>::compare(a.c_str(), b.data(), b.size()) == 0;
}

MessageBatcher::Bucket* MessageBatcher::lastBucket(const std::string& chatId) {
  if (contiguous_)   // only the drain's newest bucket, and only if it is this chat's
    return (!buckets_.empty() && sameChat(buckets_.back().chatId, chatId)) ? &buckets_.back()
                                                                           : nullptr;
  for (size_t i = buckets_.size(); i-- > 0;)
    if (sameChat(buckets_[i].chatId, chatId)) return &buckets_[i];
  return nullptr;
}

bool MessageBatcher::full(const Bucket& b) {
  return b.msgs.size() >= kBatchMaxMsgsPerChat || b.bytes >= kBatchMaxBytesPerChat;
}

void MessageBatcher::append(Bucket& b, const std::string& from, const std::string& text) {
  b.msgs.emplace_back();
  Msg& m = b.msgs.back();
  m.chatFrom.assign(from.data(), from.size());
  m.text.assign(text.data(), text.size());
  b.bytes += text.size();
  totalBytes_ += text.size();
}

// Another message for a chat whose newest bucket is still open: enforce the per-turn
// count and byte caps and the shared total-bytes cap. A trip marks the bucket capped
// (its render says more are waiting) and defers this message; the caller re-serves it
// next drain.
BatchAdd MessageBatcher::addToOpen(Bucket& b, const std::string& from, const std::string& text) {
  const size_t n = text.size();
  if (b.msgs.size() >= kBatchMaxMsgsPerChat || b.bytes + n > kBatchMaxBytesPerChat ||
      totalBytes_ + n > kBatchMaxTotalBytes) {
    b.capped = true;
    return BatchAdd::Deferred;
  }
  append(b, from, text);
  return full(b) ? BatchAdd::TurnFull : BatchAdd::Accepted;
}

// A new turn: the chat's first message in this drain, a slash command, or the first
// message after one. Two ceilings gate it; either means "re-serve this message, it did
// not fit this drain", never a silent drop. A lone message always fits a fresh
// accumulator (one Telegram or injected message is smaller than the total cap), so a
// message can never be stuck. When the chat already has an open bucket, that bucket is
// marked capped so its render says more are waiting; a command's own bucket is never
// marked, so a command always renders byte for byte.
BatchAdd MessageBatcher::addNewBucket(Bucket* last, const std::string& chatId,
                                      const std::string& from, const std::string& text,
                                      bool command, int32_t updateId) {
  if (buckets_.size() >= kBatchMaxChats || totalBytes_ + text.size() > kBatchMaxTotalBytes) {
    if (last && !last->command) last->capped = true;
    return BatchAdd::Deferred;
  }
  buckets_.emplace_back();   // may move the buckets: `last` is not used past this point
  Bucket& b = buckets_.back();
  b.chatId.assign(chatId.data(), chatId.size());
  b.turnFrom.assign(from.data(), from.size());
  b.command = command;
  b.firstId = updateId;
  append(b, from, text);
  if (command) return BatchAdd::CommandBoundary;
  return full(b) ? BatchAdd::TurnFull : BatchAdd::Accepted;
}

BatchAdd MessageBatcher::add(const std::string& chatId, const std::string& from,
                             const std::string& text, int32_t updateId) {
  const bool command = isCommandText(text);
  Bucket* last = lastBucket(chatId);
  if (last && !last->command && !command) return addToOpen(*last, from, text);
  return addNewBucket(last, chatId, from, text, command, updateId);
}

PsString MessageBatcher::render(size_t i) const {
  const Bucket& b = buckets_[i];
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
  buckets_.clear();
  totalBytes_ = 0;
  contiguous_ = false;
}

}  // namespace orch
}  // namespace nimbus
