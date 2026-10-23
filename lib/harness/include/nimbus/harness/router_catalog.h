#pragma once
// router_catalog - the Cumulo router's model list, on the device and on a hosted
// Virtual Nimbus (CUM-530, lane F L19-49).
//
// The router's GET /router/models lists every routable model as
// "<upstream>/<model>" (~95 KB, over 600 rows), ordered size class first, then
// upstream name, then model. The device reads it whole as a stream
// (nimbus::orch::ModelsBodyReader) into its model store, and this header is the one
// place that decides which router models the store keeps and in what order:
//   - one slot per model: a dated snapshot whose undated alias is listed is dropped,
//     and the dot and dash spellings of one version ("claude-opus-5.5",
//     "claude-opus-5-5") are one model, kept under the dash spelling;
//   - the router default first, then the first (flagship) model of each upstream,
//     then every upstream's current chat models, then the rest;
//   - at most kCatalogMaxRows rows (nimbus::orch::kModelStoreMaxRows); what does
//     not fit is counted, so the stored list says "<n> more";
//   - the short list (the [AVAILABLE MODELS] line every turn carries, and the quick
//     picks) is capped by count and bytes and always holds the router default.
// Header-only and Arduino-free so test/test_router_catalog pins it on the host, and
// nimbusd (which builds from lib/ alone) bounds its catalog by the same rule.

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include <algorithm>
#include <cstring>

#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_key.h"      // usageModelKey - the price row's key
#include "nimbus/orch/model_ledger.h"   // ModelRate, kMaxRouterRates
#include "nimbus/orch/router_route.h"

namespace agent {
namespace router_catalog {

// The short list: at most this many ids and this many CSV bytes.
inline constexpr size_t kChoicesMax      = 12;
inline constexpr size_t kChoicesMaxBytes = 480;
// The router's rows in the model store (/data/models_cumulo.txt, GET /api/models):
// the store's own bound, which holds every routable model the router lists today.
inline constexpr size_t kCatalogMaxRows  = nimbus::orch::kModelStoreMaxRows;

// The upstreams the device runs a head on natively (and zai, the router's GLM):
// always grouped. Any other upstream is grouped only while fewer than
// kOtherUpstreamsMax of them are (CUM-530 gate: the body is untrusted, and one
// queue per distinct upstream made a body of thousands of one-row upstreams cost a
// linear search and a queue each; the rows of an upstream past the bound are not
// kept).
inline constexpr size_t kOtherUpstreamsMax = 8;
inline bool knownUpstream(const std::string& up) {
  return up == "openai" || up == "anthropic" || up == "mistral" || up == "zai";
}

// "<upstream>/<model>" for any selector; a bare id is the openai upstream (the
// router's default), exactly as the head and the sub-agent resolve it.
inline std::string canonical(const std::string& sel) {
  const nimbus::orch::RouterRoute r = nimbus::orch::resolveRouterRoute(sel);
  return r.model.empty() ? std::string() : r.upstream + "/" + r.model;
}

inline std::string upstreamOf(const std::string& sel) {
  return nimbus::orch::resolveRouterRoute(sel).upstream;
}

// A selector some upstream can run (nimbus::orch::routableSelector, bench
// CUM-530): "mistral/mistral/zai-glm-5-3" is routed but no upstream runs it, so
// the device never offers one.
inline bool routable(const std::string& sel) { return nimbus::orch::routableSelector(sel); }

// The undated alias of a dated snapshot id ("...-20251101" or "...-2025-04-16"),
// or "" when the id carries no date suffix.
inline std::string undatedAlias(const std::string& id) {
  auto digits = [&](size_t from, size_t n) {
    if (from + n > id.size()) return false;
    for (size_t i = from; i < from + n; ++i)
      if (!isdigit((unsigned char)id[i])) return false;
    return true;
  };
  const size_t n = id.size();
  if (n > 9 && id[n - 9] == '-' && digits(n - 8, 8)) return id.substr(0, n - 9);
  if (n > 11 && id[n - 11] == '-' && digits(n - 10, 4) && id[n - 6] == '-' &&
      digits(n - 5, 2) && id[n - 3] == '-' && digits(n - 2, 2))
    return id.substr(0, n - 11);
  return std::string();
}

// ---- which router rows are chat models -------------------------------------------

// Non-chat FAMILIES that can't run the turn contract (embeddings/audio/image/...),
// by id substring - the one list in nimbus::orch (model_catalog.h).
inline bool neverChatFamily(const std::string& id) { return nimbus::orch::neverChatFamily(id); }

// A router id the device can run as a chat model. The router lists only ROUTABLE
// (priced + enabled) models, so other upstreams are curated by it; the openai
// upstream still drops dated snapshots and non-chat families by name, and no
// upstream's ":batch" price twin is a model the device can call (routerRowKept).
inline bool routerChatModel(const std::string& id) {
  if (id.size() < 3 || neverChatFamily(id) || !nimbus::orch::routerRowKept(id)) return false;
  const nimbus::orch::RouterRoute r = nimbus::orch::resolveRouterRoute(id);
  if (r.upstream != "openai") return true;
  const std::string& m = r.model;
  if (m.find("-202") != std::string::npos) return false;   // dated snapshot of an alias
  return m.rfind("gpt-", 0) == 0 || (m.size() > 1 && m[0] == 'o' && m[1] >= '1' && m[1] <= '9');
}

// Each upstream's CURRENT generation first (rows priced only for back-compat
// billing - claude-3-*, gpt-4o - come after).
inline bool routerFlagship(const std::string& id) { return nimbus::orch::routerFlagshipSelector(id); }

namespace detail {

// The ordering key of a model id within its upstream: its version numbers
// ("claude-opus-4-8" -> {4,8}, "gpt-5.6-sol" -> {5,6}, "gpt-6-astra" -> {6}) and
// whether a variant suffix follows them ("-sol", "-flash", "-mini"). A run of 3+
// digits is a date/build stamp and a number followed by 'b' is a parameter size
// ("pixtral-12b"); neither is a version. Keys compare as a total preorder, so the
// stable sort below is well defined.
struct VersionKey {
  std::vector<int> v;
  bool variant = false;
};
inline VersionKey versionKey(const std::string& model) {
  VersionKey k;
  size_t lastEnd = 0;
  for (size_t i = 0; i < model.size();) {
    if (!isdigit((unsigned char)model[i])) { ++i; continue; }
    size_t j = i;
    while (j < model.size() && isdigit((unsigned char)model[j])) ++j;
    const bool size = j < model.size() && (model[j] == 'b' || model[j] == 'B');
    if (j - i <= 2 && !size) {
      k.v.push_back(std::atoi(model.substr(i, j - i).c_str()));
      lastEnd = j;
    }
    i = j;
  }
  k.variant = !k.v.empty() && lastEnd < model.size();
  return k;
}

// Newer first; on the same version the base model before its variants; otherwise
// the router's order (the sort is stable).
inline std::string undatedModel(const std::string& sel) {
  const std::string m = nimbus::orch::resolveRouterRoute(sel).model;
  const std::string alias = undatedAlias(m);
  return alias.empty() ? m : alias;
}
inline bool newerFirst(const VersionKey& ka, const VersionKey& kb) {
  if (ka.v != kb.v) return ka.v > kb.v;
  return !ka.variant && kb.variant;
}

// Distinct, non-twin candidates (indexes into canon) in list order, one per model:
// the spellings of one model (sameModelKey: "claude-opus-5.5" and "claude-opus-5-5")
// count once, kept under the spelling equal to the key (the dash form the upstream's
// own API uses) when listed, else the first; a dated twin of a listed alias is
// dropped. O(n log n): indexes sorted by (key, position), "is the alias listed" a
// binary search - no pairwise scan (the list comes from an untrusted router body).
inline std::vector<size_t> candidates(const std::vector<std::string>& canon) {
  std::vector<std::string> key(canon.size());
  for (size_t i = 0; i < canon.size(); ++i) key[i] = nimbus::orch::sameModelKey(canon[i]);
  std::vector<size_t> byKey(canon.size());
  for (size_t i = 0; i < byKey.size(); ++i) byKey[i] = i;
  std::sort(byKey.begin(), byKey.end(), [&](size_t a, size_t b) {
    const int c = key[a].compare(key[b]);
    return c != 0 ? c < 0 : a < b;
  });
  // The representative of each key's run: its dash spelling if listed, else its first.
  std::vector<bool> rep(canon.size(), false);
  for (size_t k = 0; k < byKey.size();) {
    size_t e = k + 1;
    while (e < byKey.size() && key[byKey[e]] == key[byKey[k]]) ++e;
    size_t pick = byKey[k];
    for (size_t j = k; j < e; ++j)
      if (canon[byKey[j]] == key[byKey[j]]) { pick = byKey[j]; break; }
    rep[pick] = true;
    k = e;
  }
  auto listed = [&](const std::string& id) {
    const std::string kid = nimbus::orch::sameModelKey(id);
    const auto it = std::lower_bound(byKey.begin(), byKey.end(), kid,
                                     [&](size_t i, const std::string& v) { return key[i] < v; });
    return it != byKey.end() && key[*it] == kid;
  };
  std::vector<size_t> cand;
  for (size_t i = 0; i < canon.size(); ++i) {
    if (canon[i].empty() || !rep[i]) continue;
    const std::string alias = undatedAlias(canon[i]);
    if (!alias.empty() && listed(alias)) continue;   // a dated twin of a listed alias
    cand.push_back(i);
  }
  return cand;
}

// Candidates grouped per upstream, upstreams in first-appearance order (at most the
// four known ones plus kOtherUpstreamsMax others, so each lookup is a bounded scan).
struct Queues {
  std::vector<std::string> ups;
  std::vector<std::vector<size_t>> q;
  size_t dropped = 0;   // candidates of upstreams past the bound (counted as "more")
};
// Each upstream's chat models lead its queue, its current generation (routerFlagship)
// first, then newest first, so neither a non-chat row nor a junk row whose name reads
// as a higher number ever becomes the upstream's head. Each id's key is computed once.
inline void sortQueues(const std::vector<std::string>& canon, const std::vector<size_t>& cand, Queues& qs) {
  std::vector<VersionKey> keys(canon.size());
  std::vector<uint8_t> rank(canon.size(), 0);   // 0 current chat, 1 other chat, 2 not chat
  for (size_t i : cand) {
    keys[i] = versionKey(undatedModel(canon[i]));
    rank[i] = !routerChatModel(canon[i]) ? 2 : routerFlagship(canon[i]) ? 0 : 1;
  }
  for (auto& qv : qs.q)
    std::stable_sort(qv.begin(), qv.end(), [&](size_t a, size_t b) {
      if (rank[a] != rank[b]) return rank[a] < rank[b];
      return newerFirst(keys[a], keys[b]);
    });
}

inline Queues byUpstream(const std::vector<std::string>& canon, const std::vector<size_t>& cand,
                         const std::vector<size_t>& skip) {
  Queues qs;
  size_t others = 0;
  for (size_t i : cand) {
    bool skipped = false;
    for (size_t k : skip) skipped = skipped || k == i;
    if (skipped) continue;
    const std::string up = upstreamOf(canon[i]);
    size_t q = 0;
    while (q < qs.ups.size() && qs.ups[q] != up) ++q;
    if (q == qs.ups.size()) {
      const bool known = knownUpstream(up);
      if (!known && others >= kOtherUpstreamsMax) {   // past the bound: not kept
        ++qs.dropped;
        continue;
      }
      others += known ? 0 : 1;
      qs.ups.push_back(up);
      qs.q.emplace_back();
    }
    qs.q[q].push_back(i);
  }
  // The router lists each upstream alphabetically within a size class (oldest
  // generation first): order each upstream newest-first so a cut list keeps
  // claude-opus-5 / gpt-6-astra / glm-5.3, not the 4.x / 5.5 / glm-5 rows.
  // Each id's key is computed once (not twice per comparison).
  sortQueues(canon, cand, qs);
  return qs;
}

// Every upstream's first model, then every upstream's second, and so on. `defUp`
// already had its first pick (the default), so its queue starts one round late.
inline void roundRobin(const Queues& qs, const std::string& defUp, std::vector<size_t>& order) {
  size_t rounds = 0;
  for (const auto& qv : qs.q) rounds = qv.size() + 1 > rounds ? qv.size() + 1 : rounds;
  for (size_t round = 0; round < rounds; ++round) {
    for (size_t q = 0; q < qs.q.size(); ++q) {
      const size_t had = (qs.ups[q] == defUp) ? 1 : 0;
      if (round >= had && round - had < qs.q[q].size()) order.push_back(qs.q[q][round - had]);
    }
  }
}

}  // namespace detail

// The keep order over `ids` (already filtered to chat-capable models, in the
// order to prefer them): indexes into `ids`, deduped, dated twins and ids no
// upstream runs (routable) dropped. The
// router default first, then round-robin across upstreams in their order of first
// appearance, each upstream newest-first - so any cap keeps the default and each
// upstream's newest model, and a cut list stays a spread of upstreams rather than
// one upstream's whole family.
// `dropped` (optional) receives how many models of upstreams past the bound were
// left out, so a cut list still says the router offered more.
inline std::vector<size_t> keepOrder(const std::vector<std::string>& ids,
                                     const std::string& defaultSel, size_t* dropped = nullptr) {
  std::vector<std::string> canon;
  canon.reserve(ids.size());
  for (const std::string& id : ids) canon.push_back(routable(id) ? canonical(id) : std::string());
  const std::vector<size_t> cand = detail::candidates(canon);
  std::vector<size_t> order;
  const std::string def = canonical(defaultSel);
  const std::string defKey = nimbus::orch::sameModelKey(def);
  for (size_t i : cand)
    if (!def.empty() && nimbus::orch::sameModelKey(canon[i]) == defKey) { order.push_back(i); break; }
  const std::string defUp = order.empty() ? std::string() : upstreamOf(def);
  const detail::Queues qs = detail::byUpstream(canon, cand, order);
  if (dropped) *dropped = qs.dropped;
  detail::roundRobin(qs, defUp, order);
  return order;
}

// The compact choice list. `total` counts every distinct model the router offered
// (after the twin/duplicate rule); kept < total means the list was cut.
struct Shortlist {
  std::string csv;
  size_t kept = 0;
  size_t total = 0;
  bool truncated() const { return kept < total; }
};

inline Shortlist shortlist(const std::vector<std::string>& ids, const std::string& defaultSel,
                           size_t maxCount = kChoicesMax, size_t maxBytes = kChoicesMaxBytes) {
  Shortlist out;
  size_t dropped = 0;
  const std::vector<size_t> order = keepOrder(ids, defaultSel, &dropped);
  out.total = order.size() + dropped;
  for (size_t i : order) {
    const std::string& id = ids[i];
    const size_t add = id.size() + (out.csv.empty() ? 0 : 1);
    if (out.kept >= maxCount || out.csv.size() + add > maxBytes) continue;
    if (!out.csv.empty()) out.csv += ",";
    out.csv += id;
    ++out.kept;
  }
  return out;
}

// ---- the router rows the model store keeps ------------------------------------------

// The router selector of a parsed catalog entry. parseModelsList keeps the bare
// model in `id` and the upstream in `upstream` (classified against its upstream),
// so the selector a pick must send is "<upstream>/<id>".
inline std::string selectorOf(const nimbus::orch::ModelInfo& m) { return nimbus::orch::modelSelector(m); }

namespace detail {
// Rows no upstream runs and ":batch" price twins go; a non-chat router id keeps only
// its non-chat roles (an embedding or speech row), and a row with no role left goes.
inline void keepRunnableRows(std::vector<nimbus::orch::ModelInfo>& cat) {
  using namespace nimbus::orch;
  size_t w = 0;
  for (size_t i = 0; i < cat.size(); ++i) {
    ModelInfo& m = cat[i];
    const std::string sel = selectorOf(m);
    if (!routerRowKept(sel)) continue;
    if (!routerChatModel(sel)) m.roles &= (uint16_t)~(RoleOrchestrator | RoleSubAgent | RoleVision);
    if (!m.roles) continue;
    if (w != i) cat[w] = std::move(m);
    ++w;
  }
  cat.resize(w);
}

// Each kept row's price is the highest of its spellings' (fail-closed money: spend on
// that model is never priced at the cheaper spelling's rate).
inline void raiseToSpellingPrice(std::vector<nimbus::orch::ModelInfo>& cat, const std::vector<std::string>& ids,
                                 const std::vector<size_t>& order) {
  std::vector<std::pair<std::string, size_t>> keyed;
  keyed.reserve(ids.size());
  for (size_t i = 0; i < ids.size(); ++i)
    if (cat[i].priced) keyed.emplace_back(nimbus::orch::sameModelKey(canonical(ids[i])), i);
  std::sort(keyed.begin(), keyed.end());
  for (size_t i : order) {
    const std::string k = nimbus::orch::sameModelKey(canonical(ids[i]));
    auto it = std::lower_bound(keyed.begin(), keyed.end(), std::make_pair(k, (size_t)0));
    for (; it != keyed.end() && it->first == k; ++it) {
      const nimbus::orch::ModelInfo& o = cat[it->second];
      cat[i].priced = true;
      cat[i].priceIn = std::max(cat[i].priceIn, o.priceIn);
      cat[i].priceOut = std::max(cat[i].priceOut, o.priceOut);
    }
  }
}

// A second rate for a key already kept raises it to the higher price, per field.
inline bool mergeKeyed(std::vector<nimbus::orch::ModelRate>& v, const nimbus::orch::ModelRate& r) {
  for (auto& o : v) {
    if (std::strcmp(o.model, r.model) != 0) continue;
    if (r.microIn > o.microIn) o.microIn = r.microIn;
    if (r.microOut > o.microOut) o.microOut = r.microOut;
    return true;
  }
  return false;
}
}  // namespace detail

// Reorder a parsed router catalog in place for the model store and bound it to
// kCatalogMaxRows (CUM-530: the body is untrusted, and thousands of tiny ids used to
// reach the store step whole). Rows no upstream runs and ":batch" twins go, one row
// stays per model (keepOrder), the router default and each upstream's head come
// first, then every upstream's current chat models (routerFlagship), then the other
// chat models, then the non-chat rows - so neither the row cap nor a size trim from
// the back costs the default, an upstream or a flagship listed late. Returns how many
// models the cap cut or the upstream bound left out (the stored list says "<n> more").
inline size_t protectHeads(std::vector<nimbus::orch::ModelInfo>& cat, const std::string& defaultSel) {
  detail::keepRunnableRows(cat);
  std::vector<std::string> ids;
  ids.reserve(cat.size());
  for (const auto& m : cat) ids.push_back(selectorOf(m));
  size_t dropped = 0;
  std::vector<size_t> order = keepOrder(ids, defaultSel, &dropped);
  detail::raiseToSpellingPrice(cat, ids, order);
  std::vector<std::string> ups;   // the heads: the first row of each upstream (bounded scan)
  size_t heads = 0;
  while (heads < order.size() && heads < kCatalogMaxRows) {
    const std::string up = upstreamOf(ids[order[heads]]);
    if (std::find(ups.begin(), ups.end(), up) != ups.end()) break;
    ups.push_back(up);
    ++heads;
  }
  const auto rest = order.begin() + static_cast<std::ptrdiff_t>(heads);
  const auto chatEnd = std::stable_partition(rest, order.end(), [&](size_t i) { return routerChatModel(ids[i]); });
  std::stable_partition(rest, chatEnd, [&](size_t i) { return routerFlagship(ids[i]); });
  const size_t cut = order.size() > kCatalogMaxRows ? order.size() - kCatalogMaxRows : 0;
  order.resize(order.size() - cut);
  std::vector<nimbus::orch::ModelInfo> out;
  out.reserve(order.size());
  for (size_t i : order) out.push_back(std::move(cat[i]));
  cat.swap(out);
  return cut + dropped;
}

// The router's short list from its store rows (protectHeads order): shortlist() over
// the offered chat rows, with the router default always in it ("the Cumulo list
// always holds CUMULO_MODEL"): first when the router lists it, else placed first
// anyway (the router still answers its own default).
inline std::string storeChoices(const std::vector<nimbus::orch::ModelInfo>& rows, const std::string& defaultSel,
                                uint32_t now) {
  std::vector<std::string> ids;
  ids.reserve(rows.size());
  for (const auto& m : rows)
    if (nimbus::orch::rowOffered(m, nimbus::orch::RoleOrchestrator, now) && routerChatModel(selectorOf(m)))
      ids.push_back(selectorOf(m));
  const Shortlist sl = shortlist(ids, defaultSel);
  const std::string def = canonical(defaultSel);
  if (def.empty()) return sl.csv;
  for (const std::string& id : ids)
    if (nimbus::orch::sameModelKey(canonical(id)) == nimbus::orch::sameModelKey(def)) return sl.csv;
  const Shortlist rest = shortlist(ids, defaultSel, kChoicesMax - 1, kChoicesMaxBytes - def.size() - 1);
  return rest.csv.empty() ? def : def + "," + rest.csv;
}

// The ids whose router prices the device keeps in its spend ledger: the short list
// (CSV) plus the configured head and sub-agent selectors. Bounded by kChoicesMax + 2.
inline std::vector<std::string> priceKeepIds(const std::string& choicesCsv,
                                             const std::string& orchSel,
                                             const std::string& subSel) {
  std::vector<std::string> keep;
  keep.reserve(kChoicesMax + 2);
  size_t p = 0;
  while (p < choicesCsv.size() && keep.size() < kChoicesMax) {
    size_t c = choicesCsv.find(',', p);
    if (c == std::string::npos) c = choicesCsv.size();
    if (c > p) keep.push_back(choicesCsv.substr(p, c - p));
    p = c + 1;
  }
  for (const std::string* s : {&orchSel, &subSel})
    if (!s->empty()) keep.push_back(*s);
  return keep;
}

// The ledger rates of `keepIds` from the router's store rows: each kept id priced by
// its own model's row (either spelling), keyed by the id as the device names it, at
// most ModelLedger::kMaxRouterRates rows. A kept id no priced row matches is named in
// `missing` (canonical form): its price stays unset (the next tier prices it), never
// a wrong one.
inline size_t routerRates(const std::vector<nimbus::orch::ModelInfo>& rows,
                          const std::vector<std::string>& keepIds,
                          std::vector<nimbus::orch::ModelRate>& out,
                          std::vector<std::string>* missing = nullptr) {
  out.clear();
  if (missing) missing->clear();
  for (const std::string& id : keepIds) {
    if (out.size() >= nimbus::orch::ModelLedger::kMaxRouterRates) break;
    if (!routable(id)) continue;
    const std::string k = nimbus::orch::sameModelKey(canonical(id));
    const nimbus::orch::ModelInfo* hit = nullptr;
    for (const auto& m : rows)
      if (m.priced && nimbus::orch::sameModelKey(canonical(selectorOf(m))) == k) { hit = &m; break; }
    if (!hit) {
      if (missing && std::find(missing->begin(), missing->end(), canonical(id)) == missing->end())
        missing->push_back(canonical(id));
      continue;
    }
    nimbus::orch::ModelRate r;
    std::strcpy(r.prov, "cumulo");
    nimbus::orch::usageModelKey("cumulo", id.c_str(), "", r.model);
    r.microIn = hit->priceIn;
    r.microOut = hit->priceOut;
    if (!r.model[0] || (!r.microIn && !r.microOut) || detail::mergeKeyed(out, r)) continue;
    out.push_back(r);
  }
  return out.size();
}

}  // namespace router_catalog
}  // namespace agent
