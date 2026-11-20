#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/truth.h"

namespace nimbus {
namespace orch {
namespace tx {
class TxArena;
class TxFiles;
}  // namespace tx
}  // namespace orch
}  // namespace nimbus

// plain/truth.h - the truth check bound into the plain engine (PRD section 8.8; lane RB-G1). The
// check itself is lib/core's (nimbus::orch::truth, lane RB-G0: the measured probe, the facts item,
// the receipt line, the modes and their counters); this binds it to the engine's seams:
//
//   beforeDeliver (HookSlot::Truth)    this turn's effects and the chat's recent receipts are read
//                                      from the transcript (the arena's c and r items; RecentSource
//                                      for what is no longer resident), the probe is asked, and in
//                                      enforce mode a yes holds the reply BEFORE delivery: one
//                                      '[device] effects this turn: ...' item and one more round; what
//                                      that round answers is delivered unchanged. The second call
//                                      (DeliverCtx::held) decides the failed-write receipt. Nothing
//                                      is held after delivery, on a cut answer or on a guard's last
//                                      round, and at most once a turn (the engine's rule too).
//   afterDeliver (HookSlot::Truth)     the turn's check closes: counters, the mode move (nextMode),
//                                      the state saved; a shadow turn is queued.
//   idleStep (HookSlot::ShadowProbe)   shadow mode: one queued turn is probed after delivery;
//                                      counters only. It sends nothing and stores nothing.
//
// The probe never rewrites, drops or adds a stored item of its own: it reads the transcript. A
// probe request is not conversation: it goes through TruthDeps::probe (never the engine's
// transport), is counted in DeliverDecision::probes (the e item's probeRequests, their own column)
// and never in the head requests, the turn's tokens or the conversation's marks. A hold never makes
// the owner's answer poorer or silent: when the extra round fails or comes back empty, the held
// reply is what goes out. A held turn ticks TruthDeps::progress (the RB-E3 status-line class), so a
// long hold says the device is waiting for the model's answer.
//
// Seam declared by lane RB-B0a; the body (plain_truth.cpp) is lane RB-G1's.
namespace agent {
namespace plain {

class ProgressReporter;

enum class TruthMode : uint8_t { Off = 0, ReceiptOnly = 1, Shadow = 2, Enforce = 3 };
inline const char* truthModeSlug(TruthMode m) {
  switch (m) {
    case TruthMode::Off: return "off";
    case TruthMode::ReceiptOnly: return "receipt";
    case TruthMode::Shadow: return "shadow";
    case TruthMode::Enforce: return "enforce";
  }
  return "off";
}

inline constexpr uint32_t kProbeTimeoutMs = nimbus::orch::truth::kProbeTimeoutMs;
// The recent receipts the probe is shown: the newest of the chat, as many as the old engine's ring.
inline constexpr size_t kRecentReceipts = nimbus::orch::fx::kRingEntries;

// One effect receipt read from the transcript: a c item of an effect tool and the r item that answers it.
struct Receipt {
  uint64_t q = 0;        // the r item
  uint32_t turn = 0;
  uint64_t epoch = 0;    // r.t (0: the clock was not set)
  std::string tool;      // the tool the call reached (a `more` call: its target)
  bool ok = false;
  std::string text;      // the result's first line (the receipt, or the tool's error)
};

// Receipts stored before the arena's oldest resident item (the card): the newest `want` of them
// with q < beforeQ, oldest first. `whole` says the list reaches the chat's first item (nothing older
// exists). False: the source could not read (the list is then unknown).
using RecentSource =
    std::function<bool(uint64_t beforeQ, size_t want, std::vector<Receipt>& out, bool& whole)>;

// What one turn's check did (TruthDeps::done): the parity rig's per-turn columns.
struct TruthReport {
  uint32_t turn = 0;
  nimbus::orch::truth::Truth mode = nimbus::orch::truth::Truth::ReceiptOnly;   // the turn's effective mode
  bool ringKnown = false;    // the recent receipts were the chat's whole record
  uint8_t probes = 0;        // probe requests sent before delivery
  bool held = false;         // the reply was held for one facts-only round
  bool retried = false;      // that round's reply went out
  bool receipt = false;      // a failed-write receipt went out
  bool shadowDue = false;    // queued for the idle step
  nimbus::orch::truth::Verdict first = nimbus::orch::truth::Verdict::Error;
};

struct TruthDeps {
  // The most the binding allows: Off registers nothing; ReceiptOnly never asks the probe; Shadow
  // and Enforce let the head's table row and stored state decide, up to that mode.
  TruthMode mode = TruthMode::Enforce;
  // The chat's arena (the engine's; required): this turn's effects and the recent receipts.
  const nimbus::orch::tx::TxArena* arena = nullptr;
  // Sends one probe request (the binding's transport and key: the device's TLS slot, nimbusd's
  // client, a test's script). Empty: no probe, every head receipt-only (counted as failures).
  nimbus::orch::truth::ProbeFn probe;
  // Every head's mode and counters (truth_state.json), shared by the chats; null: kept in RAM.
  nimbus::orch::truth::State* state = nullptr;
  std::function<void(const nimbus::orch::truth::State&)> save;   // after a turn changed it (optional)
  bool allowShadow = false;   // nextMode may move a head enforce -> shadow (PRD 8.8)
  RecentSource older;          // receipts older than the arena (optional; recentFromLog)
  std::function<int64_t()> now;   // epoch seconds (null: TurnCtx::startedAt)
  ProgressReporter* progress = nullptr;   // a hold ticks it (ProgressKind::ProviderWait)
  std::function<uint32_t()> nowMs;        // its clock (null: TurnCtx::startedMs)
  std::function<void(const TruthReport&)> done;   // each closed check (optional)
};

// Registers the three handlers. InvalidArgument (nothing registered) without an arena; Busy when a
// slot is taken; with mode Off nothing is registered.
Status installTruth(PlainHooks& hooks, const TruthDeps& deps);

// ---- the pieces, for the hosts and the tests -------------------------------------------------
// The receipt kind of an engine-2 tool (the noun of the receipt line): `effect` false is Read.
nimbus::orch::truth::Kind receiptKind(const std::string& tool, bool effect);
// The effect receipts of a sequence of items in q order (c and r items paired in their turn; any
// other kind is skipped): `out` gets one Receipt per answered effect call.
class ReceiptScan {
 public:
  void add(const nimbus::orch::tx::Item& it, std::vector<Receipt>& out);

 private:
  struct Open {
    uint32_t turn;
    std::string id, tool;
    bool effect;
  };
  std::vector<Open> open_;
};
// RecentSource over the chat's stored segments (TxFiles): reads whole segments backwards from the
// one before `beforeQ`, at most `maxSegments` of them (each one bounded read per slice).
RecentSource recentFromLog(nimbus::orch::tx::TxFiles& files, const std::string& chatKey, size_t maxSegments = 2);

}  // namespace plain
}  // namespace agent
