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

// One image part for `it`, its bytes at `marker` (spliced at send time) unless it
// rides as an Anthropic file reference.
void putImage(ArduinoJson::JsonArray parts, const orch::TurnImage& it, const std::string& marker, Dialect d) {
  ArduinoJson::JsonObject p = parts.add<ArduinoJson::JsonObject>();
  auto dataUrl = [&] { return std::string("data:") + it.mime + ";base64," + marker; };
  switch (d) {
    case Dialect::Anthropic:
      p["type"] = "image";
      if (!it.fileId.empty() && it.fileHost == "anthropic") {
        p["source"]["type"] = "file";
        p["source"]["file_id"] = it.fileId;
      } else {
        p["source"]["type"] = "base64";
        p["source"]["media_type"] = it.mime;
        p["source"]["data"] = marker;
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
  if (!imgs || imgs->empty()) return nullptr;
  // The latest render decides what the head has: image.look on these photos answers without a
  // second request only while the host in use sees them (a failover to a blind host reads them
  // as transcriptions again).
  imgs->shown = !imgs->rejected && orch::headTakesImages(host, model);
  return imgs->shown ? imgs : nullptr;
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
    putImage(parts, imgs.items[i], imgs.marker(i), d);
  }
  putText(parts, text, d);
}

namespace {
struct Hit {
  size_t at;
  const orch::TurnImage* it;
  size_t len;
};

// The seeded photos' markers (strict: a missing one fails the request).
bool photoHits(const std::string& body, const orch::TurnImages& imgs, std::vector<Hit>& hits, std::string& err) {
  for (size_t i = 0; i < imgs.items.size(); ++i) {
    if (!inlineImage(imgs.items[i])) continue;
    const std::string mk = imgs.marker(i);
    const size_t at = body.find(mk);
    if (at == std::string::npos) {
      err = "image " + std::to_string(i + 1) + " is missing from the request";
      hlog::logf("wire: %s", err.c_str());
      return false;
    }
    hits.push_back({at, &imgs.items[i], mk.size()});
  }
  return true;
}

// Cut every hit's marker out of `body`, recording its splice (base64 at send time).
void cutHits(std::string& body, std::vector<Hit>& hits, BodySplices& out) {
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });
  std::string cut;
  cut.reserve(body.size());
  size_t from = 0;
  for (const Hit& h : hits) {
    cut.append(body, from, h.at - from);
    out.push_back(BodySplice{cut.size(), h.it->bytes, h.it->len, true});
    from = h.at + h.len;
  }
  cut.append(body, from, std::string::npos);
  body.swap(cut);
}
}  // namespace

bool spliceMarkers(std::string& body, const orch::TurnImages& imgs, BodySplices& out, std::string& err) {
  return spliceAll(body, &imgs, true, out, err);
}

bool spliceAll(std::string& body, const orch::TurnImages* imgs, bool photos, BodySplices& out, std::string& err) {
  if (!imgs) return true;
  std::vector<Hit> hits;
  if (photos && !photoHits(body, *imgs, hits, err)) return false;
  for (size_t j = 0; j < imgs->looked.size(); ++j) {
    const orch::TurnImage& it = imgs->looked[j].img;
    if (!inlineImage(it)) continue;
    const std::string mk = imgs->lookMarker(j);
    const size_t at = body.find(mk);
    if (at != std::string::npos) hits.push_back({at, &it, mk.size()});
  }
  cutHits(body, hits, out);
  return true;
}

// ---- image.look: an earlier photo shown again ---------------------------------------

bool sees(const orch::Transcript& tr, const std::string& host, const std::string& model) {
  const orch::TurnImages* imgs = tr.images();
  return imgs && !imgs->rejected && orch::headTakesImages(host, model);
}

int lookedFor(const orch::TurnImages* imgs, const orch::TranscriptItem& result) {
  if (!imgs || imgs->looked.empty() || result.kind != orch::TranscriptItem::Kind::ToolResult || result.isError)
    return -1;
  if (result.name != "image.look" && result.name != "image_look") return -1;
  return imgs->lookedIndex(orch::lookResultRef(result.text));
}

std::string lookText(orch::TurnImages& imgs, size_t j, bool showing, Dialect d) {
  if (!showing) return imgs.lookAnswer(j);
  const std::string& tag = imgs.looked[j].img.tag;
  return d == Dialect::Anthropic
             ? tag + " The photo is attached to this result as an image: answer from what you see in it."
             : tag + " The photo follows in the next message as an image: answer from what you see in it.";
}

void putLookResult(ArduinoJson::JsonObject toolResult, orch::TurnImages& imgs, size_t j) {
  ArduinoJson::JsonArray c = toolResult["content"].to<ArduinoJson::JsonArray>();
  putText(c, lookText(imgs, j, true, Dialect::Anthropic), Dialect::Anthropic);
  putImage(c, imgs.looked[j].img, imgs.lookMarker(j), Dialect::Anthropic);
}

void putLookMessage(ArduinoJson::JsonArray msgs, const orch::TurnImages& imgs, const std::vector<size_t>& js,
                    Dialect d) {
  if (js.empty()) return;
  ArduinoJson::JsonObject m = msgs.add<ArduinoJson::JsonObject>();
  if (d == Dialect::OpenAIResponses) m["type"] = "message";
  m["role"] = "user";
  ArduinoJson::JsonArray parts = m["content"].to<ArduinoJson::JsonArray>();
  for (size_t j : js) {
    putText(parts, imgs.looked[j].img.tag + " (the photo image.look shows you again)", d);
    putImage(parts, imgs.looked[j].img, imgs.lookMarker(j), d);
  }
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
  imgs->shown = false;
  imgs->sent = false;   // refused: no request of this turn shows them again
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
void queueDelete(const orch::WorkingString& id) {
  auto& q = pendingDeletes();
  if (q.size() >= kMaxPendingDeletes) {
    hlog::logf("anthropic: upload delete queue full - %s stays at the provider", q.front().c_str());
    q.erase(q.begin());
  }
  q.push_back(id);
}
void queueDeletes(orch::TurnImages& imgs) {
  for (auto& it : imgs.items) {
    if (it.fileId.empty() || it.fileHost != "anthropic") continue;
    queueDelete(orch::workingCopy(it.fileId));
    it.fileId.clear();
    it.fileHost.clear();
  }
}

// ---- re-attach: the kept file ids -------------------------------------------------
struct FileRef {
  orch::WorkingString ref, id;
  orch::WorkingString chat;   // the per-chat cache key of the turn it was uploaded in
  uint8_t idle = 0;   // head loops since it last rode a request
  bool used = false;  // rode a request in the current loop
};
orch::WorkingVector<FileRef>& fileRefs() {
  static orch::WorkingVector<FileRef> v;
  return v;
}
orch::WorkingString& loopChat() {
  static orch::WorkingString c;
  return c;
}

// The kept file of `ref` for the loop's chat (nullptr = none).
FileRef* findRef(const std::string& ref) {
  for (auto& f : fileRefs())
    if (!ref.empty() && f.ref == ref.c_str() && f.chat == loopChat()) return &f;
  return nullptr;
}

// Keep the turn's uploads for re-attach (their deletion is queued once unused); a set
// the provider refused, or a photo with no valid ref, is deleted as before.
void keepRef(const std::string& ref, const std::string& id) {
  auto& v = fileRefs();
  if (v.size() >= kMaxFileRefs) {   // the bound: the longest unused goes
    auto oldest =
        std::max_element(v.begin(), v.end(), [](const FileRef& a, const FileRef& b) { return a.idle < b.idle; });
    queueDelete(oldest->id);
    v.erase(oldest);
  }
  FileRef f;
  f.ref = orch::workingCopy(ref);
  f.id = orch::workingCopy(id);
  f.chat = loopChat();
  f.used = true;
  v.push_back(f);
}

void keepUploads(orch::TurnImages& imgs) {
  for (auto& it : imgs.items) {
    if (it.fileId.empty() || it.fileHost != "anthropic") continue;
    const FileRef* have = findRef(it.ref);
    if (!imgs.rejected && orch::validRef(it.ref) && !have) keepRef(it.ref, it.fileId);
    else if (!have || have->id != it.fileId.c_str()) queueDelete(orch::workingCopy(it.fileId));
    it.fileId.clear();
    it.fileHost.clear();
  }
}

// One head loop ended: an id that rode no request ages; past kFileRefIdleTurns its
// deletion is queued.
void ageFileRefs() {
  auto& v = fileRefs();
  for (size_t i = 0; i < v.size();) {
    FileRef& f = v[i];
    f.idle = f.used ? 0 : (uint8_t)(f.idle + 1);
    f.used = false;
    if (f.idle > kFileRefIdleTurns) {
      queueDelete(f.id);
      v.erase(v.begin() + (long)i);
      continue;
    }
    ++i;
  }
}

// The photo labels of a history text, in order: each bracket that names a ref with a
// kept id, once per ref.
struct Label {
  std::string tag;
  const FileRef* file;
};
std::vector<Label> keptLabels(const std::string& text) {
  std::vector<Label> out;
  for (size_t at = text.find("[image"); at != std::string::npos; at = text.find("[image", at + 1)) {
    const size_t close = text.find(']', at);
    if (close == std::string::npos || close - at > 120) continue;
    const std::string tag = text.substr(at, close - at + 1);
    const size_t r = tag.find(" ref=");
    if (r == std::string::npos || r + 5 + 16 > tag.size()) continue;
    const std::string ref = tag.substr(r + 5, 16);
    const FileRef* f = orch::validRef(ref) ? findRef(ref) : nullptr;
    if (!f) continue;
    bool dup = false;
    for (const Label& l : out) dup = dup || l.file == f;
    if (!dup) out.push_back({tag, f});
  }
  return out;
}
}  // namespace

void setLoopChat(const std::string& chatKey) { loopChat() = orch::workingCopy(chatKey); }

std::string fileRefFor(const std::string& ref) {
  const FileRef* f = findRef(ref);
  return f ? orch::plainCopy(f->id) : std::string();
}

void useFileRef(orch::TurnImage& it) {
  FileRef* f = findRef(it.ref);
  if (!f) return;
  f->used = true;
  it.fileId = orch::plainCopy(f->id);
  it.fileHost = "anthropic";
}

int reattachHistory(ArduinoJson::JsonArray messages, size_t count) {
  if (fileRefs().empty()) return 0;
  int added = 0;
  for (size_t i = 0; i < count && i < messages.size(); ++i) {
    ArduinoJson::JsonObject m = messages[i];
    if (strcmp(m["role"] | "", "user") != 0 || !m["content"].is<ArduinoJson::JsonArray>()) continue;
    ArduinoJson::JsonArray c = m["content"].as<ArduinoJson::JsonArray>();
    if (c.size() != 1) continue;   // one text block (prompt_layout's history shape)
    const std::vector<Label> kept = keptLabels(c[0]["text"] | "");
    if (kept.empty()) continue;
    JsonDocument textBlock;   // the text block, cache point and all, copied out first
    textBlock.set(c[0]);
    ArduinoJson::JsonArray parts = m["content"].to<ArduinoJson::JsonArray>();
    for (const Label& l : kept) {
      ArduinoJson::JsonObject t = parts.add<ArduinoJson::JsonObject>();
      t["type"] = "text";
      t["text"] = l.tag;
      ArduinoJson::JsonObject p = parts.add<ArduinoJson::JsonObject>();
      p["type"] = "image";
      p["source"]["type"] = "file";
      p["source"]["file_id"] = l.file->id.c_str();
      const_cast<FileRef*>(l.file)->used = true;
      ++added;
    }
    parts.add(textBlock.as<ArduinoJson::JsonObjectConst>());
  }
  return added;
}

void forgetFileRefs() {
  if (!fileRefs().empty()) hlog::logf("anthropic: %u kept photo file(s) refused - re-sent without them",
                                      (unsigned)fileRefs().size());
  fileRefs().clear();
}

void dropFileRefs() {
  for (const FileRef& f : fileRefs()) queueDelete(f.id);
  fileRefs().clear();
}

size_t fileRefCount() { return fileRefs().size(); }

bool refusedFileRefs(int httpStatus, const std::string& errText) {
  return (httpStatus == 400 || httpStatus == 404) && lower(errText).find("file") != std::string::npos;
}

bool bodyHasFileRef(const std::string& body) { return body.find("\"source\":{\"type\":\"file\"") != std::string::npos; }

LoopImages::LoopImages(orch::Transcript& tr) : tr_(tr) { tr.setImages(orch::activeTurnImages()); }
LoopImages::~LoopImages() {
  if (tr_.images()) {
    if (tr_.images()->uploadOnce) keepUploads(*tr_.images());
    else queueDeletes(*tr_.images());
  }
  ageFileRefs();
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
