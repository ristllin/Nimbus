#include "nimbus/harness/vision_caption.h"

#include <ArduinoJson.h>

#include <cstring>
#include <vector>

#include "nimbus/harness/log.h"
#include "nimbus/harness/output_caps.h"   // anthropicTakesEffort, mistralTakesEffort
#include "nimbus/harness/providers.h"     // testStatusOverride - the bench's scripted answer
#include "nimbus/orch/billing_refusal.h"  // billingNoticeDue, billingNotice
#include "nimbus/orch/model_catalog.h"    // gptGeneration
#include "nimbus/orch/spend_guard.h"   // lane QW4: sentWithoutAnswer, sentRequestEstimate
#include "nimbus/orch/model_defaults.h"   // the role-default table: the caption fallback (lane DEF)
#include "nimbus/orch/spend_ceiling.h"    // paidCallAllowed - the monthly spend ceiling (lane DEF)
#include "nimbus/orch/provider_error.h"   // oneLineText
#include "nimbus/orch/provider_park.h"    // the one per-provider park
#include "nimbus/orch/reply_text.h"       // messagesContentText, chatContentText
#include "nimbus/orch/token_usage_json.h"
#include "nimbus/orch/turn_images.h"   // kImagePartBytes - the record queue's size budget
#include "nimbus/utf8.h"

namespace agent {
namespace vision {

namespace orch = nimbus::orch;

namespace {


const char* kAnthropicHost = "api.anthropic.com";
const char* kOpenAIHost = "api.openai.com";
const char* kMistralHost = "api.mistral.ai";
const char* kMarker = "nimg:caption";   // where the photo's base64 goes (spliced at send time)

const char* hostOf(const std::string& provider) {
  if (provider == "anthropic") return kAnthropicHost;
  if (provider == "openai") return kOpenAIHost;
  if (provider == "mistral") return kMistralHost;
  return "";
}

bool openaiReasons(const std::string& model) {
  return orch::gptGeneration(model) >= 5 || (model.size() > 1 && model[0] == 'o' && model[1] >= '1' && model[1] <= '9');
}

uint32_t outCap(const std::string& provider, const std::string& model, CaptionKind k) {
  uint32_t n = k == CaptionKind::Memory ? 320 : (k == CaptionKind::Question ? 640 : 900);
  // A reasoning model's completion budget includes its (low-effort) reasoning.
  if (provider == "openai" && openaiReasons(model)) n += 1600;
  return n;
}

const char* kFallbackPrompt =
    "You are reading a photo for an assistant that cannot see it. First copy every piece of "
    "text in the photo exactly as written (labels, numbers, units, prices, dates, names), "
    "one item per line. Then describe what the photo shows in one or two plain sentences. "
    "If the sender asked something, say what in the photo answers it. Do not guess at text "
    "you cannot read; say it is unreadable.";
const char* kMemoryPrompt =
    "Write a short record of this photo for a memory search: one or two plain sentences on "
    "what it shows, then any key text or numbers in it copied exactly. No preamble.";
const char* kQuestionPrompt =
    "Answer the question below about this photo. Copy any text you rely on exactly. If the "
    "photo does not show the answer, say so plainly.";

std::string promptOf(const CaptionRequest& in) {
  std::string p = in.kind == CaptionKind::Memory     ? kMemoryPrompt
                  : in.kind == CaptionKind::Question ? kQuestionPrompt
                                                     : kFallbackPrompt;
  if (in.kind == CaptionKind::Question && !in.question.empty()) p += "\n\nQuestion: " + in.question;
  if (!in.context.empty()) p += "\n\nThe sender wrote: " + in.context;
  std::string t = p;
  nimbus::utf8Repair(t);   // the sender's words may be cut mid-character upstream
  return t;
}

// The provider's error fields as one text (the billing classifier reads tokens in
// the message, the type or the code).
std::string errorText(const JsonDocument& doc) {
  std::string t;
  for (const char* k : {"message", "type", "code"}) {
    JsonVariantConst v = doc["error"][k];
    if (v.is<const char*>()) t += std::string(v.as<const char*>()) + " ";
  }
  if (doc["error"].is<const char*>()) t += std::string(doc["error"].as<const char*>()) + " ";
  JsonVariantConst m = doc["message"];
  if (m.is<const char*>()) t += std::string(m.as<const char*>()) + " ";
  if (doc["code"].is<const char*>()) t += std::string(doc["code"].as<const char*>()) + " ";
  return t;
}
std::string errorCode(const JsonDocument& doc) {
  for (const char* k : {"code", "type"}) {
    JsonVariantConst v = doc["error"][k];
    if (v.is<const char*>() && *v.as<const char*>()) return v.as<const char*>();
  }
  if (doc["code"].is<const char*>()) return doc["code"].as<const char*>();
  if (doc["type"].is<const char*>()) return doc["type"].as<const char*>();
  return std::string();
}
std::string errorMessage(const JsonDocument& doc) {
  if (doc["error"]["message"].is<const char*>()) return doc["error"]["message"].as<const char*>();
  if (doc["message"].is<const char*>()) return doc["message"].as<const char*>();
  return std::string();
}

struct Walk {
  std::string sticky;   // the provider that answered last on this page
} g_walk;

LastError g_last;

void noteLast(const CaptionDeps& d, const CaptionResult& r) {
  g_last.atMs = d.nowMs ? d.nowMs() : 1;
  if (!g_last.atMs) g_last.atMs = 1;
  g_last.provider = r.provider;
  g_last.fail = r.fail;
  g_last.status = r.status;
}

std::vector<std::string> walkOrder(const CaptionDeps& d) {
  std::string prio = d.priority ? d.priority() : std::string();
  if (prio.empty()) prio = "openai,mistral,anthropic";
  std::vector<std::string> order;
  auto add = [&order](const std::string& p) {
    for (const auto& o : order)
      if (o == p) return;
    order.push_back(p);
  };
  if (!g_walk.sticky.empty()) add(g_walk.sticky);   // sticky per page
  size_t s = 0;
  while (s <= prio.size()) {
    size_t e = prio.find(',', s);
    if (e == std::string::npos) e = prio.size();
    std::string p = prio.substr(s, e - s);
    while (!p.empty() && p.front() == ' ') p.erase(p.begin());
    while (!p.empty() && p.back() == ' ') p.pop_back();
    if (!p.empty()) add(p);
    s = e + 1;
  }
  add("cumulo");   // a router-only device still reads photos
  return order;
}

// The wire a provider speaks for a caption (the router's upstream for "cumulo").
struct Route {
  std::string wire;    // anthropic | openai | mistral ("" = no caption path)
  std::string model;
  std::string host;    // "" = the provider's own host
};
// The model a caption request to `wire` carries: the device's pick when it reads
// images, else the role default (a catalog pick without image input is never sent).
std::string modelFor(const CaptionDeps& d, const std::string& provider, const std::string& wire) {
  const std::string picked = d.model ? d.model(provider) : std::string();
  if (!picked.empty() && orch::headTakesImages(wire, picked)) return picked;
  return captionModel(wire);
}

Route routeOf(const CaptionDeps& d, const std::string& provider) {
  Route r;
  if (provider != "cumulo") {
    if (!*captionModel(provider)) return r;
    r.wire = provider;
    r.model = modelFor(d, provider, provider);
    return r;
  }
  const std::string host = d.routerHost ? d.routerHost() : std::string();
  if (host.empty()) return r;
  const std::string sel = d.routerSelection ? d.routerSelection() : std::string();
  const size_t slash = sel.find('/');
  const std::string up = slash != std::string::npos && slash > 0 ? sel.substr(0, slash) : std::string("openai");
  if (!*captionModel(up)) return r;   // zai or unknown: no router caption path
  r.wire = up;
  r.model = captionModel(up);   // the upstream's caption default (the router's list: lanes/E follow-up 04)
  r.host = host;
  return r;
}

std::string closedWords(const std::string& provider, const orch::ParkVerdict& v) {
  if (v.paused) return "paid requests to " + provider + " are paused";
  if (v.kind == orch::ParkKind::Billing)
    return provider + " out of credit" + (v.httpStatus ? " (HTTP " + std::to_string(v.httpStatus) + ")" : "");
  if (v.kind == orch::ParkKind::Auth) return provider + " refused the key";
  return provider + " is not available";
}

}  // namespace

const char* readFailSlug(ReadFail f) {
  switch (f) {
    case ReadFail::None:          return "none";
    case ReadFail::NoProvider:    return "no-provider";
    case ReadFail::Closed:        return "closed";
    case ReadFail::Billing:       return "billing";
    case ReadFail::Auth:          return "auth";
    case ReadFail::TooLarge:      return "too-large";
    case ReadFail::Unsupported:   return "unsupported";
    case ReadFail::LowMemory:     return "low-memory";
    case ReadFail::ProviderError: return "provider-error";
    case ReadFail::Network:       return "network";
    case ReadFail::Count:         break;
  }
  return "?";
}

const char* captionModel(const std::string& provider) {
  if (provider != "anthropic" && provider != "openai" && provider != "mistral") return "";
  const orch::RoleDefault* row = orch::roleDefaultRow(provider.c_str(), orch::DefaultRole::Caption);
  return row ? row->fallback : "";
}

bool buildCaptionRequest(const std::string& provider, const std::string& model, const std::string& key,
                         const CaptionRequest& in, const std::string& routedHost, HttpRequest& out) {
  const char* host = hostOf(provider);
  if (!*host || !in.bytes || !in.len) return false;
  const std::string mime = in.mime.empty() ? "image/jpeg" : in.mime;
  JsonDocument req;
  req["model"] = model;
  const uint32_t cap = outCap(provider, model, in.kind);
  JsonArray content = req["messages"][0]["content"].to<JsonArray>();
  req["messages"][0]["role"] = "user";
  // The photo first, the words after it: the sender's text can never sit before the
  // marker the body splices at (the first occurrence is always the photo's).
  JsonObject img = content.add<JsonObject>();
  JsonObject txt;
  if (provider == "anthropic") {
    req["max_tokens"] = cap;
    if (providers::anthropicTakesEffort(model)) req["output_config"]["effort"] = "low";
    img["type"] = "image";
    img["source"]["type"] = "base64";
    img["source"]["media_type"] = mime;
    img["source"]["data"] = kMarker;
    txt = content.add<JsonObject>();
    txt["type"] = "text";
  } else if (provider == "openai") {
    // The token field per model family: a reasoning model refuses max_tokens.
    if (openaiReasons(model)) {
      req["max_completion_tokens"] = cap;
      req["reasoning_effort"] = "low";
    } else {
      req["max_tokens"] = cap;
    }
    img["type"] = "image_url";
    img["image_url"]["url"] = std::string("data:") + mime + ";base64," + kMarker;
    img["image_url"]["detail"] = "high";
    txt = content.add<JsonObject>();
    txt["type"] = "text";
  } else {   // mistral
    req["max_tokens"] = cap;
    if (providers::mistralTakesEffort(model)) req["reasoning_effort"] = "none";   // Mistral's low setting
    img["type"] = "image_url";
    img["image_url"] = std::string("data:") + mime + ";base64," + kMarker;
    txt = content.add<JsonObject>();
    txt["type"] = "text";
  }
  txt["text"] = promptOf(in);
  std::string body;
  serializeJson(req, body);
  const size_t at = body.find(kMarker);
  if (at == std::string::npos) return false;
  body.erase(at, std::strlen(kMarker));
  out = HttpRequest();
  out.method = "POST";
  out.body = std::move(body);
  out.splices = {BodySplice{at, in.bytes, in.len, true}};
  out.timeoutMs = 60000;
  const bool anthropic = provider == "anthropic";
  const char* path = anthropic ? "/v1/messages" : "/v1/chat/completions";
  if (!routedHost.empty()) {
    out.host = routedHost;
    out.path = "/router/" + provider + path;
    out.headers.push_back({"Authorization", "Bearer " + key});   // the router injects the upstream key
  } else {
    out.host = host;
    out.path = path;
    if (anthropic) out.headers.push_back({"x-api-key", key});
    else out.headers.push_back({"Authorization", "Bearer " + key});
  }
  if (anthropic) out.headers.push_back({"anthropic-version", "2023-06-01"});
  out.headers.push_back({"Content-Type", "application/json"});
  return true;
}

std::string errLine(const std::string& host, const std::string& model, int status, const std::string& code,
                    const std::string& message) {
  std::string line = "vision: " + host + " " + model + " HTTP " + std::to_string(status);
  if (!code.empty()) line += " " + orch::oneLineText(code, 48);
  const std::string msg = orch::oneLineText(message, kErrLineMax);
  if (!msg.empty()) line += ": " + msg;
  return orch::oneLineText(line, kErrLineMax);
}

namespace {

// The most telling failure decides the label: a refusal beats a network blip, which
// beats a provider skipped for an earlier refusal.
int failRank(ReadFail f) {
  switch (f) {
    case ReadFail::Billing: return 6;
    case ReadFail::Auth: return 5;
    case ReadFail::ProviderError: return 4;
    case ReadFail::Network: return 3;
    case ReadFail::Closed: return 2;
    default: return 1;
  }
}

JsonDocument answerFilter() {
  JsonDocument filter;
  filter["content"][0]["type"] = true;
  filter["content"][0]["text"] = true;
  filter["choices"][0]["message"]["content"] = true;
  filter["usage"] = true;
  filter["model"] = true;
  filter["error"] = true;
  filter["message"] = true;
  filter["code"] = true;
  filter["type"] = true;
  return filter;
}

// One request: the HTTP status (0 = no answer) with the filtered answer in `doc`.
int send(const CaptionDeps& d, const HttpRequest& req, JsonDocument& doc, std::string& terr) {
  const JsonDocument filter = answerFilter();
#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
  // The bench's scripted provider answer (PROVFAULT): answered here, nothing sent.
  int ovStatus = 0;
  std::string ovBody;
  if (providers::testStatusOverride(req.host, ovStatus, ovBody)) {
    hlog::logf("vision: TEST override answered %s with HTTP %d", req.host.c_str(), ovStatus);
    if (ovStatus > 0) deserializeJson(doc, ovBody, DeserializationOption::Filter(filter));
    return ovStatus > 0 ? ovStatus : 0;
  }
#endif
  return d.http->execJson(req, doc, filter, terr);
}

// A 2xx answer: the transcription, or a provider error when it carries no usable text.
void readAnswer(const HttpRequest& req, const Route& route, const JsonDocument& doc, const std::string& terr,
                size_t photoLen, CaptionResult& r) {
  r.usage = orch::tokenUsageFromJson(doc["usage"].as<JsonObjectConst>());
  std::string text = route.wire == "anthropic" ? orch::messagesContentText(doc["content"])
                                               : orch::chatContentText(doc["choices"][0]["message"]["content"]);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
  if (!terr.empty() || text.empty() || text.size() > kMaxCaptionText) {
    r.fail = ReadFail::ProviderError;
    r.words = "provider error";
    hlog::logf("vision: %s %s HTTP %d but %s (%u B)", req.host.c_str(), route.model.c_str(), r.status,
               !terr.empty() ? "the answer did not parse" : (text.empty() ? "no text" : "the answer is too long"),
               (unsigned)text.size());
    return;
  }
  r.text = std::move(text);
  hlog::logf("vision: %s %s read %u bytes -> %u chars", r.provider.c_str(), route.model.c_str(),
             (unsigned)photoLen, (unsigned)r.text.size());
}

// A refusal: the park learns it (a billing or key refusal closes the provider for every
// seam) and the result is typed; the walk then moves on to the next keyed provider.
void readRefusal(const HttpRequest& req, const Route& route, const JsonDocument& doc, uint32_t keyFp,
                 uint32_t now, CaptionResult& r) {
  hlog::logf("%s", errLine(req.host, route.model, r.status, errorCode(doc), errorMessage(doc)).c_str());
  const orch::ParkKind k =
      orch::providerPark().noteAnswer(r.provider.c_str(), orch::PaidSeam::Vision, keyFp, r.status, errorText(doc), now);
  r.fail = k == orch::ParkKind::Billing ? ReadFail::Billing
           : k == orch::ParkKind::Auth  ? ReadFail::Auth
                                        : ReadFail::ProviderError;
  r.words = failWords(r.fail, r.provider, r.status);
}

// Ask one open provider for the photo.
CaptionResult askOne(const CaptionDeps& d, const std::string& p, const Route& route, const std::string& key,
                     uint32_t now, const CaptionRequest& in) {
  CaptionResult r;
  r.provider = p;
  r.model = route.model;
  HttpRequest req;
  if (!buildCaptionRequest(route.wire, route.model, key, in, route.host, req)) {
    r.fail = ReadFail::Unsupported;
    r.words = failWords(r.fail, p, 0);
    return r;
  }
  JsonDocument doc;
  std::string terr;
  r.requests = 1;
  r.status = send(d, req, doc, terr);
  if (r.status <= 0) {
    orch::providerPark().noteFailure(p.c_str(), now);
    r.status = 0;
    r.fail = ReadFail::Network;
    r.words = failWords(r.fail, p, 0);
    hlog::logf("vision: %s %s no answer (%s)", req.host.c_str(), route.model.c_str(),
               orch::oneLineText(terr, 80).c_str());
  } else if (r.status >= 200 && r.status < 300) {
    orch::providerPark().noteSuccess(p.c_str(), orch::PaidSeam::Vision);
    readAnswer(req, route, doc, terr, in.len, r);
  } else {
    readRefusal(req, route, doc, orch::parkKeyFingerprint(key), now, r);
  }
  // Lane QW4 (CUM-776): the photo went out and no usable answer came back (no response,
  // or an answer cut mid-way): it may well have been billed, so it counts an estimate
  // (the request's text at two characters a token, the photo at the image estimate).
  if (r.usage.empty() && !r.usage.cacheReadTokens && orch::sentWithoutAnswer(r.status, terr)) {
    r.usage = orch::sentRequestEstimate(req.body.size(), req.splices.size());
    hlog::logf("vision: %s %s sent but no usable answer - counted at an estimate (%u input tokens)",
               req.host.c_str(), route.model.c_str(), (unsigned)r.usage.promptTokens);
  }
  return r;
}

// One owner notice per billing episode (shared with every seam), naming who reads
// photos meanwhile; a quiet background turn leaves it for the next owner turn.
void noticeBilling(const CaptionDeps& d, const std::string& billedBy, int status, const std::string& next) {
  if (billedBy.empty() || !d.ownerReachable || !d.notifyOwner || !d.ownerReachable() ||
      !orch::billingNoticeDue(billedBy.c_str()))
    return;
  std::string words = orch::billingNotice(billedBy, status, "");
  words += next.empty() ? " Photos cannot be read until credit is added to " + billedBy + "."
                        : " Until then, photos are read by " + next + ".";
  d.notifyOwner(words);
}

// Ask provider `p`, or skip it with no request when it cannot be asked: no key or no
// caption path (NoProvider), or closed by the park (Closed, with its words).
CaptionResult tryProvider(const CaptionDeps& d, const std::string& p, uint32_t now, const CaptionRequest& in) {
  CaptionResult r;
  r.fail = ReadFail::NoProvider;
  const std::string key = d.key ? d.key(p) : std::string();
  const Route route = key.empty() ? Route() : routeOf(d, p);
  if (route.wire.empty()) return r;
  const orch::ParkVerdict v =
      orch::providerPark().available(p.c_str(), orch::PaidSeam::Vision, orch::parkKeyFingerprint(key), now);
  if (v.open) return askOne(d, p, route, key, now, in);
  r.fail = ReadFail::Closed;
  r.provider = p;
  r.status = v.httpStatus;
  r.words = closedWords(p, v);
  hlog::logf("vision: %s skipped (%s)", p.c_str(), orch::parkWords(p.c_str(), v).c_str());
  return r;
}

// Refuse before any request: no transport or bytes, or the monthly spend ceiling.
bool refusedUpFront(const CaptionDeps& d, const CaptionRequest& in, CaptionResult& res) {
  if (!d.http || !in.bytes || !in.len) {
    res.fail = ReadFail::LowMemory;
    res.words = failWords(res.fail, "", 0);
  } else if (!orch::paidCallAllowed(orch::SpendSeam::VisionFallback)) {   // lane DEF
    res.fail = ReadFail::Closed;
    res.words = "paid requests are paused (the monthly spend limit)";
  } else {
    return false;
  }
  noteLast(d, res);
  return true;
}

}  // namespace

CaptionResult caption(const CaptionDeps& d, const CaptionRequest& in) {
  CaptionResult res;
  if (refusedUpFront(d, in, res)) return res;
  const uint32_t now = d.nowMs ? d.nowMs() : 0;
  CaptionResult worst;
  worst.fail = ReadFail::NoProvider;
  worst.words = failWords(worst.fail, "", 0);
  int requests = 0;
  orch::TokenUsage billed;   // every answer billed, a failed one included
  std::vector<CaptionResult::Spend> spends;   // ...each on its own provider (lane QW4)
  std::string billedBy;      // the first provider that refused for billing this photo
  int billedStatus = 0;
  for (const std::string& p : walkOrder(d)) {
    const CaptionResult r = tryProvider(d, p, now, in);
    requests += r.requests;
    billed += r.usage;
    if (!r.usage.empty() || r.usage.cacheReadTokens) spends.push_back({r.provider, r.model, r.usage});
    if (r.fail == ReadFail::Billing && billedBy.empty()) {
      billedBy = p;
      billedStatus = r.status;
    }
    if (!r.text.empty()) {
      g_walk.sticky = p;
      res = r;
      break;
    }
    if (failRank(r.fail) > failRank(worst.fail)) worst = r;
  }
  noticeBilling(d, billedBy, billedStatus, res.text.empty() ? std::string() : res.provider);
  CaptionResult& out = res.text.empty() ? worst : res;
  out.requests = requests;
  out.usage = billed;
  out.spends = spends;
  if (out.text.empty()) noteLast(d, out);
  return out;
}

void endPage() { g_walk = Walk(); }

bool lookArgsValid(const std::string& ref, const std::string& question, std::string& err) {
  if (!orch::validRef(ref)) {
    err = "ref must be the 16 hex characters from the photo's [image ... ref=...] label";
    return false;
  }
  if (question.empty()) {
    err = "need 'question': what to look for in the photo";
    return false;
  }
  if (question.size() > kLookQuestionMax) {
    err = "question too long (at most " + std::to_string(kLookQuestionMax) + " bytes)";
    return false;
  }
  return true;
}

std::string failWords(ReadFail f, const std::string& provider, int status) {
  const std::string st = status > 0 ? " (HTTP " + std::to_string(status) + ")" : std::string();
  switch (f) {
    case ReadFail::None:          return std::string();
    case ReadFail::NoProvider:    return "no provider can read photos";
    case ReadFail::Closed:        return provider + " is not available";
    case ReadFail::Billing:       return provider + " out of credit" + st;
    case ReadFail::Auth:          return provider + " refused the key" + st;
    case ReadFail::TooLarge:      return "too large";
    case ReadFail::Unsupported:   return "a format no provider reads";
    case ReadFail::LowMemory:     return "device low on memory";
    case ReadFail::ProviderError: return "provider error";
    case ReadFail::Network:       return provider + " did not answer";
    case ReadFail::Count:         break;
  }
  return "not read";
}

bool rereadLater(ReadFail f) {
  return f == ReadFail::Billing || f == ReadFail::Auth || f == ReadFail::Closed || f == ReadFail::Network;
}

std::string notReadWords(const CaptionResult& r) {
  return rereadLater(r.fail)
             ? r.words + "; it is kept and re-read once the provider is back, so it need not be sent again"
                         " (the owner can ask about it then)"
             : r.words;
}

bool RecordQueue::push(const PendingRecord& r) {
  if (r.ref.size() != 16 || !r.len || r.len > nimbus::orch::kImagePartBytes) return false;
  for (const auto& it : items_)
    if (it.ref == r.ref && it.chatId == r.chatId) return false;
  if (items_.size() >= kMax) items_.erase(items_.begin());   // drop the oldest
  items_.push_back(r);
  return true;
}

const PendingRecord* RecordQueue::due(uint32_t nowMs, bool providerOpen) const {
  if (!providerOpen) return nullptr;
  for (size_t i = items_.size(); i-- > 0;)   // newest first
    if ((int32_t)(nowMs - items_[i].notBeforeMs) >= 0) return &items_[i];
  return nullptr;
}

void RecordQueue::settle(const PendingRecord::Str& ref, bool ok, uint32_t nowMs) {
  for (size_t i = 0; i < items_.size(); ++i) {
    PendingRecord& it = items_[i];
    if (it.ref != ref) continue;
    if (!ok && ++it.tries < kRecordTries) {
      it.notBeforeMs = nowMs + (kRetryBaseMs << (it.tries - 1));
      return;
    }
    items_.erase(items_.begin() + (long)i);
    return;
  }
}

RecordQueue& recordQueue() {
  static RecordQueue q;
  return q;
}

LastError lastError() { return g_last; }
void clearLastError() {
  g_last = LastError();
  g_walk = Walk();
}

}  // namespace vision
}  // namespace agent
