#include "image_parts.h"

#include <algorithm>
#include <cstring>

#include "nimbus/harness/log.h"
#include "nimbus/orch/psram_alloc.h"
#include "wire.h"

namespace agent {
namespace providers {
namespace imageparts {

namespace orch = nimbus::orch;

namespace {

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// The turn text of the seeded message, whether it is still a string or already
// text blocks (Anthropic's textAsBlocks runs first).
std::string contentText(ArduinoJson::JsonObject m) {
  ArduinoJson::JsonVariant c = m["content"];
  if (c.is<const char*>()) return c.as<const char*>();
  std::string out;
  if (!c.is<ArduinoJson::JsonArray>()) return out;
  for (ArduinoJson::JsonObject b : c.as<ArduinoJson::JsonArray>()) {
    const char* t = b["text"] | (const char*)nullptr;
    if (!t) continue;
    if (!out.empty()) out += "\n";
    out += t;
  }
  return out;
}

void putText(ArduinoJson::JsonArray parts, const std::string& text, Dialect d) {
  if (text.empty()) return;
  ArduinoJson::JsonObject t = parts.add<ArduinoJson::JsonObject>();
  t["type"] = d == Dialect::OpenAIResponses ? "input_text" : "text";
  t["text"] = text;
}

void putImage(ArduinoJson::JsonArray parts, const orch::TurnImages& imgs, size_t i, Dialect d) {
  const orch::TurnImage& it = imgs.items[i];
  ArduinoJson::JsonObject p = parts.add<ArduinoJson::JsonObject>();
  auto dataUrl = [&] { return std::string("data:") + it.mime + ";base64," + imgs.marker(i); };
  switch (d) {
    case Dialect::Anthropic:
      p["type"] = "image";
      if (!it.fileId.empty() && it.fileHost == "anthropic") {
        p["source"]["type"] = "file";
        p["source"]["file_id"] = it.fileId;
      } else {
        p["source"]["type"] = "base64";
        p["source"]["media_type"] = it.mime;
        p["source"]["data"] = imgs.marker(i);
      }
      break;
    case Dialect::OpenAIResponses:
      p["type"] = "input_image";
      p["image_url"] = dataUrl();
      p["detail"] = kOpenAIDetail;
      break;
    case Dialect::MistralChat:
      p["type"] = "image_url";
      p["image_url"] = dataUrl();
      break;
  }
}

bool inlineImage(const orch::TurnImage& it) { return it.fileId.empty(); }

}  // namespace

orch::TurnImages* shown(const orch::Transcript& tr, const std::string& host, const std::string& model) {
  orch::TurnImages* imgs = tr.images();
  if (!imgs || imgs->empty() || imgs->rejected) return nullptr;
  return orch::headTakesImages(host, model) ? imgs : nullptr;
}

std::string userText(const orch::Transcript& tr, const std::string& text, bool showing) {
  orch::TurnImages* imgs = tr.images();
  if (showing || !imgs || imgs->empty()) return text;
  return imgs->blindText(text);
}

void putParts(ArduinoJson::JsonObject m, const orch::TurnImages& imgs, Dialect d) {
  const std::string text = contentText(m);
  ArduinoJson::JsonArray parts = m["content"].to<ArduinoJson::JsonArray>();
  for (size_t i = 0; i < imgs.items.size(); ++i) {
    putText(parts, imgs.items[i].tag, d);
    putImage(parts, imgs, i, d);
  }
  putText(parts, text, d);
}

bool spliceMarkers(std::string& body, const orch::TurnImages& imgs, BodySplices& out, std::string& err) {
  struct Hit {
    size_t at;
    size_t i;
    size_t len;
  };
  std::vector<Hit> hits;
  for (size_t i = 0; i < imgs.items.size(); ++i) {
    if (!inlineImage(imgs.items[i])) continue;
    const std::string mk = imgs.marker(i);
    const size_t at = body.find(mk);
    if (at == std::string::npos) {
      err = "image " + std::to_string(i + 1) + " is missing from the request";
      hlog::logf("wire: %s", err.c_str());
      return false;
    }
    hits.push_back({at, i, mk.size()});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });
  std::string cut;
  cut.reserve(body.size());
  size_t from = 0;
  for (const Hit& h : hits) {
    cut.append(body, from, h.at - from);
    const orch::TurnImage& it = imgs.items[h.i];
    out.push_back(BodySplice{cut.size(), it.bytes, it.len, true});
    from = h.at + h.len;
  }
  cut.append(body, from, std::string::npos);
  body.swap(cut);
  return true;
}

bool refusedImages(int httpStatus, const std::string& errText) {
  if (httpStatus == 413) return true;
  if (httpStatus != 400 && httpStatus != 422) return false;
  const std::string e = lower(errText);
  return e.find("image") != std::string::npos;
}

bool rejectAndRetry(orch::TurnImages* imgs, const char* who, int httpStatus) {
  if (!imgs || imgs->rejected) return false;
  imgs->rejected = true;
  hlog::logf("%s: the provider refused the photos (HTTP %d) - re-sent with their transcriptions", who,
             httpStatus);
  return true;
}

std::string blindInputs(const std::string& inputs) {
  orch::TurnImages* imgs = orch::activeTurnImages();
  return imgs && !imgs->empty() ? imgs->blindText(inputs) : inputs;
}

namespace {
// Uploaded photos waiting to be deleted (file ids). Bounded: past it the oldest is
// dropped with a log line (that file stays at the provider until deleted by hand).
// Through the working-set allocator (PSRAM on the device): ids can wait many minutes.
constexpr size_t kMaxPendingDeletes = 32;
orch::WorkingVector<orch::WorkingString>& pendingDeletes() {
  static orch::WorkingVector<orch::WorkingString> v;
  return v;
}
void queueDeletes(orch::TurnImages& imgs) {
  for (auto& it : imgs.items) {
    if (it.fileId.empty() || it.fileHost != "anthropic") continue;
    auto& q = pendingDeletes();
    if (q.size() >= kMaxPendingDeletes) {
      hlog::logf("anthropic: upload delete queue full - %s stays at the provider", q.front().c_str());
      q.erase(q.begin());
    }
    q.push_back(orch::workingCopy(it.fileId));
    it.fileId.clear();
    it.fileHost.clear();
  }
}
}  // namespace

LoopImages::LoopImages(orch::Transcript& tr) : tr_(tr) { tr.setImages(orch::activeTurnImages()); }
LoopImages::~LoopImages() {
  if (tr_.images()) queueDeletes(*tr_.images());
}

bool anyFileRef(const orch::TurnImages* imgs) {
  if (!imgs) return false;
  for (const auto& it : imgs->items)
    if (!it.fileId.empty() && it.fileHost == "anthropic") return true;
  return false;
}

// ---- upload once (Anthropic Files API) ---------------------------------------------

namespace {
const char* kAntHost = "api.anthropic.com";

// A multipart boundary the photo's bytes do not contain (the bytes are the sender's;
// a fixed, known boundary inside them would let them add parts to the upload).
std::string boundaryFor(const orch::TurnImage& it, const std::string& nonce) {
  std::string b = "nimbus-photo-" + nonce + "-" + it.ref;
  for (int guard = 0; guard < 8; ++guard) {
    const std::string mark = "--" + b;
    if (std::search(it.bytes, it.bytes + it.len, mark.begin(), mark.end()) == it.bytes + it.len) return b;
    b += "x";
  }
  return std::string();   // give up: this photo stays inline
}

const char* extOf(const std::string& mime) {
  if (mime == "image/png") return "png";
  if (mime == "image/webp") return "webp";
  if (mime == "image/gif") return "gif";
  return "jpg";
}

HeaderList antFileHeaders(const ProviderDeps& pd, const char* contentType) {
  HeaderList h = {
      {"x-api-key", pd.key ? pd.key("anthropic") : std::string()},
      {"anthropic-version", "2023-06-01"},
      {"anthropic-beta", kAntFilesBeta},
  };
  if (contentType) h.push_back({"Content-Type", contentType});
  return h;
}
}  // namespace

int antUploadImages(const ProviderDeps& pd, orch::TurnImages& imgs) {
  if (pd.routerBase && !pd.routerBase().empty()) return 0;   // a router head stays inline
  // Back-pressure: an upload is only made when its deletion can be queued, so no photo
  // is ever left at the provider because the delete queue was full.
  if (pendingDeletes().size() + imgs.items.size() > kMaxPendingDeletes) {
    hlog::logf("anthropic: %u uploads still to delete - this turn's photos go inline",
               (unsigned)pendingDeletes().size());
    return 0;
  }
  int uploaded = 0;
  for (size_t i = 0; i < imgs.items.size(); ++i) {
    orch::TurnImage& it = imgs.items[i];
    if (!it.fileId.empty() || !it.bytes || !it.len) continue;
    const std::string kBoundary = boundaryFor(it, imgs.nonce);
    if (kBoundary.empty()) continue;
    std::string body = std::string("--") + kBoundary +
                       "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + it.ref + "." +
                       extOf(it.mime) + "\"\r\nContent-Type: " + it.mime + "\r\n\r\n";
    BodySplices sp{BodySplice{body.size(), it.bytes, it.len, false}};
    body += std::string("\r\n--") + kBoundary + "--\r\n";
    JsonDocument filter;
    filter["id"] = true;
    filter["error"]["message"] = true;
    JsonDocument resp;
    const std::string ct = std::string("multipart/form-data; boundary=") + kBoundary;
    const int code = wire::exchange(pd, kAntHost, 443, true, "POST", "/v1/files",
                                    antFileHeaders(pd, ct.c_str()), std::move(body), 60000, resp, filter,
                                    nullptr, &sp);
    const char* id = resp["id"] | "";
    if (code >= 200 && code < 300 && id[0]) {
      it.fileId = id;
      it.fileHost = "anthropic";
      ++uploaded;
      continue;
    }
    hlog::logf("anthropic: photo %d/%d upload HTTP %d (%s) - sent inline", it.k, it.n, code,
               orch::oneLineText(resp["error"]["message"] | "", 80).c_str());
    if (code <= 0) break;   // the network is down: the rest would fail the same way
  }
  if (uploaded) hlog::logf("anthropic: %d photo(s) uploaded once for this turn", uploaded);
  return uploaded;
}

}  // namespace imageparts

size_t pendingUploadDeletes() { return imageparts::pendingDeletes().size(); }

int deletePendingUploads(const ProviderDeps& pd, int max) {
  auto& q = imageparts::pendingDeletes();
  int done = 0;
  while (!q.empty() && done < max) {
    const std::string id = nimbus::orch::plainCopy(q.front());
    q.erase(q.begin());
    JsonDocument filter, resp;
    filter["id"] = true;
    const int code = wire::exchange(pd, imageparts::kAntHost, 443, true, "DELETE", "/v1/files/" + id,
                                    imageparts::antFileHeaders(pd, nullptr), std::string(), 20000, resp, filter);
    if ((code >= 200 && code < 300) || code == 404) {
      ++done;
      continue;
    }
    if (code > 0 && code < 500 && code != 429) {   // refused for good (the key changed)
      hlog::logf("anthropic: photo upload not deleted (HTTP %d) - dropped", code);
      ++done;
      continue;
    }
    hlog::logf("anthropic: photo upload not deleted yet (HTTP %d) - retried later", code);
    q.push_back(nimbus::orch::workingCopy(id));   // kept for a later idle step (a network blip, a 5xx, a rate limit)
    break;
  }
  return done;
}

namespace imageparts {

}  // namespace imageparts
}  // namespace providers
}  // namespace agent
