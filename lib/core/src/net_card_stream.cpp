#include "nimbus/net/card_stream.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace nimbus {
namespace net {

namespace {

uint8_t* heapGet(size_t bytes) { return new (std::nothrow) uint8_t[bytes]; }
void heapPut(uint8_t* buf) { delete[] buf; }

constexpr char kHex[] = "0123456789abcdef";
constexpr char kLastChunk[] = "0\r\n\r\n";

}  // namespace

CardBufferAlloc cardHeapAlloc() { return CardBufferAlloc{&heapGet, &heapPut}; }

CardStream::CardStream(CardSource& src, bool chunked, uint32_t waitBudgetMs, CardBufferAlloc alloc)
    : src_(src), alloc_(alloc), budget_(waitBudgetMs), chunked_(chunked) {}

CardStream::~CardStream() { dropBuffer(); }

size_t CardStream::pass(CardSink& sink, size_t acked, uint32_t nowMs) {
  inFlight_ -= std::min(acked, inFlight_);
  if (inFlight_ && sink.drained()) inFlight_ = 0;   // an acknowledgement event went missing
  if (phase_ == Phase::Open) step(nowMs);
  size_t queued = 0;
  if (phase_ == Phase::Head) queued += pushHead(sink);
  if (phase_ == Phase::Body) queued += pushBody(sink, nowMs);
  if (phase_ == Phase::Failed) {
    dropBuffer();
    if (!aborted_) {
      aborted_ = true;
      sink.abort();
    }
    return 0;
  }
  // Only real bytes are sent: a pass that queued nothing must not tell the connection
  // it waits for an acknowledgement (AsyncTCP's ack timeout counts from a send).
  if (queued) {
    sink.send();
    inFlight_ += queued;
  }
  return queued;
}

// The open step: the card is tried by the source; busy keeps the stream waiting.
void CardStream::step(uint32_t now) {
  const CardStep st = src_.open();
  if (st == CardStep::Busy) return waitPass(now);
  endWait(now);
  if (st == CardStep::Ready) {
    head_ = src_.head();
    pace_ = src_.pace();   // fillCap keeps every fill inside the buffer whatever the pace
    phase_ = Phase::Head;
  } else if (st == CardStep::Answer) {
    startAnswer(src_.answer());
  } else {
    phase_ = Phase::Failed;   // Fail, or a step that has no meaning at the open
  }
}

// A pass that found the card held (or no staging memory): counted, and past the
// budget the stream gives up: a busy answer while nothing is sent, a cut after.
void CardStream::waitPass(uint32_t now) {
  if (!waiting_) {
    waiting_ = true;
    waitFrom_ = now;
    ++stalls_;
  }
  ++busyPasses_;
  if (waitedMs(now) < budget_) return;
  endWait(now);
  gaveUp_ = true;
  if (phase_ == Phase::Open) startAnswer(src_.busyAnswer());
  else phase_ = Phase::Failed;
}

void CardStream::endWait(uint32_t now) {
  if (!waiting_) return;
  waited_ += now - waitFrom_;
  waiting_ = false;
}

void CardStream::startAnswer(std::string bytes) {
  head_ = std::move(bytes);
  headAt_ = 0;
  answerOnly_ = true;
  phase_ = Phase::Head;
}

size_t CardStream::pushHead(CardSink& s) {
  const size_t n = headAt_ < head_.size() ? s.add(head_.data() + headAt_, head_.size() - headAt_) : 0;
  headAt_ += n;
  if (headAt_ < head_.size()) return n;   // the socket is full: the rest on the next pass
  std::string().swap(head_);
  if (answerOnly_) phase_ = Phase::Done;
  else phase_ = Phase::Body;
  return n;
}

size_t CardStream::pushBody(CardSink& s, uint32_t now) {
  size_t queued = 0;
  while (phase_ == Phase::Body) {
    if (pendLen_ && !drainPending(s, queued)) break;   // the socket is full: next ack
    if (ended_) {
      finish();
      break;
    }
    if (!fillOne(s, queued, now)) break;
  }
  return queued;
}

bool CardStream::drainPending(CardSink& s, size_t& queued) {
  const size_t n = s.add(reinterpret_cast<const char*>(buf_ + pendAt_), pendLen_ - pendAt_);
  queued += n;
  pendAt_ += n;
  if (pendAt_ < pendLen_) return false;
  pendAt_ = pendLen_ = 0;
  return true;
}

// The body bytes the next fill may take: what the pace and the socket leave after the
// framing, or 0 for a sliver (wait for an acknowledgement instead).
size_t CardStream::fillCap(CardSink& s, size_t queued) {
  const size_t flight = inFlight_ + queued;
  if (flight >= pace_.maxInFlight) return 0;
  const size_t frameBytes = chunked_ ? kCardChunkFrame : 0;
  const size_t room = std::min(s.space(), pace_.maxInFlight - flight);
  const size_t cap = std::min({room > frameBytes ? room - frameBytes : 0, kCardStreamBuf - frameBytes, pace_.maxChunk});
  return cap < std::min(kCardMinFill, pace_.maxChunk) ? 0 : cap;
}

// One fill. Returns whether the body loop may go on (false: wait for an
// acknowledgement or a poll, or the stream failed).
bool CardStream::fillOne(CardSink& s, size_t queued, uint32_t now) {
  const size_t cap = fillCap(s, queued);
  if (!cap) return false;
  if (!buf_) buf_ = alloc_.get(kCardStreamBuf);
  if (!buf_) {
    waitPass(now);   // no staging memory: wait like a busy card, inside the same budget
    return false;
  }
  size_t n = 0;
  const CardStep st = src_.fill(buf_ + (chunked_ ? kCardChunkHead : 0), cap, n);
  if (st == CardStep::Busy) {
    dropBuffer();   // a waiting stream holds no staging memory
    waitPass(now);
    return false;
  }
  endWait(now);
  return settle(st, n, cap);
}

// What a fill that ran answered: a chunk, the end, or a failure (a read error, a step
// a fill cannot answer, or a source that overran its buffer: nothing it wrote is
// trusted).
bool CardStream::settle(CardStep st, size_t n, size_t cap) {
  const bool known = st == CardStep::Data || st == CardStep::End;
  if (n > cap || !known) {
    phase_ = Phase::Failed;
    return false;
  }
  if (st == CardStep::Data && n > 0) {
    frame(n);
    return true;
  }
  ended_ = true;   // End (or Data with nothing): the last chunk
  if (chunked_) {
    memcpy(buf_, kLastChunk, sizeof(kLastChunk) - 1);
    pendLen_ = sizeof(kLastChunk) - 1;
  }
  return true;
}

void CardStream::frame(size_t n) {
  bodyBytes_ += n;
  if (!chunked_) {
    pendLen_ = n;
    return;
  }
  buf_[0] = (uint8_t)kHex[(n >> 12) & 0xF];
  buf_[1] = (uint8_t)kHex[(n >> 8) & 0xF];
  buf_[2] = (uint8_t)kHex[(n >> 4) & 0xF];
  buf_[3] = (uint8_t)kHex[n & 0xF];
  buf_[4] = '\r';
  buf_[5] = '\n';
  buf_[kCardChunkHead + n] = '\r';
  buf_[kCardChunkHead + n + 1] = '\n';
  pendLen_ = n + kCardChunkFrame;
}

void CardStream::finish() {
  phase_ = Phase::Done;
  dropBuffer();
}

void CardStream::dropBuffer() {
  if (!buf_) return;
  alloc_.put(buf_);
  buf_ = nullptr;
  pendLen_ = pendAt_ = 0;
}

CardConn afterPass(const CardStream& s, bool aborted, bool first, size_t acked) {
  if (aborted) return first ? CardConn::OweAbort : CardConn::Abort;
  if (!first && !acked && s.phase() == CardStream::Phase::Done && s.idle()) return CardConn::Close;
  return CardConn::Keep;
}

CardStep fileFillStep(size_t got, size_t cap, size_t& delivered, size_t sizeAtOpen) {
  if (got > cap) return CardStep::Fail;
  if (got > 0) {
    delivered += got;
    return CardStep::Data;
  }
  return delivered >= sizeAtOpen ? CardStep::End : CardStep::Fail;
}

CardStep zipFillStep(size_t n, bool failed) {
  if (n) return CardStep::Data;
  return failed ? CardStep::Fail : CardStep::End;
}

}  // namespace net
}  // namespace nimbus
