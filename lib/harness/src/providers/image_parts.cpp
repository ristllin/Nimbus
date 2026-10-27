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
  imgs->shown = !imgs->refusedBy(host) && orch::headTakesImages(host, model);
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

// The seeded photos' markers (strict: a missing one fails the request). A photo with
// an Anthropic file id has no marker in an Anthropic request, but a request to another
// host after a failover writes it inline: its marker is spliced whenever present.
bool photoHits(const std::string& body, const orch::TurnImages& imgs, std::vector<Hit>& hits, std::string& err) {
  for (size_t i = 0; i < imgs.items.size(); ++i) {
    const std::string mk = imgs.marker(i);
    const size_t at = body.find(mk);
    if (at == std::string::npos && !inlineImage(imgs.items[i])) continue;
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

bool spliceAll(std::string& body, const orch::TurnImages* imgs, bool photos, BodySplices& out, std::string& err) {
  if (!imgs) return true;
  std::vector<Hit> hits;
  if (photos && !photoHits(body, *imgs, hits, err)) return false;
  for (size_t j = 0; j < imgs->looked.size(); ++j) {
    const orch::TurnImage& it = imgs->looked[j].img;   // a file id or not: the marker decides
    const std::string mk = imgs->lookMarker(j);
    // Every occurrence: a marker left in the body would reach the provider as text.
    for (size_t at = body.find(mk); at != std::string::npos; at = body.find(mk, at + mk.size()))
      hits.push_back({at, &it, mk.size()});
  }
  cutHits(body, hits, out);
  return true;
}

// ---- image.look: an earlier photo shown again ---------------------------------------

bool sees(const orch::Transcript& tr, const std::string& host, const std::string& model) {
  const orch::TurnImages* imgs = tr.images();
  return imgs && !imgs->refusedBy(host) && orch::headTakesImages(host, model);
}

namespace {
// The looked photo a transcript entry shows again (-1 = none): an image.look result
// (lookResultText) naming a photo of the set's `looked`.
int lookedFor(const orch::TurnImages* imgs, const orch::TranscriptItem& result) {
  if (!imgs || imgs->looked.empty() || result.kind != orch::TranscriptItem::Kind::ToolResult || result.isError)
    return -1;
  if (result.name != "image.look" && result.name != "image_look") return -1;
  return imgs->lookedIndex(orch::lookResultRef(result.text));
}
}  // namespace

LookRender::LookRender(const orch::Transcript& tr, bool seesImages, Dialect dialect)
    : imgs(tr.images()), sees(seesImages), d(dialect) {}

LookRender::Show LookRender::render(const orch::TranscriptItem& result, std::string& text, size_t& j) {
  const int look = lookedFor(imgs, result);
  if (look < 0) return Show::NotALook;
  j = (size_t)look;
  if (!sees) {
    text = imgs->lookAnswer(j);
    return Show::Text;
  }
  const std::string& tag = imgs->looked[j].img.tag;
  if (std::find(attached.begin(), attached.end(), j) != attached.end()) {   // the same photo asked again
    text = tag + " The photo is shown above in this conversation: answer from what you see in it.";
    return Show::Text;
  }
  attached.push_back(j);
  if (d != Dialect::Anthropic) pending.push_back(j);
  text = d == Dialect::Anthropic
             ? tag + " The photo is attached to this result as an image: answer from what you see in it."
             : tag + " The photo follows in the next message as an image: answer from what you see in it.";
  return Show::Attach;
}

void LookRender::putResult(ArduinoJson::JsonObject toolResult, const std::string& text, size_t j) const {
  ArduinoJson::JsonArray c = toolResult["content"].to<ArduinoJson::JsonArray>();
  putText(c, text, Dialect::Anthropic);
  putImage(c, imgs->looked[j].img, imgs->lookMarker(j), Dialect::Anthropic);
}

void LookRender::putMessage(ArduinoJson::JsonArray msgs) {
  if (pending.empty()) return;
  ArduinoJson::JsonObject m = msgs.add<ArduinoJson::JsonObject>();
  if (d == Dialect::OpenAIResponses) m["type"] = "message";
  m["role"] = "user";
  ArduinoJson::JsonArray parts = m["content"].to<ArduinoJson::JsonArray>();
  for (size_t j : pending) {
    putText(parts, imgs->looked[j].img.tag + " (the photo image.look shows you again)", d);
    putImage(parts, imgs->looked[j].img, imgs->lookMarker(j), d);
  }
  pending.clear();
}

void noteSent(orch::TurnImages* imgs, int httpStatus) {
  if (imgs && httpStatus == 200) imgs->sent = true;
}

bool refusedImages(int httpStatus, const std::string& errText) {
  if (httpStatus == 413) return true;
  if (httpStatus != 400 && httpStatus != 422) return false;
  const std::string e = lower(errText);
  return e.find("image") != std::string::npos;
}

bool rejectAndRetry(orch::TurnImages* imgs, const char* who, int httpStatus, const std::string& host) {
  if (!imgs || imgs->refusedBy(host)) return false;
  imgs->rejected = true;
  imgs->rejectedBy = host;
  imgs->shown = false;   // `sent` stays: a request that showed them earlier was answered
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
// ---- re-attach: the kept file ids -------------------------------------------------
struct FileRef {
  orch::WorkingString ref, id;
  orch::WorkingString chat;   // the per-chat cache key of the turn it was uploaded in
  uint32_t usedMs = 0;        // when it last rode a request (the device clock)
  uint32_t keptMs = 0;        // when it was kept (its upload's turn): the provider's expiry runs from about then
};
orch::WorkingVector<FileRef>& fileRefs() {
  static orch::WorkingVector<FileRef> v;
  return v;
}
struct LoopNow {
  orch::WorkingString chat;   // the loop in flight's chat (its cache key)
  uint32_t ms = 0;            // its clock
};
LoopNow& loopNow() {
  static LoopNow n;
  return n;
}

// The kept file of `ref` for the loop's chat (nullptr = none).
FileRef* findRef(const std::string& ref) {
  for (auto& f : fileRefs())
    if (!ref.empty() && f.ref == ref.c_str() && f.chat == loopNow().chat) return &f;
  return nullptr;
}

void touch(FileRef& f) { f.usedMs = loopNow().ms; }

// Keep an upload for re-attach; at the bound the longest unused one is deleted.
void keepRef(const std::string& ref, const std::string& id) {
  auto& v = fileRefs();
  if (v.size() >= kMaxFileRefs) {
    const uint32_t now = loopNow().ms;
    auto oldest = std::max_element(v.begin(), v.end(), [now](const FileRef& a, const FileRef& b) {
      return now - a.usedMs < now - b.usedMs;
    });
    queueDelete(oldest->id);
    v.erase(oldest);
  }
  FileRef f;
  f.ref = orch::workingCopy(ref);
  f.id = orch::workingCopy(id);
  f.chat = loopNow().chat;
  touch(f);
  f.keptMs = f.usedMs;
  v.push_back(f);
}

// The loop's uploads: kept for re-attach (`keep`), else deleted. A set the provider
// refused, or a photo with no valid ref, is always deleted.
void settleUploads(orch::TurnImages& imgs, bool keep) {
  for (auto& it : imgs.items) {
    if (it.fileId.empty() || it.fileHost != "anthropic") continue;
    const FileRef* have = findRef(it.ref);
    if (keep && !imgs.refusedBy("anthropic") && orch::validRef(it.ref) && !have) keepRef(it.ref, it.fileId);
    else if (!have || have->id != it.fileId.c_str()) queueDelete(orch::workingCopy(it.fileId));
    it.fileId.clear();
    it.fileHost.clear();
  }
}

// The photo labels of a history text, in order: each bracket that names a ref with a
// kept id not attached yet in this request (`done`): a photo rides its first, original
// message once, however often its label is repeated later.
struct Label {
  std::string tag;
  FileRef* file;
};
std::vector<Label> keptLabels(const std::string& text, std::vector<FileRef*>& done) {
  std::vector<Label> out;
  for (size_t at = text.find("[image"); at != std::string::npos; at = text.find("[image", at + 1)) {
    const size_t close = text.find(']', at);
    if (close == std::string::npos || close - at > 120) continue;
    const std::string tag = text.substr(at, close - at + 1);
    const size_t r = tag.find(" ref=");
    if (r == std::string::npos || r + 5 + 16 > tag.size()) continue;
    const std::string ref = tag.substr(r + 5, 16);
    FileRef* f = orch::validRef(ref) ? findRef(ref) : nullptr;
    if (!f || std::find(done.begin(), done.end(), f) != done.end()) continue;
    done.push_back(f);
    out.push_back({tag, f});
  }
  return out;
}

}  // namespace

void setLoopChat(const std::string& chatKey, uint32_t nowMs) {
  loopNow().chat = orch::workingCopy(chatKey);
  loopNow().ms = nowMs;
}

std::string fileRefFor(const std::string& ref) {
  const FileRef* f = findRef(ref);
  return f ? orch::plainCopy(f->id) : std::string();
}

void useFileRefs(orch::TurnImages& imgs) {
  for (orch::LookedImage& l : imgs.looked) {
    FileRef* f = findRef(l.img.ref);
    if (!f) continue;
    touch(*f);
    l.img.fileId = orch::plainCopy(f->id);
    l.img.fileHost = "anthropic";
  }
}

int reattachHistory(ArduinoJson::JsonArray messages, size_t count) {
  if (fileRefs().empty()) return 0;
  int added = 0;
  std::vector<FileRef*> done;
  for (size_t i = 0; i < count && i < messages.size(); ++i) {
    ArduinoJson::JsonObject m = messages[i];
    if (strcmp(m["role"] | "", "user") != 0 || !m["content"].is<ArduinoJson::JsonArray>()) continue;
    ArduinoJson::JsonArray c = m["content"].as<ArduinoJson::JsonArray>();
    if (c.size() != 1) continue;   // one text block (prompt_layout's history shape)
    const std::vector<Label> kept = keptLabels(c[0]["text"] | "", done);
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
      touch(*l.file);
      ++added;
    }
    parts.add(textBlock.as<ArduinoJson::JsonObjectConst>());
  }
  return added;
}

void forgetFileRefs(orch::TurnImages* imgs, const std::string& errText) {
  // The refusal names the file it could not find: only that one goes (the others still
  // ride their messages); a refusal naming none of them takes them all.
  auto& v = fileRefs();
  const auto named = [&errText](const FileRef& f) { return errText.find(f.id.c_str()) != std::string::npos; };
  const bool one = !errText.empty() && std::any_of(v.begin(), v.end(), named);
  for (size_t i = 0; i < v.size();) {
    if (one && !named(v[i])) {
      ++i;
      continue;
    }
    queueDelete(v[i].id);   // a gone id settles as 404; a live one is not left at the provider
    if (imgs)
      for (orch::LookedImage& l : imgs->looked)
        if (l.img.fileId == v[i].id.c_str()) l.img.fileId.clear();
    hlog::logf("anthropic: kept photo file %s refused - deleted, the round re-sent without it", v[i].id.c_str());
    v.erase(v.begin() + (long)i);
  }
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
  if (tr_.images()) settleUploads(*tr_.images(), tr_.images()->uploadOnce);
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
    // The provider deletes the upload by itself after kUploadExpiryS whatever happens
    // here (a restart loses the RAM ledger of kept ids), so no photo is left there for long.
    std::string body = std::string("--") + kBoundary +
                       "\r\nContent-Disposition: form-data; name=\"expires_in_seconds\"\r\n\r\n" +
                       std::to_string(kUploadExpiryS) + "\r\n--" + kBoundary +
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
namespace {

// A kept file past its use: unused for kKeptUploadIdleMs, or near the provider's own
// expiry (a reference to it would soon be refused).
constexpr uint32_t kKeptMaxAgeMs = kUploadExpiryS * 1000u - 20u * 60u * 1000u;
bool keptExpired(const FileRef& f, uint32_t nowMs) {
  return nowMs - f.usedMs > kKeptUploadIdleMs || nowMs - f.keptMs > kKeptMaxAgeMs;
}

// Queue the deletion of every kept file past its use.
int expireKept(uint32_t nowMs) {
  auto& v = fileRefs();
  int n = 0;
  for (size_t i = 0; i < v.size();) {
    if (!keptExpired(v[i], nowMs)) {
      ++i;
      continue;
    }
    queueDelete(v[i].id);
    v.erase(v.begin() + (long)i);
    ++n;
  }
  return n;
}

}  // namespace
}  // namespace imageparts

bool keptUploadsDue(uint32_t nowMs) {
  for (const auto& f : imageparts::fileRefs())
    if (imageparts::keptExpired(f, nowMs)) return true;
  return false;
}

int tendKeptUploads(uint32_t nowMs) { return imageparts::expireKept(nowMs); }

}  // namespace providers
}  // namespace agent
