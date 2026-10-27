#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// card_stream - a web download read from the SD card, sent without ever waiting for the
// card on the web task (nimbus/sys/web_guard.h) and without ending early while a tool
// holds the card.
//
// The web library's own streamed responses (ESPAsyncWebServer 3.12.1,
// AsyncAbstractResponse::write_send_buffs) gate every fill on "in-flight credits": a fill
// pass takes one, an acknowledgement gives one back, a poll gives none. A card read that
// finds the card held answers "try again"; that pass still takes its credit, queues
// nothing, and calls send() anyway, which tells AsyncTCP the connection now waits for an
// acknowledgement. With nothing in flight, two such passes leave no credit, nothing is
// ever acknowledged again, and AsyncTCP's 5 s acknowledgement timeout closes the
// connection: the client sees the download end early.
//
// A card stream keeps its own rules instead:
//   - flow control by bytes: it queues more only while the bytes sent and not yet
//     acknowledged stay under its pace, and never queues a sliver;
//   - a pass that finds the card held queues nothing, sends nothing and takes nothing:
//     the next acknowledgement, or the server's poll (every 500 ms while the socket has
//     room), tries again, and a waiting stream holds no staging buffer;
//   - opening the file is a step of the stream too, so a download that starts while a
//     tool holds the card waits for the card like any later read;
//   - the card waits of one request add up against a budget. Past it, a stream that has
//     sent nothing answers its source's busy answer (503 + Retry-After), and a stream
//     that has started is cut (reset), so the client sees a failed download, never a
//     short one that looks whole. A read error cuts the stream the same way.
//
// The device side (src/net/web_admission.cpp, admission::CardStreamResponse) adapts it
// to the web library and the card; test_card_stream drives it against a simulated TCP
// connection, including the library's credit rule as the counter-test.
namespace nimbus {
namespace net {

// What a source answers for one step.
enum class CardStep : uint8_t {
  Ready,    // open: the stream starts; head() is the response head
  Answer,   // open: no stream; answer() is a whole small answer (an error, a list)
  Data,     // fill: `n` body bytes are in the buffer
  Busy,     // open or fill: the card is held; nothing happened, ask again later
  End,      // fill: the body is complete
  Fail,     // open or fill: the body cannot be completed; the stream is cut
};

// How fast a stream may go: bytes sent and not yet acknowledged, at most, and body
// bytes per fill, at most.
struct CardPace {
  size_t maxInFlight;
  size_t maxChunk;
};

// One TCP segment (CONFIG_LWIP_TCP_MSS on the device).
inline constexpr size_t kCardSegment = 1436;
// One fill's staging buffer: two segments, the web library's own send buffer size.
inline constexpr size_t kCardStreamBuf = 2 * kCardSegment;
// Chunked framing around one fill: four hex digits and CRLF before, CRLF after.
inline constexpr size_t kCardChunkHead = 6;
inline constexpr size_t kCardChunkFrame = 8;
// The smallest fill worth a chunk: below it, wait for the next acknowledgement.
inline constexpr size_t kCardMinFill = 512;
// Full speed: the socket's whole send buffer in flight (two staging buffers, 5,744 B).
inline constexpr CardPace kCardPaceFull = {2 * kCardStreamBuf, kCardStreamBuf - kCardChunkFrame};
// A trickle (nimbus/cloud/relay_heap.h, internal memory short or a turn in flight):
// about two segments in flight, one segment per fill.
inline constexpr CardPace kCardPaceTrickle = {2 * kCardSegment, kCardSegment};
// The card waits one download may add up: a LAN browser waits out any tool's hold up to
// this; the cloud relay's replay gets less, inside its own 15 s read limit.
inline constexpr uint32_t kCardStreamWaitMs = 60000;
inline constexpr uint32_t kCardStreamRelayWaitMs = 10000;

static_assert(kCardMinFill < kCardPaceTrickle.maxChunk, "a trickle fill is never a sliver");
static_assert(kCardPaceFull.maxChunk + kCardChunkFrame <= kCardStreamBuf, "a framed fill fits the buffer");

// Where the stream's bytes go: the connection.
class CardSink {
 public:
  virtual ~CardSink() = default;
  virtual size_t space() = 0;                            // bytes the socket can take now
  virtual size_t add(const char* data, size_t len) = 0;  // queue; returns what it took
  virtual void send() = 0;                               // push the queued bytes out
  virtual void abort() = 0;                              // reset: the download failed
  // Nothing queued and nothing unacknowledged in the socket. A stream that counts bytes
  // in flight trusts this over its own count, so a lost acknowledgement event (the
  // server drops one when it cannot allocate it) never stalls it.
  virtual bool drained() { return false; }
};

// Where they come from: the card. Every call runs on the web task and may only try the
// card lock (web_guard.h), answering Busy when it is held.
class CardSource {
 public:
  virtual ~CardSource() = default;
  virtual CardStep open() = 0;
  // After Ready: the response head.
  virtual std::string head() = 0;
  // After Answer: the whole answer (head and body).
  virtual std::string answer() = 0;
  // After Ready: how fast the body may go.
  virtual CardPace pace() = 0;
  // Up to `cap` body bytes into `buf`; Data sets `n`.
  virtual CardStep fill(uint8_t* buf, size_t cap, size_t& n) = 0;
  // The whole answer when the wait budget runs out before anything was sent.
  virtual std::string busyAnswer() = 0;
};

// The staging buffer's home (the device keeps it off internal SRAM).
struct CardBufferAlloc {
  uint8_t* (*get)(size_t bytes);
  void (*put)(uint8_t* buf);
};
CardBufferAlloc cardHeapAlloc();

class CardStream {
 public:
  enum class Phase : uint8_t { Open, Head, Body, Done, Failed };

  // `chunked`: HTTP/1.1 chunked framing; false (HTTP/1.0): raw, ended by the close.
  CardStream(CardSource& src, bool chunked, uint32_t waitBudgetMs, CardBufferAlloc alloc = cardHeapAlloc());
  ~CardStream();
  CardStream(const CardStream&) = delete;
  CardStream& operator=(const CardStream&) = delete;

  // One pass, from the server's first call, an acknowledgement of `acked` bytes, or a
  // poll (acked 0). Queues what the pace and the socket allow, sends it, and returns the
  // bytes queued. A failed stream is aborted once, here.
  size_t pass(CardSink& sink, size_t acked, uint32_t nowMs);

  Phase phase() const { return phase_; }
  bool finished() const { return phase_ == Phase::Done || phase_ == Phase::Failed; }
  // Nothing in flight and nothing staged: a finished raw stream may close now.
  bool idle() const { return inFlight_ == 0 && pendLen_ == 0; }
  size_t inFlight() const { return inFlight_; }
  // The card waits so far (a wait in progress included), and how they ended.
  uint32_t waitedMs(uint32_t nowMs) const { return waited_ + (waiting_ ? nowMs - waitFrom_ : 0); }
  uint32_t stalls() const { return stalls_; }
  uint32_t busyPasses() const { return busyPasses_; }
  size_t bodyBytes() const { return bodyBytes_; }
  bool gaveUp() const { return gaveUp_; }   // the wait budget ran out
  bool holdsBuffer() const { return buf_ != nullptr; }

 private:
  void step(uint32_t now);
  void waitPass(uint32_t now);
  void endWait(uint32_t now);
  void startAnswer(std::string bytes);
  size_t pushHead(CardSink& s);
  size_t pushBody(CardSink& s, uint32_t now);
  bool drainPending(CardSink& s, size_t& queued);
  size_t fillCap(CardSink& s, size_t queued);
  bool fillOne(CardSink& s, size_t queued, uint32_t now);
  bool settle(CardStep st, size_t n, size_t cap);
  void frame(size_t n);
  void finish();
  void dropBuffer();

  CardSource& src_;
  CardBufferAlloc alloc_;
  std::string head_;
  size_t headAt_ = 0;
  CardPace pace_ = kCardPaceFull;
  uint8_t* buf_ = nullptr;
  size_t pendLen_ = 0;   // staged bytes (framed), of which pendAt_ are queued
  size_t pendAt_ = 0;
  size_t inFlight_ = 0;
  size_t bodyBytes_ = 0;
  uint32_t budget_;
  uint32_t waited_ = 0;
  uint32_t waitFrom_ = 0;
  uint32_t stalls_ = 0;
  uint32_t busyPasses_ = 0;
  Phase phase_ = Phase::Open;
  bool chunked_;
  bool answerOnly_ = false;
  bool ended_ = false;
  bool waiting_ = false;
  bool gaveUp_ = false;
  bool aborted_ = false;
};

// What the connection does after a pass; the device adapter carries it out.
//   - Abort: the pass asked for a reset (past the wait budget, or a read error);
//   - OweAbort: the same during the server's first call (_respond), inside which the
//     connection must not go away: the reset is carried out at the next pass;
//   - Close: a finished stream with nothing in flight, at a poll (the library closes a
//     finished response only at an acknowledgement, and a raw HTTP/1.0 stream can
//     finish when none is coming).
enum class CardConn : uint8_t { Keep, Close, Abort, OweAbort };
CardConn afterPass(const CardStream& s, bool aborted, bool first, size_t acked);

// One read of a card file that had `sizeAtOpen` bytes when it opened: `got` bytes came
// back into a buffer of `cap`, and `delivered` counts what was delivered so far (it is
// updated). Data while bytes come; End once the reads stop at the size it opened with
// (a log that grows meanwhile is read to that size); Fail when they stop short of it
// (a read error) or overran the buffer: never a short download that looks whole.
CardStep fileFillStep(size_t got, size_t cap, size_t& delivered, size_t sizeAtOpen);
// One read of an archived day's ZIP: `n` bytes, or 0 once it is complete or a read
// failed (`failed`).
CardStep zipFillStep(size_t n, bool failed);

}  // namespace net
}  // namespace nimbus
