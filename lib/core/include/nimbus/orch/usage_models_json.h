#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "nimbus/orch/usage_ledger.h"

// usage_models_json - the body of GET /api/usage/models (per-model prices, 2026-10).
// Portable so the worst-case size is host-tested; the device writes it into ONE
// PSRAM buffer sized by usageModelsJsonCap() and streams it chunked.
//
// {"asOf":"2026-10",
//  "models":[{prov,m,pIn,pOut,pCr,pCw,tIn,tOut,rIn,rOut,src,est}],   per model row
//  "residual":[{prov,pIn,pOut,pCr,pCw,rIn,rOut,src,est}],            unattributed spend
//  "editable":{"<prov>":[{m,rIn,rOut,src,oIn,oOut}]},                the Prices table rows
//  "provRate":{"<prov>":{rIn,rOut,rCall,src,srcCall,fIn,fOut,fSrc,est,callEst,approx,
//                        allEst,allApprox}}}
// provRate r* is the residual's price (unattributed spend, the "~other" row), f* what
// any other model of the provider costs (the Prices table's "Any other" row), allEst
// the all-time estimate (all-time tokens at today's prices, the All time tile).
// Rates are micro-USD per 1M tokens (rCall: per 1000 calls); est is cents, src one of
// "owner" | "router" | "provider" | "est" | "none". p* are this period's tokens (as
// priced: clamped to the provider counters), t* all-time. For every provider, the
// models' est + its residual est + provRate.callEst add up EXACTLY to provRate.est,
// which equals byProvider.estCents (UsageLedger::estCents, the one formula).

namespace nimbus {
namespace orch {

struct EditableModels {
  std::string prov;
  std::string csv;   // comma-separated keys (editableModelKeys)
};

// An upper bound on the body for this ledger and these lists.
size_t usageModelsJsonCap(const UsageLedger& L, const std::vector<EditableModels>& editable);

// Write the body into buf (cap bytes, NUL-terminated). Returns the length, or 0 when
// it did not fit (never a truncated document).
size_t writeUsageModelsJson(const UsageLedger& L, const std::vector<EditableModels>& editable,
                            char* buf, size_t cap);

}  // namespace orch
}  // namespace nimbus
