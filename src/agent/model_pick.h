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
// same pick. A refusal is a 400 whose error names the allowed values.
// Header-only and Arduino-free: test/test_router_catalog pins it on the host.

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "nimbus/orch/model_choices.h"
#include "router_catalog.h"

namespace agent {
namespace model_pick {

// The selectors the picker offers for `role` ("orchestrator" | "sub-agent") from a
// stored catalog blob (the /data/mcat_<p>.json array): usable entries that carry
// the role. A router entry's selector is "<upstream>/<id>".
inline std::vector<std::string> catalogSelectors(const std::string& blob, const char* role,
                                                 ArduinoJson::Allocator* alloc = nullptr) {
  std::vector<std::string> out;
  if (blob.empty() || !role) return out;
  JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
  if (deserializeJson(doc, blob)) return out;
  for (JsonObjectConst m : doc.as<JsonArrayConst>()) {
    if (!(m["usable"] | true)) continue;
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

inline bool allowed(const PickLists& l, const std::string& v) {
  if (v.empty()) return true;
  auto same = [&](const std::string& a) {
    if (a == v) return true;
    return l.router && !a.empty() &&
           router_catalog::canonical(a) == router_catalog::canonical(v);
  };
  if (same(l.defaultModel)) return true;
  if (nimbus::orch::modelInChoices(l.choices, v)) return true;
  size_t start = 0;
  while (l.router && start <= l.choices.size()) {
    size_t comma = l.choices.find(',', start);
    if (comma == std::string::npos) comma = l.choices.size();
    std::string item = l.choices.substr(start, comma - start);
    const size_t b = item.find_first_not_of(" \t");
    const size_t e = item.find_last_not_of(" \t");
    if (b != std::string::npos && same(item.substr(b, e - b + 1))) return true;
    start = comma + 1;
  }
  for (const std::string& c : l.catalog)
    if (same(c)) return true;
  return false;
}

// "That model isn't available for <label>. Choose one of: a, b, c." The list is the
// compact choice list (bounded), plus the default when it is not already in it.
inline std::string rejectMessage(const std::string& label, const PickLists& l) {
  std::string names;
  size_t start = 0;
  bool hasDefault = l.defaultModel.empty();
  while (start <= l.choices.size()) {
    size_t comma = l.choices.find(',', start);
    if (comma == std::string::npos) comma = l.choices.size();
    std::string item = l.choices.substr(start, comma - start);
    const size_t b = item.find_first_not_of(" \t");
    if (b != std::string::npos) {
      item = item.substr(b, item.find_last_not_of(" \t") - b + 1);
      if (!names.empty()) names += ", ";
      names += item;
      hasDefault = hasDefault || item == l.defaultModel ||
                   (l.router && router_catalog::canonical(item) ==
                                    router_catalog::canonical(l.defaultModel));
    }
    start = comma + 1;
  }
  if (!hasDefault) names = l.defaultModel + (names.empty() ? "" : ", " + names);
  return "That model isn't available for " + label + ". Choose one of: " +
         (names.empty() ? std::string("(default)") : names) + ".";
}

}  // namespace model_pick
}  // namespace agent
