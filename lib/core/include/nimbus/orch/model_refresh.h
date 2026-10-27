#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/cloud/relay_heap.h"     // kRelayHeapFloorLargest - the one-connection floor
#include "nimbus/orch/models_tool.h"      // RefreshAsk
#include "nimbus/orch/provider_slots.h"   // kProviderSlotCount

// model_refresh - when the device refreshes a provider's model list (owner ruling
// R5). Three things ask for one: the head (models.list refresh:true, or an
// orch_model the cached list does not hold), the owner opening a model dropdown
// (POST /api/models/refresh), and the passive check (weekly, or after a firmware
// update). They all land in one RAM queue with one rule: at most one refresh per
// provider per hour, whoever asks. The main loop starts a queued refresh only
// when the device is free (no turn in flight, online, the verify slot idle, the
// resting heap above the one-connection floor), one provider at a time, on the
// self-deleting verify task: a request never opens a connection itself. A refresh
// that ran but left the list still due (no answer, a cut list, a failed write) is
// tried again on a growing delay, not an hour or six later.
//
// The passive check reads the header of the provider's model store on flash (the
// list's fetch time and the firmware that wrote it), never NVS.
//
// Portable, Arduino-free, host-tested (test_models_refresh).
namespace nimbus {
namespace orch {

// The resting largest internal block a list refresh needs. A refresh is ONE GET on
// one TLS connection, the same handshake every turn request and the relay dial make,
// so it takes their per-connection floor (relay_heap.h, == ORCH_TURN_MIN_LARGEST_BLOCK),
// not the verify's 8,000 B. Production, 26 Oct 2026: Lumi's largest block rests at
// 7,156 B after its first turns (7,668 B on v4.5.16) while every turn's handshake runs
// at that size; behind the 8,000 B gate no refresh ever started, so the lists stayed
// the legacy merge of the update for hours (every provider listSrc legacy, Mistral empty).
inline constexpr uint32_t kRefreshMinLargest = (uint32_t)nimbus::cloud::kRelayHeapFloorLargest;
inline bool refreshHeapOk(uint32_t restingLargest) { return restingLargest >= kRefreshMinLargest; }

// Where a due refresh runs. Its own worker (the verify task) needs its 8 KB stack as
// ONE internal block plus the spawn margin (heap_watch.h spawnVerdict). On a fragmented
// heap that block does not exist (Lumi: 7,156 B largest with 32 KB free at rest), so
// the refresh runs on the Telegram poll task instead, between its own work, with no new
// task (the staged-work hook an OTA check that does not fit uses), at a turn's own
// floors: the one-connection largest block and the total-free backstop. Wait: a turn is
// in flight, the heap is below those floors, or no poll task runs.
enum class RefreshRoute : uint8_t { Wait, Task, Poll };
inline constexpr uint32_t kRefreshTaskStackBytes = 8192;   // provider_verify's worker stack
inline constexpr uint32_t kRefreshTaskSlackBytes = 512;    // its task control block and allocator header
inline constexpr uint32_t kRefreshPollMinFree = 12000;     // a turn's total-free floor (ORCH_TURN_HARD_FLOOR)
RefreshRoute refreshRoute(bool inTurn, bool pollAlive, uint32_t intFree, uint32_t liveLargest,
                          uint32_t restingLargest);
const char* refreshRouteToken(RefreshRoute r);   // "wait" | "task" | "poll"

class ModelRefreshQueue {
 public:
  // One refresh per provider per this window, counted from the last start.
  static constexpr uint32_t kMinGapMs = 3600u * 1000u;
  // A refresh that ran but left the list due again: the next try waits this long,
  // doubling per failure up to kRetryMaxMs (2, 4, 8, 16, 32, 60, 60... minutes).
  static constexpr uint32_t kRetryFirstMs = 2u * 60u * 1000u;
  static constexpr uint32_t kRetryMaxMs = 60u * 60u * 1000u;

  // Ask for a refresh of `slug` at `nowMs`. A provider with no key gets NoKey; one
  // already queued (or running, or waiting for its retry) Pending; one started less
  // than kMinGapMs ago Recent; otherwise it is queued. A slug that is not a provider
  // slot: NoKey. `force` (a key just saved or verified: the list may differ now) skips
  // the hourly window and a waiting retry's delay.
  RefreshAsk request(const char* slug, bool keyed, uint32_t nowMs, bool force = false);

  // The queued provider to start now, or nullptr: never during a turn, offline,
  // while the verify slot is busy, while the resting heap is below the floor
  // (refreshHeapOk), or before a failed refresh's retry time. Providers take turns
  // in registry order after the last one started.
  const char* due(bool inTurn, bool online, bool busy, bool restingOk, uint32_t nowMs) const;

  // `slug`'s refresh started at `nowMs` (the rate window starts here).
  void started(const char* slug, uint32_t nowMs);

  // `slug`'s refresh finished at `nowMs`. `failed` (the stored list is still due):
  // it is queued again for kRetryFirstMs from now, doubling per consecutive failure
  // up to kRetryMaxMs, whatever the hourly window says. A landed refresh ends the
  // streak. Returns the retry delay in ms (0 when it landed).
  uint32_t finished(const char* slug, bool failed, uint32_t nowMs);

  // A queued refresh that could not start (the verify task refused it): it stays
  // queued and is tried again on a later pass.
  bool queued(const char* slug) const;
  // Queued again after a failure and still before its retry time (no refresh is
  // under way for it: the owner's page does not wait on it).
  bool retryWaiting(const char* slug, uint32_t nowMs) const;
  // Last start time of `slug` (0 with ran=false when it never ran this boot).
  bool lastStart(const char* slug, uint32_t& atMs) const;

 private:
  struct Slot {
    bool     wanted = false;
    bool     ran = false;
    uint32_t lastMs = 0;
    uint8_t  fails = 0;        // consecutive refreshes that left the list due
    bool     retry = false;    // wanted again after a failure: not before retryAt
    uint32_t retryAt = 0;
  };
  static int indexOf(const char* slug);
  Slot   slots_[kProviderSlotCount];
  size_t next_ = 0;   // registry index the next due() scan starts at
};

// ---- the stamp: the header of the provider's model store ---------------------
// Lane F's model store (/data/models_<slug>.txt, model_catalog.h) starts with
// "#models v1 fetchedAt=<unix s> fw=<firmware version> ...": when the list was fetched
// and the firmware that wrote it. The passive refresh reads those two fields.
struct ModelsStamp {
  bool        present = false;   // the store has a header (a list was fetched or merged)
  uint32_t    fetchedAt = 0;     // 0 when the clock was unset at the fetch (or a legacy merge)
  std::string fw;                // the firmware version that wrote the store
  std::string src;               // "api" (fetched) | "legacy" (an older build's lists merged)
};
// The stamp of a store text (parseModelStoreHeader); present=false with no header.
ModelsStamp stampFromStore(const std::string& storeText);

// ---- the passive refresh ------------------------------------------------------
// Why a stored list is due for a passive refresh.
enum class PassiveWhy : uint8_t {
  None,         // fresh enough
  NoStamp,      // never fetched by a build that keeps the stamp
  Firmware,     // fetched by another firmware (the first boots after an update)
  Stale,        // older than kPassiveMaxAgeS
  UnknownAge,   // fetched while the clock was unset; the clock is set now
  Legacy,       // the boot merge of an older build's lists: never fetched, clock or not
};
constexpr uint32_t kPassiveMaxAgeS = 7u * 24u * 3600u;   // weekly
// nowEpoch 0 = clock unset (no age can be judged then).
PassiveWhy passiveRefreshDue(const ModelsStamp& s, const std::string& runningFw, uint32_t nowEpoch);
const char* passiveWhyToken(PassiveWhy w);   // "none" | "no-stamp" | "firmware" | "stale" | "unknown-age" | "legacy"

// The passive check's choice for one provider: keyed, its key not rejected (a
// rejected key is the owner's to fix), and its stored list due. A provider paused by
// a spend limit or parked by a refusal still qualifies: the list GET is free and
// spends nothing, so no pause or park is an input here (by design).
bool passiveWants(bool keyed, int8_t verifyVerdict, const ModelsStamp& s, const std::string& runningFw,
                  uint32_t nowEpoch);

}  // namespace orch
}  // namespace nimbus
