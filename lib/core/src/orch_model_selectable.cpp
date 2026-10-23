#include "nimbus/orch/model_selectable.h"

#include "nimbus/orch/model_catalog.h"   // forEachStoreRow, rowOffered, sameModelKey
#include "nimbus/orch/model_choices.h"   // choiceItems - the cold-start list
#include "nimbus/orch/router_route.h"    // resolveRouterRoute, routableSelector

namespace nimbus {
namespace orch {

namespace {

uint16_t roleBit(PickRole r) {
  switch (r) {
    case PickRole::Head: return RoleOrchestrator;
    case PickRole::Sub: return RoleSubAgent;
    default: return 0;   // either chat role
  }
}

// The comparison key of a pick: the id itself for a direct provider; for the router
// its canonical "<upstream>/<model>" with the version separator unified.
std::string pickKey(bool router, const std::string& id) {
  if (!router) return id;
  const RouterRoute r = resolveRouterRoute(id);
  return r.model.empty() ? std::string() : sameModelKey(r.upstream + "/" + r.model);
}

}  // namespace

namespace {
// Whether the store holds an offered model for the role, and whether `want` is one.
struct StoreVerdict {
  bool anyOffered = false;
  bool accepted = false;
};
StoreVerdict scanStore(const std::string& provider, bool router, const std::string& want, uint16_t bit,
                       const SelectableSources& src) {
  StoreVerdict v;
  if (!src.storeText || src.storeText->empty()) return v;
  forEachStoreRow(provider, *src.storeText, [&](const ModelInfo& m) {
    const bool offered = rowOffered(m, bit, src.now);
    v.anyOffered = v.anyOffered || offered;
    v.accepted = offered && pickKey(router, modelSelector(m)) == want;
    return !v.accepted;
  });
  return v;
}

// Whether `want` is an item of the comma-separated cold-start list.
bool inColdStart(bool router, const std::string& want, const std::string& cs) {
  for (const std::string& item : choiceItems(cs))
    if (pickKey(router, item) == want) return true;
  return false;
}

// The provider's role defaults, always allowed (lane DEF: the sub default for a sub pick).
bool isRoleDefault(bool router, const std::string& want, PickRole role, const SelectableSources& src) {
  if (!src.defaultModel.empty() && pickKey(router, src.defaultModel) == want) return true;
  return role != PickRole::Head && !src.subDefaultModel.empty() &&
         pickKey(router, src.subDefaultModel) == want;
}
}  // namespace

bool modelSelectable(const std::string& provider, const std::string& id, PickRole role,
                     const SelectableSources& src) {
  if (id.empty()) return false;
  const bool router = provider == "cumulo";
  if (router && !routableSelector(id)) return false;
  const std::string want = pickKey(router, id);
  if (want.empty()) return false;
  if (isRoleDefault(router, want, role, src)) return true;
  // The store is the authority once it offers anything for the role: a model it
  // lists but cannot run now (unusable, retired) is refused like an unlisted one.
  const StoreVerdict v = scanStore(provider, router, want, roleBit(role), src);
  if (v.accepted) return true;
  if (v.anyOffered) return false;
  // No store (or none of its rows can run the role): the compiled cold-start list.
  return inColdStart(router, want, src.coldStart);
}

}  // namespace orch
}  // namespace nimbus
