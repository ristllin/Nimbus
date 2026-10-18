#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/model_key.h"

// model_ledger - per-(provider, model) spend counters and per-model prices
// (owner 2026-10-13: "different models cost differently for the same provider").
// Portable + host-tested (NO Arduino). The provider ledger (UsageLedger, NVS key
// usgLedger) is unchanged and stays the budget's source of truth: this ledger only
// says WHICH models that provider spend went to, and what each one costs.
//
// Bounded by construction (zero heap growth on the turn path):
//   - fixed-size rows (char buffers, no std::string), vectors reserve()d to their
//     caps once, so recording into an existing row allocates nothing;
//   - 32 spend rows, at most 8 named per provider: a new model takes over the
//     provider's named row with the oldest lastDay and no spend this period (its
//     all-time totals fold into "<prov>|~other"); with all 8 busy it goes to ~other.
//     When every row is taken, the row with the oldest lastDay and no spend this
//     period is folded into its provider's ~other row; when every row has spend
//     this period, a provider with no row at all takes the room of another
//     provider's smallest row, which folds into that provider's ~other (gate G2).
//     Spend is never dropped and lands in a row (the one exception: a hand-made file
//     whose 32 rows are fold rows of 32 providers), and ~other is priced at the
//     provider's highest known price (fail-closed).
//   - once tracking has started, spend recorded with no model files under ~other,
//     so the rows of a provider add up to its period counters minus its earlier
//     usage and its lost spend (ProvTrack). A mismatch at load means the two stores
//     diverged: a usgLedger or spend-file write that did not land, a settings-only
//     reset, or spend an earlier release recorded after a downgrade (no rows).
//   - 32 owner price rows (only an owner edit writes them), 24 router price rows
//     (only the Cumulo verify harvest writes them).
// Persisted by the device to three LittleFS files (never NVS), one text form each:
// a "!v|1" header group, then ';'-separated rows of '|'-separated fields. Fields are
// append-only (a missing tail field reads 0), garbage and over-cap rows are skipped,
// parsing stops at the cap. The spend file also carries "!t" (tracking started) and
// one "!p|..." group per provider track; '!' groups are skipped by a reader that does
// not know them.

namespace nimbus {
namespace orch {

inline constexpr size_t kProvKeyCap = 16;   // provider id buffer (15 chars + NUL)

// One record's token spend: prompt, completion, and the prompt-cache counters
// (recorded only for providers that exclude cache traffic from the prompt count).
struct TokenDelta {
  uint64_t in = 0, out = 0, cacheR = 0, cacheW = 0;
  bool empty() const { return !in && !out && !cacheR && !cacheW; }
};

struct ModelSpend {
  char     prov[kProvKeyCap] = {0};
  char     model[kModelKeyCap] = {0};
  uint32_t periodKey = 0;            // the provider period these counters belong to
  uint64_t tokIn = 0, tokOut = 0;    // this period
  uint64_t cacheR = 0, cacheW = 0;   // this period (excluded-from-input providers only)
  uint64_t totIn = 0, totOut = 0;    // all time (never rolls)
  uint32_t lastDay = 0;              // UTC day of the last record (eviction order)
};

struct ModelRate {
  char     prov[kProvKeyCap] = {0};
  char     model[kModelKeyCap] = {0};
  uint32_t microIn = 0;    // micro-USD per 1M input tokens; 0 = unset
  uint32_t microOut = 0;   // micro-USD per 1M output tokens; 0 = unset
};

// What model tracking needs beside the rows, per provider (gate G1-G3): the spend
// from before tracking started on this device (earlier usage: no model was ever
// known for it) and the spend whose model attribution a load-time rebase had to drop
// (a storage failure; it may have been any model, so it is priced at the provider's
// highest known price). `pre` and `lost` belong to `periodKey` and roll with the
// provider's period like a row; the all-time part never rolls.
struct ProvTrack {
  char       prov[kProvKeyCap] = {0};
  uint32_t   periodKey = 0;
  TokenDelta pre;
  TokenDelta lost;
  uint64_t   preTotIn = 0, preTotOut = 0;   // all-time totals before tracking started
  // The provider's all-time totals as of the latest usgLedger write that landed (or
  // the load), and the part of them from before that write's period: stored counters
  // only grow from there, and their period spend counts only since their latest roll,
  // so less at a later load means they were re-created (gate M1).
  uint64_t   storedTotIn = 0, storedTotOut = 0;
  uint64_t   storedBeforeIn = 0, storedBeforeOut = 0;
};

// The answer to an owner price edit.
enum class RateEdit {
  Ok,            // stored (or cleared)
  Invalid,       // not a provider id / model key the ledger can carry: nothing changed
  Full,          // a new row past kMaxOwnerRates: nothing changed
  WriteFailed,   // the store refused it: the table was put back exactly as it was
};

class ModelLedger {
 public:
  static constexpr size_t kMaxRows = 32;
  static constexpr size_t kMaxNamedPerProv = 8;
  static constexpr size_t kMaxOwnerRates = 32;
  static constexpr size_t kMaxRouterRates = 24;
  static constexpr size_t kMaxTracks = 12;   // one per provider (UsageLedger::kMaxProviders)

  // ---- spend ----
  // Add spend to (prov, model) for `periodKey`, rolling the row first when its
  // period differs (the UsageLedger rules: an incoming 0 keeps the stored period, a
  // stored 0 carries into the first real one). `dayKey` (UTC days, 0 = unknown)
  // stamps lastDay. model may be kOtherModel; any other key must be modelKeySafe.
  // A recorded row starts tracking. The device records through
  // UsageLedger::recordTokens, which first keeps every provider's earlier usage; a
  // direct call keeps none, so all spend no row holds then reads as lost spend
  // (priced at the provider's highest price: fail-closed).
  // `cacheInTotal` (owner ruling R4): the provider counts its cache tokens inside its
  // input (cacheFoldedInNvs), so the row's all-time input keeps that native count,
  // like the provider's all-time total does; the period counters keep the split.
  void record(const char* prov, const char* model, const TokenDelta& d, uint32_t periodKey,
              uint32_t dayKey, bool cacheInTotal = false);
  // Roll every row and track to its provider's current period (periodOf(ctx, prov);
  // 0 = keep), so their period counters always belong to the period the provider
  // counts in.
  using PeriodOf = uint32_t (*)(const void* ctx, const char* prov);
  void syncPeriods(PeriodOf periodOf, const void* ctx);
  // Zero row i's period counters (all-time totals kept).
  void clearPeriodSpend(size_t i);
  // Row i now counts in `periodKey` (load time: a row follows its provider's period).
  void setRowPeriod(size_t i, uint32_t periodKey) {
    if (i < rows_.size()) rows_[i].periodKey = periodKey;
  }
  // Row i's period counters and all-time totals are zeroed (its provider's stored
  // counters were re-created, so none of what the row counted is in them).
  void forgetRow(size_t i) {
    if (i >= rows_.size()) return;
    clearPeriodSpend(i);
    rows_[i].totIn = rows_[i].totOut = 0;
  }
  // Row i's period spend leaves the row entirely (its period counters, and the same
  // amounts off its all-time totals): a load-time rebase found the rows and the
  // provider counters disagree, so that spend's model is no longer known.
  void dropAttribution(size_t i);
  const std::vector<ModelSpend>& rows() const { return rows_; }
  const ModelSpend* findRow(const char* prov, const char* model) const;

  // ---- tracking (gate G1-G3) ----
  // Whether per-model tracking ever started on this device (a model record, or a
  // spend file that shows one). Before it, every period counter is earlier usage.
  bool tracking() const { return tracking_; }
  void markTracking() { tracking_ = true; }
  // Whether the provider counters' latest store did not land (a usgLedger write that
  // failed with no later one that landed): the rows may then hold spend the stored
  // counters lack. The device sets it after every usgLedger write; it rides the spend
  // file ("!n"), so a load can tell a spend file that is only BEHIND the counters
  // (rows kept) from one that may be ahead of them (attribution dropped).
  bool countersBehind() const { return countersBehind_; }
  void setCountersBehind(bool behind) { countersBehind_ = behind; }
  // Whether the spend file on flash holds what the load decided (the device sets it at
  // load: false when the file is missing or the load's store of it failed; true after
  // every write that landed). While tracking is on and it is not, a record must store the
  // spend file BEFORE its usgLedger write (gate M2), marked countersBehind (that write
  // has not landed yet): tracked spend may then reach the counters only after a file
  // that shows tracking started, and a failure between the two leaves the file ahead
  // (attribution dropped: fail-closed), never counters a later load would take for
  // earlier usage.
  void setSpendFileStored(bool stored) { spendFileStored_ = stored; }
  bool spendFileFirst() const { return tracking_ && !spendFileStored_; }
  // Whether a spend file IS on flash but lacks what the load decided (its store at load
  // failed: the device sets it then). Until a write lands, a record's history bucket
  // names no model (historyKey): a model bucket on the day the stale rows last
  // recorded would hide that an earlier release rewrote the history (gate M1).
  void setSpendFileStale(bool stale) { spendFileStale_ = stale; }
  const char* historyKey(const char* key) const {
    return spendFileStale_ && !spendFileStored_ ? "" : key;
  }
  // The key a record files its spend under: `model` when set; once tracking has
  // started, spend with no model goes to ~other (priced at the provider's highest
  // known price); before that, "" (provider level only, earlier usage).
  const char* spendKey(const char* model) const;
  const ProvTrack* track(const char* prov) const;
  // The provider's track, created when absent (nullptr past kMaxTracks or for a
  // provider id the ledger cannot carry).
  ProvTrack* trackFor(const char* prov);
  void dropTrack(const char* prov);
  const std::vector<ProvTrack>& tracks() const { return tracks_; }

  // ---- owner prices ----
  // 0:0 clears the row. A new row past the cap answers Full and changes nothing.
  RateEdit setOwnerRate(const char* prov, const char* model, uint32_t microIn, uint32_t microOut);
  // The owner edit with its store step: only the owner table is snapshotted; when
  // `persist` (handed the serialized table) fails, the snapshot goes back and the
  // answer is WriteFailed, so RAM never diverges from the stored table (CUM-479).
  RateEdit setOwnerRateDurable(const char* prov, const char* model, uint32_t microIn,
                               uint32_t microOut,
                               const std::function<bool(const std::string&)>& persist);
  const ModelRate* ownerRate(const char* prov, const char* model) const;
  const std::vector<ModelRate>& ownerRates() const { return owner_; }

  // ---- router prices (Cumulo) ----
  // Replace the whole table (the verify harvest's result), keeping at most
  // kMaxRouterRates valid rows.
  void replaceRouterRates(const std::vector<ModelRate>& rows);
  const ModelRate* routerRate(const char* prov, const char* model) const;
  const std::vector<ModelRate>& routerRates() const { return router_; }

  // ---- text forms ----
  std::string serializeSpend() const;
  void        deserializeSpend(const std::string& s);
  std::string serializeOwnerRates() const { return serializeRates(owner_); }
  void        deserializeOwnerRates(const std::string& s) { deserializeRates(s, owner_, kMaxOwnerRates); }
  std::string serializeRouterRates() const { return serializeRates(router_); }
  void        deserializeRouterRates(const std::string& s) { deserializeRates(s, router_, kMaxRouterRates); }

 private:
  void reserveAll();
  ModelSpend* findMut(const char* prov, const char* model);
  ModelSpend* rowFor(const char* prov, const char* model, uint32_t periodKey);
  ModelSpend* otherRowFor(const char* prov, uint32_t periodKey);
  ModelSpend* pushRow(const char* prov, const char* model);
  ModelSpend* recycleStale(const char* prov, const char* model, uint32_t periodKey);
  bool        evictOne(const char* prov, uint32_t periodKey);
  bool        foldSmallestElsewhere(const char* prov);
  ModelSpend* relabelSmallest(const char* prov);
  size_t      namedCount(const char* prov) const;
  bool        hasRow(const char* prov) const;
  static std::string serializeRates(const std::vector<ModelRate>& v);
  static void deserializeRates(const std::string& s, std::vector<ModelRate>& v, size_t cap);
  void        parseTrack(const std::string& group);

  std::vector<ModelSpend> rows_;
  std::vector<ModelRate>  owner_;
  std::vector<ModelRate>  router_;
  std::vector<ProvTrack>  tracks_;
  bool                    tracking_ = false;
  bool                    countersBehind_ = false;
  bool                    spendFileStored_ = false;
  bool                    spendFileStale_ = false;
};

// The models an owner may price for `prov` (the web field's allowlist and the Prices
// table's rows), as a comma-separated list of keys (usageModelKey), deduped, at most
// kMaxEditable: the provider's choice list (CSV, already the effective list), its
// head and sub-agent models, `extra` (the custom endpoint's model for "custom", else
// ""), every model with a spend row and every model with an owner price row. Never
// "~other". One string (one allocation, PSRAM on the device), not one per key.
inline constexpr size_t kMaxEditable = 64;
std::string editableModelKeys(const char* prov, const std::string& choicesCsv,
                              const std::string& orchSel, const std::string& subSel,
                              const std::string& extra, const ModelLedger& L);
// Whether `key` is one item of such a list.
bool editableHas(const std::string& csv, const char* key);

// Parse the web field "prov:microIn:microOut:model" (the model last, so a model id
// holding ':' or '/' survives). Both numbers must be all digits and fit u32; prov and
// model must be non-empty. Returns false on any malformed value.
struct ModelRateField {
  std::string prov;
  std::string model;
  uint32_t    microIn = 0;
  uint32_t    microOut = 0;
};
bool parseModelRateField(const std::string& value, ModelRateField& out);

}  // namespace orch
}  // namespace nimbus
