#pragma once
// router_catalog - the Cumulo router's model list, on the device and on a hosted
// Virtual Nimbus (CUM-530).
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
// Header-only and Arduino-free so test/test_router_catalog pins it on the host, and
// nimbusd (which builds from lib/ alone) bounds its catalog by the same rule.

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include <algorithm>
#include <cstring>

#include "nimbus/harness/log.h"   // hlog - one line when the harvest bound is hit
#include "nimbus/orch/model_catalog.h"
#include "nimbus/orch/model_key.h"      // usageModelKey - the price row's key
#include "nimbus/orch/model_ledger.h"   // ModelRate, kMaxRouterRates
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
// The most chat ids one harvest keeps from a /router/models body. The body is
// untrusted and only byte-bounded (64 KB): ~6500 tiny ids would otherwise each
// cost a string and a sort slot inside the verify slot. Far above any real router
// list, and ten times the choice list.
inline constexpr size_t kHarvestMax = 128;
// The most rows the router catalog keeps from the same body (protectHeads), so the
// store step is bounded: the size trim that follows re-serializes once per
// dropped row. (The parse and keepOrder before it are O(n log n) over the body,
// and keepOrder groups at most 4 + kOtherUpstreamsMax upstreams.)
inline constexpr size_t kCatalogMaxRows = kHarvestMax;

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
  if (id.size() < 3 || neverChatFamily(id) || !routable(id)) return false;
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
// order, at most kHarvestMax ids (flagships fill first, wherever they appear; one
// log line says when the bound cut the list). An id over 64 chars ends the scan
// (it is not an id list).
inline std::vector<std::string> harvestIds(const char* body) {
  std::vector<std::string> flag, rest;
  size_t found = 0;
  const char* p = body;
  while (p && (p = std::strstr(p, "\"id\":")) != nullptr) {
    p += 5;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '"') continue;
    const char* e = std::strchr(++p, '"');
    if (!e || e - p > 64) break;
    const std::string id(p, (size_t)(e - p));
    p = e;
    if (!routerChatModel(id)) continue;
    ++found;
    std::vector<std::string>& group = routerFlagship(id) ? flag : rest;
    if (group.size() < kHarvestMax) group.push_back(id);
  }
  const size_t room = kHarvestMax - flag.size();
  const size_t take = std::min(room, rest.size());
  flag.insert(flag.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(take));
  if (found > flag.size())
    agent::hlog::logf("router_catalog: harvest keeps %u of %u router chat ids (bound %u, "
                      "flagships first)",
                      (unsigned)flag.size(), (unsigned)found, (unsigned)kHarvestMax);
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
inline std::string undatedModel(const std::string& sel) {
  const std::string m = nimbus::orch::resolveRouterRoute(sel).model;
  const std::string alias = undatedAlias(m);
  return alias.empty() ? m : alias;
}
inline bool newerFirst(const VersionKey& ka, const VersionKey& kb) {
  if (ka.v != kb.v) return ka.v > kb.v;
  return !ka.variant && kb.variant;
}

// Distinct, non-twin candidates (indexes into canon) in list order. O(n log n):
// indexes sorted by (id, position) put each id's first occurrence at the head of
// its run, and "is the alias listed" is a binary search - no pairwise scan, no
// copied strings (the list comes from an untrusted router body).
inline std::vector<size_t> candidates(const std::vector<std::string>& canon) {
  std::vector<size_t> byId(canon.size());
  for (size_t i = 0; i < byId.size(); ++i) byId[i] = i;
  std::sort(byId.begin(), byId.end(), [&](size_t a, size_t b) {
    const int c = canon[a].compare(canon[b]);
    return c != 0 ? c < 0 : a < b;
  });
  std::vector<bool> first(canon.size(), false);
  for (size_t k = 0; k < byId.size(); ++k)
    first[byId[k]] = k == 0 || canon[byId[k]] != canon[byId[k - 1]];
  auto listed = [&](const std::string& id) {
    const auto it = std::lower_bound(byId.begin(), byId.end(), id,
                                     [&](size_t i, const std::string& v) { return canon[i] < v; });
    return it != byId.end() && canon[*it] == id;
  };
  std::vector<size_t> cand;
  for (size_t i = 0; i < canon.size(); ++i) {
    if (canon[i].empty() || !first[i]) continue;
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
  std::vector<VersionKey> keys(canon.size());
  for (size_t i : cand) keys[i] = versionKey(undatedModel(canon[i]));
  for (auto& qv : qs.q)
    std::stable_sort(qv.begin(), qv.end(),
                     [&](size_t a, size_t b) { return newerFirst(keys[a], keys[b]); });
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
  for (size_t i : cand)
    if (canon[i] == def) { order.push_back(i); break; }
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

// ---- router prices (per-model prices, 2026-10) -------------------------------------
// The router's /router/models rows carry what Cumulo bills per model
// (in_micro_per_mtok / out_micro_per_mtok, micro-USD per 1M tokens). The harvest
// keeps ONLY the device's own models - the choice list plus the configured head and
// sub-agent selectors - at most kMaxRouterRates rows, reading the body in place
// (the verify task's PSRAM buffer): no copy of the body, one short string per id.

namespace detail {
// The unsigned integer after "<field>": inside [from, to), saturating at u32 max;
// 0 when the field is absent or not a number.
inline uint32_t priceField(const char* from, const char* to, const char* field) {
  const size_t fl = std::strlen(field);
  for (const char* p = from; p + fl <= to; ++p) {
    if (std::memcmp(p, field, fl) != 0) continue;
    p += fl;
    while (p < to && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    uint64_t v = 0;
    bool any = false;
    while (p < to && *p >= '0' && *p <= '9') {
      any = true;
      v = v * 10 + (uint64_t)(*p - '0');
      if (v > UINT32_MAX) return UINT32_MAX;
      ++p;
    }
    return any ? (uint32_t)v : 0;
  }
  return 0;
}

// The next "id" string at or after `p`: `idKey` is where its key starts, `p` leaves
// on its closing quote. False at the end of the body, or when an id over 64 chars
// says this is not an id list.
inline bool nextId(const char*& p, const char*& idKey, std::string& id) {
  while ((p = std::strstr(p, "\"id\":")) != nullptr) {
    idKey = p;
    p += 5;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '"') continue;
    const char* e = std::strchr(++p, '"');
    if (!e || e - p > 64) return false;
    id.assign(p, (size_t)(e - p));
    p = e;
    return true;
  }
  return false;
}

// The price row of the router object around one id: from its '{' to its '}' (router
// rows are flat objects). False when the object is unterminated.
inline bool rowPrice(const char* body, const char* idKey, const char* idEnd, const std::string& id,
                     nimbus::orch::ModelRate& r) {
  const char* lb = idKey;
  while (lb > body && *lb != '{') --lb;
  const char* rb = std::strchr(idEnd, '}');
  if (!rb) return false;
  r = nimbus::orch::ModelRate{};
  std::strcpy(r.prov, "cumulo");
  nimbus::orch::usageModelKey("cumulo", id.c_str(), "", r.model);
  r.microIn = priceField(lb, rb, "\"in_micro_per_mtok\":");
  r.microOut = priceField(lb, rb, "\"out_micro_per_mtok\":");
  return true;
}

// A second router row for a key already kept (two ids can share one key, e.g. an
// alias and its 4-digit build) raises the kept price to the higher one, per field
// (fail-closed: spend on that key is never priced at the cheaper id's rate).
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

// The ids whose router prices the device keeps: the compact choice list (CSV) plus
// the configured head and sub-agent selectors. Bounded by kChoicesMax + 2.
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

inline size_t harvestPrices(const char* body, const std::vector<std::string>& keepIds,
                            std::vector<nimbus::orch::ModelRate>& out) {
  out.clear();
  if (!body) return 0;
  out.reserve(nimbus::orch::ModelLedger::kMaxRouterRates);
  std::vector<std::string> keep;   // canonical "<upstream>/<model>", bounded by the caller
  keep.reserve(keepIds.size());
  for (const std::string& k : keepIds)
    if (routable(k)) keep.push_back(canonical(k));
  const char* p = body;
  const char* idKey = nullptr;
  std::string id;
  // Each kept id appears once in a real body; the bound caps the row scans a
  // malformed body (one id repeated without prices) could cost the verify task.
  size_t scans = 0;
  const size_t maxScans = 4 * nimbus::orch::ModelLedger::kMaxRouterRates;
  while (out.size() < nimbus::orch::ModelLedger::kMaxRouterRates && scans < maxScans &&
         detail::nextId(p, idKey, id)) {
    if (!routable(id) || std::find(keep.begin(), keep.end(), canonical(id)) == keep.end()) continue;
    ++scans;
    nimbus::orch::ModelRate r;
    if (!detail::rowPrice(body, idKey, p, id, r)) break;
    if (!r.model[0] || (!r.microIn && !r.microOut) || detail::mergeKeyed(out, r)) continue;
    out.push_back(r);
  }
  return out.size();
}

// The router selector of a parsed catalog entry. parseModelsList keeps the bare
// model in `id` and the upstream in `upstream` (classified against its upstream),
// so the selector a pick must send is "<upstream>/<id>".
inline std::string selectorOf(const nimbus::orch::ModelInfo& m) {
  return m.upstream.empty() ? m.id : m.upstream + "/" + m.id;
}

// Reorder a parsed router catalog in place to keepOrder() (dropping dated twins
// and duplicates) and bound it to kCatalogMaxRows (CUM-530: the body is untrusted,
// and thousands of tiny ids used to reach the store step whole). The router
// default and each upstream's head come first, then every upstream's current
// generation (routerFlagship), then the rest, so neither a row cap nor the size
// trim from the back costs the default, an upstream or a flagship listed late.
// Returns how many rows the cap cut or the upstream bound left out (the stored
// list says "<n> more").
inline size_t protectHeads(std::vector<nimbus::orch::ModelInfo>& cat, const std::string& defaultSel) {
  std::vector<std::string> ids;
  ids.reserve(cat.size());
  for (const auto& m : cat) ids.push_back(selectorOf(m));
  size_t dropped = 0;
  std::vector<size_t> order = keepOrder(ids, defaultSel, &dropped);
  std::vector<std::string> ups;   // the heads: the first row of each upstream (bounded scan)
  size_t heads = 0;
  while (heads < order.size() && heads < kCatalogMaxRows) {
    const std::string up = upstreamOf(ids[order[heads]]);
    if (std::find(ups.begin(), ups.end(), up) != ups.end()) break;
    ups.push_back(up);
    ++heads;
  }
  std::stable_partition(order.begin() + static_cast<std::ptrdiff_t>(heads), order.end(),
                        [&](size_t i) { return routerFlagship(ids[i]); });
  const size_t cut = order.size() > kCatalogMaxRows ? order.size() - kCatalogMaxRows : 0;
  order.resize(order.size() - cut);
  std::vector<nimbus::orch::ModelInfo> out;
  out.reserve(order.size());
  for (size_t i : order) out.push_back(std::move(cat[i]));
  cat.swap(out);
  return cut + dropped;
}

}  // namespace router_catalog
}  // namespace agent
