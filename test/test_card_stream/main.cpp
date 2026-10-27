// test_card_stream - a card download never ends early while a tool holds the card, and
// never waits for the card on the web task (nimbus/net/card_stream.h).
//
// The stream runs against a simulated connection that keeps AsyncTCP's rules: a send
// buffer of 5,744 B, acknowledgements a round trip after a send, the server's poll every
// 500 ms while the socket has room, and the 5 s acknowledgement timeout that closes a
// connection whose last send is newer than its last acknowledgement. The card is a
// schedule of holds. The client decodes what arrived and compares it byte for byte.
//
// The counter-test runs the same holds through the web library's own rule (one
// in-flight credit per fill pass, a "try again" pass included, one back per
// acknowledgement, none per poll; ESPAsyncWebServer 3.12.1 write_send_buffs): a hold of
// about a second with nothing in flight ends that download early, which is the field
// bug this stream exists for. The source checks keep every card download on the stream.
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/net/card_stream.h"

using namespace nimbus::net;

void setUp() {}
void tearDown() {}

namespace {

constexpr size_t kSndBuf = 5744;        // CONFIG_LWIP_TCP_SND_BUF_DEFAULT
constexpr uint32_t kPollMs = 500;       // lwIP's coarse timer: AsyncTCP's poll
constexpr uint32_t kAckTimeoutMs = 5000;   // CONFIG_ASYNC_TCP_MAX_ACK_TIME
constexpr uint32_t kRttMs = 20;
// About 7 s at the simulated rate (5,744 B per 20 ms round trip): every hold below
// starts while the body is still going out.
constexpr size_t kBig = 2000000;
const char kHead[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
const char kBusy[] = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 5\r\nContent-Length: 17\r\n\r\n{\"error\":\"busy\"}\n";

struct Hold {
  uint32_t from, to;
};

// The connection, as AsyncTCP and lwIP keep it.
struct Net : CardSink {
  std::string unsent, wire, received;
  uint32_t now = 0, txLast = 0, rxLastAck = 0, sentAt = 0;
  size_t addCap = (size_t)-1;   // < full: the socket takes only part of an add
  int sends = 0, emptySends = 0, aborts = 0;
  int lostAckEvents = 0;        // acknowledgements the server never reports (no event memory)
  bool reportsDrained = true;
  size_t peakInFlight = 0;

  size_t space() override { return kSndBuf - unsent.size() - wire.size(); }
  size_t add(const char* d, size_t n) override {
    const size_t k = std::min({n, space(), addCap});
    unsent.append(d, k);
    return k;
  }
  void send() override {
    ++sends;
    if (unsent.empty()) ++emptySends;
    txLast = now;   // AsyncClient::send stamps the send whether or not bytes went
    if (wire.empty()) sentAt = now;
    wire += unsent;
    unsent.clear();
    peakInFlight = std::max(peakInFlight, wire.size());
  }
  void abort() override { ++aborts; }
  bool drained() override { return reportsDrained && unsent.empty() && wire.empty(); }
  size_t ack(size_t most) {
    const size_t n = std::min(most, wire.size());
    received += wire.substr(0, n);
    wire.erase(0, n);
    rxLastAck = now;
    sentAt = now;
    return n;
  }
  // AsyncClient::_poll: the last send is newer than the last acknowledgement, 5 s ago.
  bool ackTimedOut() const { return (int32_t)(txLast - rxLastAck) > 0 && now - txLast >= kAckTimeoutMs; }
};

// The card: a file of `size` bytes and a schedule of holds.
struct Card : CardSource {
  std::vector<Hold> holds;
  std::string body;
  const uint32_t& clock;
  size_t at = 0, failAt = (size_t)-1, served = 0;
  bool answer404 = false;
  CardPace pacing = kCardPaceFull;
  int opens = 0, fills = 0, busyAnswers = 0;

  Card(size_t size, const uint32_t& now) : clock(now) {
    for (size_t i = 0; i < size; ++i) body.push_back((char)('a' + (i * 7 + i / 251) % 26));
  }
  bool held() const {
    for (const Hold& h : holds)
      if (clock >= h.from && clock < h.to) return true;
    return false;
  }
  CardStep open() override {
    ++opens;
    if (held()) return CardStep::Busy;
    return answer404 ? CardStep::Answer : CardStep::Ready;
  }
  std::string head() override { return kHead; }
  std::string answer() override { return "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"; }
  CardPace pace() override { return pacing; }
  CardStep fill(uint8_t* buf, size_t cap, size_t& n) override {
    ++fills;
    if (held()) return CardStep::Busy;
    if (at >= failAt) return CardStep::Fail;
    if (at >= body.size()) return CardStep::End;
    n = std::min(cap, body.size() - at);
    memcpy(buf, body.data() + at, n);
    at += n;
    served = std::max(served, n);
    return CardStep::Data;
  }
  std::string busyAnswer() override {
    ++busyAnswers;
    return kBusy;
  }
};

// One response, driven the way AsyncWebServerRequest drives it.
struct Responder {
  virtual ~Responder() = default;
  virtual size_t pass(Net& net, size_t acked) = 0;
  virtual bool finished() const = 0;
  virtual bool closesOnPoll() const { return false; }
};

struct StreamResponder : Responder {
  CardStream s;
  StreamResponder(Card& c, bool chunked, uint32_t budget, CardBufferAlloc a = cardHeapAlloc())
      : s(c, chunked, budget, a) {}
  size_t pass(Net& net, size_t acked) override { return s.pass(net, acked, net.now); }
  bool finished() const override { return s.finished(); }
  // The device adapter's rule after a poll pass (afterPass), not a copy of it.
  bool closesOnPoll() const override { return afterPass(s, false, false, 0) == CardConn::Close; }
};

// The web library's rule, faithfully: ESPAsyncWebServer 3.12.1
// AsyncAbstractResponse::write_send_buffs, chunked branch (in-flight credits).
struct LibraryResponder : Responder {
  Card& card;
  size_t credit = 2, inFlight = 0;
  bool headSent = false, ended = false;
  std::string pending;
  explicit LibraryResponder(Card& c) : card(c) {}
  size_t pass(Net& net, size_t acked) override {
    if (acked) {
      ++credit;
      inFlight -= std::min(acked, inFlight);
    }
    if (!credit || kCardStreamBuf > net.space()) {
      if (acked) --credit;   // the ack is ignored; other in-flight data will bring one
      return 0;
    }
    size_t payload = 0;
    if (!headSent) {
      payload += net.add(kHead, sizeof(kHead) - 1);
      headSent = true;
    }
    payload += content(net);
    net.send();   // always, a "try again" pass included
    inFlight += payload;
    --credit;     // always, a "try again" pass included
    return payload;
  }
  size_t content(Net& net) {
    size_t payload = 0;
    do {
      if (!pending.empty()) {
        const size_t n = net.add(pending.data(), pending.size());
        payload += n;
        pending.erase(0, n);
        if (!pending.empty()) break;
      }
      const size_t win = net.space();
      if (win == 0 || ended || win < kCardSegment / 2) break;
      uint8_t buf[kCardStreamBuf];
      size_t n = 0;
      const CardStep st = card.fill(buf, std::min(kCardStreamBuf, win) - kCardChunkFrame, n);
      if (st == CardStep::Busy) continue;   // RESPONSE_TRY_AGAIN: nothing staged
      if (st != CardStep::Data) n = 0;
      char hex[8];
      snprintf(hex, sizeof(hex), "%04x\r\n", (unsigned)n);
      pending = std::string(hex) + std::string((const char*)buf, n) + "\r\n";
      ended = n == 0;
    } while (!pending.empty());
    return payload;
  }
  bool finished() const override { return ended && pending.empty(); }
};

struct Outcome {
  bool complete = false;   // finished, everything acknowledged
  bool timedOut = false;   // AsyncTCP's ack timeout closed it
  bool aborted = false;
  uint32_t endedMs = 0;
};

// A close: lwIP still delivers what is queued, then the FIN.
void closeGracefully(Net& net) {
  net.received += net.wire + net.unsent;
  net.wire.clear();
  net.unsent.clear();
}

// One 10 ms tick: an acknowledgement or a poll. Returns false when the connection ends
// (closed, timed out); `o.timedOut` says which.
bool tick(Responder& r, Net& net, bool halfAcks, Outcome& o) {
  size_t acked = 0;
  if (!net.wire.empty() && net.now >= net.sentAt + kRttMs)
    acked = net.ack(halfAcks && net.wire.size() > 2 ? net.wire.size() / 2 : net.wire.size());
  if (acked && net.lostAckEvents > 0) {   // TCP has it; the response never hears of it
    --net.lostAckEvents;
    return true;
  }
  bool close = false;
  if (acked) {
    if (!r.finished()) r.pass(net, acked);
    close = r.finished();
  } else if (net.now % kPollMs == 0) {
    if (net.ackTimedOut()) {
      o.timedOut = true;
      return false;
    }
    if (net.space() > 0) r.pass(net, 0);
    close = r.closesOnPoll();
  }
  if (!close || net.aborts) return true;
  closeGracefully(net);
  return false;
}

// Responds, then every 10 ms: an acknowledgement a round trip after a send (the whole
// window, or half of it when `halfAcks`), or the poll every 500 ms while the socket has
// room. As the library does, a response finished at an acknowledgement is closed there,
// and a finished stream with nothing in flight is closed at the poll (the device
// adapter does that).
Outcome run(Responder& r, Net& net, uint32_t maxMs, bool halfAcks = false) {
  Outcome o;
  r.pass(net, 0);
  for (net.now = 10; net.now < maxMs && !net.aborts; net.now += 10)
    if (!tick(r, net, halfAcks, o)) break;
  o.aborted = net.aborts > 0;
  o.endedMs = net.now;
  o.complete = !o.timedOut && !o.aborted && r.finished() && net.wire.empty() && net.unsent.empty();
  return o;
}

// The client: the head, then the chunked body; false when the framing is cut or wrong.
bool decodeChunked(const std::string& got, std::string& head, std::string& body) {
  const size_t h = got.find("\r\n\r\n");
  if (h == std::string::npos) return false;
  head = got.substr(0, h + 4);
  size_t at = h + 4;
  for (;;) {
    const size_t eol = got.find("\r\n", at);
    if (eol == std::string::npos) return false;
    const std::string hex = got.substr(at, eol - at);
    char* endp = nullptr;
    const size_t n = std::strtoul(hex.c_str(), &endp, 16);
    if (hex.empty() || *endp) return false;
    at = eol + 2;
    if (got.size() < at + n + 2 || got.compare(at + n, 2, "\r\n") != 0) return false;
    if (n == 0) return at + 2 == got.size();
    body.append(got, at, n);
    at += n + 2;
  }
}

void assertWhole(const Net& net, const Card& card) {
  std::string head, body;
  TEST_ASSERT_TRUE_MESSAGE(decodeChunked(net.received, head, body), "the chunked body is cut or malformed");
  TEST_ASSERT_EQUAL_STRING(kHead, head.c_str());
  TEST_ASSERT_EQUAL_size_t(card.body.size(), body.size());
  TEST_ASSERT_TRUE(body == card.body);
}

}  // namespace

// ---- the stream on a free card -------------------------------------------------------

static void test_a_free_card_streams_whole_at_the_pace() {
  Net net;
  Card card(100000, net.now);
  StreamResponder r(card, true, kCardStreamWaitMs);
  const Outcome o = run(r, net, 60000);
  TEST_ASSERT_TRUE(o.complete);
  assertWhole(net, card);
  TEST_ASSERT_EQUAL_INT(0, net.emptySends);   // never a send with nothing queued
  TEST_ASSERT_TRUE(net.peakInFlight <= kCardPaceFull.maxInFlight);
  TEST_ASSERT_TRUE(card.served <= kCardPaceFull.maxChunk);
  TEST_ASSERT_EQUAL_UINT32(0, r.s.stalls());
  TEST_ASSERT_FALSE(r.s.holdsBuffer());       // a finished stream keeps no staging buffer
}

static void test_a_trickle_keeps_two_segments_in_flight() {
  Net net;
  Card card(40000, net.now);
  card.pacing = kCardPaceTrickle;
  StreamResponder r(card, true, kCardStreamWaitMs);
  const Outcome o = run(r, net, 60000, /*halfAcks=*/true);
  TEST_ASSERT_TRUE(o.complete);
  assertWhole(net, card);
  // The head goes out with the first chunk; after it never more than the pace.
  TEST_ASSERT_TRUE(net.peakInFlight <= kCardPaceTrickle.maxInFlight + sizeof(kHead));
  TEST_ASSERT_TRUE(card.served <= kCardSegment);
}

// ---- the field bug: a hold with nothing in flight ------------------------------------

// The counter-test: the library's credit rule loses this download (a 1.2 s hold that
// starts with everything acknowledged), and the ack timeout ends it.
static void test_the_library_rule_ends_a_download_early_on_a_short_hold() {
  Net net;
  Card card(kBig, net.now);
  card.holds = {{1000, 2200}};
  LibraryResponder lib(card);
  const Outcome o = run(lib, net, 60000);
  TEST_ASSERT_TRUE_MESSAGE(o.timedOut, "the counter-test no longer reproduces the early end");
  TEST_ASSERT_FALSE(o.complete);
  TEST_ASSERT_TRUE(net.emptySends > 0);
}

// The same hold, and every hold length up to the budget at several points of the file,
// on the stream: whole, no ack timeout, no send during the hold, one card try per pass.
static void test_any_hold_inside_the_budget_is_waited_out() {
  const uint32_t lens[] = {300, 1200, 5000, 6000, 30000, 59000};
  const uint32_t starts[] = {0, 30, 1000, 4100};
  for (uint32_t len : lens) {
    for (uint32_t at : starts) {
      Net net;
      Card card(kBig, net.now);
      card.holds = {{at, at + len}};
      StreamResponder r(card, true, kCardStreamWaitMs);
      const Outcome o = run(r, net, at + len + 120000);
      char msg[96];
      snprintf(msg, sizeof(msg), "hold %u ms from %u ms", (unsigned)len, (unsigned)at);
      TEST_ASSERT_TRUE_MESSAGE(o.complete, msg);
      assertWhole(net, card);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, net.emptySends, msg);
      TEST_ASSERT_FALSE_MESSAGE(r.s.gaveUp(), msg);
      // No thrash: each wait is one card try per pass, and passes only come with the
      // poll while nothing is in flight (two per second, plus a few acknowledgements).
      TEST_ASSERT_TRUE_MESSAGE(r.s.busyPasses() <= len / kPollMs + 4, msg);
      TEST_ASSERT_TRUE_MESSAGE(r.s.waitedMs(net.now) <= len + kPollMs, msg);
    }
  }
}

// A download that starts during a hold waits for the open too: nothing is sent until
// the card is free, then the whole file with a 200.
static void test_a_download_started_during_a_hold_waits_for_the_open() {
  Net net;
  Card card(30000, net.now);
  card.holds = {{0, 20000}};
  StreamResponder r(card, true, kCardStreamWaitMs);
  r.pass(net, 0);
  TEST_ASSERT_EQUAL_INT(0, net.sends);
  TEST_ASSERT_TRUE(r.s.phase() == CardStream::Phase::Open);
  TEST_ASSERT_FALSE(r.s.holdsBuffer());
  const Outcome o = run(r, net, 80000);
  TEST_ASSERT_TRUE(o.complete);
  assertWhole(net, card);
  TEST_ASSERT_TRUE(card.opens >= 40 && card.opens <= 42);   // one try per poll
  TEST_ASSERT_EQUAL_INT(0, net.emptySends);
}

// While a stream waits it holds no staging buffer (a parked download costs no memory).
static void test_a_waiting_stream_holds_no_buffer() {
  Net net;
  Card card(kBig, net.now);
  card.holds = {{50, 10000}};
  StreamResponder r(card, true, kCardStreamWaitMs);
  r.pass(net, 0);
  for (net.now = 10; net.now < 5000; net.now += 10) {
    const size_t acked = net.wire.empty() ? 0 : net.ack(net.wire.size());
    if (acked || net.now % kPollMs == 0) r.pass(net, acked);
  }
  TEST_ASSERT_TRUE(r.s.stalls() == 1);
  TEST_ASSERT_FALSE(r.s.holdsBuffer());
  TEST_ASSERT_EQUAL_size_t(0, r.s.inFlight());
}

// The server drops an acknowledgement event when it cannot allocate it: the bytes are
// acknowledged in TCP but the response never hears of it. The stream trusts the
// socket's drained state over its own count and goes on; counting alone would stall.
static void test_a_lost_acknowledgement_event_never_stalls_the_stream() {
  Net net;
  Card card(200000, net.now);
  net.lostAckEvents = 3;
  StreamResponder r(card, true, kCardStreamWaitMs);
  const Outcome o = run(r, net, 60000);
  TEST_ASSERT_TRUE(o.complete);
  assertWhole(net, card);
  // Counter-test: a socket that cannot say it is drained stalls on the first lost event.
  Net net2;
  Card card2(200000, net2.now);
  net2.lostAckEvents = 1;
  net2.reportsDrained = false;
  StreamResponder r2(card2, true, kCardStreamWaitMs);
  const Outcome o2 = run(r2, net2, 20000);
  TEST_ASSERT_FALSE(o2.complete);
  TEST_ASSERT_FALSE(o2.timedOut);   // no send after the last acknowledgement: it just waits
}

// ---- the budget ------------------------------------------------------------------------

// Past the budget before anything was sent: the busy answer, whole, and no cut.
static void test_past_the_budget_before_the_head_answers_busy() {
  Net net;
  Card card(10000, net.now);
  card.holds = {{0, 1000000}};
  StreamResponder r(card, true, kCardStreamRelayWaitMs);
  const Outcome o = run(r, net, 60000);
  TEST_ASSERT_TRUE(o.complete);
  TEST_ASSERT_TRUE(r.s.gaveUp());
  TEST_ASSERT_EQUAL_STRING(kBusy, net.received.c_str());
  TEST_ASSERT_EQUAL_INT(1, card.busyAnswers);
  TEST_ASSERT_EQUAL_INT(0, net.aborts);
  TEST_ASSERT_TRUE(o.endedMs >= kCardStreamRelayWaitMs && o.endedMs <= kCardStreamRelayWaitMs + kPollMs + kRttMs);
}

// Past the budget after the head: cut, once, never a terminator (the client sees a
// failed download, not a short one that looks whole).
static void test_past_the_budget_mid_body_cuts_the_stream() {
  Net net;
  Card card(kBig, net.now);
  card.holds = {{2000, 1000000}};
  StreamResponder r(card, true, kCardStreamWaitMs);
  const Outcome o = run(r, net, 200000);
  TEST_ASSERT_TRUE(o.aborted);
  TEST_ASSERT_FALSE(o.timedOut);
  TEST_ASSERT_EQUAL_INT(1, net.aborts);
  TEST_ASSERT_TRUE(r.s.gaveUp());
  TEST_ASSERT_TRUE(r.s.phase() == CardStream::Phase::Failed);
  std::string head, body;
  TEST_ASSERT_FALSE(decodeChunked(net.received, head, body));
  TEST_ASSERT_TRUE(o.endedMs >= 2000 + kCardStreamWaitMs);
  TEST_ASSERT_TRUE(o.endedMs <= 2000 + kCardStreamWaitMs + 2 * kPollMs);
}

// The budget is the request's: holds that each fit add up, and past the sum it gives up.
static void test_the_budget_counts_every_wait_of_the_request() {
  Net net;
  Card card(2 * kBig, net.now);
  for (uint32_t i = 0; i < 8; ++i) card.holds.push_back({1000 + i * 12000, 1000 + i * 12000 + 9000});
  StreamResponder r(card, true, 30000);
  const Outcome o = run(r, net, 300000);
  TEST_ASSERT_TRUE(o.aborted);
  TEST_ASSERT_TRUE(r.s.gaveUp());
  TEST_ASSERT_TRUE(r.s.stalls() >= 4);
}

// ---- endings ----------------------------------------------------------------------------

static void test_a_read_error_cuts_the_stream() {
  Net net;
  Card card(100000, net.now);
  card.failAt = 50000;
  StreamResponder r(card, true, kCardStreamWaitMs);
  const Outcome o = run(r, net, 60000);
  TEST_ASSERT_TRUE(o.aborted);
  TEST_ASSERT_EQUAL_INT(1, net.aborts);
  TEST_ASSERT_FALSE(r.s.gaveUp());
  TEST_ASSERT_TRUE(net.received.find("\r\n0\r\n\r\n") == std::string::npos);
}

static void test_an_answer_goes_out_whole_with_no_body() {
  Net net;
  Card card(1000, net.now);
  card.answer404 = true;
  StreamResponder r(card, true, kCardStreamWaitMs);
  const Outcome o = run(r, net, 5000);
  TEST_ASSERT_TRUE(o.complete);
  TEST_ASSERT_EQUAL_STRING("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", net.received.c_str());
  TEST_ASSERT_EQUAL_INT(0, card.fills);
}

// HTTP/1.0: no framing; the end is the close, once nothing is left in flight.
static void test_a_raw_stream_ends_idle_with_the_bytes_only() {
  Net net;
  Card card(400000, net.now);
  card.holds = {{300, 3000}};
  StreamResponder r(card, false, kCardStreamWaitMs);
  const Outcome o = run(r, net, 60000);
  TEST_ASSERT_TRUE(o.complete);
  TEST_ASSERT_TRUE(r.s.phase() == CardStream::Phase::Done);
  TEST_ASSERT_TRUE(r.s.stalls() == 1);
  TEST_ASSERT_TRUE(net.received == std::string(kHead) + card.body);
}

// A socket that takes only part of an add: the rest stays staged and goes next.
static void test_a_partial_add_keeps_the_rest_staged() {
  Net net;
  Card card(50000, net.now);
  net.addCap = 1000;
  StreamResponder r(card, true, kCardStreamWaitMs);
  const Outcome o = run(r, net, 120000);
  TEST_ASSERT_TRUE(o.complete);
  assertWhole(net, card);
}

// No staging memory: waits like a busy card, inside the same budget, then streams.
static int g_allocFails = 0;
static uint8_t* flakyGet(size_t n) {
  if (g_allocFails > 0) {
    --g_allocFails;
    return nullptr;
  }
  return new uint8_t[n];
}
static void flakyPut(uint8_t* b) { delete[] b; }

static void test_no_staging_memory_waits_inside_the_budget() {
  Net net;
  Card card(30000, net.now);
  g_allocFails = 6;
  StreamResponder r(card, true, kCardStreamWaitMs, CardBufferAlloc{&flakyGet, &flakyPut});
  const Outcome o = run(r, net, 60000);
  TEST_ASSERT_TRUE(o.complete);
  assertWhole(net, card);
  TEST_ASSERT_EQUAL_UINT32(1, r.s.stalls());
  g_allocFails = 1000000;
  Net net2;
  Card card2(30000, net2.now);
  StreamResponder r2(card2, true, 5000, CardBufferAlloc{&flakyGet, &flakyPut});
  const Outcome o2 = run(r2, net2, 60000);
  TEST_ASSERT_TRUE(o2.aborted);   // the head was out: past the budget, cut
  TEST_ASSERT_TRUE(r2.s.gaveUp());
  g_allocFails = 0;
}

// ---- the adapter's decisions and the card reads ------------------------------------------

static void test_after_a_pass_the_connection_is_cut_closed_or_kept() {
  Net net;
  Card card(1000, net.now);
  CardStream s(card, true, kCardStreamWaitMs);
  // A reset asked for during the server's first call is owed, never carried out inside it.
  TEST_ASSERT_TRUE(afterPass(s, true, true, 0) == CardConn::OweAbort);
  TEST_ASSERT_TRUE(afterPass(s, true, false, 0) == CardConn::Abort);
  TEST_ASSERT_TRUE(afterPass(s, true, false, 900) == CardConn::Abort);
  TEST_ASSERT_TRUE(afterPass(s, false, false, 0) == CardConn::Keep);   // still opening
  s.pass(net, 0, 0);   // whole file and terminator queued: Done, but in flight
  TEST_ASSERT_TRUE(s.phase() == CardStream::Phase::Done);
  TEST_ASSERT_TRUE(afterPass(s, false, false, 0) == CardConn::Keep);   // its acknowledgement closes it
  s.pass(net, net.ack(net.wire.size()), 20);
  TEST_ASSERT_TRUE(s.idle());
  TEST_ASSERT_TRUE(afterPass(s, false, false, 0) == CardConn::Close);   // a poll with nothing in flight
  TEST_ASSERT_TRUE(afterPass(s, false, false, 64) == CardConn::Keep);   // an ack: the library closes it
  TEST_ASSERT_TRUE(afterPass(s, false, true, 0) == CardConn::Keep);     // never inside _respond
}

// A card file's reads: Data while bytes come, End only at the size it opened with,
// Fail when the reads stop short (a read error) or overrun the buffer.
static void test_a_card_file_ends_only_at_its_size() {
  size_t delivered = 0;
  TEST_ASSERT_TRUE(fileFillStep(1000, 2864, delivered, 1500) == CardStep::Data);
  TEST_ASSERT_TRUE(fileFillStep(500, 2864, delivered, 1500) == CardStep::Data);
  TEST_ASSERT_TRUE(fileFillStep(0, 2864, delivered, 1500) == CardStep::End);
  TEST_ASSERT_EQUAL_size_t(1500, delivered);
  delivered = 0;   // a read error halfway: the reads stop short of the size
  TEST_ASSERT_TRUE(fileFillStep(700, 2864, delivered, 1500) == CardStep::Data);
  TEST_ASSERT_TRUE(fileFillStep(0, 2864, delivered, 1500) == CardStep::Fail);
  delivered = 0;   // an empty file ends at once
  TEST_ASSERT_TRUE(fileFillStep(0, 2864, delivered, 0) == CardStep::End);
  delivered = 0;   // a log that grew after it opened is read to its size at open
  TEST_ASSERT_TRUE(fileFillStep(1600, 2864, delivered, 1500) == CardStep::Data);
  TEST_ASSERT_TRUE(fileFillStep(0, 2864, delivered, 1500) == CardStep::End);
  delivered = 0;   // a read that overran its buffer is not trusted
  TEST_ASSERT_TRUE(fileFillStep(3000, 2864, delivered, 9000) == CardStep::Fail);
  TEST_ASSERT_TRUE(zipFillStep(512, false) == CardStep::Data);
  TEST_ASSERT_TRUE(zipFillStep(0, false) == CardStep::End);
  TEST_ASSERT_TRUE(zipFillStep(0, true) == CardStep::Fail);
}

// ---- every card download is a card stream (source) -------------------------------------

static std::string readSource(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The body of the brace block that opens at or after `from`.
static std::string blockAfter(const std::string& code, size_t from) {
  const size_t open = code.find('{', from);
  if (open == std::string::npos) return std::string();
  int d = 0;
  for (size_t j = open; j < code.size(); ++j) {
    if (code[j] == '{') ++d;
    if (code[j] == '}' && --d == 0) return code.substr(open, j - open + 1);
  }
  return code.substr(open);
}

// The lambda shape of a library filler (AwsResponseFiller: buf, maxLen, index), with or
// without parameter names.
static const std::regex& fillerShape() {
  static const std::regex k(R"(\[[^\]]*\]\s*\(\s*uint8_t\s*\*\s*\w*\s*,\s*size_t\s*\w*\s*,\s*size_t\s*\w*\s*\))");
  return k;
}

// A library filler runs inside the library's credit gate, so none may touch the card: a
// card read that answers "try again" there ends the download early (the field bug), and
// one that answers 0 ends it short.
static void test_no_library_filler_reads_the_card() {
  // The shape sees the filler this replaced (its index parameter had no name) and the
  // in-memory one, and not a card stream's fill (whose count is a reference).
  for (const char* old : {"[f](uint8_t* buf, size_t maxLen, size_t) -> size_t {",
                          "[body, len](uint8_t* buf, size_t maxLen, size_t index) -> size_t {"})
    TEST_ASSERT_TRUE_MESSAGE(std::regex_search(std::string(old), fillerShape()), old);
  TEST_ASSERT_FALSE(std::regex_search(std::string("[f](uint8_t* buf, size_t cap, size_t& n) mutable"), fillerShape()));
  const std::regex& kFiller = fillerShape();
  const char* files[] = {"src/net/web_admission.cpp", "src/net/web_files.cpp", "src/net/web_tg_archive.cpp",
                         "src/net/errlog_routes.cpp", "src/net/web_memory.cpp", "src/net/webui.cpp"};
  size_t fillers = 0;
  for (const char* p : files) {
    const std::string code = readSource(p);
    TEST_ASSERT_FALSE_MESSAGE(code.empty(), p);
    for (std::sregex_iterator it(code.begin(), code.end(), kFiller), end; it != end; ++it) {
      ++fillers;
      const std::string body = blockAfter(code, (size_t)it->position(0));
      for (const char* bad : {"WebLock", "memory::", "->read(", ".read("})
        TEST_ASSERT_TRUE_MESSAGE(body.find(bad) == std::string::npos,
                                 (std::string(p) + ": a library filler touches the card (" + bad + ")").c_str());
    }
    TEST_ASSERT_TRUE_MESSAGE(code.find("cardFileFiller") == std::string::npos, p);
  }
  TEST_ASSERT_TRUE(fillers >= 4);   // the scan still sees the in-memory fillers
}

// The four card downloads (the durable log, a file, an archive item, an archive day)
// go out as card streams, and their open runs inside the stream (a download that
// starts during a hold waits instead of answering busy at the door).
static void test_every_card_download_is_a_card_stream() {
  const std::pair<const char*, const char*> uses[] = {
      {"src/net/errlog_routes.cpp", "void streamFile("},
      {"src/net/web_files.cpp", "on(\"/api/files/dl\""},
      {"src/net/web_tg_archive.cpp", "void streamItem("},
      {"src/net/web_tg_archive.cpp", "void handleZip("},
  };
  for (const auto& u : uses) {
    const std::string code = readSource(u.first);
    const size_t at = code.find(u.second);
    TEST_ASSERT_TRUE_MESSAGE(at != std::string::npos, u.second);
    const std::string body = blockAfter(code, at);
    TEST_ASSERT_TRUE_MESSAGE(body.find("cardStream(") != std::string::npos, u.second);
    TEST_ASSERT_TRUE_MESSAGE(body.find("beginChunkedResponse") == std::string::npos, u.second);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_free_card_streams_whole_at_the_pace);
  RUN_TEST(test_a_trickle_keeps_two_segments_in_flight);
  RUN_TEST(test_the_library_rule_ends_a_download_early_on_a_short_hold);
  RUN_TEST(test_any_hold_inside_the_budget_is_waited_out);
  RUN_TEST(test_a_download_started_during_a_hold_waits_for_the_open);
  RUN_TEST(test_a_waiting_stream_holds_no_buffer);
  RUN_TEST(test_a_lost_acknowledgement_event_never_stalls_the_stream);
  RUN_TEST(test_past_the_budget_before_the_head_answers_busy);
  RUN_TEST(test_past_the_budget_mid_body_cuts_the_stream);
  RUN_TEST(test_the_budget_counts_every_wait_of_the_request);
  RUN_TEST(test_a_read_error_cuts_the_stream);
  RUN_TEST(test_an_answer_goes_out_whole_with_no_body);
  RUN_TEST(test_a_raw_stream_ends_idle_with_the_bytes_only);
  RUN_TEST(test_a_partial_add_keeps_the_rest_staged);
  RUN_TEST(test_no_staging_memory_waits_inside_the_budget);
  RUN_TEST(test_after_a_pass_the_connection_is_cut_closed_or_kept);
  RUN_TEST(test_a_card_file_ends_only_at_its_size);
  RUN_TEST(test_no_library_filler_reads_the_card);
  RUN_TEST(test_every_card_download_is_a_card_stream);
  return UNITY_END();
}
