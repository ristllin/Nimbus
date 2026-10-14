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
//     period is folded into its provider's ~other row. Spend is never dropped: what
//     no row can hold stays in the provider counter, where the residual rule prices
//     it, and ~other is priced at the provider's highest known price (fail-closed).
//   - 32 owner price rows (only an owner edit writes them), 24 router price rows
//     (only the Cumulo verify harvest writes them).
// Persisted by the device to three LittleFS files (never NVS), one text form each:
// a "!v|1" header group, then ';'-separated rows of '|'-separated fields. Fields are
// append-only (a missing tail field reads 0), garbage and over-cap rows are skipped,
// parsing stops at the cap.

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

  // ---- spend ----
  // Add spend to (prov, model) for `periodKey`, rolling the row first when its
  // period differs (the UsageLedger rules: an incoming 0 keeps the stored period, a
  // stored 0 carries into the first real one). `dayKey` (UTC days, 0 = unknown)
  // stamps lastDay. model may be kOtherModel; any other key must be modelKeySafe.
  void record(const char* prov, const char* model, const TokenDelta& d, uint32_t periodKey,
              uint32_t dayKey);
  // Roll every row to its provider's current period (periodOf(ctx, prov); 0 = keep),
  // so a row's period counters always belong to the period its provider counts in.
  using PeriodOf = uint32_t (*)(const void* ctx, const char* prov);
  void syncPeriods(PeriodOf periodOf, const void* ctx);
  // Zero row i's period counters (all-time totals kept): its period spend reads as
  // the provider's residual.
  void clearPeriodSpend(size_t i);
  const std::vector<ModelSpend>& rows() const { return rows_; }
  const ModelSpend* findRow(const char* prov, const char* model) const;

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
  ModelSpend* relabelSmallest(const char* prov);
  size_t      namedCount(const char* prov) const;
  static std::string serializeRates(const std::vector<ModelRate>& v);
  static void deserializeRates(const std::string& s, std::vector<ModelRate>& v, size_t cap);

  std::vector<ModelSpend> rows_;
  std::vector<ModelRate>  owner_;
  std::vector<ModelRate>  router_;
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
