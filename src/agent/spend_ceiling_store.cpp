#include "spend_ceiling_store.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <esp_heap_caps.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "fs_atomic.h"
#include "nimbus/orch/loop_delivery.h"   // deliveryChannelOf - which chats get the paused reply
#include "nimbus/orch/model_price_defaults.h"   // setCustomEndpointPaid - the custom endpoint's estimate
#include "nimbus/orch/provider_park.h"   // the one per-provider park: its pause switch
#include "nimbus/orch/spend_guard.h"     // customEndpointIsLocal, the provisional month, the ledger floor
#include "nimbus/orch/usage_ledger.h"    // civilDayKey - the daily warning's local day
#include "store.h"
#include "../sys/agent_log.h"
#include "../sys/config_nvs.h"   // notePersist: an owner edit that did not store answers 507
#include "../sys/web_task.h"     // webTake: the web task never waits out a commit's flash write

namespace agent {
namespace spend {

namespace {

using nimbus::orch::NoticeChannel;
using nimbus::orch::ProviderCaps;
using nimbus::orch::SpendCeiling;

constexpr const char* kPath = "/data/spend_ceiling.txt";
constexpr size_t kFileMax = 2048;   // the main line and a row per provider; longer is not ours
// Lane QW4: what a start without a clock owes the counts once it is set (ClockDebt in
// spend_guard.h: the ledger's estimate of this month, and the spend made meanwhile), kept
// in a file of its own (the ceiling file's format is shared with earlier releases),
// written before the ceiling file and removed once a save holds no debt.
constexpr const char* kSeedPath = "/data/spend_seed.txt";

struct PendingRow {
  char     provider[16];
  uint64_t micro;
};

// The state behind g_mx lives in PSRAM (docs/memory.md): only the locks and the atomics
// the main loop reads stay in internal RAM.
struct State {
  SpendCeiling          c;
  nimbus::orch::DaySpend day;
  ProviderCaps          caps;
  bool                  loaded = false;
  bool                  dirty = false;      // RAM holds a state the file does not
  nimbus::orch::ClockDebt debt;             // owed once the clock is set (kSeedPath)
  bool                  debtOnDisk = false; // kSeedPath exists
  uint32_t              debtHash = 0;       // the marker content last written (no rewrite of the same)
  uint32_t              reportSeq = 0;      // each report's order (a stale one never lands)
  uint64_t              pending = 0;        // the turn in flight's billed rounds (RAM only)
  PendingRow            pend[ProviderCaps::kMaxRows] = {};   // ... by provider
  uint32_t              lastLog = 0;        // the last "write failed" log line (rate limit)
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
std::atomic<uint64_t> g_dailyView{0};
std::atomic<bool>     g_loadedView{false};   // the file is loaded and the park holds its freezes

// Lane QW4 round 3 (merge review major 5): the clock, else the newest server date seen
// plus the uptime since (a network that blocks NTP never sets the clock), so the month
// rolls and the provisional count settles either way.
time_t spendNow() { return (time_t)nimbus::orch::serverClock().epoch((int64_t)time(nullptr), millis()); }
uint32_t monthNow() { return nimbus::orch::ceilingMonthKey(nimbus::orch::serverClock().epoch((int64_t)time(nullptr), millis())); }
nimbus::orch::LocalDate dateNow() { return nimbus::orch::localDateOf(spendNow()); }
uint32_t dayNow() {
  const nimbus::orch::LocalDate d = dateNow();
  return d.valid() ? nimbus::orch::civilDayKey(d.year, d.month, d.day) : 0;
}

// Lane QW4: a missing file is the first boot; a file that exists but would not open is
// asked again a few times (a transient LittleFS failure, short of memory for its buffer,
// must never pass for a torn file and have the good one recounted and overwritten). Only
// a file that keeps failing, or whose body is not ours, is recounted from the ledger.
enum class FileRead : uint8_t { Missing, Read, Unreadable };
FileRead readFile(std::string& out) {
  out.clear();
  if (!LittleFS.exists(kPath)) return FileRead::Missing;
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (attempt) delay(50);
    File f = LittleFS.open(kPath, "r");
    if (!f) continue;
    if (f.size() <= kFileMax) {   // longer is not ours: read as torn
      const String s = f.readString();
      out.assign(s.c_str(), s.length());
    }
    f.close();
    return FileRead::Read;
  }
  return FileRead::Unreadable;
}

uint32_t textHash(const std::string& s) {
  uint32_t h = 2166136261u;
  for (char ch : s) h = (h ^ (uint8_t)ch) * 16777619u;
  return h;
}

// Caller holds g_mx. The clock debt's marker (kSeedPath), read once at load: a marker
// that is there but unreadable owes the seed (fail-closed).
void loadDebtLocked() {
  g_st.debt = nimbus::orch::ClockDebt();
  g_st.debtOnDisk = LittleFS.exists(kSeedPath);
  if (!g_st.debtOnDisk) return;
  File f = LittleFS.open(kSeedPath, "r");
  const String text = f ? f.readString() : String();
  if (f) f.close();
  g_st.debt.deserialize(std::string(text.c_str(), text.length()));
  g_st.debtHash = textHash(std::string(text.c_str(), text.length()));
}

// Caller holds g_mx. The marker follows the debt: written (before the ceiling file, so a
// restart in between still owes it) while anything must survive a restart, removed once
// a ceiling file holding no debt is saved (afterSave).
void saveDebtLocked(bool afterSave) {
  const std::string m = g_st.debt.serialize();
  if (m.empty()) {
    if (afterSave && g_st.debtOnDisk && LittleFS.remove(kSeedPath)) g_st.debtOnDisk = false;
    return;
  }
  if (afterSave || (g_st.debtOnDisk && textHash(m) == g_st.debtHash)) return;
  LittleFS.mkdir("/data");
  if (!agent::writeFileAtomic(kSeedPath, m, "spend_seed", false)) {
    alogf("spend: the clock-wait record was not saved - the counts hold in RAM");
    return;
  }
  g_st.debtOnDisk = true;
  g_st.debtHash = textHash(m);
}

// Caller holds g_mx. A failed write leaves the state dirty (tick retries).
bool persistLocked() {
  LittleFS.mkdir("/data");
  saveDebtLocked(/*afterSave=*/false);
  const bool ok = agent::writeFileAtomic(
      kPath, nimbus::orch::serializeCeilingFile(g_st.c, g_st.day, g_st.caps), "spend_ceiling", false);
  g_st.dirty = !ok;
  if (ok) saveDebtLocked(/*afterSave=*/true);
  if (!ok) {
    const uint32_t now = millis();
    if (g_st.lastLog == 0 || now - g_st.lastLog > 60000) {
      g_st.lastLog = now ? now : 1;
      alogf("spend: limits file write failed - the freezes hold in RAM, retrying");
    }
  }
  return ok;
}

// Caller holds g_mx. The owner's provider limits (the usage ledger's budget cents) into
// the counts; with any limit set the overall ceiling is off unless the owner chose one.
bool applyRulesLocked() {
  std::vector<nimbus::orch::CapRule> rules;
  store::providerCapRules(rules);
  bool changed = g_st.caps.setRules(rules, dateNow());
  changed = g_st.c.setProviderCaps(g_st.caps.anyCap()) || changed;
  return changed;
}

// Caller holds g_mx. Each provider's count starts from the ledger's estimate for its
// current period (fail-closed: the period, not "since the update"), on top of whatever
// the row counted meanwhile (a provisional count made while the clock was unset).
void seedProvidersLocked() {
  std::vector<std::pair<std::string, uint64_t>> seeds;
  store::providerPeriodSeeds(seeds);
  for (const auto& s : seeds) {
    const int row = g_st.caps.find(s.first.c_str());
    const uint64_t had = row >= 0 ? g_st.caps.rows[row].spentMicro : 0;
    g_st.caps.seed(s.first.c_str(), nimbus::orch::addSatMicro(had, s.second), dateNow());
  }
}

// Caller holds g_mx. Lane QW4: the ledger's estimates need the date. Without a clock the
// counts start provisionally (month 0, enforced at once) and owe the seed (kSeedPath).
void oweSeedLocked(bool migration) {
  const uint8_t v = migration ? 2 : (g_st.debt.seed ? g_st.debt.seed : 1);
  if (g_st.debt.seed == v) return;
  g_st.debt.seed = v;   // saved by the caller's persistLocked
  alogf("spend: the clock is not set - counting from zero and enforcing the ceiling now; "
        "this month's earlier spend is added once the clock is set");
}

bool floorLocked();
void refreshCustomPricing();

// Caller holds g_mx. The first valid clock after counts made without one settles what
// they owe (ClockDebt::settle: the provisional count moves into this month, a count
// still filed under an earlier month starts the new month with the spend made
// meanwhile, an owed seed adds the ledger's estimates on top, a load made with no clock
// is floored at the ledger), with the first-boot notice of a device that spent before
// this release. False when nothing is owed or the clock is still unset.
bool reconcileLocked() {
  const uint32_t month = monthNow();
  if (month == 0 || !g_st.debt.owed()) return false;
  const uint64_t mtd = g_st.debt.needsLedger() ? store::monthToDateMicro() : 0;
  const nimbus::orch::ClockDebt::Settled done = g_st.debt.settle(g_st.c, month, mtd);
  const bool changed = g_st.caps.roll(dateNow()) || done.changed;
  if (!done.seeded) return changed;
  seedProvidersLocked();
  g_st.day.add(store::todayMicro(), dayNow());
  if (done.migration) g_st.c.owesMigration(store::telegramToken().length() > 0);
  alogf("spend: the clock is set - this month's earlier spend added (so far %s)",
        nimbus::orch::formatUsd(g_st.c.spentMicro).c_str());
  return true;
}

// The first boot on this release (no file): a device that already spent starts this
// month's counts from the ledger's estimates and owes the owner one notice. Lane QW4:
// with no clock yet the counts start at once (month 0, the ceiling enforced) and the
// ledger's part is added when the clock is set (reconcileLocked).
void firstLoadLocked() {
  const bool spentBefore = store::usageHasHistory();
  const bool clock = monthNow() != 0;
  if (clock) g_st.debt = nimbus::orch::ClockDebt();   // the first file starts from the ledger itself
  g_st.c = SpendCeiling::firstBoot(monthNow(), spentBefore && clock,
                                   spentBefore && clock ? store::monthToDateMicro() : 0,
                                   store::telegramToken().length() > 0);
  g_st.day = nimbus::orch::DaySpend();
  g_st.day.add(spentBefore && clock ? store::todayMicro() : 0, dayNow());
  g_st.caps = ProviderCaps();
  if (spentBefore && clock) seedProvidersLocked();
  if (spentBefore && !clock) oweSeedLocked(/*migration=*/true);
  applyRulesLocked();
  alogf("spend: limits file created (overall %s, month %lu, so far %s, provider limits %s%s)",
        g_st.c.active() ? nimbus::orch::formatUsd(g_st.c.limitMicro).c_str() : "off",
        (unsigned long)g_st.c.monthKey, nimbus::orch::formatUsd(g_st.c.spentMicro).c_str(),
        g_st.caps.anyCap() ? "set" : "none",
        !spentBefore ? "" : clock ? ", first-boot notice owed" : ", first-boot notice owed once the clock is set");
  persistLocked();
}

// Caller holds g_mx. A torn, foreign or unopenable file: the default ceiling over a zero
// count would un-freeze a frozen device, so the counts restart from the ledger's
// estimates (with no clock: provisionally, owing that seed).
void recountLocked(const char* why) {
  g_st.c = SpendCeiling();
  g_st.c.monthKey = monthNow();
  g_st.c.add(store::monthToDateMicro(), g_st.c.monthKey);
  g_st.caps = ProviderCaps();
  if (monthNow() != 0) {
    seedProvidersLocked();
    g_st.debt.seed = 0;   // the recount is the ledger's estimate already
  } else {
    oweSeedLocked(/*migration=*/false);
  }
  alogf("spend: limits file %s - recounted this month from the usage ledger", why);
  persistLocked();
}

// Caller holds g_mx: the file into the state (or the first file, or a recount). True for
// a file from the release before provider limits were counted here.
bool readStateLocked() {
  std::string text;
  const FileRead r = readFile(text);
  bool earlier = false;
  if (r == FileRead::Missing) {
    firstLoadLocked();
  } else if (r == FileRead::Unreadable) {
    recountLocked("would not open");
  } else if (!nimbus::orch::deserializeCeilingFile(text, g_st.c, g_st.day, g_st.caps, &earlier)) {
    recountLocked("unreadable");
  } else if (earlier && store::usageHasHistory()) {
    if (monthNow() == 0) {
      oweSeedLocked(/*migration=*/false);
    } else {
      seedProvidersLocked();
      g_st.day.add(store::todayMicro(), dayNow());
    }
  }
  return earlier;
}

// Caller holds g_mx. Lane QW4: the count read back never sits below the usage ledger's
// estimate of this month (a commit whose file write failed before a restart is still
// counted). Needs the clock; a provisional count is floored when it is reconciled.
bool floorLocked() {
  const uint32_t month = monthNow();
  if (month == 0 || g_st.debt.seed) return false;
  const uint64_t before = g_st.c.spentMicro;
  if (!nimbus::orch::floorFromLedger(g_st.c, month, store::monthToDateMicro())) return false;
  alogf("spend: this month's count raised to the usage ledger's estimate (%s, was %s)",
        nimbus::orch::formatUsd(g_st.c.spentMicro).c_str(), nimbus::orch::formatUsd(before).c_str());
  return true;
}

// Caller holds g_mx. Lane QW4: the counts load with or without a clock. Without one they
// count into a provisional month and the ceiling holds at once (fail-closed); the first
// valid clock reconciles them (reconcileLocked). False only while the file area is not
// mounted. The custom endpoint is classified first: a recount or seed prices the
// ledger's history with it.
bool loadedLocked() {
  if (g_st.loaded) return true;
  if (!agent::flashMounted()) return false;
  refreshCustomPricing();
  g_st.loaded = true;
  loadDebtLocked();
  g_st.debt.floor = monthNow() == 0;
  const bool earlier = readStateLocked();
  bool changed = applyRulesLocked() || earlier;
  // The owner's provider limits now stop paid calls (and the overall ceiling is off for
  // them): one notice saying which limits apply.
  if (earlier && g_st.caps.anyCap()) g_st.c.owesMigration(store::telegramToken().length() > 0);
  changed = reconcileLocked() || changed;
  changed = g_st.c.roll(monthNow()) || changed;
  changed = g_st.caps.roll(dateNow()) || changed;
  changed = g_st.day.roll(dayNow()) || changed;
  changed = floorLocked() || changed;
  if (changed) persistLocked();
  g_dailyView.store(g_st.day.thresholdMicro);
  return true;
}

uint64_t pendingOfLocked(const char* provider) {
  for (const PendingRow& r : g_st.pend)
    if (r.provider[0] && std::strncmp(r.provider, provider, sizeof(r.provider) - 1) == 0) return r.micro;
  return 0;
}

// ---- the park ------------------------------------------------------------------------
// The freezes ride the one per-provider park (lane K2). The overall one pauses every
// provider (cause "spend ceiling"); a provider's own limit pauses that provider alone
// (cause "provider limit"), so every seam that consults the park (the chat fabric, the
// sub-agent picker, embeddings, the caption step, speech, the download scan, image
// generation) answers "paused" for it while the others keep working. The park keeps the
// pauses in RAM only: this file is the source of truth, re-applied at every report.

void freezeToPark(bool isFrozen) {
  if (isFrozen)
    nimbus::orch::providerPark().pause(nimbus::orch::kPauseAllProviders, nimbus::orch::kPauseSpendCeiling);
  else
    nimbus::orch::providerPark().resume(nimbus::orch::kPauseAllProviders, nimbus::orch::kPauseSpendCeiling);
}

struct ProviderFreezes {
  char     provider[ProviderCaps::kMaxRows][16] = {};
  bool     frozen[ProviderCaps::kMaxRows] = {};
  uint8_t  n = 0;
  uint32_t seq = 0;   // lane QW4: taken under g_mx, so the reports land in state order
};

// Caller holds g_mx: each tracked provider's freeze (its count plus its turn in flight).
ProviderFreezes freezesLocked() {
  ProviderFreezes f;
  for (int i = 0; i < (int)g_st.caps.n; ++i) {
    const ProviderCaps::Row& r = g_st.caps.rows[i];
    std::strncpy(f.provider[f.n], r.provider, sizeof(f.provider[0]) - 1);
    f.frozen[f.n] = g_st.caps.frozenWith(r.provider, pendingOfLocked(r.provider));
    ++f.n;
  }
  return f;
}

std::mutex g_parkMx;   // the applied pauses (never held with g_mx)
ProviderFreezes g_applied;

// Hands each change of a provider's freeze to the park (a change only: the park is not
// asked again for a pause it already holds).
void applyProviderFreezes(const ProviderFreezes& want) {
  std::lock_guard<std::mutex> lk(g_parkMx);
  for (int i = 0; i < (int)want.n; ++i) {
    int at = -1;
    for (int j = 0; j < (int)g_applied.n; ++j)
      if (std::strcmp(g_applied.provider[j], want.provider[i]) == 0) at = j;
    if (at < 0 && g_applied.n < ProviderCaps::kMaxRows) {
      at = g_applied.n++;
      std::strncpy(g_applied.provider[at], want.provider[i], sizeof(g_applied.provider[0]) - 1);
      g_applied.frozen[at] = !want.frozen[i];   // force the first report through
    }
    if (at < 0 || g_applied.frozen[at] == want.frozen[i]) continue;
    g_applied.frozen[at] = want.frozen[i];
    if (want.frozen[i]) {
      nimbus::orch::providerPark().pause(want.provider[i], nimbus::orch::kPauseProviderLimit);
      alogf("spend: %s paused - its monthly limit is reached", want.provider[i]);
    } else {
      nimbus::orch::providerPark().resume(want.provider[i], nimbus::orch::kPauseProviderLimit);
    }
  }
}

// ---- notices ---------------------------------------------------------------------------

nimbus::orch::CeilingFacts factsLocked() {
  nimbus::orch::CeilingFacts f;
  f.limitMicro = g_st.c.limitMicro;
  f.spentMicro = g_st.c.spentMicro;
  f.monthKey = g_st.c.monthKey;
  const nimbus::orch::LocalDate d = dateNow();
  if (d.valid()) f.dayOfMonth = d.day;
  const String host = store::resolvedOrchHost();
  if (host.length()) f.headModel = store::orchModel(host).c_str();
  return f;
}

nimbus::orch::CapFacts capFactsLocked(int row) {
  nimbus::orch::CapFacts f;
  const ProviderCaps::Row& r = g_st.caps.rows[row];
  f.provider = r.provider;
  f.capMicro = r.capMicro;
  f.spentMicro = r.spentMicro;
  f.periodKey = r.periodKey;
  f.resetDay = r.resetDay;
  return f;
}

// The first-boot text: the provider limits when the owner set any, else the ceiling's.
std::string migrationTextLocked() {
  if (!g_st.caps.anyCap()) return nimbus::orch::migrationNoticeText(factsLocked());
  std::vector<nimbus::orch::CapRule> rules;
  for (int i = 0; i < (int)g_st.caps.n; ++i) {
    const ProviderCaps::Row& r = g_st.caps.rows[i];
    if (!r.capMicro) continue;
    nimbus::orch::CapRule c;
    c.provider = r.provider;
    c.capMicro = r.capMicro;
    c.resetDay = r.resetDay;
    rules.push_back(c);
  }
  return nimbus::orch::capsMigrationText(rules, g_st.c.active(), g_st.c.limitMicro);
}

// The meter's sink: a paid call outside a turn (nimbus::orch::meterSeamUsage /
// meterSeamMicro). A sub-agent's spend is filed in the usage ledger like a turn's (the
// Usage page and the budgets show it under "spawn:<provider>"); every other seam (an
// embedding runs several times a turn) counts toward the limits alone, so it never adds
// a settings-store write per call.
void meterSink(nimbus::orch::SpendSeam seam, const char* provider,
               const nimbus::orch::TurnSpend& spend, uint64_t estimateMicro) {
  if (seam == nimbus::orch::SpendSeam::Sub && !spend.empty()) {
    store::recordSideSpend(spend, (std::string("spawn:") + provider).c_str());
  } else if (!spend.empty()) {
    Part parts[nimbus::orch::TurnSpend::kSlots];
    const size_t n = store::priceSpendParts(spend, parts, nimbus::orch::TurnSpend::kSlots);
    commit(parts, n, false);
  }
  if (estimateMicro) {
    const Part est{provider, estimateMicro};
    commit(&est, 1, false);
  }
}

// Lane QW4: a report runs outside g_mx, so two tasks can race to the park. Each carries
// the order its state was read in, and one read before a report that already landed is
// dropped: a stale "not frozen" can never undo a newer freeze.
std::mutex g_reportMx;      // orders the reports (never held with g_mx)
uint32_t   g_reportedSeq = 0;

void report(bool global, const ProviderFreezes& providers) {
  std::lock_guard<std::mutex> lk(g_reportMx);
  if (g_reportedSeq != 0 && (int32_t)(providers.seq - g_reportedSeq) <= 0) return;   // stale
  g_reportedSeq = providers.seq;
  g_frozenView.store(global);
  nimbus::orch::noteFreeze(global);   // the overall freeze pauses every provider
  applyProviderFreezes(providers);
  g_loadedView.store(true);   // reports run only on a loaded state: the park holds its freezes
}

// Lane QW4: the custom endpoint's estimate follows its configuration: a keyless host on
// the device's own network is a local model (free until priced), anything else a paid
// service (the highest built-in price until priced; model_price_defaults.h). Read at the
// first load and then at most every few seconds from the paid-call gate (frozen() runs
// before every paid request), so a base or key changed on the web page is priced right
// within seconds, and the hot path does not read (allocate) the settings on every call.
constexpr uint32_t kCustomRefreshMs = 3000;
void refreshCustomPricing() {
  static std::mutex mx;
  static uint32_t lastHash = 0, lastAt = 0;
  static bool seen = false;
  {
    std::lock_guard<std::mutex> lk(mx);
    const uint32_t now = millis();
    if (seen && now - lastAt < kCustomRefreshMs) return;
    lastAt = now;
  }
  const String base = store::customBase();
  const bool keyed = store::customKey().length() > 0;
  uint32_t h = keyed ? 0x9e3779b9u : 2166136261u;   // FNV-1a: the base itself is not kept
  for (size_t i = 0; i < base.length(); ++i) h = (h ^ (uint8_t)base[i]) * 16777619u;
  std::lock_guard<std::mutex> lk(mx);
  if (seen && h == lastHash) return;
  seen = true;
  lastHash = h;
  const bool paid = !nimbus::orch::customEndpointIsFree(std::string(base.c_str()), keyed);
  nimbus::orch::setCustomEndpointPaid(paid);
  if (base.length())
    alogf("spend: the custom endpoint is priced as %s until you set its price",
          paid ? "a paid service (the highest built-in estimate)" : "a local model (free)");
}

}  // namespace

void install() {
  nimbus::orch::installSpendFreeze(&frozen);
  nimbus::orch::installPendingSpend(&store::priceSpendMicro, &addPending);
  nimbus::orch::installSeamMeter(&meterSink);
  nimbus::orch::installFreezeListener(&freezeToPark);
}

bool frozen() {
  refreshCustomPricing();
  bool f = false;
  ProviderFreezes p;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!loadedLocked()) return false;
    // Lane QW4: what a start without a clock owed lands before the first paid call the
    // clock allows (not a minute later at the tick).
    if (reconcileLocked()) persistLocked();
    f = g_st.c.frozenWith(g_st.pending);
    g_limitView.store(g_st.c.limitMicro);
    p = freezesLocked();
    p.seq = ++g_st.reportSeq;
  }
  report(f, p);
  return f;
}

namespace {
// Caller holds g_mx: one provider's share into its count; whether the state changed.
bool addPartLocked(const Part& part, const nimbus::orch::LocalDate& today) {
  const int row = g_st.caps.find(part.provider);
  const bool wasFrozen = row >= 0 && g_st.caps.rows[row].frozen;
  const bool changed = g_st.caps.add(part.provider, part.micro, today);
  const int now = g_st.caps.find(part.provider);
  if (now >= 0 && g_st.caps.rows[now].frozen && !wasFrozen)
    alogf("spend: %s monthly limit reached (%s of %s) - paid calls to it paused", part.provider,
          nimbus::orch::formatUsd(g_st.caps.rows[now].spentMicro).c_str(),
          nimbus::orch::formatUsd(g_st.caps.rows[now].capMicro).c_str());
  return changed;
}
}  // namespace

void commit(const Part* parts, size_t n, bool turn) {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (turn) {
      g_st.pending = 0;
      for (PendingRow& r : g_st.pend) r = PendingRow{};
    }
    if (!loadedLocked()) return;
    uint64_t total = 0;
    const nimbus::orch::LocalDate today = dateNow();
    const bool wasFrozen = g_st.c.frozen;
    // Lane QW4: a commit right after the clock is set must not roll the count past what
    // the start without a clock still owes.
    bool changed = reconcileLocked();
    for (size_t i = 0; i < n; ++i) {
      if (!parts[i].micro) continue;
      total = nimbus::orch::addSatMicro(total, parts[i].micro);
      changed = addPartLocked(parts[i], today) || changed;
    }
    if (total) {
      g_st.debt.noteCommit(total, monthNow() != 0);
      changed = g_st.c.add(total, monthNow()) || changed;
      changed = g_st.day.add(total, dayNow()) || changed;
    }
    if (changed) persistLocked();
    if (g_st.c.frozen && !wasFrozen)
      alogf("spend: monthly ceiling reached (%s of %s) - paid calls paused",
            nimbus::orch::formatUsd(g_st.c.spentMicro).c_str(),
            nimbus::orch::formatUsd(g_st.c.limitMicro).c_str());
  }
  frozen();   // report the change (outside the lock)
}

void addPending(const char* host, uint64_t micro) {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_st.pending = nimbus::orch::addSatMicro(g_st.pending, micro);
    if (host && *host) {
      PendingRow* slot = nullptr;
      for (PendingRow& r : g_st.pend) {
        if (r.provider[0] && std::strncmp(r.provider, host, sizeof(r.provider) - 1) == 0) { slot = &r; break; }
        if (!slot && !r.provider[0]) slot = &r;
      }
      if (slot) {
        if (!slot->provider[0]) std::strncpy(slot->provider, host, sizeof(slot->provider) - 1);
        slot->micro = nimbus::orch::addSatMicro(slot->micro, micro);
      }
    }
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
  frozen();   // a raised (or switched off) ceiling lifts the park's pause at once
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
  if (!g_st.c.active())
    alogf("spend: the overall monthly ceiling is off");
  else
    alogf("spend: monthly ceiling set to %s (%s)", nimbus::orch::formatUsd(g_st.c.limitMicro).c_str(),
          g_st.c.frozen ? "still paused" : "paid calls allowed");
  return true;
}

bool setDailyWarnMicro(uint64_t micro) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!loadedLocked()) return nimbus::sys::notePersist(false, "lfs_spend");
  const nimbus::orch::DaySpend before = g_st.day;
  if (!g_st.day.setThreshold(micro)) return true;
  if (!persistLocked()) {
    g_st.day = before;
    g_st.dirty = false;
    nimbus::sys::notePersist(false, "lfs_spend");
    return false;
  }
  g_dailyView.store(micro);
  alogf("spend: daily warning %s", micro ? nimbus::orch::formatUsd(micro).c_str() : "back to the default rule");
  return true;
}

uint64_t dailyWarnMicro() { return g_dailyView.load(); }

bool providerAtLimit(const char* provider, bool& known) {
  known = g_loadedView.load();
  return known && provider && *provider &&
         (nimbus::orch::providerPark().pausedBy(provider) & nimbus::orch::kPauseProviderLimit) != 0;
}

void capsChanged() {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!loadedLocked()) return;
    if (applyRulesLocked()) persistLocked();
  }
  frozen();   // a raised limit lifts that provider's pause at once
}

void tick() {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!loadedLocked()) return;
    const bool wasFrozen = g_st.c.frozen;
    bool changed = applyRulesLocked();
    // Lane QW4: the first valid clock after a provisional start adds what it owed
    // before the rollover below can take the count into the month without it.
    changed = reconcileLocked() || changed;
    if (g_st.c.roll(monthNow())) {
      changed = true;
      if (wasFrozen) alogf("spend: a new month started - paid calls allowed again");
    }
    changed = g_st.caps.roll(dateNow()) || changed;
    changed = g_st.day.roll(dayNow()) || changed;
    if (changed || g_st.dirty) persistLocked();
  }
  frozen();   // the state at boot and after a rollover reaches the park
}

Owed nextNotice(NoticeChannel ch, std::string& text) {
  std::lock_guard<std::mutex> lk(g_mx);
  Owed o;
  if (!loadedLocked()) return o;
  o.global = g_st.c.nextNotice(ch);
  if (o.global == SpendCeiling::Notice::Migration) {
    text = migrationTextLocked();
    return o;
  }
  if (o.global != SpendCeiling::Notice::None) {
    text = nimbus::orch::noticeText(o.global, factsLocked());
    return o;
  }
  o.provider = g_st.caps.nextOwed(ch);
  if (o.provider.kind == ProviderCaps::Owed::Freeze) {
    text = nimbus::orch::providerFreezeText(capFactsLocked(o.provider.row));
    return o;
  }
  if (o.provider.kind == ProviderCaps::Owed::Warn) {
    text = nimbus::orch::providerWarnText(capFactsLocked(o.provider.row));
    return o;
  }
  if (g_st.day.owed(ch)) {
    o.daily = true;
    text = nimbus::orch::dailyWarnText(g_st.day.spentMicro, g_st.day.thresholdMicro);
  }
  return o;
}

void delivered(NoticeChannel ch, const Owed& owed) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!loadedLocked()) return;
  bool changed = false;
  if (owed.global != SpendCeiling::Notice::None) changed = g_st.c.markDelivered(ch, owed.global);
  if (owed.provider.kind != ProviderCaps::Owed::None)
    changed = g_st.caps.markDelivered(ch, owed.provider) || changed;
  if (owed.daily) changed = g_st.day.mark(ch) || changed;
  if (changed) persistLocked();
}

std::string screenLine(std::string& detail) {
  if (!g_frozenView.load()) return std::string();
  nimbus::orch::CeilingFacts f;
  f.limitMicro = g_limitView.load();
  detail = nimbus::orch::freezeScreenDetail(f);
  return nimbus::orch::freezeScreenLine(f);
}

std::string frozenReplyFor(const std::string& chatId, bool answered, bool owner) {
  const bool tg = nimbus::orch::deliveryChannelOf(chatId) == nimbus::orch::DeliveryChannel::Telegram;
  if (!tg || answered) return std::string();
  std::lock_guard<std::mutex> lk(g_mx);
  const bool paused = loadedLocked() && g_st.c.frozenWith(g_st.pending);
  if (!nimbus::orch::frozenReplyDue(tg, paused, answered)) return std::string();
  // Round 3 (merge review 7): the owner at most once per 30 minutes, any other chat one
  // neutral line per freeze (named by its month and ceiling).
  static nimbus::orch::FrozenReplyThrottle throttle;   // guarded by g_mx
  const uint32_t episode = g_st.c.monthKey * 2654435761u ^ (uint32_t)(g_st.c.limitMicro / 10000ULL);
  if (!throttle.allow(chatId, owner, episode, millis())) return std::string();
  return owner ? nimbus::orch::frozenReplyText(factsLocked()) : nimbus::orch::frozenReplyTextOther();
}

std::string stateJson() {
  std::unique_lock<std::mutex> lk(g_mx, std::defer_lock);
  if (!nimbus::sys::webTake(lk)) return std::string();   // a commit is writing flash
  const bool loaded = loadedLocked();
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"limitCents\":%llu,\"spentCents\":%llu,\"frozen\":%s,\"ownerSet\":%s,\"active\":%s,"
           "\"month\":%lu,\"resetsOn\":\"%s\",\"saved\":%s,\"loaded\":%s,\"dailyWarnCents\":%llu,"
           "\"todayCents\":%llu,\"providers\":[",
           (unsigned long long)(g_st.c.limitMicro / 10000ULL),
           (unsigned long long)(g_st.c.spentMicro / 10000ULL),
           g_st.c.frozenWith(g_st.pending) ? "true" : "false", g_st.c.ownerSet ? "true" : "false",
           g_st.c.active() ? "true" : "false", (unsigned long)g_st.c.monthKey,
           nimbus::orch::resetDateText(g_st.c.monthKey).c_str(), g_st.dirty ? "false" : "true",
           loaded ? "true" : "false", (unsigned long long)(g_st.day.thresholdMicro / 10000ULL),
           (unsigned long long)(g_st.day.spentMicro / 10000ULL));
  std::string out = buf;
  for (int i = 0; i < (int)g_st.caps.n; ++i) {
    const ProviderCaps::Row& r = g_st.caps.rows[i];
    snprintf(buf, sizeof(buf),
             "%s{\"p\":\"%s\",\"capCents\":%llu,\"spentCents\":%llu,\"frozen\":%s,\"resetsOn\":\"%s\"}",
             i ? "," : "", r.provider, (unsigned long long)(r.capMicro / 10000ULL),
             (unsigned long long)(r.spentMicro / 10000ULL),
             g_st.caps.frozenWith(r.provider, pendingOfLocked(r.provider)) ? "true" : "false",
             nimbus::orch::capResetText(r.periodKey, r.resetDay).c_str());
    out += buf;
  }
  out += "]}";
  return out;
}

std::string setupText() {
  std::unique_lock<std::mutex> lk(g_mx, std::defer_lock);
  if (!nimbus::sys::webTake(lk)) return std::string();   // a commit is writing flash
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

void testSetProviderSpent(const char* provider, uint64_t micro) {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!loadedLocked()) return;
    g_st.caps.seed(provider, micro, dateNow());
    applyRulesLocked();
    persistLocked();
  }
  frozen();   // the park takes the freeze (or its end) now, like every other change
}

void testSetDaySpent(uint64_t micro) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!loadedLocked()) return;
  g_st.day.roll(dayNow());
  g_st.day.spentMicro = micro;
  g_st.day.told = 0;
  persistLocked();
}
#endif

}  // namespace spend
}  // namespace agent
