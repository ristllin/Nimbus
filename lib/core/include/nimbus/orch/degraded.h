#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/provider_park.h"

// degraded - the device says when it runs degraded (lane DEGRADE, owner 6 Nov 2026).
//
// Before this, six ways of running below par were visible only to someone reading the
// logs: the head provider down with a 2nd or 3rd choice answering, memory without its
// embeddings, a provider out of credit, a full file store, Telegram gone quiet and an
// unsynced clock. The 4-6 Nov outage kept /api/state green for 28 hours while no
// message was answered. One module now owns the six conditions, so the device screen,
// the web page, the details card and /api/state read ONE truth and cannot disagree.
//
//   facts (the device collects them)  ->  computeDegraded  ->  DegradedDebounce  ->  surfaces
//
// - computeDegraded is a pure function of the facts: one line per condition (short, on
//   the screen and the web banner), and the reason with the one next step (one tap away).
// - DegradedDebounce is what keeps it calm: a condition shows only after it has held for
//   kRaiseMs, hides only after it has been gone for kClearMs, so a flapping input moves
//   the panel at most once. No sound, no popup, no message to the owner: a status line.
// - ServingTracker remembers which provider answered the last turn and which ones it
//   went past, because "the main provider is down" is only known from a turn that failed
//   over.
//
// Portable (no Arduino), host-tested (test_degraded). Copy follows AGENTS.md section 6:
// sentence case, ASCII, no em dash, the device is "it".
namespace nimbus {
namespace orch {

// The six conditions, in display order (the most consequential first). The slugs are the
// machine ids /api/state carries: append-only, never renamed.
enum class DegradedId : uint8_t {
  HeadFallback = 0,   // the main provider is unavailable, a later choice answers
  ProviderParked,     // a provider is out of credit, refused its key, or is paused
  Memory,             // embeddings are unavailable: memory is keyword-only, saves queue
  StoreFull,          // the file store (or the internal flash) is full
  TelegramSilent,     // the Telegram loop is not answering
  ClockUnsynced,      // the clock has not synced
};
inline constexpr int kDegradedCount = 6;
const char* degradedSlug(DegradedId id);   // "head_fallback", "provider_parked", ...

// The longest status line, in characters (ASCII). It fits the home screen's strip on the
// smallest supported panel (320 px wide) with its bullet and its chevron.
inline constexpr size_t kDegradedLineMax = 40;

// How long a condition must hold before it shows, and be gone before it hides.
inline constexpr uint32_t kRaiseMs = 10u * 1000u;
inline constexpr uint32_t kClearMs = 30u * 1000u;

// The clock gets this long after boot to sync before it counts as unsynced.
inline constexpr uint32_t kClockGraceMs = 120u * 1000u;
// A head that is not held off (no park, no pause) and has not failed a turn for this long
// is taken as recovered: the next turn will say otherwise if not.
inline constexpr uint32_t kFallbackQuietMs = 30u * 60u * 1000u;
// The Telegram loop is silent when it has not checked in for this long (a normal cycle is
// under a minute; the 4-6 Nov outage lasted 28 hours, so ten minutes is early enough and
// clear of a slow file download). A turn or staged work in flight gets the longer
// allowance: it runs on the same task.
inline constexpr uint32_t kTgIdleMs = 10u * 60u * 1000u;
inline constexpr uint32_t kTgIdleBusyMs = 30u * 60u * 1000u;
// Telegram is unreachable after this many getUpdates failures in a row.
inline constexpr uint32_t kTgFailsUnreachable = 5;

struct DegradedCondition {
  DegradedId  id = DegradedId::HeadFallback;
  std::string line;   // the status line: <= kDegradedLineMax, printable ASCII
  std::string why;    // what happened and the one next step, sentences
};
// In display order, at most one entry per id.
using DegradedSet = std::vector<DegradedCondition>;

// ---- the facts -------------------------------------------------------------------

// memory_tools.h defines it; the facts carry it opaquely so this header (which the screen's
// render context includes) does not pull the memory tools in.
enum class EmbedFail;

// What the last turn that billed went past (ServingTracker::fact).
struct FallbackFact {
  bool active = false;                // a later choice answered the last turn
  std::string head;                   // the provider the turn started on (slug)
  std::string served;                 // the provider that answered (slug)
  std::vector<std::string> down;      // every provider it went past, in order (slugs)
  uint32_t ageMs = 0;                 // since that turn ended
  bool headClosed = false;            // the head is held off right now (a park or a pause)
  std::string headWhy;                // "is out of credit", "refused its key", ... ("" = none known)
  std::string headFix;                // the one next step ("" = it returns by itself)
};

// One provider the park holds off (ProviderPark::snapshot, reduced).
struct ParkedFact {
  std::string provider;                 // slug
  ParkKind kind = ParkKind::None;
  int httpStatus = 0;
  uint8_t seamMask = 0;                 // the seams a refusal closes (bit = 1 << PaidSeam)
  uint8_t paused = 0;                   // PauseCause bits
  ParkCause cause = ParkCause::Unspecified;
};

struct TelegramFact {
  bool configured = false;     // a bot token is set
  bool taskAlive = true;       // the poll task exists
  bool authRejected = false;   // Telegram refused the token (repeated 401/403)
  uint32_t fails = 0;          // getUpdates failures in a row
  uint32_t idleMs = 0;         // since the poll loop last checked in (0 = just now)
  bool turnBusy = false;       // a turn is running on that task
  bool wifiUp = true;          // the station link is up (a down network is the Wi-Fi glyph's news)
};

struct DegradedFacts {
  bool orchestrator = true;    // false in Notifier mode (none of these subsystems) and on a device still in first-run setup (not degraded, not set up yet)
  uint32_t uptimeMs = 0;
  FallbackFact fallback;
  std::vector<ParkedFact> parks;
  bool embedParked = false;    // embeddings are parked (EmbedPark)
  EmbedFail embedKind = EmbedFail(0);   // why (EmbedFail::None = not known), with embedDetail
  std::string embedDetail;     // classifyEmbedFail's secret-safe detail ("1024/768", a provider slug)
  int  pendingEmbeds = 0;      // memory writes waiting for an embedding
  bool storeFull = false;      // the file store refused a save for room, not yet made
  bool flashFull = false;      // the internal flash is full (memory persists pause)
  TelegramFact telegram;
  bool clockSynced = true;
  bool wifiUp = true;
};

// The conditions that hold right now, no dwell (DegradedDebounce adds that).
DegradedSet computeDegraded(const DegradedFacts& f);

// The provider as the owner knows it: the registry's label ("Cumulo Nimbus", "OpenAI"),
// else the slug.
std::string degradedProviderLabel(const std::string& slug);

// What a parked or paused provider's reason is, in words: the short phrase after the
// name ("is out of credit") and the one next step ("Add credit with the provider"). Both ""
// for a record that holds nothing. The spend ceiling's pause is not here (its own line).
struct ParkReason {
  std::string phrase;
  std::string fix;
};
ParkReason degradedParkReason(const ParkedFact& p);

// ---- the served-by memory ------------------------------------------------------------

// Remembers whether the last turn that billed was answered by a later choice than the
// head it started on. RAM only. Thread-safe (the turn task writes, the main loop reads).
class ServingTracker {
 public:
  // A turn ended. `requested` is the head it started on, `served` the provider that
  // answered, `path` the providers it ran on in order ("a -> b -> c"; "" = just the head),
  // `answered` whether any provider billed (a turn nothing answered says nothing).
  void noteTurn(const std::string& requested, const std::string& served, const std::string& path,
                bool answered, uint32_t nowMs);
  FallbackFact fact(uint32_t nowMs) const;
  void clear();
  // Locks and unlocks the mutex once, so a device creates its semaphore here (ESP-IDF makes a
  // std::mutex's at its first lock) and not at a runtime lock: see begin() in agent/degraded_device.h.
  void prime();

 private:
  mutable std::mutex mx_;
  bool active_ = false;
  std::string head_, served_;
  std::vector<std::string> down_;
  uint32_t atMs_ = 0;
};

// ---- the calm ------------------------------------------------------------------------

class DegradedDebounce {
 public:
  // Fold the conditions that hold at `nowMs` into the shown set. True when the set that
  // is SHOWN changed (a condition raised, cleared, or its status line changed); a change in
  // a shown condition's reason alone updates it silently.
  bool update(const DegradedSet& now, uint32_t nowMs);
  const DegradedSet& shown() const { return shown_; }
  uint32_t generation() const { return gen_; }
  void reset();

 private:
  struct Slot {
    bool present = false;       // held at the last update
    bool pending = false;       // a transition is timing
    uint32_t sinceMs = 0;
  };
  Slot slot_[kDegradedCount];
  DegradedSet shown_;
  uint32_t gen_ = 0;
};

// ---- the surfaces ----------------------------------------------------------------------

// The strip's text rows when it has `maxRows`: one per condition, or (when there are
// more) the first maxRows - 1 and "+N more".
std::vector<std::string> degradedStripRows(const DegradedSet& set, size_t maxRows);

// The details card (the reason one tap away): each condition's line (after a "- ") and its
// reason, blank-line separated. "" when nothing is degraded. The card's header carries the
// title, and its body draws at the small size (ScreenCtx::askCompact).
std::string degradedDetailText(const DegradedSet& set);

// /api/state "degraded": {"count":N,"gen":G,"items":[{"id","line","why"}]}. Always the
// same keys, so a poller never meets an absent field.
void degradedInto(const DegradedSet& set, uint32_t generation, JsonObject out);

}  // namespace orch
}  // namespace nimbus
