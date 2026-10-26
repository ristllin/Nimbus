#include "provider_gate.h"

#include <LittleFS.h>
#include <time.h>

#include <string>

#include "../sys/agent_log.h"
#include "fs_atomic.h"      // flashMounted, writeFileAtomic - the park's flash file
#include "../sys/psram_static.h"   // psramOrInternalCalloc - the park's records in PSRAM
#include "orchestrator.h"   // inScheduledTurn, firstAllowedChat - the daily spend notice
#include "store.h"          // modelRankPrice - the ledger's prices (CUM-708)
#include "nimbus/orch/spend_ceiling.h"  // spendFrozen - the freeze reaches the park first
#include "nimbus/orch/usage_ledger.h"   // setRankPriceSource
#include "telegram.h"       // send - the daily spend notice

namespace agent::gate {

namespace {
namespace orch = nimbus::orch;

constexpr const char* kParkPath = "/data/provpark.txt";

// The park's flash file: 1 = read, 0 = none yet, -1 = storage not ready (the park asks
// again on its next call, so a park learned before a restart is never lost to an early
// question). Lane QW4: a file that exists but would not open is "not ready", never "none
// yet" - read as none, the park's next save would overwrite the refusals it holds.
int loadPark(std::string& blob) {
  if (!agent::flashMounted()) return -1;
  if (!LittleFS.exists(kParkPath)) return 0;
  File f = LittleFS.open(kParkPath, "r");
  if (!f) {
    static uint32_t s_lastLog = 0;   // once a minute: the park asks on every request
    const uint32_t now = (uint32_t)millis();
    if (s_lastLog == 0 || now - s_lastLog > 60000) {
      s_lastLog = now ? now : 1;
      alog("park: the provider park file would not open - asking again on the next request");
    }
    return -1;
  }
  const String s = f.readString();
  f.close();
  blob.assign(s.c_str(), s.length());
  return 1;
}

bool savePark(const std::string& blob) {
  if (!agent::flashMounted()) return false;
  LittleFS.mkdir("/data");
  const bool ok = agent::writeFileAtomic(kParkPath, blob, "provpark", false);
  if (!ok) alog("park: the provider park was not saved (kept in memory until restart)");
  return ok;
}

uint32_t epochNow() {
  const time_t t = time(nullptr);
  return t > 1609459200 ? (uint32_t)t : 0u;   // 2021-01-01: the clock is set
}

// Registered before setup() runs: the park loads its file on the first question
// any seam asks, whichever seam that is (the chat fabric included).
const bool kStorageRegistered = [] {
  // The park's records live in PSRAM (internal RAM if PSRAM is missing): only its
  // locks and hooks stay in internal RAM.
  orch::providerPark().setRecordMemory(::nimbus::psramOrInternalCalloc);
  orch::ParkStorage s;
  s.load = loadPark;
  s.save = savePark;
  s.epochS = epochNow;
  s.nowMs = [] { return (uint32_t)millis(); };
  orch::providerPark().setStorage(std::move(s));
  // CUM-708: the spawn-model fallback ranks by the ledger's own prices.
  orch::setRankPriceSource([](const std::string& prov, const std::string& model) {
    return agent::store::modelRankPrice(prov, model);
  });
  return true;
}();
}  // namespace

orch::ParkVerdict check(const char* provider, orch::PaidSeam seam, const String& key) {
  (void)kStorageRegistered;
  // Lane QW4: the park holds the spend ceiling's freeze only once the ceiling store has
  // reported it, and the seams that ask the park alone (embeddings, speech, the
  // download scan, images) can run before any turn did (a voice note in the boot
  // backlog, the embedding backfill). Ask the ceiling first: it loads its file and hands
  // the park the current freeze before the park answers.
  (void)nimbus::orch::spendFrozen();
  const orch::ParkVerdict v = orch::providerPark().available(
      provider, seam, orch::parkKeyFingerprint(std::string(key.c_str())), (uint32_t)millis());
  if (v.probe) alogf("park: %s re-check due - one probe request (%s)", provider, orch::paidSeamSlug(seam));
  if (!v.open)
    alogf("park: %s closed for %s (%s) - no request", provider, orch::paidSeamSlug(seam),
          v.paused ? "paused" : orch::parkKindSlug(v.kind));
  return v;
}

orch::ParkKind noteAnswer(const char* provider, orch::PaidSeam seam, const String& key, int status,
                          const char* text) {
  const uint32_t now = (uint32_t)millis();
  if (status <= 0) {
    orch::providerPark().noteFailure(provider, now);
    return orch::ParkKind::None;
  }
  const orch::ParkKind k = orch::providerPark().noteAnswer(
      provider, seam, orch::parkKeyFingerprint(std::string(key.c_str())), status, text ? text : "", now);
  if (k != orch::ParkKind::None)
    alogf("park: %s parked (%s, HTTP %d) from %s", provider, orch::parkKindSlug(k), status,
          orch::paidSeamSlug(seam));
  return k;
}

String words(const char* provider, const orch::ParkVerdict& v) {
  return String(orch::parkWords(provider, v).c_str());
}

bool ownerReachable() {
  return !orchestrator::inScheduledTurn() && orchestrator::firstAllowedChat().length() > 0;
}

void sendOwnerNotice(const char* provider, const String& text) {
  const String owner = orchestrator::firstAllowedChat();
  if (!owner.length()) return;
  alogf("spend: %s daily warning sent", provider);
  agent::telegram::send(owner, text, /*block=*/false);
}

}  // namespace agent::gate
