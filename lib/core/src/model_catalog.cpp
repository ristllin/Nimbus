#include "nimbus/orch/model_catalog.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "nimbus/orch/compact.h"  // modelCtxTokens - the family context-window table

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
    return has(m, "gpt-4o") || startsWith(m, "gpt-5") || has(m, "-vision") || has(m, "omni");
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
  static const char* kSmall[] = {"nano", "tiny", "-air", "flash", "lite", "small", "mini", nullptr};
  for (int i = 0; kSmall[i]; ++i)
    if (has(m, kSmall[i])) return 'S';
  if (has(m, "medium") || has(m, "turbo")) return 'M';
  static const char* kLarge[] = {"large", "opus", "-max", nullptr};
  for (int i = 0; kLarge[i]; ++i)
    if (has(m, kLarge[i])) return 'L';
  return 0;
}
// Flagship id-family default size when no tier word is present.
char sizeFromFlagship(const std::string& m) {
  if (startsWith(m, "gpt-5") || startsWith(m, "glm-5") || startsWith(m, "glm-4")) return 'L';
  if (isOSeries(m)) return 'L';
  return 0;
}
}  // namespace

std::string modelFamily(const std::string& provider, const std::string& id) {
  const std::string m = lower(id);
  if (isClaude(provider, m)) return claudeFamily(m);
  if (isOSeries(m)) return "o-series";
  struct Fam {
    const char* s;
    const char* f;
    bool prefix;
  };
  static const Fam kFams[] = {
      {"gpt-5", "gpt-5", true},     {"gpt-4", "gpt-4", true}, {"magistral", "magistral", false},
      {"mistral", "mistral", false}, {"pixtral", "pixtral", false}, {"glm-", "glm", true},
      {"embed", "embedding", false},
  };
  for (const Fam& f : kFams)
    if (f.prefix ? startsWith(m, f.s) : has(m, f.s)) return f.f;
  return "";
}

char modelSizeClass(const std::string& provider, const std::string& id) {
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

// Locate the JSON body when HTTP response headers precede it.
const char* jsonStart(const std::string& body) {
  size_t p = body.find("\r\n\r\n");
  if (p != std::string::npos) return body.c_str() + p + 4;
  p = body.find("\n\n");
  if (p != std::string::npos) return body.c_str() + p + 2;
  return body.c_str();
}

bool alreadyHave(const std::vector<ModelInfo>& out, const std::string& id) {
  for (const ModelInfo& e : out)
    if (e.id == id) return true;
  return false;
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

size_t parseModelsList(const std::string& provider, const std::string& body,
                       std::vector<ModelInfo>& out, ArduinoJson::Allocator* alloc) {
  // A null allocator would make ArduinoJson dereference nullptr on the first
  // allocation; fall back to its default allocator (host tests pass nullptr).
  JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
  DeserializationError err =
      deserializeJson(doc, jsonStart(body), DeserializationOption::NestingLimit(16));
  if (err) return 0;
  JsonArrayConst arr = doc["data"].as<JsonArrayConst>();
  if (arr.isNull()) return 0;

  const size_t startCount = out.size();
  for (JsonObjectConst m : arr) {
    std::string id =
        (provider == "mistral") ? mistralCanonicalId(m) : std::string((const char*)(m["id"] | ""));
    if (id.size() < 2) continue;

    ModelInfo mi = classifyEntry(provider, id);

    bool keep = true;
    if (provider == "anthropic") {
      overlayAnthropic(m, mi);
    } else if (provider == "mistral") {
      overlayMistral(m, mi, keep);  // Mistral drops models the API marks deprecated
    } else if (!m["shutdown_date"].isNull() || !m["deprecation"].isNull()) {
      // A future shutdown date still leaves the model usable: flag it, keep it,
      // and let the usability probe / UI decide. (Mistral is stricter above.)
      mi.deprecated = true;
    }
    if (mi.roles == 0) keep = false;  // role-less (non-agentic) models never surface
    if (!keep) continue;
    if (alreadyHave(out, mi.id)) continue;
    out.push_back(std::move(mi));
  }
  sortFlagshipFirst(out);
  return out.size() - startCount;
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

void modelsToJson(const std::vector<ModelInfo>& models, JsonArray arr, bool includeUnusable) {
  for (const ModelInfo& mi : models) {
    if (!includeUnusable && !mi.usable) continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"] = mi.id;
    JsonArray roles = o["roles"].to<JsonArray>();
    for (const RoleBit& b : kRoleBits)
      if (mi.roles & b.r) roles.add(b.t);
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
}

}  // namespace orch
}  // namespace nimbus
