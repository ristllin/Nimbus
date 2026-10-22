#include "spend_ceiling_store.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <esp_heap_caps.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>

#include "fs_atomic.h"
#include "store.h"
#include "../sys/agent_log.h"
#include "../sys/config_nvs.h"   // notePersist: an owner edit that did not store answers 507

namespace agent {
namespace spend {

namespace {

using nimbus::orch::NoticeChannel;
using nimbus::orch::SpendCeiling;

constexpr const char* kPath = "/data/spend_ceiling.txt";
constexpr size_t kFileMax = 512;   // one short line; anything longer is not ours

// The state behind g_mx lives in PSRAM (docs/memory.md): only the lock and the two
// atomics the main loop reads stay in internal RAM, so the ceiling costs the internal
// heap a 4-byte reference instead of the whole state.
struct State {
  SpendCeiling c;
  bool         loaded = false;
  bool         dirty = false;      // RAM holds a state the file does not
  uint64_t     pending = 0;        // the turn in flight's billed rounds (RAM only)
  uint32_t     lastLog = 0;        // the last "write failed" log line (rate limit)
};
State& psramState() {
  void* p = heap_caps_calloc(1, sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(1, sizeof(State), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) abort();   // cannot happen at static-init time; a static never fails either
  return *new (p) State();
}

std::mutex g_mx;
State&     g_st = psramState();   // guarded by g_mx
// The screen line's view, read lock-free by the watched main loop (it never waits for
// g_mx: a commit holds it across a flash write).
std::atomic<bool>     g_frozenView{false};
std::atomic<uint64_t> g_limitView{SpendCeiling::kDefaultLimitMicro};

uint32_t monthNow() { return nimbus::orch::ceilingMonthKey(time(nullptr)); }

std::string readFile() {
  if (!LittleFS.exists(kPath)) return std::string();
  File f = LittleFS.open(kPath, "r");
  if (!f) return std::string();
  if (f.size() > kFileMax) { f.close(); return std::string(); }
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}

// Caller holds g_mx. A failed write leaves the state dirty (tick retries).
bool persistLocked() {
  LittleFS.mkdir("/data");
  const bool ok = agent::writeFileAtomic(kPath, g_st.c.serialize(), "spend_ceiling", false);
  g_st.dirty = !ok;
  if (!ok) {
    const uint32_t now = millis();
    if (g_st.lastLog == 0 || now - g_st.lastLog > 60000) {
      g_st.lastLog = now ? now : 1;
      alogf("spend: ceiling file write failed - the freeze holds in RAM, retrying");
    }
  }
  return ok;
}

// The first boot on this release (no file): a device that already spent starts this
// month's count from the ledger's month-to-date estimate (fail-closed: the month is
// a calendar month, not "since the update") and owes the owner one notice.
void firstLoadLocked() {
  const bool spentBefore = store::usageHasHistory();
  g_st.c = SpendCeiling::firstBoot(monthNow(), spentBefore,
                                spentBefore ? store::monthToDateMicro() : 0,
                                store::telegramToken().length() > 0);
  alogf("spend: ceiling file created (limit %s, month %lu, so far %s%s)",
        nimbus::orch::formatUsd(g_st.c.limitMicro).c_str(), (unsigned long)g_st.c.monthKey,
        nimbus::orch::formatUsd(g_st.c.spentMicro).c_str(),
        spentBefore ? ", first-boot notice owed" : "");
  persistLocked();
}

// Caller holds g_mx. False while the file area is not mounted (nothing to count yet), and
// before the first file exists while the clock is unset: the first file starts from the
// ledger's month-to-date estimate, which needs the month (spend meanwhile is in the
// ledger, so that seed counts it).
bool loadedLocked() {
  if (g_st.loaded) return true;
  if (!agent::flashMounted()) return false;
  if (!LittleFS.exists(kPath) && monthNow() == 0) return false;
  const std::string text = readFile();
  g_st.loaded = true;
  if (text.empty() && !LittleFS.exists(kPath)) {
    firstLoadLocked();
  } else if (!g_st.c.deserialize(text)) {
    // A torn or foreign file: the default ceiling over a zero count would un-freeze a
    // frozen device, so the count restarts from the ledger's month-to-date estimate.
    g_st.c = SpendCeiling();
    g_st.c.monthKey = monthNow();
    g_st.c.add(store::monthToDateMicro(), g_st.c.monthKey);
    alogf("spend: ceiling file unreadable - recounted this month from the usage ledger");
    persistLocked();
  }
  if (g_st.c.roll(monthNow())) persistLocked();
  return true;
}

nimbus::orch::CeilingFacts factsLocked() {
  nimbus::orch::CeilingFacts f;
  f.limitMicro = g_st.c.limitMicro;
  f.spentMicro = g_st.c.spentMicro;
  f.monthKey = g_st.c.monthKey;
  const time_t now = time(nullptr);
  if (now >= nimbus::orch::kCeilingClockValid) {
    struct tm tmv;
    if (localtime_r(&now, &tmv)) f.dayOfMonth = tmv.tm_mday;
  }
  const String host = store::resolvedOrchHost();
  if (host.length()) f.headModel = store::orchModel(host).c_str();
  return f;
}

// The meter's sink: a paid call outside a turn (nimbus::orch::meterSeamUsage /
// meterSeamMicro). A sub-agent's spend is filed in the usage ledger like a turn's (the
// Usage page and the budgets show it under "spawn:<provider>"); every other seam (an
// embedding runs several times a turn) counts toward the ceiling alone, so it never adds
// a settings-store write per call.
void meterSink(nimbus::orch::SpendSeam seam, const char* provider,
               const nimbus::orch::TurnSpend& spend, uint64_t estimateMicro) {
  if (seam == nimbus::orch::SpendSeam::Sub && !spend.empty()) {
    store::recordSideSpend(spend, (std::string("spawn:") + provider).c_str());
    commit(estimateMicro, false);
    return;
  }
  commit(nimbus::orch::addSatMicro(store::priceSpendMicro(spend), estimateMicro), false);
}

}  // namespace

void install() {
  nimbus::orch::installSpendFreeze(&frozen);
  nimbus::orch::installPendingSpend(&store::priceSpendMicro, &addPending);
  nimbus::orch::installSeamMeter(&meterSink);
}

bool frozen() {
  bool f = false;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!loadedLocked()) return false;
    f = g_st.c.frozenWith(g_st.pending);
    g_limitView.store(g_st.c.limitMicro);
  }
  g_frozenView.store(f);
  nimbus::orch::noteFreeze(f);   // lane K2's park pauses every provider while it holds
  return f;
}

void commit(uint64_t micro, bool turn) {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (turn) g_st.pending = 0;
    if (!loadedLocked() || micro == 0) return;
    const bool wasFrozen = g_st.c.frozen;
    if (g_st.c.add(micro, monthNow())) persistLocked();
    if (g_st.c.frozen && !wasFrozen)
      alogf("spend: monthly ceiling reached (%s of %s) - paid calls paused",
            nimbus::orch::formatUsd(g_st.c.spentMicro).c_str(),
            nimbus::orch::formatUsd(g_st.c.limitMicro).c_str());
  }
  frozen();   // report the change (outside the lock)
}

void addPending(uint64_t micro) {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_st.pending = nimbus::orch::addSatMicro(g_st.pending, micro);
  }
  frozen();
}

uint64_t limitMicro() {
  std::lock_guard<std::mutex> lk(g_mx);
  loadedLocked();
  return g_st.c.limitMicro;
}

static bool setLimitLocked(uint64_t micro);

bool setLimitMicro(uint64_t micro) {
  bool ok;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    ok = setLimitLocked(micro);
  }
  frozen();   // a raised ceiling lifts the park's pause at once
  return ok;
}

// Caller holds g_mx.
static bool setLimitLocked(uint64_t micro) {
  if (!loadedLocked()) return nimbus::sys::notePersist(false, "lfs_spend");
  const SpendCeiling before = g_st.c;
  if (!g_st.c.setLimit(micro)) return true;   // unchanged
  if (!persistLocked()) {
    g_st.c = before;   // a 507 means nothing changed
    g_st.dirty = false;
    nimbus::sys::notePersist(false, "lfs_spend");
    return false;
  }
  alogf("spend: monthly ceiling set to %s (%s)", nimbus::orch::formatUsd(g_st.c.limitMicro).c_str(),
        g_st.c.frozen ? "still paused" : "paid calls allowed");
  return true;
}

void tick() {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!loadedLocked()) return;
    const bool wasFrozen = g_st.c.frozen;
    if (g_st.c.roll(monthNow())) {
      persistLocked();
      if (wasFrozen) alogf("spend: a new month started - paid calls allowed again");
    } else if (g_st.dirty) {
      persistLocked();
    }
  }
  frozen();   // the state at boot and after a rollover reaches the park
}

NoticeKind nextNotice(NoticeChannel ch, std::string& text) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!loadedLocked()) return NoticeKind::None;
  const NoticeKind k = g_st.c.nextNotice(ch);
  if (k != NoticeKind::None) text = nimbus::orch::noticeText(k, factsLocked());
  return k;
}

void delivered(NoticeChannel ch, NoticeKind kind) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (loadedLocked() && g_st.c.markDelivered(ch, kind)) persistLocked();
}

std::string screenLine(std::string& detail) {
  if (!g_frozenView.load()) return std::string();
  nimbus::orch::CeilingFacts f;
  f.limitMicro = g_limitView.load();
  detail = nimbus::orch::freezeScreenDetail(f);
  return nimbus::orch::freezeScreenLine(f);
}

std::string stateJson() {
  std::lock_guard<std::mutex> lk(g_mx);
  const bool loaded = loadedLocked();
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"limitCents\":%llu,\"spentCents\":%llu,\"frozen\":%s,\"ownerSet\":%s,"
           "\"month\":%lu,\"resetsOn\":\"%s\",\"saved\":%s,\"loaded\":%s}",
           (unsigned long long)(g_st.c.limitMicro / 10000ULL),
           (unsigned long long)(g_st.c.spentMicro / 10000ULL),
           g_st.c.frozenWith(g_st.pending) ? "true" : "false", g_st.c.ownerSet ? "true" : "false",
           (unsigned long)g_st.c.monthKey, nimbus::orch::resetDateText(g_st.c.monthKey).c_str(),
           g_st.dirty ? "false" : "true", loaded ? "true" : "false");
  return buf;
}

std::string setupText() {
  std::lock_guard<std::mutex> lk(g_mx);
  loadedLocked();
  return nimbus::orch::setupDefaultsText(g_st.c.limitMicro);
}

#if defined(NIMBUS_TEST)
void testSetSpent(uint64_t micro) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!loadedLocked()) return;
  g_st.c.roll(monthNow());
  g_st.c.spentMicro = 0;
  g_st.c.frozen = false;
  g_st.c.noticesSent = 0;
  g_st.c.add(micro, monthNow());
  persistLocked();
}
#endif

}  // namespace spend
}  // namespace agent
