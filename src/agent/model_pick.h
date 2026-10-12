#pragma once
// model_pick - which model a POST /api/orch orchM_<p> / subM_<p> may store, and
// the owner copy when it may not (CUM-530).
//
// The save used to validate against the compact choice list only and answer
// 200 {"ok":true} on a refusal, so an owner (or the bench) "saved" a model and
// nothing changed. A pick is now allowed when it is:
//   - empty (back to the provider default),
//   - in the choice list (live-harvested, else the compiled cold-start list),
//   - in the provider's catalog (GET /api/models, the list the picker renders)
//     for that role, usable entries only, or
//   - the provider's compiled default model (so the default can always be
//     re-picked by name).
// Router (Cumulo) selectors compare in canonical "<upstream>/<model>" form, so
// the default's bare spelling "gpt-5.6" and the router's "openai/gpt-5.6" are the
// same pick, and a selector no upstream runs ("mistral/mistral/zai-glm-5-3") is
// never allowed, offered or named, even from a list an older build stored. A
// refusal is a 400 whose error names the allowed values.
// Header-only and Arduino-free: test/test_router_catalog pins it on the host.

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "nimbus/orch/model_choices.h"
#include "router_catalog.h"

namespace agent {
namespace model_pick {

// A stored catalog row some upstream can run: a router row ("upstream" set) whose
// "<upstream>/<id>" is routable, or any row of a direct provider. A catalog an
// older build harvested can still hold "mistral" / "mistral/zai-glm-5-3" (CUM-530).
inline bool rowRuns(JsonObjectConst m) {
  const std::string up = m["upstream"] | "";
  return up.empty() || router_catalog::routable(up + "/" + std::string(m["id"] | ""));
}

// The selectors the picker offers for `role` ("orchestrator" | "sub-agent") from a
// stored catalog blob (the /data/mcat_<p>.json array): usable entries that carry
// the role and that some upstream runs. A router entry's selector is
// "<upstream>/<id>".
inline std::vector<std::string> catalogSelectors(const std::string& blob, const char* role,
                                                 ArduinoJson::Allocator* alloc = nullptr) {
  std::vector<std::string> out;
  if (blob.empty() || !role) return out;
  JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
  if (deserializeJson(doc, blob)) return out;
  for (JsonObjectConst m : doc.as<JsonArrayConst>()) {
    if (!(m["usable"] | true) || !rowRuns(m)) continue;
    bool hasRole = false;
    for (JsonVariantConst r : m["roles"].as<JsonArrayConst>())
      hasRole = hasRole || std::string(r | "") == role;
    if (!hasRole) continue;
    const std::string id = m["id"] | "";
    const std::string up = m["upstream"] | "";
    if (!id.empty()) out.push_back(up.empty() ? id : up + "/" + id);
  }
  return out;
}

struct PickLists {
  std::string choices;               // the effective compact choice list (CSV)
  std::vector<std::string> catalog;  // catalogSelectors() for the role
  std::string defaultModel;          // the compiled provider default
  bool router = false;               // Cumulo: compare canonical selectors
};

// The trimmed, non-empty items of a comma-separated list.
inline std::vector<std::string> csvItems(const std::string& csv) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string::npos) comma = csv.size();
    const std::string item = csv.substr(start, comma - start);
    const size_t b = item.find_first_not_of(" \t");
    if (b != std::string::npos) out.push_back(item.substr(b, item.find_last_not_of(" \t") - b + 1));
    start = comma + 1;
  }
  return out;
}

// One pick: equal, or - for the router - the same canonical selector.
inline bool samePick(const PickLists& l, const std::string& a, const std::string& b) {
  if (a == b) return true;
  return l.router && !a.empty() && !b.empty() &&
         router_catalog::canonical(a) == router_catalog::canonical(b);
}

// A list item the router lists can be offered: always for a direct provider; for
// the router, only a selector some upstream runs.
inline bool offered(const PickLists& l, const std::string& item) {
  return !l.router || router_catalog::routable(item);
}

inline bool allowed(const PickLists& l, const std::string& v) {
  if (v.empty()) return true;
  if (!offered(l, v)) return false;   // before any list: a stored list may still name it
  if (samePick(l, l.defaultModel, v)) return true;
  for (const std::string& item : csvItems(l.choices))
    if (samePick(l, item, v)) return true;
  for (const std::string& c : l.catalog)
    if (samePick(l, c, v)) return true;
  return false;
}

// "That model isn't available for <label>. Choose one of: a, b, c." The list is the
// compact choice list (bounded), plus the default when it is not already in it.
inline std::string rejectMessage(const std::string& label, const PickLists& l) {
  std::string names;
  bool hasDefault = l.defaultModel.empty();
  for (const std::string& item : csvItems(l.choices)) {
    if (!offered(l, item)) continue;
    names += (names.empty() ? "" : ", ") + item;
    hasDefault = hasDefault || samePick(l, item, l.defaultModel);
  }
  if (!hasDefault) names = l.defaultModel + (names.empty() ? "" : ", " + names);
  return "That model isn't available for " + label + ". Choose one of: " +
         (names.empty() ? std::string("(default)") : names) + ".";
}

}  // namespace model_pick
}  // namespace agent
