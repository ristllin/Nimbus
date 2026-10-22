#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "nimbus/orch/models_tool.h"      // RefreshAsk
#include "nimbus/orch/provider_slots.h"   // kProviderSlotCount

// model_refresh - when the device refreshes a provider's model list (owner ruling
// R5). Three things ask for one: the head (models.list refresh:true, or an
// orch_model the cached list does not hold), the owner opening a model dropdown
// (POST /api/models/refresh), and the passive check (weekly, or after a firmware
// update). They all land in one RAM queue with one rule: at most one refresh per
// provider per hour, whoever asks. The main loop starts a queued refresh only
// when the device is free (no turn in flight, online, the verify slot idle, the
// resting heap above the handshake floor), one provider at a time, on the
// self-deleting verify task: a request never opens a connection itself.
//
// The passive check reads a stamp kept with the provider's model list on flash
// (the list's fetch time and the firmware that fetched it), never NVS.
//
// Portable, Arduino-free, host-tested (test_models_refresh).
namespace nimbus {
namespace orch {

class ModelRefreshQueue {
 public:
  // One refresh per provider per this window, counted from the last start.
  static constexpr uint32_t kMinGapMs = 3600u * 1000u;

  // Ask for a refresh of `slug` at `nowMs`. A provider with no key gets NoKey; one
  // already queued (or running) Pending; one started less than kMinGapMs ago
  // Recent; otherwise it is queued. A slug that is not a provider slot: NoKey.
  RefreshAsk request(const char* slug, bool keyed, uint32_t nowMs);

  // The queued provider to start now, or nullptr: never during a turn, offline,
  // while the verify slot is busy or while the resting heap is below the floor.
  // Providers take turns in registry order after the last one started.
  const char* due(bool inTurn, bool online, bool busy, bool restingOk) const;

  // `slug`'s refresh started at `nowMs` (the rate window starts here).
  void started(const char* slug, uint32_t nowMs);

  // A queued refresh that could not start (the verify task refused it): it stays
  // queued and is tried again on a later pass.
  bool queued(const char* slug) const;
  // Last start time of `slug` (0 with ran=false when it never ran this boot).
  bool lastStart(const char* slug, uint32_t& atMs) const;

 private:
  struct Slot {
    bool     wanted = false;
    bool     ran = false;
    uint32_t lastMs = 0;
  };
  static int indexOf(const char* slug);
  Slot   slots_[kProviderSlotCount];
  size_t next_ = 0;   // registry index the next due() scan starts at
};

// ---- the stamp kept with a provider's model list ----------------------------
// One header line, "#models v1 fetchedAt=<unix s> fw=<firmware>", space-separated
// key=value pairs; a reader ignores keys it does not know.
struct ModelsStamp {
  bool        present = false;
  uint32_t    fetchedAt = 0;   // 0 when the clock was unset at the fetch
  std::string fw;              // the firmware version that fetched the list
};
std::string formatModelsStamp(uint32_t fetchedAt, const std::string& fw);
// Reads the first "#models v1" line of `text`; false (present=false) when none.
bool parseModelsStamp(const std::string& text, ModelsStamp& out);

// ---- the passive refresh ------------------------------------------------------
// Why a stored list is due for a passive refresh.
enum class PassiveWhy : uint8_t {
  None,         // fresh enough
  NoStamp,      // never fetched by a build that keeps the stamp
  Firmware,     // fetched by another firmware (the first boots after an update)
  Stale,        // older than kPassiveMaxAgeS
  UnknownAge,   // fetched while the clock was unset; the clock is set now
};
constexpr uint32_t kPassiveMaxAgeS = 7u * 24u * 3600u;   // weekly
// nowEpoch 0 = clock unset (no age can be judged then).
PassiveWhy passiveRefreshDue(const ModelsStamp& s, const std::string& runningFw, uint32_t nowEpoch);
const char* passiveWhyToken(PassiveWhy w);   // "none" | "no-stamp" | "firmware" | "stale" | "unknown-age"

}  // namespace orch
}  // namespace nimbus
