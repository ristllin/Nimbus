#include "nimbus/orch/model_defaults.h"

#include <cstring>

#include "nimbus/orch/model_choices.h"
#include "nimbus/orch/model_price_defaults.h"

namespace nimbus {
namespace orch {

const char* defaultRoleSlug(DefaultRole r) {
  switch (r) {
    case DefaultRole::Head:       return "head";
    case DefaultRole::Sub:        return "sub";
    case DefaultRole::Caption:    return "caption";
    case DefaultRole::Background: return "background";
  }
  return "head";
}

const RoleDefault* roleDefaultRow(const char* slot, DefaultRole role) {
  if (!slot) return nullptr;
  for (const RoleDefault& r : kRoleDefaults)
    if (r.role == role && std::strcmp(r.slot, slot) == 0) return &r;
  return nullptr;
}

namespace {

// Whether the price row `family` is in one of the '|'-separated class items: the item
// itself, or the item plus a version ("claude-sonnet" holds "claude-sonnet-5-5", so a
// new version with a price row joins its class; "glm-5.3" never holds "glm-5.3-flash").
bool familyListed(const char* families, const char* family) {
  const size_t fl = std::strlen(family);
  for (const char* p = families; *p;) {
    const char* bar = std::strchr(p, '|');
    const size_t n = bar ? (size_t)(bar - p) : std::strlen(p);
    if (n <= fl && std::strncmp(p, family, n) == 0 &&
        (fl == n || (family[n] == '-' && family[n + 1] >= '0' && family[n + 1] <= '9')))
      return true;
    if (!bar) break;
    p = bar + 1;
  }
  return false;
}

uint64_t listPrice(const BuiltinPrice& row) { return (uint64_t)row.microIn + row.microOut; }

}  // namespace

bool inPriceClass(const RoleDefault& row, const char* model) {
  const BuiltinPrice* p = builtinModelPrice(row.slot, model);
  return p && familyListed(row.families, p->family);
}

std::string roleDefaultModel(const char* slot, DefaultRole role, const std::string& catalogCsv,
                             bool harvested) {
  const RoleDefault* row = roleDefaultRow(slot, role);
  if (!row) return std::string();
  if (!harvested || catalogCsv.empty()) return row->fallback;
  std::string best;
  uint64_t bestCost = UINT64_MAX;
  bool bestNamed = false;
  size_t start = 0;
  while (start <= catalogCsv.size()) {
    size_t comma = catalogCsv.find(',', start);
    if (comma == std::string::npos) comma = catalogCsv.size();
    std::string id = catalogCsv.substr(start, comma - start);
    start = comma + 1;
    const size_t b = id.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) continue;
    id = id.substr(b, id.find_last_not_of(" \t\r\n") - b + 1);
    if (!inPriceClass(*row, id.c_str())) continue;
    const uint64_t cost = listPrice(*builtinModelPrice(row->slot, id.c_str()));
    const bool named = id == row->fallback;
    // Cheaper wins; on a tie the named fallback wins, else the earlier item.
    if (cost < bestCost || (cost == bestCost && named && !bestNamed)) {
      best = id;
      bestCost = cost;
      bestNamed = named;
    }
  }
  return best.empty() ? std::string(row->fallback) : best;
}

// ---- the turn in flight ---------------------------------------------------------

namespace {
thread_local TurnRole t_turnRole = TurnRole::Owner;
}  // namespace

TurnRole currentTurnRole() { return t_turnRole; }

BackgroundTurnScope::BackgroundTurnScope() : prev_(t_turnRole) {
  t_turnRole = TurnRole::Background;
}
BackgroundTurnScope::~BackgroundTurnScope() { t_turnRole = prev_; }

}  // namespace orch
}  // namespace nimbus
