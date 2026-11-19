// test_plain_rig - engine 2 on the hosted process (lane RB-I1a): one owner text through the
// plain loop with a scripted model at the transport and the stand-in (or, once landed, the
// real) wire, through nimbusd/src/plain_rig.h.
//   (a) a text turn on every dialect: the scripted answer is the turn's reply, the channel
//       got it and nothing else, one request, and the transcript holds what the loop core
//       says a text turn writes (RB-E1a: u t a(fin) d e, cause "reply", one head request);
//   (b) the chat goes on: a second turn carries the first, and a rig rebooted over the same
//       storage carries it too (the host's store, not the rig's RAM);
//   (c) the failure faces: the provider errors (each class), hangs past the wall clock,
//       answers with an unreadable body, or answers with nothing at all. Every turn ends
//       with a stated outcome and never a crash; an error is one plain line to the owner;
//   (d) the host's own faults: a turn before boot, a missing binding, a second boot, a
//       dialect whose wire has not landed (one plain line, never a request);
//   (e) a tool binding registered on the rig is what a call reaches, with the chat's
//       Principal; arguments that are not an object reach it as an empty object;
//   (f) the old engine's rig and the plain rig compile in one translation unit;
//   (g) a handler that throws is an error result for that call; a refused allocation in a handler is
//       one plain line (ruling 56).
// Offline: scripted provider, virtual clock, no keys.
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../test/support/scripted_provider/scripted_provider.h"
#include "../../test/support/tx_battery/mem_files.h"
#include "../../test/support/tx_battery/stub_wire.h"
#include "plain_rig.h"
#include "rig.h"   // (f): both engines' host headers in one translation unit
#include "test_util.h"

namespace plain = agent::plain;
namespace sp = harness_test::scripted;
namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

namespace {

constexpr uint64_t kEpoch0 = 1794920520ULL;   // Tue 17 Nov 2026 13:02 UTC

// One hosted chat on engine 2: a virtual clock, a scripted model, storage that outlives the
// rig (a reboot keeps it), and the rig itself.
struct Host {
  explicit Host(plain::Dialect d = plain::Dialect::AnthropicMessages, bool withWire = true) : dialect(d) {
    platform.nowMs = [this] { return ms; };
    platform.nowEpoch = [this] { return kEpoch0 + ms / 1000u; };
    platform.delayMs = [this](uint32_t n) { ms += n; };
    platform.freeHeap = [] { return 200000u; };
    platform.allocLarge = [](size_t n) { return std::malloc(n); };
    platform.freeLarge = [](void* p) { std::free(p); };
    model.clock = &ms;
    wire = withWire ? txb::batteryWire(d) : nullptr;
    reboot();
  }

  void reboot() {
    rig.reset(new nimbusd::PlainRig(config(), nimbusd::PlainRigBindings{&platform, &model, &files, wire, hostSend, nullptr}));
    if (bindTools) bindTools(*rig);
    booted = rig->boot();
  }

  nimbusd::PlainRigConfig config() const {
    nimbusd::PlainRigConfig c;
    c.chatKey = "2001";
    c.dialect = dialect;
    c.host = dialect == plain::Dialect::AnthropicMessages ? "api.anthropic.com" : "api.example.test";
    c.model = dialect == plain::Dialect::AnthropicMessages ? "claude-sonnet-5-5" : "scripted-model";
    c.key = [] { return std::string("sk-fake-rig"); };
    return c;
  }

  std::vector<tx::Item> stored() const {
    std::vector<tx::Item> out;
    std::string dir;
    if (!tx::txDir("2001", dir).ok()) return out;
    for (const std::string& path : const_cast<txb::MemFiles&>(files).segments(dir)) {
      size_t at = 0;
      const std::string& seg = files.files.at(path);
      while (at < seg.size()) {
        const size_t nl = seg.find('\n', at);
        if (nl == std::string::npos) break;
        tx::Item it;
        if (tx::decodeLine(seg.data() + at, nl - at, it).ok()) out.push_back(it);
        at = nl + 1;
      }
    }
    std::sort(out.begin(), out.end(), [](const tx::Item& a, const tx::Item& b) { return a.q < b.q; });
    return out;
  }
  std::string kinds() const {
    std::string k;
    for (const tx::Item& it : stored()) k += tx::kindLetter(it.kind);
    return k;
  }
  // The newest e item, by value (a default item with turn 0 when there is none).
  tx::Item lastEnd() const {
    tx::Item e;
    for (const tx::Item& it : stored())
      if (it.kind == tx::ItemKind::TurnEnd) e = it;
    return e;
  }

  plain::Dialect dialect;
  agent::Platform platform;
  uint32_t ms = 1000;
  sp::ScriptedProvider model;
  txb::MemFiles files;
  const plain::WireCodec* wire = nullptr;
  std::function<void(nimbusd::PlainRig&)> bindTools;
  std::function<bool(const std::string&, const std::string&)> hostSend;
  std::unique_ptr<nimbusd::PlainRig> rig;
  plain::Status booted;
};

std::string tag(plain::Dialect d, const std::string& what) { return std::string(plain::dialectSlug(d)) + ": " + what; }

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// The reader above finds the segments by the log's own naming; an empty read is a red, not a pass.
void checkSegmentsFound(ndtest::Ctx& c, const Host& h) {
  c.ok(!h.stored().empty(), "the transcript is found on the host's storage");
}

// ---- (a) a text turn on every dialect ----------------------------------------------------
void testTextTurnOnEveryDialect(ndtest::Ctx& c) {
  std::printf("\n-- (a) one scripted text turn on engine 2, every dialect\n");
  for (plain::Dialect d : plain::kAllDialects) {
    Host h(d);
    c.ok(h.booted.ok(), tag(d, "the rig boots over the host's storage"));
    h.model.push(sp::say("Hello from engine two."));
    const nimbusd::PlainTurn t = h.rig->say("hello");
    c.ok(t.status.ok(), tag(d, "runTurn is ok"));
    c.eq(nimbusd::plainEndingSlug(t.ending), "reply", tag(d, "the turn ended with the model's reply"));
    c.eq(t.stated, "Hello from engine two.", tag(d, "the scripted answer is the turn's reply"));
    c.eqi((long)t.sent.size(), 1, tag(d, "the channel got one message"));
    c.eq(t.sent.empty() ? "" : t.sent[0], "Hello from engine two.", tag(d, "and it is the model's text, unchanged"));
    c.ok(t.out.delivered, tag(d, "the outcome says delivered"));
    c.eqi((long)h.model.requests().size(), 1, tag(d, "one request"));
    c.ok(!h.model.requests().empty() && has(h.model.last().body, "hello"), tag(d, "the owner text is in the request"));
    checkSegmentsFound(c, h);
    c.eq(h.kinds(), "utade", tag(d, "the transcript holds u t a d e (RB-E1a's text turn)"));
    const std::vector<tx::Item> items = h.stored();
    c.ok(items.size() == 5 && items[2].fin && items[2].x == "Hello from engine two.", tag(d, "the a item is the final text"));
    const tx::Item e = h.lastEnd();
    c.eq(e.end.cause, "reply", tag(d, "the e item's cause is reply"));
    c.eqi((long)e.end.headRequests, 1, tag(d, "the e item counts one head request"));
    std::string why;
    c.ok(h.model.verify(why), tag(d, "the script was used exactly (" + why + ")"));
  }
}

// ---- (b) the chat goes on, across turns and a reboot ---------------------------------------
void testChatGoesOn(ndtest::Ctx& c) {
  std::printf("\n-- (b) a second turn, and a reboot over the same storage\n");
  Host h;
  h.model.push(sp::say("First answer."));
  h.model.push(sp::say("Second answer."));
  h.model.push(sp::say("Third answer."));
  (void)h.rig->say("first question");
  h.ms += 60000;
  const nimbusd::PlainTurn t2 = h.rig->say("second question");
  c.eq(t2.stated, "Second answer.", "the second turn answers");
  c.ok(has(h.model.requests()[1].body, "First answer.") && has(h.model.requests()[1].body, "first question"),
       "the second request carries the first turn");
  c.eq(h.kinds(), "utadeutade", "two text turns, each u t a d e");
  h.rig.reset();   // the process is gone; the storage stays
  h.ms += 60000;
  h.reboot();
  c.ok(h.booted.ok(), "a rig rebooted over the same storage boots");
  const nimbusd::PlainTurn t3 = h.rig->say("third question");
  c.eq(t3.stated, "Third answer.", "the rebooted rig answers");
  const std::string& body = h.model.requests()[2].body;
  c.ok(has(body, "first question") && has(body, "Second answer."), "and its request carries the turns before the reboot");
  // After a restart the loop opens cold and moves the stub pointer over the closed turns (an
  // x item, RB-E1a's turn open); nothing of the earlier turns is written again.
  c.eq(h.kinds(), "utadeutadeutxade", "the third turn is appended after a cold open, nothing written twice");
  const std::vector<tx::Item> items = h.stored();
  c.ok(items.size() == 16 && items[12].why == "cold", "the x item is the restart's cold open");
  c.eqi((long)h.lastEnd().turn, 3, "the rebooted engine numbers the turn 3");
}

// ---- (c) the failure faces ------------------------------------------------------------------
struct Face {
  const char* name;
  sp::Step step;
  const char* cause;
  bool park;
};

void checkOneLine(ndtest::Ctx& c, const Host& h, const nimbusd::PlainTurn& t, const std::string& name, const char* cause) {
  c.ok(t.status.ok(), name + ": runTurn is ok (a failed exchange is an outcome, not a fault)");
  c.eq(nimbusd::plainEndingSlug(t.ending), "status", name + ": the turn ended with a status line");
  c.eqi((long)t.sent.size(), 1, name + ": the owner got exactly one message");
  c.ok(!t.stated.empty() && !t.sent.empty() && t.stated == t.sent[0] && t.stated == t.out.statusText,
       name + ": it is the engine's plain line");
  c.ok(t.out.status == plain::StatusLine::ProviderError, name + ": the line is the provider-error line");
  c.eq(h.lastEnd().end.cause, cause, name + ": the e item names the cause");
}

void testFailureFaces(ndtest::Ctx& c) {
  std::printf("\n-- (c) the provider errors, hangs, answers unreadably, or answers nothing\n");
  const std::vector<Face> faces = {
      {"server 500", sp::httpError(500), "provider:server", false},
      {"overloaded 529", sp::httpError(529), "provider:server", false},
      {"rate limited 429", sp::httpError(429), "provider:rate-limited", false},
      {"key refused 401", sp::httpError(401), "provider:key", true},
      {"not found 404", sp::httpError(404), "provider:other", false},
      {"connection dropped", sp::transportCut(), "provider:transport", false},
      {"unreadable 200", sp::raw(200, "this is not json"), "provider:malformed", false},
  };
  for (const Face& f : faces) {
    Host h;
    const sp::Step step = f.step;
    h.model.respondWith([step](const sp::Recorded&, size_t) { return step; });
    const uint32_t t0 = h.ms;
    const nimbusd::PlainTurn t = h.rig->say("are you there?");
    checkOneLine(c, h, t, f.name, f.cause);
    c.ok(t.out.parkHost == f.park, std::string(f.name) + ": the head is parked only for a refused key");
    c.ok(h.ms - t0 <= plain::kHardWallMs, std::string(f.name) + ": the turn ends inside the hard wall");
    c.ok(h.model.requests().size() <= 3, std::string(f.name) + ": at most two retries");
    // The chat goes on after the failure.
    h.model.respondWith(nullptr);
    h.model.push(sp::say("Back again."));
    h.ms += 60000;
    c.eq(h.rig->say("again").stated, "Back again.", std::string(f.name) + ": the next turn answers");
  }

  // A provider that never answers: every read hangs for two minutes.
  {
    Host h;
    h.model.respondWith([](const sp::Recorded&, size_t) { return sp::transportCut(120000, "read timed out"); });
    const uint32_t t0 = h.ms;
    const nimbusd::PlainTurn t = h.rig->say("hello?");
    checkOneLine(c, h, t, "hung provider", "provider:transport");
    c.ok(h.ms - t0 <= plain::kSoftWallMs + 10000, "hung provider: the turn ends by its wall clock");
  }

  // A 200 with no text and no call: the loop states it with one plain line (RB-E1a's review
  // fix), never silence and never a crash.
  {
    Host h;
    h.model.push(sp::say(""));
    const nimbusd::PlainTurn t = h.rig->say("say nothing");
    c.ok(t.status.ok(), "empty answer: no crash, runTurn is ok");
    c.eq(nimbusd::plainEndingSlug(t.ending), "status", "empty answer: the turn ends with a status line, not silence");
    c.eqi((long)t.sent.size(), 1, "empty answer: the owner got exactly one message");
    c.ok(!t.stated.empty() && !t.sent.empty() && t.stated == t.sent[0] && t.stated == t.out.statusText,
         "empty answer: it is the engine's plain line");
    c.ok(t.out.status != plain::StatusLine::None, "empty answer: the line is one of the closed list");
    c.ok(!t.out.delivered, "empty answer: the outcome does not claim a model reply");
    h.model.push(sp::say("Here now."));
    h.ms += 60000;
    c.eq(h.rig->say("again").stated, "Here now.", "empty answer: the next turn answers");
  }
}

// ---- (d) the host's own faults ---------------------------------------------------------------
void testHostFaults(ndtest::Ctx& c) {
  std::printf("\n-- (d) before boot, missing bindings, a second boot, a wire not landed\n");
  agent::Platform none;
  sp::ScriptedProvider model;
  txb::MemFiles files;
  nimbusd::PlainRig early(nimbusd::PlainRigConfig{}, nimbusd::PlainRigBindings{&none, &model, &files});
  const nimbusd::PlainTurn t = early.say("hello");
  c.eq(nimbusd::plainEndingSlug(t.ending), "failed", "a turn before boot fails");
  c.ok(t.status.err == tx::Err::InvalidArgument && !t.stated.empty(), "with a stated reason");
  c.ok(early.boot().err == tx::Err::InvalidArgument, "boot refuses a platform with no clock");
  c.ok(model.requests().empty(), "and nothing reached the provider");

  Host h;
  c.ok(h.rig->boot().err == tx::Err::Busy, "a second boot is refused");

  // A refused allocation at boot (ruling 30): a status, an unbooted rig, never an abort.
  {
    Host a;
    a.platform.allocLarge = [](size_t) -> void* { return nullptr; };
    a.reboot();
    c.ok(!a.booted.ok(), "a refused arena allocation fails the boot with a status");
    const nimbusd::PlainTurn ta = a.rig->say("hello");
    c.eq(nimbusd::plainEndingSlug(ta.ending), "failed", "and a turn on the unbooted rig fails, stated");
    c.ok(a.model.requests().empty(), "with nothing sent to the provider");
  }

  // The host's channel: it gets what the engine sends, to the chat; a send it refuses is
  // never reported as delivered.
  {
    Host s;
    std::vector<std::string> got;
    bool refuse = false;
    s.hostSend = [&](const std::string& chat, const std::string& text) {
      got.push_back(chat + "|" + text);
      return !refuse;
    };
    s.reboot();
    s.model.push(sp::say("To the host."));
    const nimbusd::PlainTurn ok = s.rig->say("hello");
    c.ok(got.size() == 1 && got[0] == "2001|To the host.", "the host's channel gets the reply, to the chat");
    c.eq(nimbusd::plainEndingSlug(ok.ending), "reply", "a reply the host took is a reply");
    refuse = true;
    s.model.push(sp::say("Lost on the way."));
    s.ms += 60000;
    const nimbusd::PlainTurn lost = s.rig->say("again");
    c.eq(nimbusd::plainEndingSlug(lost.ending), "failed", "a reply the host refused is not a reply");
    c.ok(lost.sent.empty() && lost.unsent.size() == 1 && lost.unsent[0] == "Lost on the way.",
         "it is kept as unsent");
    c.ok(!lost.out.delivered && !lost.stated.empty(), "and the outcome says so");
    c.ok(s.kinds().find("utae", 5) != std::string::npos, "no d item for a refused send");
  }

  // One turn at a time: a host send that answers back into the rig is refused, and the outer
  // turn's record is kept whole.
  {
    Host r;
    nimbusd::PlainTurn inner;
    r.hostSend = [&](const std::string&, const std::string&) {
      inner = r.rig->say("from inside");
      return true;
    };
    r.reboot();
    r.model.push(sp::say("Outer."));
    const nimbusd::PlainTurn outer = r.rig->say("hello");
    c.ok(inner.status.err == tx::Err::Busy && !inner.stated.empty(), "a say inside a turn is refused as busy");
    c.ok(outer.ending == nimbusd::PlainEnding::Reply && outer.sent.size() == 1, "the outer turn is whole");
    r.hostSend = nullptr;
    r.model.push(sp::say("Next."));
    r.ms += 60000;
    c.eq(r.rig->say("next").stated, "Next.", "the rig takes the next turn after it");
  }
  nimbusd::PlainRig noFiles(nimbusd::PlainRigConfig{}, nimbusd::PlainRigBindings{&h.platform, &h.model, nullptr});
  c.ok(noFiles.boot().err == tx::Err::InvalidArgument, "boot refuses no storage");
  nimbusd::PlainRig noTransport(nimbusd::PlainRigConfig{}, nimbusd::PlainRigBindings{&h.platform, nullptr, &h.files});
  c.ok(noTransport.boot().err == tx::Err::InvalidArgument, "boot refuses no transport");

  // No wire given: the rig asks wireFor. Until the dialect's wire lane lands that is one
  // plain line and no request; after, it is the model's reply.
  for (plain::Dialect d : plain::kAllDialects) {
    Host w(d, false);
    const plain::WireCodec* real = nullptr;
    const bool landed = plain::wireFor(d, real).ok() && real;
    w.model.push(sp::say("Real wire."));
    const nimbusd::PlainTurn tw = w.rig->say("hello");
    if (landed) {
      c.eq(tw.stated, "Real wire.", tag(d, "the landed wire answers"));
    } else {
      c.eq(nimbusd::plainEndingSlug(tw.ending), "status", tag(d, "no wire yet: one plain line"));
      c.eqi((long)tw.sent.size(), 1, tag(d, "no wire yet: exactly one message"));
      c.ok(w.model.requests().empty(), tag(d, "no wire yet: no request is sent"));
      c.eq(w.lastEnd().end.cause, "setup:not-implemented", tag(d, "no wire yet: the e item says why"));
    }
  }
}

// ---- (e) a tool binding on the rig -----------------------------------------------------------
// A read_file binding on the rig: records each path and whether the caller was the owner; a
// missing note fails, and two paths throw (an error, a refused allocation).
struct ReadBinding {
  std::vector<std::string> seen;
  std::vector<bool> asOwner;

  orch::ToolResult handle(ArduinoJson::JsonObjectConst a, const orch::Principal& who) {
    const std::string path = a["path"] | std::string("<none>");
    if (path == "notes/throws.md") throw std::runtime_error("handler broke");
    if (path == "notes/huge.md") throw std::bad_alloc();
    seen.push_back(path);
    asOwner.push_back(who.owner);
    if (path == "notes/missing.md") return orch::ToolResult::fail("no such file: notes/missing.md");
    return orch::ToolResult::ok("alpha");
  }
  void bind(Host& h) {
    h.bindTools = [this](nimbusd::PlainRig& rig) {
      const auto* decl = plain::toolDecl("read_file");
      rig.registry().add(
          "read_file", decl ? decl->description : "",
          [this](ArduinoJson::JsonObjectConst a, const orch::Principal& who) { return handle(a, who); },
          decl ? plain::declSchema(*decl, false) : "{}");
    };
    h.reboot();
  }
};

bool lastTurnHasFailedResult(const Host& h) {
  const uint32_t turn = h.lastEnd().turn;
  bool failed = false;
  for (const tx::Item& it : h.stored())
    if (it.kind == tx::ItemKind::Result && it.turn == turn) failed = !it.ok;
  return failed;
}

void testToolBinding(ndtest::Ctx& c) {
  std::printf("\n-- (e) a tool registered on the rig is what a call reaches\n");
  Host h;
  ReadBinding rb;
  rb.bind(h);
  h.model.push(sp::call("read_file", R"({"path":"notes/a.md"})"));
  h.model.push(sp::say("It says alpha."));
  h.model.push(sp::call("read_file", R"(["not","an","object"])"));
  h.model.push(sp::say("Nothing there."));
  h.model.push(sp::call("read_file", R"({"path":"notes/missing.md"})"));
  h.model.push(sp::say("It is missing."));
  const nimbusd::PlainTurn t = h.rig->say("read my note");
  c.eq(t.stated, "It says alpha.", "the turn answers after the tool round");
  c.ok(rb.seen.size() >= 1 && rb.seen[0] == "notes/a.md", "the registered handler got the call's arguments");
  c.ok(!rb.asOwner.empty() && rb.asOwner[0], "with the chat's Principal (the owner)");
  c.ok(h.model.requests().size() >= 2 && has(h.model.requests()[1].body, "alpha"), "the result reached the next request");
  c.ok(t.out.toolCalls == 1, "the outcome counts one call");
  h.ms += 60000;
  const nimbusd::PlainTurn t2 = h.rig->say("again");
  c.eq(t2.stated, "Nothing there.", "a call with arguments that are not an object still ends in a reply");
  c.ok(rb.seen.size() == 2 && rb.seen[1] == "<none>", "and its handler saw an empty object");
  h.ms += 60000;
  c.eq(h.rig->say("and the missing one").stated, "It is missing.", "a tool that fails still ends in a reply");
  c.ok(lastTurnHasFailedResult(h), "and its failure is stored as a failed result, with the handler's error text");

  std::string why;
  c.ok(h.model.verify(why), "the script was used exactly (" + why + ")");
}

// A handler that throws: that call gets an error result at the dispatch seam and the turn goes on to
// the model's reply (ruling 56). A refused allocation inside a handler ends the turn with one plain line.
void testThrowingHandler(ndtest::Ctx& c) {
  std::printf("\n-- (g) a handler that throws\n");
  Host h;
  ReadBinding rb;
  rb.bind(h);
  h.model.push(sp::call("read_file", R"({"path":"notes/throws.md"})"));
  h.model.push(sp::say("That one broke."));
  const nimbusd::PlainTurn thrown = h.rig->say("read the broken one");
  c.eq(nimbusd::plainEndingSlug(thrown.ending), "reply", "a throwing handler: the turn still ends with the reply");
  c.eq(thrown.stated, "That one broke.", "a throwing handler: the model saw the error and answered");
  c.ok(lastTurnHasFailedResult(h), "a throwing handler: its call is answered with a failed result");
  h.model.push(sp::call("read_file", R"({"path":"notes/huge.md"})"));
  h.ms += 60000;
  const nimbusd::PlainTurn oom = h.rig->say("read the huge one");
  c.eq(nimbusd::plainEndingSlug(oom.ending), "status", "a refused allocation in a handler: one plain line, never silence");
  c.ok(oom.sent.size() == 1 && !oom.stated.empty(), "a refused allocation in a handler: exactly one message");
  h.model.push(sp::say("Fine again."));
  h.ms += 60000;
  c.eq(h.rig->say("still there?").stated, "Fine again.", "a refused allocation in a handler: the next turn answers");
  std::string why;
  c.ok(h.model.verify(why), "the script was used exactly (" + why + ")");
}

// ---- (f) both engines in one translation unit ------------------------------------------------
void testBothEnginesCompile(ndtest::Ctx& c) {
  std::printf("\n-- (f) the old engine's rig and the plain rig in one translation unit\n");
  c.ok(sizeof(nimbusd::NimbusdRig) > 0 && sizeof(nimbusd::PlainRig) > 0, "rig.h and plain_rig.h compile together");
}

}  // namespace

int main() {
  ndtest::Ctx c;
  std::printf("=== engine 2 on the hosted process (lane RB-I1a, offline) ===\n");
  testTextTurnOnEveryDialect(c);
  testChatGoesOn(c);
  testFailureFaces(c);
  testHostFaults(c);
  testToolBinding(c);
  testBothEnginesCompile(c);
  testThrowingHandler(c);   // last: on a core that lets a handler throw through, the process ends here
  std::printf("\n%d checks, %d failures\n", c.checks, c.failures);
  std::printf("%s\n", c.failures ? "FAILED" : "PASSED");
  return c.failures ? 1 : 0;
}
