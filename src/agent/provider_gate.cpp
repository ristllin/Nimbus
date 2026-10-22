#include "provider_gate.h"

#include <LittleFS.h>
#include <time.h>

#include <string>

#include "../sys/agent_log.h"
#include "fs_atomic.h"      // flashMounted, writeFileAtomic - the park's flash file
#include "orchestrator.h"   // inScheduledTurn, firstAllowedChat - the daily spend notice
#include "store.h"          // modelRankPrice - the ledger's prices (CUM-708)
#include "nimbus/orch/usage_ledger.h"   // setRankPriceSource
#include "telegram.h"       // send - the daily spend notice

namespace agent::gate {

namespace {
namespace orch = nimbus::orch;

constexpr const char* kParkPath = "/data/provpark.txt";

// The park's flash file: 1 = read, 0 = none yet, -1 = storage not mounted (the park
// asks again on its next call, so a park learned before a restart is never lost to
// an early question).
int loadPark(std::string& blob) {
  if (!agent::flashMounted()) return -1;
  File f = LittleFS.open(kParkPath, "r");
  if (!f) return 0;
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
