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
// Lane ALIAS: does row `m` name `want` - as its own selector or as one of its aliases
// (a provider that renamed a model to an alias of a live model keeps the choice)?
bool rowNames(bool router, const ModelInfo& m, const std::string& want) {
  if (pickKey(router, modelSelector(m)) == want) return true;
  if (m.aliases.empty()) return false;
  size_t b = 0;
  while (b <= m.aliases.size()) {
    const size_t e = m.aliases.find(',', b);
    const std::string al = m.aliases.substr(b, e == std::string::npos ? e : e - b);
    if (!al.empty() && pickKey(router, router && !m.upstream.empty() ? m.upstream + "/" + al : al) == want)
      return true;
    if (e == std::string::npos) break;
    b = e + 1;
  }
  return false;
}
StoreVerdict scanStore(const std::string& provider, bool router, const std::string& want, uint16_t bit,
                       const SelectableSources& src) {
  StoreVerdict v;
  if (!src.storeText || src.storeText->empty()) return v;
  forEachStoreRow(provider, *src.storeText, [&](const ModelInfo& m) {
    const bool offered = rowOffered(m, bit, src.now);
    v.anyOffered = v.anyOffered || offered;
    v.accepted = offered && rowNames(router, m, want);
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

// The model a probed alias answers as, in the same key form, or "" (no alias). For the
// router the alias is looked up under its upstream ("openai/gpt-5.6").
std::string servedFor(const std::string& provider, bool router, const std::string& want) {
  std::string up = provider, model = want;
  if (router) {
    const RouterRoute r = resolveRouterRoute(want);
    up = r.upstream;
    model = r.model;
  }
  for (const ServedAlias& a : kServedAliases)   // the router's key has its separators unified
    if (up == a.provider && model == (router ? sameModelKey(a.alias) : std::string(a.alias)))
      return router ? sameModelKey(up + "/" + a.served) : std::string(a.served);
  return std::string();
}

bool isConfigured(bool router, const std::string& want, const SelectableSources& src) {
  for (const std::string& c : src.configured)
    if (!c.empty() && pickKey(router, c) == want) return true;
  return false;
}
}  // namespace

bool modelListed(const std::string& provider, const std::string& id, PickRole role,
                 const SelectableSources& src) {
  if (id.empty()) return false;
  const bool router = provider == "cumulo";
  if (router && !routableSelector(id)) return false;
  const std::string want = pickKey(router, id);
  if (want.empty()) return false;
  if (isRoleDefault(router, want, role, src)) return true;
  // The store is the authority once it offers anything for the role: a model it
  // lists but cannot run now (unusable, retired) is refused like an unlisted one.
  // A probed alias stands for the model it answers as.
  const std::string served = servedFor(provider, router, want);
  const StoreVerdict v = scanStore(provider, router, want, roleBit(role), src);
  if (v.accepted) return true;
  if (!served.empty() && scanStore(provider, router, served, roleBit(role), src).accepted) return true;
  if (v.anyOffered) return false;
  // No store (or none of its rows can run the role): the compiled cold-start list.
  return inColdStart(router, want, src.coldStart) ||
         (!served.empty() && inColdStart(router, served, src.coldStart));
}

bool modelSelectable(const std::string& provider, const std::string& id, PickRole role,
                     const SelectableSources& src) {
  if (id.empty()) return false;
  const bool router = provider == "cumulo";
  if (router && !routableSelector(id)) return false;
  const std::string want = pickKey(router, id);
  if (!want.empty() && isConfigured(router, want, src)) return true;   // never refuse a stored pick
  return modelListed(provider, id, role, src);
}

std::string canonicalModelFor(const std::string& provider, const std::string& storeText,
                              const std::string& id) {
  if (id.empty() || storeText.empty()) return std::string();
  const bool router = provider == "cumulo";
  if (router && !routableSelector(id)) return std::string();
  const std::string want = pickKey(router, id);
  if (want.empty()) return std::string();
  // Fix pass F2: a row whose OWN selector is the pick wins over a sibling row that
  // merely lists the pick as an alias. The server's data[] order is arbitrary (a
  // magistral row can list mistral-medium-latest as its first alias), so a first-match
  // scan could show - and a save rewrite - a different model than the owner picked.
  std::string own, viaAlias;
  forEachStoreRow(provider, storeText, [&](const ModelInfo& m) {
    const std::string sel = modelSelector(m);
    if (pickKey(router, sel) == want) {
      own = sel;
      return false;
    }
    if (viaAlias.empty() && rowNames(router, m, want)) viaAlias = sel;
    return true;
  });
  return !own.empty() ? own : viaAlias;
}

// Lane ALIAS fix pass F1: the notice says what the device actually does - the pick keeps
// running while the provider answers it - and names the role's current default as the
// model to switch to. It never promises an automatic swap: nothing in the firmware
// moves a stored pick on its own (only the owner's save or the orch_model tool does).
std::string gonePickNoticeText(const std::string& providerLabel, const std::string& pick,
                               const char* roleWord, const std::string& roleDefault) {
  std::string s = providerLabel + "'s model list no longer names " + pick + ", the " + roleWord +
                  " model you chose. It keeps running while " + providerLabel +
                  " answers it; if it stops answering, choose another under Assistant > Models";
  if (!roleDefault.empty()) s += "; the current default is " + roleDefault;
  s += ".";
  return s;
}

}  // namespace orch
}  // namespace nimbus
