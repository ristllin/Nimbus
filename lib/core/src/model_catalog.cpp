#include "nimbus/orch/model_catalog.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "nimbus/orch/compact.h"       // modelCtxTokens - the family context-window table
#include "nimbus/orch/router_route.h"  // resolveRouterRoute - a router pick's upstream model

namespace nimbus {
namespace orch {

namespace {

std::string lower(const std::string& s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return o;
}
bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Coarse non-chat family detection by id substring (provider-independent). These
// families cannot run the agentic turn contract; each maps to its own role.
enum class Kind { Chat, Embedding, Stt, Tts, Realtime, Image, NonAgentic, Unknown };

Kind kindFromId(const std::string& id) {
  const std::string m = lower(id);
  // First substring match wins, so image/audio/embedding families are caught
  // before the chat default. Non-agentic text families (instruct/fim/ocr/research)
  // cannot run the tool contract, so they map to a role-less kind (dropped).
  struct Pat {
    const char* s;
    Kind k;
  };
  static const Pat kPats[] = {
      {"embed", Kind::Embedding},   {"rerank", Kind::Embedding},
      {"realtime", Kind::Realtime}, {"whisper", Kind::Stt},
      {"transcribe", Kind::Stt},    {"-stt", Kind::Stt},
      {"scribe", Kind::Stt},        {"voxtral", Kind::Stt},
      {"tts", Kind::Tts},           {"-speech", Kind::Tts},
      {"dall-e", Kind::Image},      {"-image", Kind::Image},
      {"image-", Kind::Image},      {"imagen", Kind::Image},
      {"instruct", Kind::NonAgentic}, {"-fim", Kind::NonAgentic},
      {"davinci", Kind::NonAgentic},  {"babbage", Kind::NonAgentic},
      {"moderation", Kind::NonAgentic}, {"-ocr", Kind::NonAgentic},
      {"ocr-", Kind::NonAgentic},    {"research", Kind::NonAgentic},
      {"search-", Kind::NonAgentic},
  };
  for (const Pat& p : kPats)
    if (has(m, p.s)) return p.k;
  return Kind::Chat;
}

// Multimodal (image input) heuristic when the API does not tell us.
bool visionHeuristic(const std::string& provider, const std::string& id) {
  const std::string m = lower(id);
  if (provider == "anthropic") return startsWith(m, "claude-");  // all modern Claude take images
  if (provider == "openai")
    return has(m, "gpt-4o") || gptGeneration(m) >= 5 || has(m, "-vision") || has(m, "omni");
  if (provider == "mistral") return has(m, "pixtral") || has(m, "medium") || has(m, "large");
  if (provider == "zai") return has(m, "-v") || has(m, "vision") || has(m, "glm-4.5v");
  return has(m, "vision") || has(m, "-v-") || has(m, "multimodal");
}

}  // namespace

namespace {
bool isClaude(const std::string& provider, const std::string& m) {
  return provider == "anthropic" || startsWith(m, "claude-");
}
bool isOSeries(const std::string& m) {
  return m.size() >= 2 && m[0] == 'o' && m[1] >= '1' && m[1] <= '9';
}
std::string claudeFamily(const std::string& m) {
  if (has(m, "opus")) return "claude-opus";
  if (has(m, "sonnet")) return "claude-sonnet";
  if (has(m, "haiku")) return "claude-haiku";
  return "claude";
}
// Size word ('S'|'M'|'L') from a generic tier name, 0 if none.
char sizeFromWord(const std::string& m) {
  // OpenAI's gpt-5.6+ capability tiers (2026-07): Sol = flagship, Terra =
  // balanced, Luna = fastest/cheapest. Durable tier names per the announcement,
  // so they rank with the generic size words (hyphenated to avoid substrings).
  static const char* kSmall[] = {"nano", "tiny", "-air", "flash", "lite", "small", "mini",
                                 "-luna", nullptr};
  for (int i = 0; kSmall[i]; ++i)
    if (has(m, kSmall[i])) return 'S';
  if (has(m, "medium") || has(m, "turbo") || has(m, "-terra")) return 'M';
  static const char* kLarge[] = {"large", "opus", "-max", "-sol", nullptr};
  for (int i = 0; kLarge[i]; ++i)
    if (has(m, kLarge[i])) return 'L';
  return 0;
}
// Flagship id-family default size when no tier word is present.
char sizeFromFlagship(const std::string& m) {
  if (gptGeneration(m) >= 5 || startsWith(m, "glm-5") || startsWith(m, "glm-4")) return 'L';
  if (isOSeries(m)) return 'L';
  return 0;
}
// Leading decimal run of `s` as a bounded int: "6-astra" -> 6, "5.5" -> 5,
// "realtime" -> 0. Capped at three digits so a date-like run ("2024...") never
// reads as a generation.
int leadingInt(const char* s) {
  int v = 0, digits = 0;
  for (; *s >= '0' && *s <= '9'; ++s, ++digits) {
    if (digits == 3) return 0;
    v = v * 10 + (*s - '0');
  }
  return v;
}
// Major version after a "glm-" prefix (glm-4.5-air -> 4, glm-5 -> 5); 0 otherwise.
int glmGeneration(const std::string& m) {
  return startsWith(m, "glm-") ? leadingInt(m.c_str() + 4) : 0;
}
}  // namespace

int gptGeneration(const std::string& id) {
  const std::string m = lower(id);
  // gpt-realtime / gpt-audio / gpt-image carry no generation digit -> 0.
  return startsWith(m, "gpt-") ? leadingInt(m.c_str() + 4) : 0;
}

bool isFlagshipFamily(const std::string& provider, const std::string& id) {
  const std::string m = lower(id);
  if (provider == "openai") return gptGeneration(m) >= 5;
  if (provider == "zai") return glmGeneration(m) >= 5;
  return false;
}

std::string modelFamily(const std::string& provider, const std::string& id) {
  // A Cumulo router pick ("openai/gpt-5.6", "anthropic/claude-opus-5") is its
  // upstream's model (CUM-530).
  if (provider == "cumulo") {
    const RouterRoute r = resolveRouterRoute(id);
    if (r.upstream != "cumulo") return modelFamily(r.upstream, r.model);
  }
  const std::string m = lower(id);
  if (isClaude(provider, m)) return claudeFamily(m);
  if (isOSeries(m)) return "o-series";
  // gpt-4 and newer bucket by generation ("gpt-4", "gpt-5", "gpt-6", ...); the
  // pre-4 chat ids (gpt-3.5-turbo) stay unbucketed as before.
  const int gen = gptGeneration(m);
  if (gen >= 4) return "gpt-" + std::to_string(gen);
  struct Fam {
    const char* s;
    const char* f;
    bool prefix;
  };
  static const Fam kFams[] = {
      {"magistral", "magistral", false}, {"mistral", "mistral", false},
      {"pixtral", "pixtral", false},     {"glm-", "glm", true},
      {"embed", "embedding", false},
  };
  for (const Fam& f : kFams)
    if (f.prefix ? startsWith(m, f.s) : has(m, f.s)) return f.f;
  return "";
}

char modelSizeClass(const std::string& provider, const std::string& id) {
  // A Cumulo router pick is sized as its upstream's model (CUM-530): the fallback
  // rules' size class used to read unknown for "anthropic/claude-opus-5".
  if (provider == "cumulo") {
    const RouterRoute r = resolveRouterRoute(id);
    if (r.upstream != "cumulo") return modelSizeClass(r.upstream, r.model);
  }
  const std::string m = lower(id);
  if (isClaude(provider, m)) {
    if (has(m, "opus")) return 'L';
    if (has(m, "sonnet")) return 'M';
    if (has(m, "haiku")) return 'S';
  }
  char w = sizeFromWord(m);
  if (w) return w;
  char f = sizeFromFlagship(m);
  if (f) return f;
  if (provider == "mistral" && has(m, "magistral")) return 'M';
  return 0;
}

ModelInfo classifyModel(const std::string& provider, const std::string& id) {
  ModelInfo mi;
  mi.id = id;
  mi.family = modelFamily(provider, id);
  mi.size = modelSizeClass(provider, id);
  mi.ctxTokens = modelCtxTokens(provider, id);
  const Kind k = kindFromId(id);
  switch (k) {
    case Kind::Embedding:
      mi.roles = RoleEmbedding;
      mi.caps = CapEmbedding;
      break;
    case Kind::Stt:
      mi.roles = RoleStt;
      mi.caps = CapAudioIn;
      break;
    case Kind::Tts:
      mi.roles = RoleTts;
      mi.caps = CapAudioOut;
      break;
    case Kind::Realtime:
      mi.roles = RoleStt | RoleTts;
      mi.caps = CapAudioIn | CapAudioOut;
      break;
    case Kind::Image:
      mi.roles = RoleImage;
      break;
    case Kind::Chat: {
      mi.roles = RoleOrchestrator | RoleSubAgent;
      mi.caps = CapTools | CapJson | CapStreaming;
      if (visionHeuristic(provider, id)) {
        mi.roles |= RoleVision;
        mi.caps |= CapVision;
      }
      break;
    }
    case Kind::NonAgentic:
    case Kind::Unknown:
    default:
      break;  // roles stay 0 - not surfaced under any role
  }
  return mi;
}

namespace {

// ---- per-provider API-field overlays (apiCaps=true when they fire) -----------
void overlayAnthropic(JsonObjectConst m, ModelInfo& mi) {
  uint32_t maxIn = m["max_input_tokens"] | 0u;
  uint32_t maxOut = m["max_tokens"] | 0u;
  if (maxIn) mi.ctxTokens = maxIn;
  if (maxOut) mi.maxOutTokens = maxOut;
  JsonObjectConst cap = m["capabilities"];
  if (!cap.isNull()) {
    mi.apiCaps = true;
    if (cap["image_input"]["supported"] | false) {
      mi.roles |= RoleVision;
      mi.caps |= CapVision;
    }
    if (cap["structured_outputs"]["supported"] | false) mi.caps |= CapJson;
  }
}

void overlayMistral(JsonObjectConst m, ModelInfo& mi, bool& keep) {
  JsonObjectConst cap = m["capabilities"];
  uint32_t ctx = m["max_context_length"] | 0u;
  if (ctx) mi.ctxTokens = ctx;
  if (!m["deprecation"].isNull()) mi.deprecated = true;
  if (cap.isNull()) {
    keep = mi.roles != 0 && !mi.deprecated;
    return;
  }
  mi.apiCaps = true;
  // Mistral's per-model capabilities are ground truth - rebuild the chat/vision
  // roles from them so a vision-capable-but-not-chat model (e.g. OCR) never leaks
  // in as an orchestrator, and a chat model the id heuristic missed is promoted.
  const bool chatModel = (cap["completion_chat"] | false) && (cap["function_calling"] | false);
  if (chatModel) {
    mi.roles |= RoleOrchestrator | RoleSubAgent;
    mi.caps |= CapTools | CapJson;
    if (cap["vision"] | false) {
      mi.roles |= RoleVision;
      mi.caps |= CapVision;
    } else {
      mi.roles &= ~RoleVision;
      mi.caps &= ~CapVision;
    }
  } else {
    mi.roles &= ~(RoleOrchestrator | RoleSubAgent | RoleVision);
    mi.caps &= ~(CapTools | CapJson | CapVision);
  }
  if (cap["audio_transcription"] | false) {
    mi.roles |= RoleStt;
    mi.caps |= CapAudioIn;
  }
  if (cap["audio_speech"] | false) {
    mi.roles |= RoleTts;
    mi.caps |= CapAudioOut;
  }
  keep = mi.roles != 0 && !mi.deprecated;
}

// Cumulo GET /router/models metadata: the router's own size class (the cloud's
// shared name-tier predicate) overrides the id heuristic - API-supplied beats
// guessed, same as the Anthropic/Mistral overlays.
void overlayCumulo(JsonObjectConst m, ModelInfo& mi) {
  const char sc = ((const char*)(m["size_class"] | ""))[0];
  if (sc != 's' && sc != 'm' && sc != 'l') return;
  mi.size = sc == 's' ? 'S' : (sc == 'm' ? 'M' : 'L');
  mi.apiCaps = true;
}

// Mistral: prefer the canonical "-latest" alias id when the API lists one.
std::string mistralCanonicalId(JsonObjectConst m) {
  std::string id = (const char*)(m["id"] | "");
  if (id.size() > 7 && id.rfind("-latest") == id.size() - 7) return id;  // already canonical
  for (JsonVariantConst a : m["aliases"].as<JsonArrayConst>()) {
    std::string al = (const char*)(a | "");
    if (al.size() > 7 && al.rfind("-latest") == al.size() - 7) return al;
  }
  return id;
}

// Classify one harvested id. For Cumulo the id is "<upstream>/<model>": classify
// against the UPSTREAM so its heuristics (vision/size/family) fire, then tag it.
ModelInfo classifyEntry(const std::string& provider, const std::string& id) {
  if (provider != "cumulo") return classifyModel(provider, id);
  const size_t slash = id.find('/');
  if (slash == std::string::npos || slash == 0) return classifyModel("openai", id);
  const std::string up = id.substr(0, slash);
  ModelInfo mi = classifyModel(up, id.substr(slash + 1));
  mi.upstream = up;
  return mi;
}

// Drop each row from `from` on whose model an earlier row already has (the first
// stays, order kept), with one index sort: O(n log n) however many rows an
// untrusted body lists (CUM-530: the pairwise scan was quadratic, and a router
// body of thousands of tiny ids pinned the verify slot). A router row's model is
// its upstream AND its id: "openai/foo" and "zai/foo" are two models.
void dropDuplicateIds(std::vector<ModelInfo>& out, size_t from) {
  std::vector<size_t> idx(out.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  auto cmp = [&](size_t a, size_t b) {   // one rule for the sort and the duplicate test
    const int c = out[a].upstream.compare(out[b].upstream);
    return c != 0 ? c : out[a].id.compare(out[b].id);
  };
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    const int c = cmp(a, b);
    return c != 0 ? c < 0 : a < b;
  });
  std::vector<bool> dup(out.size(), false);
  for (size_t k = 1; k < idx.size(); ++k)
    dup[idx[k]] = cmp(idx[k], idx[k - 1]) == 0;
  size_t w = from;
  for (size_t i = from; i < out.size(); ++i) {
    if (dup[i]) continue;
    if (w != i) out[w] = std::move(out[i]);
    ++w;
  }
  out.resize(w);
}

// Stable flagship-first ordering: L before M before S before unclassified, input
// order preserved within a rank.
void sortFlagshipFirst(std::vector<ModelInfo>& v) {
  auto rank = [](char s) -> int {
    switch (s) {
      case 'L': return 0;
      case 'M': return 1;
      case 'S': return 2;
      default: return 3;
    }
  };
  std::stable_sort(v.begin(), v.end(),
                   [&](const ModelInfo& a, const ModelInfo& b) { return rank(a.size) < rank(b.size); });
}

}  // namespace

ModelInfo classifyCatalogEntry(const std::string& provider, const std::string& id) {
  return classifyEntry(provider, id);
}

// ---- one fetch's rows (lane F) ---------------------------------------------------
namespace {

// The provider's own words about a model: name, line, dates and lifecycle. Untrusted
// text, so every string is cleaned and bounded; a field the provider does not send
// stays empty.
// The lifecycle word normalized: "retired", "deprecated" (a legacy model counts), or
// "active" for any other word the provider sends; "" when it sends none.
std::string lifecycleWord(const char* raw) {
  const std::string life = cleanModelText(raw ? raw : "", kModelLineMax);   // a short word
  if (life.empty()) return life;
  if (life.find("retir") != std::string::npos) return "retired";
  if (life.find("deprec") != std::string::npos || life.find("legacy") != std::string::npos)
    return "deprecated";
  return "active";
}

// The announced end: Anthropic retires_at, OpenAI shutdown_date, Mistral deprecation.
uint32_t announcedEnd(JsonObjectConst m) {
  for (const char* key : {"retires_at", "shutdown_date", "deprecation"})
    if (const char* end = m[key] | (const char*)nullptr) return parseIsoTime(end);
  return 0;
}

uint32_t createdAt(JsonObjectConst m) {
  if (m["created"].is<uint32_t>()) return m["created"].as<uint32_t>();
  const char* ca = m["created_at"] | (const char*)nullptr;
  return ca ? parseIsoTime(ca) : 0;
}

void overlayMeta(const std::string& provider, JsonObjectConst m, ModelInfo& mi) {
  const char* nm = m["display_name"] | (const char*)nullptr;
  if (!nm && provider == "mistral") nm = m["name"] | (const char*)nullptr;
  if (nm && mi.id != nm) mi.name = cleanModelText(nm, kModelNameMax);
  if (const char* ln = m["line"] | (const char*)nullptr) mi.line = cleanModelText(ln, kModelLineMax);
  mi.created = createdAt(m);
  mi.life = lifecycleWord(m["lifecycle"] | (const char*)nullptr);
  mi.retires = announcedEnd(m);
  if (!m["deprecated_at"].isNull() || mi.life == "deprecated" || mi.life == "retired")
    mi.deprecated = true;
}

// The Cumulo router bills each row at its own price (micro-USD per 1M tokens).
void overlayRouterPrice(JsonObjectConst m, ModelInfo& mi) {
  const uint32_t in = m["in_micro_per_mtok"] | 0u;
  const uint32_t out = m["out_micro_per_mtok"] | 0u;
  if (!in && !out) return;
  mi.priced = true;
  mi.priceIn = in;
  mi.priceOut = out;
}

}  // namespace

namespace {

// The row's id as the device keeps it (Mistral: its canonical -latest alias), or ""
// when the device will not keep one (counted as unsafe).
std::string rowId(const std::string& provider, JsonObjectConst m) {
  const std::string id =
      (provider == "mistral") ? mistralCanonicalId(m) : std::string((const char*)(m["id"] | ""));
  return (id.size() >= 2 && cleanModelId(id)) ? id : std::string();
}

// The per-provider API fields; false when the provider's own metadata drops the row
// (Mistral marks it deprecated, or it cannot chat).
bool overlayProvider(const std::string& provider, JsonObjectConst m, ModelInfo& mi) {
  bool keep = true;
  if (provider == "anthropic") {
    overlayAnthropic(m, mi);
  } else if (provider == "mistral") {
    overlayMistral(m, mi, keep);  // Mistral drops models the API marks deprecated
  } else if (provider == "cumulo") {
    overlayCumulo(m, mi);
    overlayRouterPrice(m, mi);
  } else if (!m["shutdown_date"].isNull() || !m["deprecation"].isNull()) {
    // A future shutdown date still leaves the model usable: flag it, keep it,
    // and let the usability probe / UI decide. (Mistral is stricter above.)
    mi.deprecated = true;
  }
  return keep && mi.roles != 0;  // role-less (non-agentic) models never surface
}

// Past the collect bound only a router chat row of a current generation of one of the
// four upstreams the device runs is taken.
bool admittedPastBound(const std::string& provider, const ModelInfo& mi, const std::string& id) {
  const bool knownUp = mi.upstream == "openai" || mi.upstream == "anthropic" ||
                       mi.upstream == "mistral" || mi.upstream == "zai";
  return provider == "cumulo" && knownUp && mi.hasRole(RoleOrchestrator) && !neverChatFamily(mi.id) &&
         routerFlagshipSelector(id);
}

}  // namespace

void harvestRow(ModelHarvest& h, const char* obj, size_t len) {
  ++h.seen;
  if (h.rows.size() >= 2 * kHarvestCollectMax) {
    ++h.overflow;
    return;
  }
  // A null allocator would make ArduinoJson dereference nullptr on the first
  // allocation; fall back to its default allocator (host tests pass nullptr).
  JsonDocument doc = h.alloc ? JsonDocument(h.alloc) : JsonDocument();
  const bool parsed = !deserializeJson(doc, obj, len, DeserializationOption::NestingLimit(16)) &&
                      doc.is<JsonObjectConst>();
  const std::string id = parsed ? rowId(h.provider, doc.as<JsonObjectConst>()) : std::string();
  if (id.empty()) {
    ++h.hostile;
    return;
  }
  if (h.provider == "cumulo" && !routerRowKept(id)) {   // never kept: not held in RAM either
    ++h.notRun;
    return;
  }
  JsonObjectConst m = doc.as<JsonObjectConst>();
  ModelInfo mi = classifyEntry(h.provider, id);
  const bool keep = overlayProvider(h.provider, m, mi);
  overlayMeta(h.provider, m, mi);
  // Past its announced end (when the clock says so) or retired: never kept.
  if (mi.life == "retired" || (h.now && mi.retires && mi.retires <= h.now)) {
    ++h.retired;
    return;
  }
  if (!keep) return;
  if (h.rows.size() >= kHarvestCollectMax && !admittedPastBound(h.provider, mi, id)) {
    ++h.overflow;
    return;
  }
  keepOffInternalHeap(mi);   // the row's strings: PSRAM, never internal SRAM, per row
  h.rows.push_back(std::move(mi));
}

namespace {
void harvestRowCb(void* ctx, const char* obj, size_t len) {
  harvestRow(*static_cast<ModelHarvest*>(ctx), obj, len);
}
}  // namespace

bool harvestBody(ModelHarvest& h, const char* body, size_t len, std::string* lastId) {
  ModelsBodyReader rd(&harvestRowCb, &h);
  rd.feed(body, len);
  if (!rd.complete()) h.cut = true;
  h.oversize += rd.oversize();
  if (lastId) *lastId = rd.lastId();
  return rd.hasMore();
}

void finishHarvest(ModelHarvest& h) {
  dropDuplicateIds(h.rows, 0);
  sortFlagshipFirst(h.rows);
}

size_t parseModelsList(const std::string& provider, const std::string& body,
                       std::vector<ModelInfo>& out, ArduinoJson::Allocator* alloc) {
  // The streamed reader (orch_model_store.cpp) over the whole body, one row at a time:
  // the same per-row rules a fetch applies, so a parse here and a fetch on the
  // device can never disagree. No clock: no retire date drops a row.
  ModelHarvest h;
  h.provider = provider;
  h.alloc = alloc;
  harvestBody(h, body.data(), body.size());
  finishHarvest(h);
  const size_t n = h.rows.size();
  for (ModelInfo& m : h.rows) out.push_back(std::move(m));
  return n;
}

// ---- role/cap tokens ---------------------------------------------------------
namespace {
const char* const kRoleTokens[] = {"orchestrator", "sub-agent", "embedding",
                                   "vision",       "stt",       "tts",
                                   "image"};
struct RoleBit {
  ModelRole r;
  const char* t;
};
const RoleBit kRoleBits[] = {{RoleOrchestrator, "orchestrator"}, {RoleSubAgent, "sub-agent"},
                             {RoleEmbedding, "embedding"},       {RoleVision, "vision"},
                             {RoleStt, "stt"},                   {RoleTts, "tts"},
                             {RoleImage, "image"}};
}  // namespace

const char* const* roleTokens(int& countOut) {
  countOut = (int)(sizeof(kRoleTokens) / sizeof(kRoleTokens[0]));
  return kRoleTokens;
}
const char* roleToken(ModelRole r) {
  for (const RoleBit& b : kRoleBits)
    if (b.r == r) return b.t;
  return "";
}
const char* capToken(ModelCap c) {
  switch (c) {
    case CapTools: return "tools";
    case CapVision: return "vision";
    case CapStreaming: return "streaming";
    case CapJson: return "json";
    case CapEmbedding: return "embedding";
    case CapAudioIn: return "audioIn";
    case CapAudioOut: return "audioOut";
  }
  return "";
}

ProbeVerdict probeVerdict(int httpStatus, const std::string& errBody) {
  if (httpStatus == 200) return ProbeVerdict::Usable;
  const std::string e = lower(errBody);
  // The model itself is rejected -> hide it. Match the shapes providers use for an
  // unknown / inaccessible model id.
  const bool modelRejected = has(e, "model_not_found") || has(e, "model not found") ||
                             has(e, "does not exist") || has(e, "no such model") ||
                             has(e, "unknown model") || has(e, "not have access") ||
                             has(e, "does not have access") || has(e, "not allowed to use") ||
                             has(e, "invalid model");
  if (modelRejected) return ProbeVerdict::Unusable;
  if (httpStatus == 404) return ProbeVerdict::Unusable;  // endpoint has no such model
  if (httpStatus == 401 || httpStatus == 403) return ProbeVerdict::Unusable;  // key can't use it
  // 429 / 5xx / network / a generic 400 (bad probe shape) are transient or
  // unrelated to the model - never demote on those.
  return ProbeVerdict::Unknown;
}

namespace {
uint16_t roleBitFromToken(const std::string& t) {
  for (const RoleBit& b : kRoleBits)
    if (t == b.t) return b.r;
  return 0;
}
uint16_t capBitFromKey(const std::string& k) {
  if (k == "tools") return CapTools;
  if (k == "vision") return CapVision;
  if (k == "streaming") return CapStreaming;
  if (k == "json") return CapJson;
  if (k == "embedding") return CapEmbedding;
  if (k == "audioIn") return CapAudioIn;
  if (k == "audioOut") return CapAudioOut;
  return 0;
}
}  // namespace

size_t modelsFromJson(JsonArrayConst arr, std::vector<ModelInfo>& out) {
  if (arr.isNull()) return 0;
  const size_t start = out.size();
  for (JsonObjectConst o : arr) {
    ModelInfo mi;
    mi.id = (const char*)(o["id"] | "");
    if (mi.id.empty()) continue;
    for (JsonVariantConst r : o["roles"].as<JsonArrayConst>())
      mi.roles |= roleBitFromToken((const char*)(r | ""));
    JsonObjectConst caps = o["caps"];
    for (JsonPairConst kv : caps)
      if (kv.value().as<bool>()) mi.caps |= capBitFromKey(kv.key().c_str());
    mi.usable = o["usable"] | true;
    mi.probed = o["probed"] | false;
    mi.ctxTokens = o["ctxTokens"] | 0u;
    mi.maxOutTokens = o["maxOutTokens"] | 0u;
    const char* sz = o["size"] | "";
    mi.size = sz[0] ? sz[0] : 0;
    mi.family = (const char*)(o["family"] | "");
    mi.apiCaps = (std::string((const char*)(o["source"] | "")) == "api");
    mi.deprecated = o["deprecated"] | false;
    mi.upstream = (const char*)(o["upstream"] | "");
    out.push_back(std::move(mi));
  }
  return out.size() - start;
}

namespace {
// The shared row body of both modelsToJson forms; `roleMask` filters the roles listed.
void rowJson(const ModelInfo& mi, JsonObject o, uint16_t roleMask) {
  o["id"] = mi.id;
  JsonArray roles = o["roles"].to<JsonArray>();
  for (const RoleBit& b : kRoleBits)
    if (mi.roles & roleMask & b.r) roles.add(b.t);
  o["usable"] = mi.usable;
  o["probed"] = mi.probed;
  o["ctxTokens"] = mi.ctxTokens;
  o["maxOutTokens"] = mi.maxOutTokens;
  JsonObject caps = o["caps"].to<JsonObject>();
  caps["tools"] = mi.hasCap(CapTools);
  caps["vision"] = mi.hasCap(CapVision);
  caps["streaming"] = mi.hasCap(CapStreaming);
  caps["json"] = mi.hasCap(CapJson);
  caps["embedding"] = mi.hasCap(CapEmbedding);
  caps["audioIn"] = mi.hasCap(CapAudioIn);
  caps["audioOut"] = mi.hasCap(CapAudioOut);
  o["family"] = mi.family;
  char sz[2] = {mi.size ? mi.size : '\0', '\0'};
  o["size"] = mi.size ? sz : "";
  o["source"] = mi.apiCaps ? "api" : "heuristic";
  o["deprecated"] = mi.deprecated;
  if (!mi.upstream.empty()) o["upstream"] = mi.upstream;
}
}  // namespace

void modelsToJson(const std::vector<ModelInfo>& models, JsonArray arr, bool includeUnusable) {
  for (const ModelInfo& mi : models) {
    if (!includeUnusable && !mi.usable) continue;
    rowJson(mi, arr.add<JsonObject>(), 0xFFFF);
  }
}

void modelRowToJson(const std::string& provider, const ModelInfo& m, JsonObject o, uint32_t now) {
  uint16_t mask = 0xFFFF;
  if (!rowOffered(m, RoleOrchestrator, now)) mask &= (uint16_t)~RoleOrchestrator;
  if (!rowOffered(m, RoleSubAgent, now)) mask &= (uint16_t)~RoleSubAgent;
  rowJson(m, o, mask);
  if (!m.name.empty()) o["name"] = m.name;
  if (!m.line.empty()) o["line"] = m.line;
  if (m.created) o["created"] = m.created;
  if (!m.life.empty()) o["life"] = m.life;
  if (m.retires) o["retires"] = m.retires;
  const ModelPrice p = modelPrice(provider, m);
  o["priceSrc"] = p.src;
  if (std::strcmp(p.src, "unknown") != 0) {
    o["priceIn"] = p.in;
    o["priceOut"] = p.out;
  }
}

void modelsToJson(const std::string& provider, const std::vector<ModelInfo>& models,
                  JsonArray arr, bool includeUnusable, uint32_t now) {
  for (const ModelInfo& mi : models) {
    if (!includeUnusable && !mi.usable) continue;
    modelRowToJson(provider, mi, arr.add<JsonObject>(), now);
  }
}

}  // namespace orch
}  // namespace nimbus
