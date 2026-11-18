#pragma once
#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>

#include "nimbus/harness/plain/compact.h"
#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/engine.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/guards.h"
#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/rails.h"
#include "nimbus/harness/plain/request.h"
#include "nimbus/harness/plain/resume.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/rules.h"
#include "nimbus/orch/tx/status.h"

// tx_battery/pending.h - how a battery property waits for the lane that makes it pass
// (lane RB-B0b, PRD section 10: "Tests that wait for an implementation report
// TEST_IGNORE_MESSAGE('PENDING RB-<lane>')").
//
//   TXB_WAITS_FOR("PENDING RB-T3", "RB-T1");
//
// probes one sentinel body of RB-T3 and of each dependency listed. While any of them
// answers Err::NotImplemented the test is ignored with exactly the literal message, which
// test/pending_roster.txt names (check_pending_roster.py reads it from the source). Once
// every body exists the test runs by itself and must pass; its roster line then has to go.
//
// Built with -DTXB_FORCE_PENDING the gate never ignores: each pending test runs into its
// first stub and fails with "<call> -> not-implemented at <stub>", which is the documented
// reason a test fails when its ignore is removed before the implementation lands.
//
// A sentinel calls a body with empty inputs: a written body answers InvalidArgument (or
// any typed outcome) for them, never crashes. A lane that writes a body keeps that true.
namespace txb {

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;

struct NullFiles : tx::TxFiles {
  bool append(const std::string&, const char*, size_t) override { return false; }
  bool readAt(const std::string&, size_t, char*, size_t, size_t& got) override {
    got = 0;
    return false;
  }
  bool size(const std::string&, size_t&) override { return false; }
  bool writeWhole(const std::string&, const std::string&) override { return false; }
  bool rename(const std::string&, const std::string&) override { return false; }
  bool remove(const std::string&) override { return false; }
  bool list(const std::string&, std::vector<std::string>&) override { return false; }
};

inline tx::Status probeWire(plain::Dialect d) {
  const plain::WireCodec* w = nullptr;
  return plain::wireFor(d, w);
}

inline tx::Status probeParse() {
  const plain::WireCodec* w = nullptr;
  tx::Status st = plain::wireFor(plain::Dialect::AnthropicMessages, w);
  if (!st.ok() || !w) return st.ok() ? tx::Status::pending("plain::wireFor anthropic") : st;
  agent::HttpResponse resp;
  resp.status = 200;
  resp.body = "{}";
  plain::ParsedStep step;
  return w->parse(resp, step);
}

// RB-T1 writes three things the battery needs: encode, decode and the arena.
inline tx::Status probeCodec() {
  std::string s;
  tx::Status st = tx::encodeLine(tx::Item{}, s);
  if (st.notImplemented()) return st;
  tx::Item it;
  if ((st = tx::decodeLine("{}", 2, it)).notImplemented()) return st;
  tx::TxArena arena;
  return arena.init(64, [](size_t n) { return std::malloc(n); }, [](void* p) { std::free(p); });
}

inline tx::Status probeEngine() {
  plain::PlainEngine engine{plain::EngineDeps{}};
  plain::TurnOutcome out;
  return engine.runTurn(plain::TurnInput{}, out);
}

inline tx::Status probeLog() {
  static NullFiles files;
  tx::TxLog log(files, "probe");
  tx::HeadState head;
  return log.open(head);
}

inline tx::Status probeCompaction() {
  plain::PlainHooks hooks;
  return plain::installCompaction(hooks, plain::CompactDeps{});
}

inline tx::Status probeResume() {
  plain::PlainHooks hooks;
  return plain::installResume(hooks, plain::ResumeDeps{});
}

inline tx::Status probeRails() {
  plain::PlainHooks hooks;
  return plain::installRails(hooks, plain::RailsDeps{});
}

// One sentinel per lane whose body a battery property needs. A lane missing here is a
// battery bug: the gate fails rather than guess.
inline bool sentinelFor(const std::string& lane, std::function<tx::Status()>& out) {
  struct Row {
    const char* lane;
    std::function<tx::Status()> probe;
  };
  static const Row rows[] = {
      {"RB-T1", probeCodec},
      {"RB-T2", probeLog},
      {"RB-T3", [] { std::string s; return tx::omissionLine(1, "17 Nov", s); }},
      {"RB-T4", [] { tx::ProfileMarks m; return tx::profileMarks(tx::Profile::Standard, m); }},
      {"RB-EX", [] { plain::ExchangeResult r; return plain::runExchange({}, {}, r); }},
      {"RB-W1a", [] { return probeWire(plain::Dialect::AnthropicMessages); }},
      {"RB-W1b", probeParse},
      {"RB-W2", [] { return probeWire(plain::Dialect::OpenAIResponses); }},
      {"RB-W3", [] { return probeWire(plain::Dialect::ChatCompletions); }},
      {"RB-W4", [] { return probeWire(plain::Dialect::Flat); }},
      {"RB-E2", [] { plain::SystemBlocks b; return plain::composeSystem(plain::RequestFacts{}, b); }},
      {"RB-E1a", probeEngine},
      {"RB-E1b", [] { plain::GuardLimits l; return plain::guardLimitsFor(8, {}, l); }},
      {"RB-E3", [] { tx::Item i; return plain::turnEndItem(plain::TurnCtx{}, {}, i); }},
      {"RB-X1", probeResume},
      {"RB-C1", [] { std::string s; return tx::chatsumMirror({}, 0, s); }},
      {"RB-C2", probeCompaction},
      {"RB-RL", probeRails},
  };
  for (const Row& r : rows)
    if (lane == r.lane) {
      out = r.probe;
      return true;
    }
  return false;
}

// The lane a "PENDING RB-<lane>" message names.
inline std::string laneOf(const char* message) {
  const char* p = std::strstr(message, "RB-");
  if (!p) return "";
  std::string lane;
  while (*p && *p != ' ') lane += *p++;
  return lane;
}

// "" when every lane's sentinel is built; else the first lane still answering a stub.
inline std::string firstUnbuilt(const char* message, std::initializer_list<const char*> deps, std::string& err) {
  std::string owner = laneOf(message);
  std::vector<std::string> lanes{owner};
  for (const char* d : deps) lanes.push_back(d);
  for (const std::string& lane : lanes) {
    std::function<tx::Status()> probe;
    if (!sentinelFor(lane, probe)) {
      err = "tx_battery: no sentinel for lane '" + lane + "' (add one in pending.h)";
      return lane;
    }
    if (probe().notImplemented()) return lane;
  }
  return "";
}

inline bool forcePending() {
#ifdef TXB_FORCE_PENDING
  return true;
#else
  return false;
#endif
}

}  // namespace txb

namespace txb {
// 0 = run the test, 1 = ignore it (still pending), 2 = the gate itself is wrong (see
// lastWaitError). No string outlives the call, so the ignore's longjmp leaks nothing.
inline std::string& lastWaitError() {
  static std::string e;
  return e;
}
inline int waitDecision(const char* message, std::initializer_list<const char*> deps) {
  std::string err;
  const std::string missing = firstUnbuilt(message, deps, err);
  lastWaitError() = err;
  if (!err.empty()) return 2;
  return !missing.empty() && !forcePending() ? 1 : 0;
}
// The failure text of TXB_OK, kept outside the test's frame (Unity's failure longjmps).
inline const char* okFailure(const char* expr, const nimbus::orch::tx::Status& st) {
  static std::string m;
  m = std::string(expr) + " -> " + nimbus::orch::tx::errSlug(st.err) + " at " + st.where;
  return m.c_str();
}
}  // namespace txb

// Ignore the running test with `message` (a literal "PENDING RB-<lane>") while the named
// lane or a listed dependency still answers NotImplemented.
#define TXB_WAITS_FOR(message, ...)                                   \
  do {                                                                \
    const int txb_d_ = txb::waitDecision(message, {__VA_ARGS__});     \
    if (txb_d_ == 2) TEST_FAIL_MESSAGE(txb::lastWaitError().c_str()); \
    if (txb_d_ == 1) TEST_IGNORE_MESSAGE(message);                    \
  } while (0)

// A typed outcome that must be ok; a stub fails with its own name (the documented reason).
#define TXB_OK(expr)                                                                      \
  do {                                                                                    \
    const nimbus::orch::tx::Status txb_st_ = (expr);                                      \
    if (!txb_st_.ok()) TEST_FAIL_MESSAGE(txb::okFailure(#expr, txb_st_));                 \
  } while (0)
