#pragma once
// model_pick - which model a POST /api/orch orchM_<p> / subM_<p> may store, and
// the owner copy when it may not (CUM-530, lane F).
//
// The save used to validate against the compact choice list only and answer
// 200 {"ok":true} on a refusal, so an owner (or the bench) "saved" a model and
// nothing changed; then it also accepted the catalog file, which the head's own
// checks did not read. A pick is now allowed exactly when it is empty (back to the
// provider default) or nimbus::orch::modelSelectable() accepts it for that role -
// the ONE check orch_model and spawns ask too (model_selectable.h): the model store,
// else the cold-start list, plus the compiled default. Router (Cumulo) selectors
// compare in canonical "<upstream>/<model>" form across the dot and dash spellings,
// so "gpt-5.6" and "openai/gpt-5.6" are the same pick, and a selector no upstream
// runs ("mistral/mistral/zai-glm-5-3") is never allowed, offered or named. A
// refusal is a 400 whose error names the short list.
// Header-only and Arduino-free: test/test_router_catalog pins it on the host.

#include <string>
#include <vector>

#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_selectable.h"
#include "router_catalog.h"

namespace agent {
namespace model_pick {

using nimbus::orch::choiceItems;   // the one CSV reader (model_choices.h)

// Whether `v` may be stored as the `role` model of `provider`: "" (the default) or
// what the one check accepts over `src`.
inline bool allowed(const std::string& provider, const std::string& v, nimbus::orch::PickRole role,
                    const nimbus::orch::SelectableSources& src) {
  return v.empty() || nimbus::orch::modelSelectable(provider, v, role, src);
}

// What a refusal names: the effective short list and the compiled default.
struct PickLists {
  std::string choices;               // the effective short list (CSV)
  std::string defaultModel;          // the compiled provider default
  bool router = false;               // Cumulo: compare canonical selectors
};

// One pick: equal, or - for the router - the same canonical selector.
inline bool samePick(const PickLists& l, const std::string& a, const std::string& b) {
  if (a == b) return true;
  return l.router && !a.empty() && !b.empty() &&
         router_catalog::canonical(a) == router_catalog::canonical(b);
}

// The router pick a dispatch runs: `stored` when some upstream runs it, else
// `fallback`. A pick an older build saved can name an id no upstream runs
// ("mistral/mistral/zai-glm-5-3"), and every request on it would be refused.
inline std::string runnableRouterPick(const std::string& stored, const std::string& fallback) {
  return router_catalog::routable(stored) ? stored : fallback;
}

// Whether a list item may be named: always for a direct provider; for the router,
// only a selector some upstream runs.
inline bool offered(const PickLists& l, const std::string& item) {
  return !l.router || router_catalog::routable(item);
}

// "That model isn't available for <label>. Choose one of: a, b, c." The list is the
// short list (bounded), plus the default when it is not already in it.
inline std::string rejectMessage(const std::string& label, const PickLists& l) {
  std::string names;
  bool hasDefault = l.defaultModel.empty();
  for (const std::string& item : choiceItems(l.choices)) {
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
