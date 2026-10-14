#include "nimbus/orch/usage_models_json.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "nimbus/orch/model_price_defaults.h"

namespace nimbus {
namespace orch {

namespace {

// Upper bounds per element (every number at its 20-digit max, every key at its cap).
constexpr size_t kEnvelope = 96;
constexpr size_t kModelRow = 420;
constexpr size_t kResidualRow = 280;
constexpr size_t kEditableProv = 48;
constexpr size_t kEditableRow = 200;
constexpr size_t kProvRateRow = 520;

// A bounded appender: stops (ok = false) instead of truncating.
struct Out {
  char*  buf;
  size_t cap;
  size_t off = 0;
  bool   ok = true;
  void f(const char* fmt, ...) {
    if (!ok) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0 || off + (size_t)n >= cap) { ok = false; return; }
    off += (size_t)n;
  }
};

typedef unsigned long long ull;

uint64_t addCap(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }

// Every provider the page may show: the editable lists' providers, then any other
// ledger provider, then tavily (calls only). Only ids the formats can carry.
std::vector<std::string> providers(const UsageLedger& L, const std::vector<EditableModels>& ed) {
  std::vector<std::string> v;
  auto add = [&](const std::string& p) {
    if (provKeySafe(p.c_str()) && std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
  };
  for (const auto& e : ed) add(e.prov);
  for (const auto& e : L.entries()) add(e.name);
  add("tavily");
  return v;
}

// One models[] entry.
struct ModelEntry {
  const char* prov;
  const char* m;
  TokenDelta  take;          // this period's tokens, as priced
  uint64_t    totIn, totOut;
  EffRate     rate;
  uint64_t    cents;
};

void modelRow(Out& o, const ModelEntry& e, bool& first) {
  o.f("%s{\"prov\":\"%s\",\"m\":\"%s\",\"pIn\":%llu,\"pOut\":%llu,\"pCr\":%llu,\"pCw\":%llu,"
      "\"tIn\":%llu,\"tOut\":%llu,\"rIn\":%llu,\"rOut\":%llu,\"src\":\"%s\",\"est\":%llu}",
      first ? "" : ",", e.prov, e.m, (ull)e.take.in, (ull)e.take.out, (ull)e.take.cacheR,
      (ull)e.take.cacheW, (ull)e.totIn, (ull)e.totOut, (ull)e.rate.in, (ull)e.rate.out,
      rateSrcName(e.rate.src()), (ull)e.cents);
  first = false;
}

// The provider's spend since tracking began that no row accounts for (its model was
// lost, gate G1-G3), as tokens.
TokenDelta lostTokens(const PriceSummary& s) {
  TokenDelta t;
  t.in = s.othIn;
  t.out = s.othOut;
  t.cacheR = s.othCr;
  t.cacheW = s.othCw;
  return t;
}

// The provider's rows; its lost-model spend is shown in its "~other" entry (the same
// price, this period and all time), which is made when the provider has no ~other
// row. The parts never exceed the provider's counters, so the sums cannot wrap.
void modelRows(Out& o, const UsageLedger& L, const std::string& prov, const RowPrice* rows,
               const PriceSummary& s, bool& first) {
  const auto& all = L.models().rows();
  const TokenDelta lost = lostTokens(s);
  bool otherShown = false;
  for (size_t i = 0; i < all.size() && i < ModelLedger::kMaxRows; ++i) {
    const ModelSpend& r = all[i];
    if (prov != r.prov) continue;
    ModelEntry e{r.prov, r.model, rows[i].take, r.totIn, r.totOut, L.rowRate(prov, r.model),
                 rows[i].cents};
    if (std::strcmp(r.model, kOtherModel) == 0) {
      otherShown = true;
      e.take.in += lost.in;
      e.take.out += lost.out;
      e.take.cacheR += lost.cacheR;
      e.take.cacheW += lost.cacheW;
      e.totIn = addCap(e.totIn, lost.in);   // row totals may be saturated
      e.totOut = addCap(e.totOut, lost.out);
      e.cents += s.othCents;
    }
    modelRow(o, e, first);
  }
  if (otherShown || lost.empty()) return;
  modelRow(o, ModelEntry{prov.c_str(), kOtherModel, lost, lost.in, lost.out, s.othRate, s.othCents},
           first);
}

void residualRow(Out& o, const std::string& prov, const PriceSummary& s, bool& first) {
  if (!s.resIn && !s.resOut && !s.resCr && !s.resCw) return;
  o.f("%s{\"prov\":\"%s\",\"pIn\":%llu,\"pOut\":%llu,\"pCr\":%llu,\"pCw\":%llu,\"rIn\":%llu,"
      "\"rOut\":%llu,\"src\":\"%s\",\"est\":%llu}",
      first ? "" : ",", prov.c_str(), (ull)s.resIn, (ull)s.resOut, (ull)s.resCr, (ull)s.resCw,
      (ull)s.resRate.in, (ull)s.resRate.out, rateSrcName(s.resRate.src()), (ull)s.resCents);
  first = false;
}

// The count of items in a comma-separated list.
size_t csvCount(const std::string& csv) {
  return csv.empty() ? 0 : (size_t)std::count(csv.begin(), csv.end(), ',') + 1;
}

void editableRow(Out& o, const UsageLedger& L, const std::string& prov, const char* m, bool& first) {
  if (!modelKeySafe(m)) return;
  const EffRate r = L.effectiveRate(prov, m);
  const ModelRate* own = L.models().ownerRate(prov.c_str(), m);
  o.f("%s{\"m\":\"%s\",\"rIn\":%llu,\"rOut\":%llu,\"src\":\"%s\",\"oIn\":%lu,\"oOut\":%lu}",
      first ? "" : ",", m, (ull)r.in, (ull)r.out, rateSrcName(r.src()),
      (unsigned long)(own ? own->microIn : 0), (unsigned long)(own ? own->microOut : 0));
  first = false;
}

void editableRows(Out& o, const UsageLedger& L, const EditableModels& e, bool& first) {
  if (!provKeySafe(e.prov.c_str())) return;
  o.f("%s\"%s\":[", first ? "" : ",", e.prov.c_str());
  first = false;
  bool f1 = true;
  size_t p = 0;
  while (p < e.csv.size()) {
    size_t c = e.csv.find(',', p);
    if (c == std::string::npos) c = e.csv.size();
    char m[kModelKeyCap];
    if (c > p && c - p < kModelKeyCap) {
      std::memcpy(m, e.csv.data() + p, c - p);
      m[c - p] = 0;
      editableRow(o, L, e.prov, m, f1);
    }
    p = c + 1;
  }
  o.f("]");
}


uint64_t minU(uint64_t a, uint64_t b) { return a < b ? a : b; }

// The most all-time tokens that can be earlier usage (from before tracking started):
// all of them before it started, else the track's part (none without a track).
void earlierAllTimeCap(const UsageLedger& L, const std::string& prov, uint64_t& in, uint64_t& out) {
  in = out = UINT64_MAX;
  if (!L.models().tracking()) return;
  const ProvTrack* t = L.models().track(prov.c_str());
  in = t ? t->preTotIn : 0;
  out = t ? t->preTotOut : 0;
}

// All-time estimate (the All time tile): each model row's all-time tokens at its row
// price; of the provider's all-time tokens no row covers, the earlier usage at the
// residual price and the rest (their model was lost) at the provider's highest price;
// all-time calls at the call price; one floor to cents. approx = a built-in price is
// in it.
uint64_t allTimeCents(const UsageLedger& L, const std::string& prov, bool& approx) {
  const ProviderBudget* e = L.find(prov);
  if (!e) return 0;
  uint64_t micro = 0, remIn = e->totalIn, remOut = e->totalOut;
  auto add = [&](uint64_t in, uint64_t out, const EffRate& r) {
    micro = addCap(addCap(micro, costMicro(in, r.in)), costMicro(out, r.out));
    if ((in && rateSrcIsEstimate(r.srcIn)) || (out && rateSrcIsEstimate(r.srcOut))) approx = true;
  };
  for (const ModelSpend& r : L.models().rows()) {
    if (prov != r.prov) continue;
    const uint64_t in = minU(r.totIn, remIn), out = minU(r.totOut, remOut);
    remIn -= in;
    remOut -= out;
    add(in, out, L.rowRate(prov, r.model));
  }
  uint64_t capIn, capOut;
  earlierAllTimeCap(L, prov, capIn, capOut);
  const uint64_t preIn = minU(remIn, capIn), preOut = minU(remOut, capOut);
  add(remIn - preIn, remOut - preOut, L.otherRate(prov));   // their model was lost
  add(preIn, preOut, L.residualRate(prov));
  RateSrc cs;
  const uint64_t cr = L.callRate(prov, &cs);
  if (e->totalCalls && cr) {
    micro = addCap(micro, (e->totalCalls > UINT64_MAX / cr ? UINT64_MAX : e->totalCalls * cr) / 1000ULL);
    if (rateSrcIsEstimate(cs)) approx = true;
  }
  return micro == UINT64_MAX ? UINT64_MAX : micro / 10000ULL;
}

// r* = the residual price (earlier usage); f* = the provider default (the owner's
// provider rate, else the built-in default); o* = the fold price (otherRate: ~other and
// any model with no row of its own, gate G4).
void provRateRow(Out& o, const UsageLedger& L, const std::string& prov, const PriceSummary& s,
                 bool& first) {
  const EffRate res = L.residualRate(prov), any = L.effectiveRate(prov, ""),
                oth = L.otherRate(prov);
  bool allApprox = false;
  const uint64_t all = allTimeCents(L, prov, allApprox);
  o.f("%s\"%s\":{\"rIn\":%llu,\"rOut\":%llu,\"rCall\":%llu,\"src\":\"%s\",\"srcCall\":\"%s\","
      "\"fIn\":%llu,\"fOut\":%llu,\"fSrc\":\"%s\",\"oIn\":%llu,\"oOut\":%llu,\"oSrc\":\"%s\","
      "\"est\":%llu,\"callEst\":%llu,\"approx\":%s,\"allEst\":%llu,\"allApprox\":%s}",
      first ? "" : ",", prov.c_str(), (ull)res.in, (ull)res.out, (ull)s.callRate,
      rateSrcName(res.src()), rateSrcName(s.callSrc), (ull)any.in, (ull)any.out,
      rateSrcName(any.src()), (ull)oth.in, (ull)oth.out, rateSrcName(oth.src()), (ull)s.cents,
      (ull)s.callCents, s.approx ? "true" : "false", (ull)all, allApprox ? "true" : "false");
  first = false;
}

}  // namespace

size_t usageModelsJsonCap(const UsageLedger& L, const std::vector<EditableModels>& editable) {
  size_t cap = kEnvelope;
  cap += L.models().rows().size() * kModelRow;
  const std::vector<std::string> provs = providers(L, editable);
  // Per provider: its residual, its rates and at most one "~other" entry made for
  // spend whose model was lost (modelRows).
  cap += provs.size() * (kResidualRow + kProvRateRow + kModelRow);
  for (const auto& e : editable) cap += kEditableProv + csvCount(e.csv) * kEditableRow;
  return cap;
}

size_t writeUsageModelsJson(const UsageLedger& L, const std::vector<EditableModels>& editable,
                            char* buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  Out o{buf, cap};
  const std::vector<std::string> provs = providers(L, editable);
  std::vector<PriceSummary> sums(provs.size());
  std::vector<RowPrice> rows(ModelLedger::kMaxRows);
  o.f("{\"asOf\":\"%s\",\"models\":[", kBuiltinPricesAsOf);
  bool first = true;
  for (size_t i = 0; i < provs.size(); ++i) {
    L.priceProvider(provs[i], &sums[i], rows.data());
    modelRows(o, L, provs[i], rows.data(), sums[i], first);
  }
  o.f("],\"residual\":[");
  first = true;
  for (size_t i = 0; i < provs.size(); ++i) residualRow(o, provs[i], sums[i], first);
  o.f("],\"editable\":{");
  first = true;
  for (const auto& e : editable) editableRows(o, L, e, first);
  o.f("},\"provRate\":{");
  first = true;
  for (size_t i = 0; i < provs.size(); ++i) provRateRow(o, L, provs[i], sums[i], first);
  o.f("}}");
  if (!o.ok) {
    buf[0] = 0;
    return 0;
  }
  return o.off;
}

}  // namespace orch
}  // namespace nimbus
