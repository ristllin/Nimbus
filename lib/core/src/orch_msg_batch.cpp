#include "nimbus/orch/msg_batch.h"

#include <cstdio>

// CUM-398 rapid-message batcher, with the command boundary, media and voice segments
// (albums, and a text with the photo or voice note sent after it, as one turn). See msg_batch.h. Pure logic, host-tested; the device
// seam (src/agent/telegram.cpp) fills it on the tg_poll task and emits one turn per
// bucket. No concurrency, no I/O.
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
  return b.msgs.size() >= kBatchMaxMsgsPerChat || b.bytes >= kBatchMaxBytesPerChat ||
         b.media >= kBatchMaxMediaPerChat;
}

const MessageBatcher::Msg& MessageBatcher::mediaSeg(size_t i, size_t k) const {
  const Bucket& b = buckets_[i];
  for (const Msg& m : b.msgs)
    if (m.kind == SegKind::Media && k-- == 0) return m;
  return b.msgs.back();   // out of range: never reached by the device (k < mediaCountAt)
}

MessageBatcher::Msg* MessageBatcher::mediaSegMut(size_t i, size_t k) {
  if (i >= buckets_.size()) return nullptr;
  for (Msg& m : buckets_[i].msgs)
    if (m.kind == SegKind::Media && k-- == 0) return &m;
  return nullptr;
}

void MessageBatcher::setMediaText(size_t i, size_t k, const std::string& text) {
  if (Msg* m = mediaSegMut(i, k)) m->segText.assign(text.data(), text.size());
}

int MessageBatcher::voiceSlotAt(size_t i) const {
  for (const Msg& m : buckets_[i].msgs)
    if (m.kind == SegKind::Voice) return m.slot;
  return -1;
}

void MessageBatcher::setVoiceText(size_t i, const std::string& transcript) {
  if (i >= buckets_.size()) return;
  for (Msg& m : buckets_[i].msgs)
    if (m.kind == SegKind::Voice) m.text.assign(transcript.data(), transcript.size());
}

void MessageBatcher::append(Bucket& b, const NewMsg& m) {
  b.msgs.emplace_back();
  Msg& out = b.msgs.back();
  out.kind = m.kind;
  out.chatFrom.assign(m.from.data(), m.from.size());
  out.text.assign(m.text.data(), m.text.size());
  out.sentAt.assign(m.sentAt.data(), m.sentAt.size());
  out.slot = m.slot;
  out.updateId = m.updateId;
  b.bytes += m.text.size();
  totalBytes_ += m.text.size();
  if (m.kind != SegKind::Text) ++mediaTotal_;
  if (m.kind == SegKind::Media) ++b.media;
  if (m.kind == SegKind::Voice) b.voice = true;
}

// Another message for a chat whose newest bucket is still open: enforce the per-turn
// count, byte and media caps and the shared total-bytes cap. A trip marks the bucket
// capped (its render says more are waiting) and defers this message; the caller
// re-serves it next drain.
BatchAdd MessageBatcher::addToOpen(Bucket& b, const NewMsg& m) {
  const size_t n = m.text.size();
  const bool media = m.kind == SegKind::Media;
  if (b.msgs.size() >= kBatchMaxMsgsPerChat || b.bytes + n > kBatchMaxBytesPerChat ||
      totalBytes_ + n > kBatchMaxTotalBytes || (media && b.media >= kBatchMaxMediaPerChat)) {
    b.capped = true;
    return BatchAdd::Deferred;
  }
  append(b, m);
  return full(b) ? BatchAdd::TurnFull : BatchAdd::Accepted;
}

// A new turn: the chat's first message in this drain, a command or the first message
// after one, or a second voice note. Two ceilings gate it; either means "re-serve this
// message, it did not fit this drain", never a silent drop. A lone message always fits a
// fresh accumulator (one Telegram or injected message is smaller than the total cap), so
// a message can never be stuck. When the chat already has an open bucket, that bucket is
// marked capped so its render says more are waiting; a sealed bucket (a command) is
// never marked, so it always renders byte for byte.
BatchAdd MessageBatcher::addNewBucket(Bucket* last, const std::string& chatId, const NewMsg& m,
                                      bool command) {
  if (buckets_.size() >= kBatchMaxChats || totalBytes_ + m.text.size() > kBatchMaxTotalBytes) {
    if (last && !sealed(*last)) last->capped = true;
    return BatchAdd::Deferred;
  }
  const bool secondVoice = m.kind == SegKind::Voice && last && last->voice;
  buckets_.emplace_back();   // may move the buckets: `last` is not used past this point
  Bucket& b = buckets_.back();
  b.chatId.assign(chatId.data(), chatId.size());
  b.turnFrom.assign(m.from.data(), m.from.size());
  b.command = command;
  b.firstId = m.updateId;
  append(b, m);
  if (command) return BatchAdd::CommandBoundary;
  if (secondVoice) return BatchAdd::VoiceBoundary;
  return full(b) ? BatchAdd::TurnFull : BatchAdd::Accepted;
}

BatchAdd MessageBatcher::addAny(const std::string& chatId, const NewMsg& m, bool command) {
  // The drain's media slots: a media item or voice note that would need one more than
  // the device has is Deferred before it touches any bucket.
  if (m.kind != SegKind::Text && mediaTotal_ >= kBatchMaxMediaPerDrain) {
    if (Bucket* last = lastBucket(chatId))
      if (!sealed(*last)) last->capped = true;
    return BatchAdd::Deferred;
  }
  Bucket* last = lastBucket(chatId);
  // One voice note per turn: a second one starts the next bucket.
  const bool boundary = command || (m.kind == SegKind::Voice && last && last->voice);
  if (last && !sealed(*last) && !boundary) return addToOpen(*last, m);
  return addNewBucket(last, chatId, m, command);
}

BatchAdd MessageBatcher::add(const std::string& chatId, const std::string& from,
                             const std::string& text, int32_t updateId, const std::string& sentAt) {
  return addAny(chatId, NewMsg{SegKind::Text, from, text, sentAt, -1, updateId}, isCommandText(text));
}

BatchAdd MessageBatcher::addMedia(const std::string& chatId, const std::string& from,
                                  const std::string& caption, int slot, int32_t updateId) {
  static const std::string kNone;
  return addAny(chatId, NewMsg{SegKind::Media, from, caption, kNone, slot, updateId}, false);
}

BatchAdd MessageBatcher::addVoice(const std::string& chatId, const std::string& from, int slot,
                                  int32_t updateId) {
  static const std::string kNone;
  return addAny(chatId, NewMsg{SegKind::Voice, from, kNone, kNone, slot, updateId}, false);
}

// One message's body inside a turn: a media item's segment text and its caption, or
// the text (a late one prefixed with its send time).
void MessageBatcher::renderBody(PsString& out, const Msg& m) {
  if (m.kind == SegKind::Media) {
    out += m.segText;
    if (!m.text.empty()) {
      out += "\n\nThey wrote: ";
      out += m.text;
    }
    return;
  }
  if (!m.sentAt.empty()) {
    out += "(sent ";
    out += m.sentAt;
    out += ") ";
  }
  out += m.text;
}

PsString MessageBatcher::render(size_t i) const {
  const Bucket& b = buckets_[i];
  const size_t nmsg = b.msgs.size();
  PsString out;

  // The common case: one message, nothing shed. Emit its body alone, so a normal
  // single text is byte-for-byte identical to the pre-batching turn, a single photo
  // reads as before (label, description, "They wrote:"), and a voice note is its
  // transcript.
  if (nmsg == 1 && !b.capped) {
    renderBody(out, b.msgs[0]);
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
    if (b.msgs[k].kind == SegKind::Voice) out += "(voice note) ";
    renderBody(out, b.msgs[k]);
  }
  if (b.capped)
    out += "\n\n(More messages from this chat are waiting and will be handled next.)";
  return out;
}

void MessageBatcher::clear() {
  buckets_.clear();
  totalBytes_ = 0;
  mediaTotal_ = 0;
  contiguous_ = false;
}

}  // namespace orch
}  // namespace nimbus
