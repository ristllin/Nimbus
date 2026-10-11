#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "nimbus/orch/model_catalog.h"

// provider_verify - an honest "verified" badge and model catalog per provider on a
// hosted instance (CUM-545, device parity with src/agent/provider_verify.cpp).
//
// The hosted page labels a key "verified" and unlocks its model pickers only when
// GET /api/orch reports verify 1, and fills the pickers from GET /api/models.
// nimbusd hard-coded verify -1 and had no /api/models, so a working Cumulo key read
// "unverified" forever and the router model picker stayed disabled. Now a key is
// checked the way the device checks it - one unmetered read of the provider's
// model list (the router's GET /router/models for Cumulo) - whose body is also the
// catalog the picker shows, and a successful turn on a head marks it verified too.
namespace nimbusd {

// Where a provider's model list lives (the device's verify request, per provider).
struct ProbeTarget {
  std::string host, path;
  agent::HeaderList headers;
};
inline bool probeTarget(const std::string& slug, const std::string& key, const std::string& zaiHost,
                        ProbeTarget& t) {
  t = ProbeTarget{};
  if (slug == "openai") t = {"api.openai.com", "/v1/models", {}};
  else if (slug == "anthropic") t = {"api.anthropic.com", "/v1/models", {}};
  else if (slug == "mistral") t = {"api.mistral.ai", "/v1/models", {}};
  else if (slug == "zai") t = {zaiHost, "/api/paas/v4/models", {}};
  else if (slug == "cumulo") t = {"app.cumulo-nimbus.ai", "/router/models", {}};
  else return false;
  if (slug == "anthropic") {
    t.headers.push_back({"x-api-key", key});
    t.headers.push_back({"anthropic-version", "2023-06-01"});
  } else {
    t.headers.push_back({"Authorization", "Bearer " + key});
  }
  t.headers.push_back({"Accept", "application/json"});
  return true;
}

// The device's verdict for a model-list status: 1 verified, 0 rejected, -1 could
// not verify (with the badge's reason token: 402 = no credits or plan; a 404 from
// the router's /router/models = a router that predates it).
inline int8_t probeVerdict(const std::string& slug, int status, std::string& reason) {
  reason.clear();
  if (status == 200) return 1;
  if (status == 401 || status == 403) return 0;
  if (status == 402) reason = "nocredits";
  else if (status == 404 && slug == "cumulo") reason = "router_outdated";
  return -1;
}

struct ProviderVerify {
  int8_t verify = -1;          // 1 verified, 0 rejected, -1 unknown / could not verify
  uint64_t vts = 0;            // epoch of the last verdict (0 = never)
  std::string reason;          // badge reason for a -1 ("nocredits", "router_outdated")
  std::vector<nimbus::orch::ModelInfo> models;   // the harvested catalog ("" before a 200)
  uint64_t catalogAt = 0;      // epoch the catalog was harvested
};

// Thread-safe per-provider state: the engine thread records verdicts, the HTTP
// thread reads them for GET /api/orch and /api/models.
class ProviderVerifyStore {
 public:
  ProviderVerify get(const std::string& slug) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = m_.find(slug);
    return it == m_.end() ? ProviderVerify{} : it->second;
  }
  // Record a verdict. A transient "could not verify" (-1, no reason) never demotes
  // a cached verified (device recordVerify rule).
  void record(const std::string& slug, int8_t verdict, const std::string& reason) {
    std::lock_guard<std::mutex> lk(mu_);
    ProviderVerify& p = m_[slug];
    if (verdict == -1 && reason.empty() && p.verify == 1) return;
    p.verify = verdict;
    p.reason = verdict == -1 ? reason : std::string();
    p.vts = (uint64_t)time(nullptr);
  }
  void setCatalog(const std::string& slug, std::vector<nimbus::orch::ModelInfo> models) {
    std::lock_guard<std::mutex> lk(mu_);
    ProviderVerify& p = m_[slug];
    p.models = std::move(models);
    p.catalogAt = (uint64_t)time(nullptr);
  }
  // A turn that answered on this head proves the key works (the cheapest truthful signal).
  void markWorking(const std::string& slug) {
    std::lock_guard<std::mutex> lk(mu_);
    ProviderVerify& p = m_[slug];
    if (p.verify == 1) return;
    p.verify = 1;
    p.reason.clear();
    p.vts = (uint64_t)time(nullptr);
  }
  void forget(const std::string& slug) {
    std::lock_guard<std::mutex> lk(mu_);
    m_.erase(slug);
  }

 private:
  mutable std::mutex mu_;
  std::map<std::string, ProviderVerify> m_;
};

// The comma list of a catalog's ids that can fill `role`, flagship-first as
// harvested, capped (the picker's Recommended group / the prompt's model list).
inline std::string catalogCsv(const std::vector<nimbus::orch::ModelInfo>& models,
                              nimbus::orch::ModelRole role, size_t cap) {
  std::string out;
  size_t n = 0;
  for (const auto& m : models) {
    if (!m.usable || !m.hasRole(role) || m.deprecated) continue;
    if (n++ >= cap) break;
    out += (out.empty() ? "" : ",") + m.id;
  }
  return out;
}
inline bool catalogHas(const std::vector<nimbus::orch::ModelInfo>& models, const std::string& id,
                       nimbus::orch::ModelRole role) {
  for (const auto& m : models)
    if (m.id == id && m.usable && m.hasRole(role)) return true;
  return false;
}

}  // namespace nimbusd
