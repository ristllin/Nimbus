#pragma once
// router_catalog - the Cumulo router's model list on the device (CUM-530).
//
// The router's GET /router/models lists every routable model as
// "<upstream>/<model>", ordered size class first, then upstream name, then model.
// The verify harvest used to keep the first 8 "preferred" ids of that list, so a
// router whose large Anthropic rows sort first filled every slot with
// anthropic/claude-opus-* ids (dated twins included): the router default
// openai/gpt-5.6 and every other upstream vanished from the picker, and a save of
// the default was refused. The rule here is the one place that decides which
// router models the device keeps:
//   - the router default first, then the first (flagship) model of each upstream,
//     then the rest in the router's own order;
//   - a dated snapshot whose undated alias is also listed is dropped (an alias,
//     not a second model);
//   - the compact choice list (NVS mch_cumulo, also the [AVAILABLE MODELS] line
//     every turn carries) is capped by count and bytes, and says when it was cut.
// Header-only and Arduino-free so test/test_router_catalog pins it on the host.

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include <algorithm>
#include <cstring>

#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/router_route.h"

namespace agent {
namespace router_catalog {

// The compact choice list: at most this many ids and this many CSV bytes (NVS).
inline constexpr size_t kChoicesMax      = 12;
inline constexpr size_t kChoicesMaxBytes = 480;
// The router's capability catalog file (/data/mcat_cumulo.json, GET /api/models).
// One router aggregates every upstream's list, so it gets about four single-provider
// budgets (nimbus::store::kCatalogMaxBytes is 3800): ~48 models.
inline constexpr size_t kCatalogMaxBytes = 16000;

// "<upstream>/<model>" for any selector; a bare id is the openai upstream (the
// router's default), exactly as the head and the sub-agent resolve it.
inline std::string canonical(const std::string& sel) {
  const nimbus::orch::RouterRoute r = nimbus::orch::resolveRouterRoute(sel);
  return r.model.empty() ? std::string() : r.upstream + "/" + r.model;
}

inline std::string upstreamOf(const std::string& sel) {
  return nimbus::orch::resolveRouterRoute(sel).upstream;
}

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

// ---- the harvest filter (moved here from provider_verify.cpp so it is tested) ----

// Non-chat FAMILIES that can't run the turn contract (embeddings/audio/image/...),
// by id substring - provider-independent. provider_verify's harvest uses the same
// list for every provider.
inline bool neverChatFamily(const std::string& id) {
  static const char* const kNever[] = {"embed", "moderation", "audio", "realtime",
                                       "transcribe", "tts", "whisper", "image",
                                       "dall-e", "ocr", "voxtral", "davinci",
                                       "babbage", "instruct", "search-", "chatgpt",
                                       "research",   // deep-research models can't run chat turns
                                       "fim",        // fill-in-middle = code completion
                                       nullptr};
  for (int i = 0; kNever[i]; ++i)
    if (id.find(kNever[i]) != std::string::npos) return true;
  return false;
}

// A router id the device can run as a chat model. The router lists only ROUTABLE
// (priced + enabled) models, so other upstreams are curated by it; the openai
// upstream still drops dated snapshots and non-chat families by name.
inline bool routerChatModel(const std::string& id) {
  if (id.size() < 3 || neverChatFamily(id)) return false;
  const nimbus::orch::RouterRoute r = nimbus::orch::resolveRouterRoute(id);
  if (r.upstream != "openai") return true;
  const std::string& m = r.model;
  if (m.find("-202") != std::string::npos) return false;   // dated snapshot of an alias
  return m.rfind("gpt-", 0) == 0 || (m.size() > 1 && m[0] == 'o' && m[1] >= '1' && m[1] <= '9');
}

// Each upstream's CURRENT generation first (rows priced only for back-compat
// billing - claude-3-*, gpt-4o - come after).
inline bool routerFlagship(const std::string& id) {
  const nimbus::orch::RouterRoute r = nimbus::orch::resolveRouterRoute(id);
  if (r.upstream == "openai" || r.upstream == "zai")
    return nimbus::orch::isFlagshipFamily(r.upstream, r.model);
  if (r.upstream == "anthropic") return r.model.rfind("claude-3", 0) != 0;
  return true;
}

// Every "id" value in a /router/models body (HTTP headers may precede it; pretty or
// compact JSON), chat models only, flagships first, each group in the router's
// order. An id over 64 chars ends the scan (it is not an id list).
inline std::vector<std::string> harvestIds(const char* body) {
  std::vector<std::string> flag, rest;
  const char* p = body;
  while (p && (p = std::strstr(p, "\"id\":")) != nullptr) {
    p += 5;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '"') continue;
    const char* e = std::strchr(++p, '"');
    if (!e || e - p > 64) break;
    const std::string id(p, (size_t)(e - p));
    p = e;
    if (routerChatModel(id)) (routerFlagship(id) ? flag : rest).push_back(id);
  }
  flag.insert(flag.end(), rest.begin(), rest.end());
  return flag;
}

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
inline bool newerFirst(const std::string& a, const std::string& b) {
  const VersionKey ka = versionKey(nimbus::orch::resolveRouterRoute(a).model);
  const VersionKey kb = versionKey(nimbus::orch::resolveRouterRoute(b).model);
  if (ka.v != kb.v) return ka.v > kb.v;
  return !ka.variant && kb.variant;
}

inline bool contains(const std::vector<std::string>& v, const std::string& x) {
  for (const std::string& e : v)
    if (e == x) return true;
  return false;
}

// Distinct, non-twin candidates (indexes into canon) in list order.
inline std::vector<size_t> candidates(const std::vector<std::string>& canon) {
  std::vector<size_t> cand;
  std::vector<std::string> kept;
  for (size_t i = 0; i < canon.size(); ++i) {
    if (canon[i].empty() || contains(kept, canon[i])) continue;
    const std::string alias = undatedAlias(canon[i]);
    if (!alias.empty() && contains(canon, alias)) continue;   // a dated twin of a listed alias
    kept.push_back(canon[i]);
    cand.push_back(i);
  }
  return cand;
}

// Candidates grouped per upstream, upstreams in first-appearance order.
struct Queues {
  std::vector<std::string> ups;
  std::vector<std::vector<size_t>> q;
};
inline Queues byUpstream(const std::vector<std::string>& canon, const std::vector<size_t>& cand,
                         const std::vector<size_t>& skip) {
  Queues qs;
  for (size_t i : cand) {
    bool skipped = false;
    for (size_t k : skip) skipped = skipped || k == i;
    if (skipped) continue;
    const std::string up = upstreamOf(canon[i]);
    size_t q = 0;
    while (q < qs.ups.size() && qs.ups[q] != up) ++q;
    if (q == qs.ups.size()) {
      qs.ups.push_back(up);
      qs.q.emplace_back();
    }
    qs.q[q].push_back(i);
  }
  // The router lists each upstream alphabetically within a size class (oldest
  // generation first): order each upstream newest-first so a cut list keeps
  // claude-opus-5 / gpt-6-astra / glm-5.3, not the 4.x / 5.5 / glm-5 rows.
  for (auto& qv : qs.q)
    std::stable_sort(qv.begin(), qv.end(),
                     [&](size_t a, size_t b) { return newerFirst(canon[a], canon[b]); });
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
// order to prefer them): indexes into `ids`, deduped, dated twins dropped. The
// router default first, then round-robin across upstreams in their order of first
// appearance, each upstream newest-first - so any cap keeps the default and each
// upstream's newest model, and a cut list stays a spread of upstreams rather than
// one upstream's whole family.
inline std::vector<size_t> keepOrder(const std::vector<std::string>& ids,
                                     const std::string& defaultSel) {
  std::vector<std::string> canon;
  canon.reserve(ids.size());
  for (const std::string& id : ids) canon.push_back(canonical(id));
  const std::vector<size_t> cand = detail::candidates(canon);
  std::vector<size_t> order;
  const std::string def = canonical(defaultSel);
  for (size_t i : cand)
    if (canon[i] == def) { order.push_back(i); break; }
  const std::string defUp = order.empty() ? std::string() : upstreamOf(def);
  detail::roundRobin(detail::byUpstream(canon, cand, order), defUp, order);
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
  const std::vector<size_t> order = keepOrder(ids, defaultSel);
  out.total = order.size();
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

// The router selector of a parsed catalog entry. parseModelsList keeps the bare
// model in `id` and the upstream in `upstream` (classified against its upstream),
// so the selector a pick must send is "<upstream>/<id>".
inline std::string selectorOf(const nimbus::orch::ModelInfo& m) {
  return m.upstream.empty() ? m.id : m.upstream + "/" + m.id;
}

// Reorder a parsed router catalog in place to keepOrder() (dropping dated twins
// and duplicates), so a size trim from the back keeps the router default and one
// model per upstream.
inline void protectHeads(std::vector<nimbus::orch::ModelInfo>& cat, const std::string& defaultSel) {
  std::vector<std::string> ids;
  ids.reserve(cat.size());
  for (const auto& m : cat) ids.push_back(selectorOf(m));
  const std::vector<size_t> order = keepOrder(ids, defaultSel);
  std::vector<nimbus::orch::ModelInfo> out;
  out.reserve(order.size());
  for (size_t i : order) out.push_back(cat[i]);
  cat.swap(out);
}

}  // namespace router_catalog
}  // namespace agent
