// test_delivery_bytes - one plain message per turn, written once (PRD goal 2, sections 1
// step 6 and 8.9; layer L1; lane RB-B0b writes it, lane RB-E3 makes it pass). The channel
// receives exactly the portable deliver function's chunks of the model's text: no sentence
// deleted, no correction appended, no claim vetoed, whatever the reply says. A device-
// written message is one of the closed list (deliver.h kAllStatusLines), sent as its own
// message, at most one after a turn. The checker is counter-tested against the old
// engine's habits (an appended 'Note:', a deleted sentence, two status lines).
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "../support/tx_battery/deliver_oracle.h"
#include "../support/tx_battery/fixtures.h"
#include "../support/tx_battery/pending.h"
#include "../support/tx_battery/props.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/orch/deliver_text.h"

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;
namespace sp = harness_test::scripted;

void setUp() {}
void tearDown() {}

namespace {

std::vector<txb::SentMessage> sent(std::initializer_list<const char*> texts) {
  std::vector<txb::SentMessage> v;
  for (const char* t : texts) v.push_back(txb::SentMessage{"1001", t});
  return v;
}

// ---- counter-tests ---------------------------------------------------------------------
void test_checker_rejects_the_old_engines_rewrites() {
  const std::vector<std::string> chunks = {"I logged 150 g of skyr. Anything else?"};
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::deliveredExactly(chunks, false, "", sent({"I logged 150 g of skyr. Anything else?"}), why),
                           why.c_str());
  // The claim scanner's appended correction, inside the reply or as a second message.
  TEST_ASSERT_FALSE(txb::deliveredExactly(
      chunks, false, "", sent({"I logged 150 g of skyr. Anything else? (Note: nothing was saved.)"}), why));
  TEST_ASSERT_FALSE(txb::deliveredExactly(
      chunks, false, "", sent({"I logged 150 g of skyr. Anything else?", "Note: nothing was saved."}), why));
  // Sentence deletion at the send seam.
  TEST_ASSERT_FALSE(txb::deliveredExactly(chunks, false, "", sent({"Anything else?"}), why));
  // A reply never sent.
  TEST_ASSERT_FALSE(txb::deliveredExactly(chunks, false, "", {}, why));
  // A status line is allowed once, as its own message, only when the turn ended with one.
  TEST_ASSERT_TRUE(txb::deliveredExactly(chunks, true, "Stopped.",
                                         sent({"I logged 150 g of skyr. Anything else?", "Stopped."}), why));
  TEST_ASSERT_FALSE(txb::deliveredExactly(chunks, true, "Stopped.",
                                          sent({"I logged 150 g of skyr. Anything else?", "Stopped. Sorry!"}), why));
  TEST_ASSERT_FALSE(txb::deliveredExactly(
      chunks, true, "", sent({"I logged 150 g of skyr. Anything else?", "Stopped.", "Provider error."}), why));
}

// ---- through the engine -----------------------------------------------------------------
void expectExact(txb::Battery& b, const plain::TurnOutcome& out, const std::string& modelText, size_t from) {
  std::vector<std::string> chunks;
  TXB_OK(txb::expectedChunks(modelText, chunks));
  const std::vector<txb::SentMessage> got(b.sent.begin() + (long)from, b.sent.end());
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::deliveredExactly(chunks, out.status != plain::StatusLine::None, "", got, why),
                           why.c_str());
}

// Claim-shaped replies with no tool behind them, markup, entities, long text: the model's
// text arrives as the deliver function makes it, and nothing else arrives.
void test_the_reply_is_the_model_text_whatever_it_claims() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1a");
  const std::string replies[] = {
      "I've logged 150 g of skyr for you.",
      "Done! Saved it to notes/plan.md and set a reminder for 7.",
      "**Bold**, _italic_, `code`, <tags> & ampersands > here.",
      "Unicode: caf\xC3\xA9, \xF0\x9F\x8D\xB2, \xE2\x82\xAC 3.50",
      "I couldn't find it. I checked notes/ and the ledger; neither has it.",
  };
  txb::Battery b;
  for (const std::string& r : replies) {
    b.model.push(sp::say(r));
    const size_t from = b.sent.size();
    b.advance(60);
    plain::TurnOutcome out;
    TXB_OK(b.owner("tell me", out));
    TEST_ASSERT_TRUE(out.status == plain::StatusLine::None);
    TEST_ASSERT_EQUAL_STRING(r.c_str(), out.text.c_str());
    expectExact(b, out, r, from);
    // Independent of the deliver function: text with no markup or entity characters
    // arrives as one message, byte for byte (a deliver function that edits claims fails).
    if (r.find_first_of("*_`<>&") == std::string::npos) {
      TEST_ASSERT_EQUAL(from + 1, b.sent.size());
      TEST_ASSERT_EQUAL_STRING(r.c_str(), b.sent.back().text.c_str());
    }
  }
}

// A long reply is split by the deliver function, without a byte lost or added.
void test_a_long_reply_is_split_without_loss() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1a");
  std::string longText;
  for (int i = 0; i < 400; ++i) longText += "Line " + std::to_string(i) + ": the boat is the Nereid II.\n";
  txb::Battery b;
  b.model.push(sp::say(longText));
  plain::TurnOutcome out;
  TXB_OK(b.owner("tell me everything", out));
  TEST_ASSERT_TRUE_MESSAGE(b.sent.size() > 1, "12 KB went out as one Telegram message");
  expectExact(b, out, longText, 0);
  // Independent of the deliver function: the chunks hold every visible byte, in order.
  std::string joined;
  for (const auto& m : b.sent) joined += m.text;
  auto visible = [](const std::string& t) {
    std::string v;
    for (char c : t)
      if (!std::isspace((unsigned char)c)) v += c;
    return v;
  };
  TEST_ASSERT_TRUE_MESSAGE(visible(joined) == visible(longText), "a split lost or added bytes");
}

// A guard trip: the last round's text, then at most one status line, as its own message.
void test_a_status_line_is_separate_and_alone() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1b");
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  for (int i = 0; i < plain::kDefaultRounds; ++i)
    b.model.push(sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(i) + "}"));
  b.model.push(sp::say("I read it sixteen times; it says alpha."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("keep reading", out));
  TEST_ASSERT_TRUE_MESSAGE(out.status == plain::StatusLine::LimitReached, plain::statusLineSlug(out.status));
  expectExact(b, out, "I read it sixteen times; it says alpha.", 0);
  std::vector<std::string> chunks;
  TXB_OK(txb::expectedChunks("I read it sixteen times; it says alpha.", chunks));
  TEST_ASSERT_EQUAL_MESSAGE(chunks.size() + 1, b.sent.size(), "the limit-reached line is one message of its own");
  TEST_ASSERT_TRUE_MESSAGE(b.sent.back().text.find("sixteen times") == std::string::npos,
                           "the status line carries the model's text");
}

// A provider that keeps failing (one try and the two same-host retries): no model text, so
// exactly the one provider-error line and nothing else.
void test_a_failed_turn_sends_exactly_one_line() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1a");
  txb::Battery b;
  for (int i = 0; i < 3; ++i) b.model.push(sp::httpError(500));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hello?", out));
  TEST_ASSERT_EQUAL_MESSAGE(3, (int)b.model.requests().size(), "a 5xx gets one try and two same-host retries");
  TEST_ASSERT_TRUE_MESSAGE(out.status == plain::StatusLine::ProviderError, plain::statusLineSlug(out.status));
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)b.sent.size(), "a failed turn sends exactly one line");
  TEST_ASSERT_FALSE(out.delivered && !out.text.empty());
}

// ---- the deliver function itself (lane RB-E3) --------------------------------------------
// A channel that records every send, refuses the send with index `refuseAt` (returns false) and
// throws a refused allocation at the send with index `throwAt` (before it goes out).
struct Recorder {
  std::vector<std::string> sent;
  long sends = 0, refuseAt = -1, throwAt = -1;
  agent::Channel ch;
  plain::TurnCtx turn;
  Recorder() {
    turn.chatId = turn.chatKey = "1001";
    turn.turn = 1;
    ch.send = [this](const std::string&, const std::string& text) {
      const long i = sends++;
      if (i == throwAt) throw std::bad_alloc();
      if (i == refuseAt) return false;
      sent.push_back(text);
      return true;
    };
    ch.isAllowed = [](const std::string&) { return true; };
  }
  plain::DeliverDeps deps() const { return plain::DeliverDeps{&ch, nullptr, nullptr}; }
};

std::string repeatLines(int n, const char* line) {
  std::string t;
  for (int i = 0; i < n; ++i) t += "Line " + std::to_string(i) + ": " + line + "\n";
  return t;
}

// The texts the device-bytes sweep sends: plain, markup the sanitiser takes, entities, UTF-8 at
// a part edge, a long reply, one past the part bound, blank only, empty.
std::vector<std::string> deliverySweep() {
  std::string utf8Edge(3998, 'u');
  utf8Edge += "\xF0\x9F\x8D\xB2 and after";
  return {"I've logged 150 g of skyr for you.",
          "<think>should I?</think>Here it is: **bold** and <b>tags</b> & more > less.",
          "Unicode: caf\xC3\xA9, \xE2\x82\xAC 3.50",
          utf8Edge,
          repeatLines(400, "the boat is the Nereid II."),
          repeatLines(1200, "a reply over the part bound keeps going."),
          "   \n\n  ",
          ""};
}

// What the channel gets is exactly the device's deliver function's parts of the model's text,
// whatever the text says, and the outcome counts them as they went.
void test_deliver_turn_sends_the_device_parts_and_nothing_else() {
  for (const std::string& text : deliverySweep()) {
    Recorder r;
    plain::DeliverInput in;
    in.turn = &r.turn;
    in.text = &text;
    plain::DeliverOutcome out;
    TXB_OK(plain::deliverTurn(r.deps(), in, out));
    const std::vector<std::string> want = nimbus::orch::telegramParts(text);
    TEST_ASSERT_EQUAL_MESSAGE(want.size(), r.sent.size(), text.substr(0, 40).c_str());
    uint32_t bytes = 0;
    for (size_t i = 0; i < want.size(); ++i) {
      TEST_ASSERT_TRUE_MESSAGE(want[i] == r.sent[i], "a part differs from the device's part");
      bytes += (uint32_t)want[i].size();
    }
    TEST_ASSERT_EQUAL(want.size(), out.parts);
    TEST_ASSERT_EQUAL(!want.empty(), out.sent);
    TEST_ASSERT_TRUE(out.replyDone);
    TEST_ASSERT_FALSE(out.statusSent);
    TEST_ASSERT_EQUAL(bytes, out.bytes);
  }
}

// Every harness-written message of the closed list goes out alone, after the reply, exactly as
// the engine wrote it (a line longer than a part as its first part: still one message).
void test_every_status_line_of_the_table_goes_out_alone() {
  const std::string reply = repeatLines(200, "a reply in two parts.");
  const std::string line = "The answer was cut off.";
  const std::string longLine(5000, 'x');
  for (plain::StatusLine s : plain::kAllStatusLines) {
    for (int shape = 0; shape < 3; ++shape) {
      Recorder r;
      plain::DeliverInput in;
      in.turn = &r.turn;
      if (shape != 1) in.text = &reply;
      in.status = s;
      in.statusText = shape == 2 ? &longLine : &line;
      plain::DeliverOutcome out;
      TXB_OK(plain::deliverTurn(r.deps(), in, out));
      std::vector<std::string> want = shape == 1 ? std::vector<std::string>{} : nimbus::orch::telegramParts(reply);
      want.push_back(shape == 2 ? longLine.substr(0, nimbus::orch::kTelegramPartBytes) : line);
      TEST_ASSERT_EQUAL_MESSAGE(want.size(), r.sent.size(), plain::statusLineSlug(s));
      for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_TRUE_MESSAGE(want[i] == r.sent[i], plain::statusLineSlug(s));
      TEST_ASSERT_TRUE(out.statusSent);
      std::string why;
      TEST_ASSERT_TRUE_MESSAGE(txb::deliveredExactly(shape == 1 ? std::vector<std::string>{} : nimbus::orch::telegramParts(reply),
                                                     true, "", [&] {
                                                       std::vector<txb::SentMessage> v;
                                                       for (const auto& t : r.sent) v.push_back(txb::SentMessage{"1001", t});
                                                       return v;
                                                     }(),
                                                     why),
                               why.c_str());
    }
  }
}

// A line under a name outside the table is never sent, whatever it says; the reply still is.
void test_a_line_outside_the_table_is_never_sent() {
  const std::string reply = "Done.";
  const std::string line = "Note: nothing was saved.";
  for (plain::StatusLine s : {plain::StatusLine::None, (plain::StatusLine)200}) {
    Recorder r;
    plain::DeliverInput in;
    in.turn = &r.turn;
    in.text = &reply;
    in.status = s;
    in.statusText = &line;
    plain::DeliverOutcome out;
    TEST_ASSERT_TRUE(plain::deliverTurn(r.deps(), in, out).err == tx::Err::InvalidArgument);
    TEST_ASSERT_EQUAL(1, (int)r.sent.size());
    TEST_ASSERT_EQUAL_STRING("Done.", r.sent[0].c_str());
    TEST_ASSERT_TRUE(out.replyDone && !out.statusSent);
  }
  // An empty or blank line under a listed name is nothing to send, not an empty message.
  for (const std::string blankLine : {std::string(), std::string("  \n "), std::string(5000, ' ')}) {
    Recorder r2;
    plain::DeliverInput in2;
    in2.turn = &r2.turn;
    in2.status = plain::StatusLine::Stopped;
    in2.statusText = &blankLine;
    plain::DeliverOutcome out2;
    TXB_OK(plain::deliverTurn(r2.deps(), in2, out2));
    TEST_ASSERT_EQUAL(0, (int)r2.sent.size());
  }
  Recorder r;
  const std::string empty;
  plain::DeliverInput in;
  in.turn = &r.turn;
  in.status = plain::StatusLine::Stopped;
  in.statusText = &empty;
  plain::DeliverOutcome out;
  TXB_OK(plain::deliverTurn(r.deps(), in, out));
  TEST_ASSERT_EQUAL(0, (int)r.sent.size());
  TEST_ASSERT_TRUE(plain::deliverTurn(plain::DeliverDeps{}, in, out).err == tx::Err::InvalidArgument);
}

// A refused allocation inside a send: the retry starts after the parts that went out and adds
// to the same outcome; no part and no line goes out twice (ruling 59 item 3, ruling 79).
void test_a_retry_never_sends_a_part_twice() {
  const std::string reply = repeatLines(300, "three parts of reply text.");
  const std::vector<std::string> want = nimbus::orch::telegramParts(reply);
  TEST_ASSERT_TRUE(want.size() >= 3);
  const std::string line = "Stopped.";
  for (long at = 0; at <= (long)want.size(); ++at) {
    Recorder r;
    r.throwAt = at;
    plain::DeliverInput in;
    in.turn = &r.turn;
    in.text = &reply;
    in.status = plain::StatusLine::Stopped;
    in.statusText = &line;
    plain::DeliverOutcome out;
    bool threw = false;
    try {
      (void)plain::deliverTurn(r.deps(), in, out);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    TEST_ASSERT_TRUE(threw);
    TEST_ASSERT_EQUAL(at < (long)want.size() ? at : (long)want.size(), (long)out.parts);
    in.fromPart = out.parts;
    if (out.replyDone) in.text = nullptr;
    TXB_OK(plain::deliverTurn(r.deps(), in, out));
    TEST_ASSERT_EQUAL(want.size() + 1, r.sent.size());
    for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_TRUE(want[i] == r.sent[i]);
    TEST_ASSERT_EQUAL_STRING("Stopped.", r.sent.back().c_str());
    TEST_ASSERT_EQUAL(want.size(), out.parts);
    TEST_ASSERT_TRUE(out.sent && out.replyDone && out.statusSent);
  }
}

// A part the channel refuses ends the reply there (no later part arrives out of order); the
// status line still goes; Io says so.
void test_a_refused_part_ends_the_reply_and_the_line_still_goes() {
  const std::string reply = repeatLines(300, "three parts of reply text.");
  const std::vector<std::string> want = nimbus::orch::telegramParts(reply);
  Recorder r;
  r.refuseAt = 1;
  const std::string line = "The provider did not answer.";
  plain::DeliverInput in;
  in.turn = &r.turn;
  in.text = &reply;
  in.status = plain::StatusLine::ProviderError;
  in.statusText = &line;
  plain::DeliverOutcome out;
  TEST_ASSERT_TRUE(plain::deliverTurn(r.deps(), in, out).err == tx::Err::Io);
  TEST_ASSERT_EQUAL(2, (int)r.sent.size());
  TEST_ASSERT_TRUE(want[0] == r.sent[0]);
  TEST_ASSERT_EQUAL_STRING(line.c_str(), r.sent[1].c_str());
  TEST_ASSERT_EQUAL(1, (int)out.parts);
  TEST_ASSERT_TRUE(out.sent && !out.replyDone && out.statusSent);
}

// ---- the turn-end item ---------------------------------------------------------------------
tx::Item blankEnd() {
  tx::Item e;
  e.kind = tx::ItemKind::TurnEnd;
  e.q = 42;
  e.turn = 7;
  e.t = txb::kT0;
  return e;
}

void test_the_turn_end_item_keeps_the_facts() {
  plain::TurnCtx turn;
  turn.turn = 7;
  tx::TurnEndFacts f;
  f.cause = "reply";
  f.rounds = 3;
  f.tools = 2;
  f.headRequests = 3;
  f.promptTokens = 1234;
  f.events = {"change:pointer"};
  f.exchanges = {tx::ExchangeFacts{100, 20, 200}};
  f.promptHash = 11;
  f.manifestHash = 22;
  tx::Item e = blankEnd();
  TXB_OK(plain::turnEndItem(turn, f, e));
  TEST_ASSERT_EQUAL(42, (int)e.q);
  TEST_ASSERT_EQUAL(7, (int)e.turn);
  TEST_ASSERT_EQUAL_STRING("reply", e.end.cause.c_str());
  TEST_ASSERT_EQUAL(1234, (int)e.end.promptTokens);
  TEST_ASSERT_EQUAL(11, (int)e.end.promptHash);
  TEST_ASSERT_EQUAL(22, (int)e.end.manifestHash);
  TEST_ASSERT_EQUAL(1, (int)e.end.exchanges.size());
  TEST_ASSERT_EQUAL(1, (int)e.end.events.size());
  // Refusals: not a TurnEnd item, a turn with no number.
  tx::Item u = blankEnd();
  u.kind = tx::ItemKind::User;
  TEST_ASSERT_TRUE(plain::turnEndItem(turn, f, u).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(plain::turnEndItem(plain::TurnCtx{}, f, e).err == tx::Err::InvalidArgument);
}

// An e item that cannot be stored leaves its turn open: whatever the facts, it encodes, keeps
// the newest exchanges and says what it left out.
tx::TurnEndFacts boundFacts(size_t events, size_t exchanges, size_t omission) {
  tx::TurnEndFacts f;
  f.cause = "reply";
  for (size_t i = 0; i < events; ++i) f.events.push_back("change:event-" + std::to_string(i));
  for (size_t i = 0; i < exchanges; ++i) {
    tx::ExchangeFacts x{(uint32_t)i, 1, 200};
    x.fromQ = 1, x.thruQ = 100 + i, x.items = 100, x.bodyHash = 1 + (uint32_t)i, x.effort = "medium";
    x.omission = std::string(omission, 'o');
    f.exchanges.push_back(x);
  }
  return f;
}

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }
bool isMarker(const std::string& ev) { return ev == "events:cut" || ev == "exchanges:cut" || ev == "dossier:shed"; }

// One bounded e item: within the caps, every cut named, the facts' own events kept in order.
void expectBounded(const tx::Item& e, size_t events, size_t exchanges, size_t omission) {
  size_t refs = 0;
  for (const tx::ExchangeFacts& x : e.end.exchanges) refs += x.bodyHash != 0;
  TEST_ASSERT_TRUE(e.end.events.size() <= tx::kMaxListItems);
  TEST_ASSERT_TRUE(e.end.exchanges.size() <= tx::kMaxListItems);
  const bool saysExchanges = has(e.end.events, "exchanges:cut"), saysShed = has(e.end.events, "dossier:shed");
  if (events > tx::kMaxListItems) TEST_ASSERT_TRUE_MESSAGE(has(e.end.events, "events:cut"), "a cut event list does not say so");
  TEST_ASSERT_EQUAL_MESSAGE(e.end.exchanges.size() < exchanges, saysExchanges, "exchanges:cut does not match a removal");
  if (refs < e.end.exchanges.size()) TEST_ASSERT_TRUE_MESSAGE(saysShed, "shed references are not named");
  if (exchanges) TEST_ASSERT_EQUAL_MESSAGE(exchanges - 1, e.end.exchanges.back().requestBytes, "the newest exchange left");
  for (size_t i = 0; i < e.end.events.size() && i < events; ++i)
    if (!isMarker(e.end.events[i]))
      TEST_ASSERT_EQUAL_STRING(("change:event-" + std::to_string(i)).c_str(), e.end.events[i].c_str());
  if (events + 3 <= tx::kMaxListItems) TEST_ASSERT_EQUAL(events + saysExchanges + saysShed, e.end.events.size());
  if (exchanges <= 16 && !omission) TEST_ASSERT_EQUAL(exchanges, e.end.exchanges.size());
}

void test_the_turn_end_item_always_encodes() {
  plain::TurnCtx turn;
  turn.turn = 7;
  for (size_t events : {0u, 255u, 256u, 257u, 1000u})
    for (size_t exchanges : {0u, 16u, 256u, 300u})
      for (size_t omission : {0u, 1024u}) {
        tx::Item e = blankEnd();
        TXB_OK(plain::turnEndItem(turn, boundFacts(events, exchanges, omission), e));
        std::string line;
        e.q = e.t = UINT64_MAX;   // the widest the store may give it
        TXB_OK(tx::encodeLine(e, line));
        expectBounded(e, events, exchanges, omission);
      }
}

// ---- the dossier by reference --------------------------------------------------------------
// What a host has after a turn: the stored items (filled into an arena from the card), the
// head, the wire and the system text and tools composed again.
struct Rebuilder {
  txb::Battery& b;
  std::vector<tx::Item> stored;
  std::unique_ptr<tx::TxArena> arena;
  plain::TurnCtx ctx;
  plain::HeadEndpoint head;
  plain::SystemBlocks system;
  std::vector<plain::ToolSpec> tools;

  explicit Rebuilder(txb::Battery& bat, const std::vector<uint64_t>& leaveOut = {}) : b(bat) {
    TXB_OK(b.stored(stored));
    arena.reset(new tx::TxArena());
    TXB_OK(arena->init(tx::kActiveChatArenaBytes, b.platform.allocLarge, b.platform.freeLarge));
    for (const tx::Item& it : stored)
      if (std::find(leaveOut.begin(), leaveOut.end(), it.q) == leaveOut.end()) TXB_OK(arena->append(it));
    ctx.chatKey = ctx.chatId = b.chatKey;
    ctx.dialect = b.dialect;
    ctx.host = "api.anthropic.com";
    ctx.model = "claude-sonnet-5-5";
    ctx.dayStartMin = b.dayStartMin;
    ctx.who = &b.who;
    head.host = ctx.host;
    head.key = [] { return std::string("sk-fake-battery"); };
    plain::RequestFacts f;
    f.turn = &ctx;
    f.reg = &b.reg;
    f.admin = true;
    f.timeZone = "Europe/London";
    TXB_OK(plain::composeSystem(f, system));
    TXB_OK(plain::toolManifest(f, tools));
  }

  tx::Status rebuild(const tx::Item& e, size_t k, agent::HttpRequest& out) {
    ctx.turn = e.turn;
    plain::RebuildInput in;
    in.end = &e.end;
    in.exchange = k;
    in.turn = &ctx;
    in.head = &head;
    in.wire = txb::batteryWire(b.dialect);
    in.system = &system;
    in.tools = &tools;
    in.items = arena.get();
    txb::Battery* bp = &b;
    in.bulk = [bp](const std::string& handle, std::string& o) {
      tx::TxLog reader(bp->files, bp->chatKey);
      return reader.readBulk(handle, 0, tx::kBulkMaxBytes, o);
    };
    return plain::rebuildRequest(in, out);
  }
};

// Every request the battery recorded, by turn, in order.
std::vector<const sp::Recorded*> requestsOf(txb::Battery& b, uint32_t turn) {
  std::vector<const sp::Recorded*> v;
  for (size_t i = 0; i < b.model.requests().size(); ++i)
    if (i < b.requestTurn.size() && b.requestTurn[i] == turn) v.push_back(&b.model.requests()[i]);
  return v;
}

// A chat with plain replies, tool rounds, a bulk result, a guard's last round (tool choice none)
// and a rung-3 omission: every request of every turn is rendered again from its e item and the
// stored items alone, and it is the request that was sent, byte for byte.
size_t rebuildEveryRequest(txb::Battery& b) {
  Rebuilder rb(b);
  size_t n = 0;
  for (const tx::Item& e : rb.stored) {
    if (e.kind != tx::ItemKind::TurnEnd) continue;
    const std::vector<const sp::Recorded*> sent = requestsOf(b, e.turn);
    TEST_ASSERT_EQUAL_MESSAGE(sent.size(), e.end.exchanges.size(), "one reference per request sent");
    for (size_t k = 0; k < sent.size(); ++k) {
      agent::HttpRequest out;
      TXB_OK(rb.rebuild(e, k, out));
      TEST_ASSERT_TRUE_MESSAGE(agent::materializeBody(out) == sent[k]->body, "the rebuilt body differs from the one sent");
      TEST_ASSERT_EQUAL_STRING(sent[k]->req.path.c_str(), out.path.c_str());
      TEST_ASSERT_EQUAL_STRING(sent[k]->req.host.c_str(), out.host.c_str());
      ++n;
    }
  }
  return n;
}

void test_a_request_is_rebuilt_byte_for_byte_from_its_reference() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1b");
  txb::Battery b;
  b.world.files["notes/a.md"] = "alpha";
  b.model.push(sp::say("Hello."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("hi there", out));
  b.advance(60);
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  b.model.push(sp::call("big_read", R"({"tag":"one","bytes":20000})"));
  b.model.push(sp::say("It says alpha, and the big one is read."));
  TXB_OK(b.owner("read both", out));
  b.advance(60);
  for (int i = 0; i < plain::kDefaultRounds; ++i)
    b.model.push(sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(i) + "}"));
  b.model.push(sp::say("I read it sixteen times."));
  TXB_OK(b.owner("keep reading", out));
  TEST_ASSERT_TRUE(out.status == plain::StatusLine::LimitReached);
  const size_t n = rebuildEveryRequest(b);
  TEST_ASSERT_EQUAL(1 + 3 + plain::kDefaultRounds + 1, (int)n);
  // The guard's last round carried tool choice none, and its reference says so.
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  const tx::Item* last = nullptr;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::TurnEnd) last = &it;
  TEST_ASSERT_TRUE(last && last->end.exchanges.back().toolChoiceNone);
}

void test_a_request_with_an_omission_is_rebuilt_byte_for_byte() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1b");
  txb::Battery b;
  // Enough stored turns that the battery head's full body (tx::budgetFor; 292,000 B on
  // claude-sonnet-5-5) is passed and rung 3 must omit the oldest of them.
  tx::HeadBudget head;
  TXB_OK(tx::budgetFor(b.headKey(), tx::Profile::Full, head));
  const size_t turnBytes = 5000, turns = head.bodyCap / turnBytes + 8;
  for (size_t i = 0; i < turns; ++i) {
    b.model.push(sp::say("ok " + std::to_string(i)));
    b.advance(60);
    plain::TurnOutcome o;
    TXB_OK(b.owner("long turn " + std::to_string(i) + " " + std::string(turnBytes, 's'), o));
  }
  b.world.files["notes/a.md"] = "alpha";
  b.model.push(sp::call("big_read", R"({"tag":"one","bytes":40000})"));
  b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  b.model.push(sp::say("Read it."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read the big one", out));
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  bool omitted = false, inherited = false;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::TurnEnd)
      for (const tx::ExchangeFacts& x : it.end.exchanges) {
        omitted |= x.omitThrough != 0 && !x.omission.empty();
        inherited |= x.omitThrough != 0 && x.omission.empty();   // kept once, read from the one before
      }
  TEST_ASSERT_TRUE_MESSAGE(omitted, "no request needed rung 3: the case does not test the omission");
  TEST_ASSERT_TRUE_MESSAGE(inherited, "no later request repeated the omission line: the case does not test it");
  rebuildEveryRequest(b);
}

// Owner ruling 81: a reference never describes less than the request carried. Each one covers
// every item the engine held (the oldest resident to the newest), the rebuilt request carries
// every owner message of the chat, and a rebuild that is given less refuses, never renders less.
void test_a_reference_never_covers_less_than_the_engine_held() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1b");
  txb::Battery b;
  std::vector<std::string> said;
  for (int i = 0; i < 6; ++i) {
    said.push_back("owner message number " + std::to_string(i) + " about the boat");
    if (i % 2) b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
    b.model.push(sp::say("reply " + std::to_string(i)));
    b.advance(600);
    plain::TurnOutcome out;
    TXB_OK(b.owner(said.back(), out));
  }
  Rebuilder rb(b);
  const uint64_t firstQ = rb.stored.front().q;
  size_t turnsSeen = 0;
  for (const tx::Item& e : rb.stored) {
    if (e.kind != tx::ItemKind::TurnEnd) continue;
    ++turnsSeen;
    for (size_t k = 0; k < e.end.exchanges.size(); ++k) {
      const tx::ExchangeFacts& x = e.end.exchanges[k];
      TEST_ASSERT_EQUAL_MESSAGE(firstQ, x.fromQ, "a request's range starts after the oldest item the engine held");
      size_t held = 0;
      for (const tx::Item& it : rb.stored) held += it.q >= x.fromQ && it.q <= x.thruQ;
      TEST_ASSERT_EQUAL_MESSAGE(held, x.items, "the range holds fewer items than the engine rendered");
      agent::HttpRequest out;
      TXB_OK(rb.rebuild(e, k, out));
      const std::string body = agent::materializeBody(out);
      for (size_t i = 0; i < turnsSeen; ++i)
        TEST_ASSERT_TRUE_MESSAGE(body.find(said[i]) != std::string::npos, "an owner message is missing from the request");
    }
  }
  TEST_ASSERT_EQUAL(6, (int)turnsSeen);
}

// A chat of six turns, some with a tool round (for the refusal cases below).
void sixTurns(txb::Battery& b) {
  for (int i = 0; i < 6; ++i) {
    if (i % 2) b.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
    b.model.push(sp::say("reply " + std::to_string(i)));
    b.advance(600);
    plain::TurnOutcome out;
    TXB_OK(b.owner("owner message number " + std::to_string(i) + " about the boat", out));
  }
}

// Ruling 81, the refusals: a rebuild given less, or a reference made to say less, never renders.
void test_a_rebuild_given_less_refuses() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1b");
  txb::Battery b;
  sixTurns(b);
  Rebuilder rb(b);
  // Given less than the range, the rebuild refuses: the first owner message left out.
  uint64_t firstOwner = 0;
  for (const tx::Item& it : rb.stored)
    if (it.kind == tx::ItemKind::User && !firstOwner) firstOwner = it.q;
  Rebuilder partial(b, {firstOwner});
  const tx::Item* lastEnd = nullptr;
  for (const tx::Item& it : partial.stored)
    if (it.kind == tx::ItemKind::TurnEnd) lastEnd = &it;
  agent::HttpRequest out;
  TEST_ASSERT_TRUE(partial.rebuild(*lastEnd, 0, out).err == tx::Err::NotFound);
  // Given less inside the range (the first turn's reply, not its first item), it refuses too.
  uint64_t firstReply = 0;
  for (const tx::Item& it : rb.stored)
    if (it.kind == tx::ItemKind::Assistant && !firstReply) firstReply = it.q;
  TEST_ASSERT_TRUE(firstReply > rb.stored.front().q);
  Rebuilder hole(b, {firstReply});
  TEST_ASSERT_TRUE(hole.rebuild(*lastEnd, 0, out).err == tx::Err::NotFound);
  // A reference made to describe less (its range raised past the first turn) does not rebuild.
  tx::Item shorter = *lastEnd;
  shorter.end.exchanges[0].fromQ = firstOwner + 1;
  shorter.end.exchanges[0].items -= 1;
  TEST_ASSERT_FALSE(rb.rebuild(shorter, 0, out).ok());
  // A stale system text or tool list is named, never rendered as if it were the turn's.
  Rebuilder stale(b);
  stale.system.notes += "pinned: a note added later\n";
  TEST_ASSERT_TRUE(stale.rebuild(*lastEnd, 0, out).err == tx::Err::NotFound);
  Rebuilder staleTools(b);
  staleTools.tools.pop_back();
  TEST_ASSERT_TRUE(staleTools.rebuild(*lastEnd, 0, out).err == tx::Err::NotFound);
  // An exchange with no reference, or past the list, is not a request to rebuild.
  TEST_ASSERT_TRUE(rb.rebuild(*lastEnd, lastEnd->end.exchanges.size(), out).err == tx::Err::InvalidArgument);
  tx::Item noRef = *lastEnd;
  noRef.end.exchanges[0].bodyHash = 0;
  TEST_ASSERT_TRUE(rb.rebuild(noRef, 0, out).err == tx::Err::InvalidArgument);
}

// The range is the engine's, not chosen: recordRequest reads it from what was rendered.
void test_the_reference_is_read_from_what_was_rendered() {
  txb::Battery b;
  plain::TurnOutcome o;
  b.model.push(sp::say("one"));
  TXB_OK(b.owner("first", o));
  b.model.push(sp::say("two"));
  TXB_OK(b.owner("second", o));
  plain::RenderInput in;
  in.items = b.arena.get();
  in.stubPointer = 3;
  in.maxTokens = 900;
  in.toolChoiceNone = true;
  plain::LadderState ladder;
  ladder.closedThrough = 2, ladder.stubThrough = 4, ladder.omitThrough = 5, ladder.omissionLine = "[left out]";
  in.ladder = &ladder;
  plain::HeadEndpoint head;
  head.effort = "low";
  in.head = &head;
  agent::HttpRequest sent;
  sent.path = "/v1/messages";
  sent.body = "{\"x\":1}";
  tx::ExchangeFacts x;
  plain::recordRequest(in, sent, x);
  tx::ArenaEntry first, last;
  TXB_OK(b.arena->header(0, first));
  TXB_OK(b.arena->header(b.arena->count() - 1, last));
  TEST_ASSERT_EQUAL(first.q, x.fromQ);
  TEST_ASSERT_EQUAL(last.q, x.thruQ);
  TEST_ASSERT_EQUAL(b.arena->count(), x.items);
  TEST_ASSERT_EQUAL(3, (int)x.stub);
  TEST_ASSERT_EQUAL(900, (int)x.maxTokens);
  TEST_ASSERT_TRUE(x.toolChoiceNone);
  TEST_ASSERT_EQUAL(2, (int)x.closedThrough);
  TEST_ASSERT_EQUAL(4, (int)x.stubThrough);
  TEST_ASSERT_EQUAL(5, (int)x.omitThrough);
  TEST_ASSERT_EQUAL_STRING("[left out]", x.omission.c_str());
  TEST_ASSERT_EQUAL_STRING("low", x.effort.c_str());
  TEST_ASSERT_EQUAL(plain::requestHashOf(sent), x.bodyHash);
  TEST_ASSERT_TRUE(x.bodyHash != 0);
  // The hash covers the path, the body and the spliced bytes, never a header.
  agent::HttpRequest other = sent;
  other.headers.push_back({"x-api-key", "sk-other"});
  TEST_ASSERT_EQUAL(plain::requestHashOf(sent), plain::requestHashOf(other));
  other.body = "{\"x\":2}";
  TEST_ASSERT_TRUE(plain::requestHashOf(sent) != plain::requestHashOf(other));
  other = sent;
  other.path = "/v1/other";
  TEST_ASSERT_TRUE(plain::requestHashOf(sent) != plain::requestHashOf(other));
  static const uint8_t px[] = {1, 2, 3};
  static const uint8_t py[] = {1, 2, 4};
  other = sent;
  other.splices.push_back(agent::BodySplice{2, px, 3, true});
  agent::HttpRequest other2 = sent;
  other2.splices.push_back(agent::BodySplice{2, py, 3, true});
  TEST_ASSERT_TRUE(plain::requestHashOf(other) != plain::requestHashOf(other2));
  TEST_ASSERT_TRUE(plain::requestHashOf(other) != plain::requestHashOf(sent));
}

// A full arena with nothing closed left to drop (ruling 60): the e item sheds its request
// references, oldest first, before it would be refused, and says so; the turn closes. The
// references never cost a resident turn of the conversation (owner ruling 81).
void test_a_full_arena_sheds_references_before_the_turn_end_item() {
  TXB_WAITS_FOR("PENDING RB-E3", "RB-E1b");
  txb::Battery b;
  TXB_OK(b.boot());
  TXB_OK(b.arena->init(16 * 1024, b.platform.allocLarge, b.platform.freeLarge));
  for (int i = 0; i < 4; ++i) {
    b.model.push(sp::say("ok " + std::to_string(i)));
    b.advance(60);
    plain::TurnOutcome o;
    TXB_OK(b.owner("short turn " + std::to_string(i) + " " + std::string(600, 's'), o));
  }
  for (int i = 0; i < 6; ++i) b.model.push(sp::call("big_read", "{\"tag\":\"m" + std::to_string(i) + "\",\"bytes\":1900}"));
  b.model.push(sp::say("Read six."));
  plain::TurnOutcome out;
  TXB_OK(b.owner("read six", out));
  TEST_ASSERT_EQUAL_STRING("Read six.", out.text.c_str());
  std::vector<tx::Item> items;
  TXB_OK(b.stored(items));
  const tx::Item& e = items.back();
  TEST_ASSERT_TRUE_MESSAGE(e.kind == tx::ItemKind::TurnEnd, "the turn was left open");
  TEST_ASSERT_TRUE(std::find(e.end.events.begin(), e.end.events.end(), "dossier:shed") != e.end.events.end());
  TEST_ASSERT_EQUAL(7, (int)e.end.exchanges.size());   // the figures stay
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)e.end.exchanges.front().bodyHash, "the oldest reference is shed first");
  // shedReference: only a TurnEnd item, only while a reference is left.
  tx::Item u = blankEnd();
  u.kind = tx::ItemKind::User;
  TEST_ASSERT_FALSE(plain::shedReference(u));
  tx::Item one = blankEnd();
  tx::ExchangeFacts x{10, 1, 200};
  x.bodyHash = 5;
  one.end.exchanges = {x, x};
  TEST_ASSERT_TRUE(plain::shedReference(one));
  TEST_ASSERT_TRUE(plain::shedReference(one));
  TEST_ASSERT_FALSE(plain::shedReference(one));
  TEST_ASSERT_EQUAL(1, (int)std::count(one.end.events.begin(), one.end.events.end(), "dossier:shed"));
  TEST_ASSERT_EQUAL(10, (int)one.end.exchanges[1].requestBytes);
}

// An omission line repeated on later exchanges is kept once and read back for each of them.
void test_a_repeated_omission_line_is_kept_once() {
  plain::TurnCtx turn;
  turn.turn = 7;
  tx::TurnEndFacts f;
  f.cause = "reply";
  for (int i = 0; i < 3; ++i) {
    tx::ExchangeFacts x{100, 1, 200};
    x.bodyHash = 9, x.omitThrough = 20, x.omission = "[2 earlier turns not shown]";
    f.exchanges.push_back(x);
  }
  f.exchanges[2].omission = "[4 earlier turns not shown]";
  tx::Item e = blankEnd();
  TXB_OK(plain::turnEndItem(turn, f, e));
  TEST_ASSERT_EQUAL_STRING("[2 earlier turns not shown]", e.end.exchanges[0].omission.c_str());
  TEST_ASSERT_EQUAL_STRING("", e.end.exchanges[1].omission.c_str());
  TEST_ASSERT_EQUAL_STRING("[4 earlier turns not shown]", e.end.exchanges[2].omission.c_str());
  // Shedding the first hands its line on to the next that read it.
  TEST_ASSERT_TRUE(plain::shedReference(e));
  TEST_ASSERT_EQUAL_STRING("[2 earlier turns not shown]", e.end.exchanges[1].omission.c_str());
}

// ---- progress on a long turn (owner ruling 82 item 6) --------------------------------------
struct ProgressSink {
  bool canEdit = true;
  std::vector<std::string> lines;              // every message sent
  std::vector<std::pair<uint32_t, std::string>> shown;   // (clock ms, text) of each send or edit
  std::string current;
  uint32_t* clock = nullptr;
  plain::ProgressDeps deps() {
    plain::ProgressDeps d;
    d.send = [this](const std::string&, const std::string& t, int64_t& id) {
      lines.push_back(t);
      shown.emplace_back(clock ? *clock : 0, t);
      current = t;
      id = (int64_t)lines.size();
      return true;
    };
    if (canEdit)
      d.edit = [this](const std::string&, int64_t, const std::string& t) {
        shown.emplace_back(clock ? *clock : 0, t);
        current = t;
        return true;
      };
    return d;
  }
};

// Tool rounds of `stepMs` each, then a reply, with progress installed.
void longTurn(txb::Battery& b, plain::ProgressReporter& r, int rounds, uint32_t stepMs, const std::string& reply) {
  TXB_OK(b.boot());
  TXB_OK(plain::installProgress(*b.hooks, r, [&b] { return b.ms; }));
  b.world.files["notes/a.md"] = "alpha";
  for (int i = 0; i < rounds; ++i) {
    sp::Step s = sp::call("read_file", "{\"path\":\"notes/a.md\",\"offset\":" + std::to_string(i) + "}");
    s.takesMs = stepMs;
    b.model.push(s);
  }
  b.model.push(sp::say(reply));
  plain::TurnOutcome out;
  TXB_OK(b.owner("work on it", out));
  TEST_ASSERT_EQUAL_STRING(reply.c_str(), out.text.c_str());
}

void test_a_short_turn_shows_no_progress() {
  txb::Battery b;
  ProgressSink sink;
  plain::ProgressReporter r(sink.deps());
  longTurn(b, r, 3, 3000, "Done quickly.");
  TEST_ASSERT_EQUAL(0, (int)sink.shown.size());
}

// A long turn: one line after 20 s with no visible update, edited in place as the turn goes
// on (at most every 10 s), its final form at the end. The answer arrives exactly as the model
// wrote it, and the progress line is not one of the after-turn messages (its own class).
void test_a_long_turn_shows_progress_edited_in_place_apart_from_the_answer() {
  txb::Battery b;
  ProgressSink sink;
  sink.clock = &b.ms;
  plain::ProgressReporter r(sink.deps());
  const std::string reply = "Here is the long answer, whole.";
  longTurn(b, r, 12, 8000, reply);
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)sink.lines.size(), "progress sent a second message where it can edit");
  TEST_ASSERT_TRUE(sink.shown.size() >= 3);
  TEST_ASSERT_TRUE_MESSAGE(sink.shown.front().first >= plain::kProgressFirstMs, "a progress line before 20 s");
  for (size_t i = 1; i + 1 < sink.shown.size(); ++i)
    TEST_ASSERT_TRUE_MESSAGE(sink.shown[i].first - sink.shown[i - 1].first >= plain::kProgressEditMs, "edits closer than 10 s");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("Worked", sink.current.substr(0, 6).c_str(), sink.current.c_str());
  for (const auto& s : sink.shown) TEST_ASSERT_TRUE(s.second.find(reply) == std::string::npos);
  std::vector<std::string> chunks;
  TXB_OK(txb::expectedChunks(reply, chunks));
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(txb::deliveredExactly(chunks, false, "", b.sent, why), why.c_str());
  TEST_ASSERT_TRUE(sink.lines[0].rfind("Still working (", 0) == 0);
}

// Where the channel cannot edit: a new line at most every 60 s and at most three a turn.
void test_without_edit_progress_lines_are_spaced_and_bounded() {
  txb::Battery b;
  ProgressSink sink;
  sink.canEdit = false;
  sink.clock = &b.ms;
  plain::ProgressReporter r(sink.deps());
  longTurn(b, r, 15, 15000, "Finally done.");   // 225 s: inside the 16 rounds and the soft wall
  TEST_ASSERT_EQUAL((int)plain::kProgressMaxLines, (int)sink.lines.size());
  TEST_ASSERT_EQUAL(sink.lines.size(), sink.shown.size());   // nothing edited
  for (size_t i = 1; i < sink.shown.size(); ++i)
    TEST_ASSERT_TRUE(sink.shown[i].first - sink.shown[i - 1].first >= plain::kProgressNewLineMs);
  std::string why;
  std::vector<std::string> chunks;
  TXB_OK(txb::expectedChunks("Finally done.", chunks));
  TEST_ASSERT_TRUE_MESSAGE(txb::deliveredExactly(chunks, false, "", b.sent, why), why.c_str());
}

// The reporter by itself: the first line is due at 20 s, each kind has its line, a name that is
// not a plain machine name is not shown, a refused allocation in the binding is dropped.
void test_the_progress_reporter_by_itself() {
  ProgressSink sink;
  plain::ProgressReporter r(sink.deps());
  plain::TurnCtx t;
  t.chatId = "1001";
  t.turn = 3;
  r.open(t, 1000);
  plain::ProgressEvent e;
  e.kind = plain::ProgressKind::ProviderWait;
  e.nowMs = 1000 + plain::kProgressFirstMs - 1;
  TEST_ASSERT_FALSE(r.tick(e));
  e.nowMs += 1;
  TEST_ASSERT_TRUE(r.tick(e));
  TEST_ASSERT_EQUAL_STRING("Still working (20 s): waiting for the model's answer.", sink.current.c_str());
  e.kind = plain::ProgressKind::ToolRound;
  e.detail = "<b>x</b>";
  e.nowMs += plain::kProgressEditMs;
  TEST_ASSERT_TRUE(r.tick(e));
  TEST_ASSERT_EQUAL_STRING("Still working (30 s, step 1): ran a tool.", sink.current.c_str());
  e.kind = plain::ProgressKind::SoftGuard;
  e.detail = "rounds";
  e.nowMs += 200000;
  TEST_ASSERT_TRUE(r.tick(e));
  TEST_ASSERT_EQUAL_STRING("Still working (3 min, step 1): slower than usual, still making progress.", sink.current.c_str());
  r.close(e.nowMs);
  TEST_ASSERT_EQUAL_STRING("Worked 3 min (1 step).", sink.current.c_str());
  TEST_ASSERT_FALSE(r.tick(e));   // closed
  // A binding that throws a refused allocation: dropped, the turn goes on.
  plain::ProgressDeps bad;
  bad.send = [](const std::string&, const std::string&, int64_t&) -> bool { throw std::bad_alloc(); };
  plain::ProgressReporter rb(bad);
  rb.open(t, 0);
  e.nowMs = plain::kProgressFirstMs;
  TEST_ASSERT_FALSE(rb.tick(e));
  rb.close(e.nowMs);
  // A turn that never showed a line ends with nothing sent.
  ProgressSink quiet;
  plain::ProgressReporter rq(quiet.deps());
  rq.open(t, 0);
  rq.close(5000);
  TEST_ASSERT_EQUAL(0, (int)quiet.shown.size());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_rejects_the_old_engines_rewrites);
  RUN_TEST(test_the_reply_is_the_model_text_whatever_it_claims);
  RUN_TEST(test_a_long_reply_is_split_without_loss);
  RUN_TEST(test_a_status_line_is_separate_and_alone);
  RUN_TEST(test_a_failed_turn_sends_exactly_one_line);
  RUN_TEST(test_deliver_turn_sends_the_device_parts_and_nothing_else);
  RUN_TEST(test_every_status_line_of_the_table_goes_out_alone);
  RUN_TEST(test_a_line_outside_the_table_is_never_sent);
  RUN_TEST(test_a_retry_never_sends_a_part_twice);
  RUN_TEST(test_a_refused_part_ends_the_reply_and_the_line_still_goes);
  RUN_TEST(test_the_turn_end_item_keeps_the_facts);
  RUN_TEST(test_the_turn_end_item_always_encodes);
  RUN_TEST(test_a_request_is_rebuilt_byte_for_byte_from_its_reference);
  RUN_TEST(test_a_request_with_an_omission_is_rebuilt_byte_for_byte);
  RUN_TEST(test_a_reference_never_covers_less_than_the_engine_held);
  RUN_TEST(test_a_rebuild_given_less_refuses);
  RUN_TEST(test_the_reference_is_read_from_what_was_rendered);
  RUN_TEST(test_a_full_arena_sheds_references_before_the_turn_end_item);
  RUN_TEST(test_a_repeated_omission_line_is_kept_once);
  RUN_TEST(test_a_short_turn_shows_no_progress);
  RUN_TEST(test_a_long_turn_shows_progress_edited_in_place_apart_from_the_answer);
  RUN_TEST(test_without_edit_progress_lines_are_spaced_and_bounded);
  RUN_TEST(test_the_progress_reporter_by_itself);
  return UNITY_END();
}
