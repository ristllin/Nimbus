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
constexpr size_t kProvRateRow = 440;

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

void modelRows(Out& o, const UsageLedger& L, const std::string& prov, const RowPrice* rows,
               bool& first) {
  const auto& all = L.models().rows();
  for (size_t i = 0; i < all.size() && i < ModelLedger::kMaxRows; ++i) {
    const ModelSpend& r = all[i];
    if (prov != r.prov) continue;
    const RowPrice& p = rows[i];
    const EffRate rate = L.rowRate(prov, r.model);
    o.f("%s{\"prov\":\"%s\",\"m\":\"%s\",\"pIn\":%llu,\"pOut\":%llu,\"pCr\":%llu,\"pCw\":%llu,"
        "\"tIn\":%llu,\"tOut\":%llu,\"rIn\":%llu,\"rOut\":%llu,\"src\":\"%s\",\"est\":%llu}",
        first ? "" : ",", r.prov, r.model, (ull)p.take.in, (ull)p.take.out, (ull)p.take.cacheR,
        (ull)p.take.cacheW, (ull)r.totIn, (ull)r.totOut, (ull)rate.in, (ull)rate.out,
        rateSrcName(rate.src()), (ull)p.cents);
    first = false;
  }
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

uint64_t addCap(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }

// All-time estimate (the All time tile): each model row's all-time tokens at its row
// price, the provider's all-time tokens no row covers at the residual price, all-time
// calls at the call price; one floor to cents. approx = a built-in price is in it.
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
    const uint64_t in = r.totIn < remIn ? r.totIn : remIn, out = r.totOut < remOut ? r.totOut : remOut;
    remIn -= in;
    remOut -= out;
    add(in, out, L.rowRate(prov, r.model));
  }
  add(remIn, remOut, L.residualRate(prov));
  RateSrc cs;
  const uint64_t cr = L.callRate(prov, &cs);
  if (e->totalCalls && cr) {
    micro = addCap(micro, (e->totalCalls > UINT64_MAX / cr ? UINT64_MAX : e->totalCalls * cr) / 1000ULL);
    if (rateSrcIsEstimate(cs)) approx = true;
  }
  return micro == UINT64_MAX ? UINT64_MAX : micro / 10000ULL;
}

// r* = the residual price (spend no model row covers); f* = the provider default (the
// owner's provider rate, else the built-in default).
void provRateRow(Out& o, const UsageLedger& L, const std::string& prov, const PriceSummary& s,
                 bool& first) {
  const EffRate res = L.residualRate(prov), any = L.effectiveRate(prov, "");
  bool allApprox = false;
  const uint64_t all = allTimeCents(L, prov, allApprox);
  o.f("%s\"%s\":{\"rIn\":%llu,\"rOut\":%llu,\"rCall\":%llu,\"src\":\"%s\",\"srcCall\":\"%s\","
      "\"fIn\":%llu,\"fOut\":%llu,\"fSrc\":\"%s\",\"est\":%llu,\"callEst\":%llu,\"approx\":%s,"
      "\"allEst\":%llu,\"allApprox\":%s}",
      first ? "" : ",", prov.c_str(), (ull)res.in, (ull)res.out, (ull)s.callRate,
      rateSrcName(res.src()), rateSrcName(s.callSrc), (ull)any.in, (ull)any.out,
      rateSrcName(any.src()), (ull)s.cents, (ull)s.callCents, s.approx ? "true" : "false",
      (ull)all, allApprox ? "true" : "false");
  first = false;
}

}  // namespace

size_t usageModelsJsonCap(const UsageLedger& L, const std::vector<EditableModels>& editable) {
  size_t cap = kEnvelope;
  cap += L.models().rows().size() * kModelRow;
  const std::vector<std::string> provs = providers(L, editable);
  cap += provs.size() * (kResidualRow + kProvRateRow);
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
    modelRows(o, L, provs[i], rows.data(), first);
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
