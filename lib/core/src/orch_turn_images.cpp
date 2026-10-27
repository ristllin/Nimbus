#include "nimbus/orch/turn_images.h"

#include <atomic>
#include <cstring>

#include "nimbus/orch/model_catalog.h"   // gptGeneration: the one "gpt-5 and newer" rule

// The photos of the head turn in flight (see turn_images.h). Pure logic plus one
// single-task slot; the bytes belong to whoever filled the set (its release hook).
namespace nimbus {
namespace orch {

namespace {

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

bool openaiTakesImages(const std::string& m) {
  for (const char* no : {"audio", "realtime", "transcribe", "tts", "image", "search", "embedding",
                         "moderation", "whisper", "dall-e", "sora"})
    if (has(m, no)) return false;
  if (startsWith(m, "gpt-4o") || startsWith(m, "gpt-4.1")) return true;
  if (gptGeneration(m) >= 5) return true;
  // The o-series with image input: o1, o3, o4-mini (o1-mini and o3-mini take none).
  return m == "o1" || startsWith(m, "o1-2") || m == "o3" || startsWith(m, "o3-2") || startsWith(m, "o4-mini");
}

bool mistralTakesImages(const std::string& m) {
  for (const char* no : {"codestral", "devstral", "voxtral", "ocr", "embed", "moderation"})
    if (has(m, no)) return false;
  for (const char* yes : {"small", "medium", "large", "ministral", "magistral", "pixtral"})
    if (has(m, yes)) return true;
  return false;
}

// The routed text an offer was written for, by its FNV-1a digest (the text itself
// can be large and already lives in the channel's turn).
uint64_t textDigest(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

struct Offer {
  std::unique_ptr<TurnImages> imgs;
  std::string chatId;
  uint64_t digest = 0;
};
Offer& offer() {
  static Offer o;
  return o;
}
TurnImages*& active() {
  static TurnImages* a = nullptr;
  return a;
}

}  // namespace

const char* imagePartMime(const std::string& mime) {
  const std::string m = lower(mime);
  if (m == "image/jpeg" || m == "image/jpg") return "image/jpeg";
  if (m == "image/png") return "image/png";
  if (m == "image/webp") return "image/webp";
  if (m == "image/gif") return "image/gif";
  return "";
}

bool isHeic(const std::string& mime) {
  const std::string m = lower(mime);
  return m == "image/heic" || m == "image/heif" || m == "image/heic-sequence" || m == "image/heif-sequence";
}

bool animatedGif(const uint8_t* bytes, size_t len) {
  if (!bytes || len < 6 || std::memcmp(bytes, "GIF8", 4) != 0) return false;
  static const char kLoop[] = "NETSCAPE2.0";
  int controls = 0;
  for (size_t i = 0; i + 3 <= len; ++i) {
    if (i + sizeof(kLoop) - 1 <= len && std::memcmp(bytes + i, kLoop, sizeof(kLoop) - 1) == 0) return true;
    // A graphic control extension precedes each frame: two of them = two frames.
    if (bytes[i] == 0x21 && bytes[i + 1] == 0xF9 && bytes[i + 2] == 0x04 && ++controls >= 2) return true;
  }
  return false;
}

const char* sniffImageMime(const uint8_t* b, size_t n) {
  if (!b) return "";
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
  if (n >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
  if (n >= 6 && (std::memcmp(b, "GIF87a", 6) == 0 || std::memcmp(b, "GIF89a", 6) == 0)) return "image/gif";
  if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return "image/webp";
  return "";
}

namespace {
std::atomic<ImageInputLookup> g_imageInput{nullptr};
}  // namespace

void installImageInputLookup(ImageInputLookup f) { g_imageInput.store(f); }

namespace {
// `viaRouter`: judged as a router selector's upstream, where the device may hold no list
// of that upstream (a Cumulo-only device): the name rule decides when no list says.
bool takesImages(const std::string& host, const std::string& model, bool viaRouter) {
  const std::string h = lower(host), m = lower(model);
  // The provider's own word first (a router selector is judged by its upstream below).
  const ImageInputLookup f = g_imageInput.load();
  const int known = (f && h != "cumulo") ? f(host.c_str(), model.c_str()) : -1;
  if (known >= 0) return known == 1;
  if (h == "anthropic") return m.empty() || startsWith(m, "claude-");
  if (h == "openai") return openaiTakesImages(m);
  // Mistral says per model; a direct head the device's list does not know yet (before the
  // first fetch) reads the photos as their transcriptions, never by a guess from its name. A
  // device with no Mistral list at all (-2: a Cumulo-only device routing to Mistral) keeps the
  // name rule, so intake and the renderer agree.
  if (h == "mistral") return (f && !viaRouter && known == -1) ? false : mistralTakesImages(m);
  if (h == "cumulo") {
    const size_t slash = m.find('/');
    if (slash == std::string::npos || slash == 0) return false;
    const std::string up = m.substr(0, slash);
    return up != "cumulo" && takesImages(up, m.substr(slash + 1), true);
  }
  return false;   // zai, custom, unknown
}
}  // namespace

bool headTakesImages(const std::string& host, const std::string& model) { return takesImages(host, model, false); }

TurnImages::~TurnImages() {
  if (release) release(*this);
}

bool TurnImages::holds(const std::string& ref) const {
  for (const TurnImage& it : items)
    if (!ref.empty() && it.ref == ref) return true;
  return false;
}

size_t TurnImages::bytes() const {
  size_t n = 0;
  for (const auto& it : items) n += it.len;
  return n;
}

// Closed by a colon, so no marker is a prefix of another ("nimg:n:1:" vs "nimg:n:10:").
std::string TurnImages::marker(size_t i) const { return "nimg:" + nonce + ":" + std::to_string(i) + ":"; }

const std::string& TurnImages::fallbackOf(size_t i) {
  TurnImage& it = items[i];
  if (it.fallback.empty() && transcribe) it.fallback = transcribe(it);
  if (it.fallback.empty()) it.fallback = it.tag + " (not read: no provider could read it)";
  return it.fallback;
}

std::string TurnImages::blindText(const std::string& text) {
  std::string out = text;
  std::string unplaced;
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string& seg = items[i].segment;
    const std::string& fb = fallbackOf(i);
    const size_t at = seg.empty() ? std::string::npos : out.find(seg);
    if (at == std::string::npos) {
      // Idempotent: a text already blinded (the engine blinds a single-shot host's
      // inputs, then the provider path blinds what it renders) keeps one copy.
      if (out.find(fb) == std::string::npos) unplaced += "\n" + fb;
      continue;
    }
    out.replace(at, seg.size(), fb);
  }
  return unplaced.empty() ? out : out + unplaced;
}

const char* admitImage(const TurnImages& set, const uint8_t* bytes, size_t len, const char*& why) {
  why = "";
  const char* mime = sniffImageMime(bytes, len);
  if (set.items.size() >= kTurnImageMax) why = "the turn's photo limit";
  else if (len > kImagePartBytes) why = "too large";
  else if (set.bytes() + len > kTurnImageBytes) why = "over the turn's photo budget";
  else if (!*mime) why = "not an image by its bytes";
  else if (animatedGif(bytes, len)) why = "an animated GIF";
  return *why ? "" : mime;
}

TurnImage attachedImage(const MediaLabel& label, const uint8_t* bytes, size_t len, const std::string& mime) {
  TurnImage it;
  it.ref = label.ref;
  it.mime = imagePartMime(mime);
  if (it.mime.empty()) it.mime = "image/jpeg";
  it.bytes = bytes;
  it.len = len;
  it.k = label.k;
  it.n = label.n;
  it.tag = mediaTag(label);
  MediaLabel a = label;
  a.cls = MediaClass::Attached;
  it.segment = mediaSegment(a);
  return it;
}

namespace {
struct PendingSpend {
  std::string provider, model;
  TokenUsage use;
};
std::vector<PendingSpend>& pendingSpend() {
  static std::vector<PendingSpend> v;
  return v;
}
}  // namespace

void addVisionSpend(const char* provider, const char* model, const TokenUsage& use) {
  if (!provider || !*provider || use.empty()) return;
  for (auto& p : pendingSpend())
    if (p.provider == provider && p.model == (model ? model : "")) {
      p.use += use;
      return;
    }
  if (pendingSpend().size() >= 4) {   // bounded: merge into the provider's first record
    for (auto& p : pendingSpend())
      if (p.provider == provider) {
        p.use += use;
        return;
      }
    pendingSpend().front().use += use;
    return;
  }
  pendingSpend().push_back({provider, model ? model : "", use});
}

void takeVisionSpend(TurnSpend& spend) {
  for (const auto& p : pendingSpend()) spend.add(p.provider.c_str(), p.model.c_str(), p.use);
  pendingSpend().clear();
}

void offerTurnImages(const std::string& chatId, const std::string& routedText,
                     std::unique_ptr<TurnImages> imgs) {
  Offer& o = offer();
  o.imgs.reset();
  if (!imgs || imgs->empty()) return;
  o.imgs = std::move(imgs);
  o.chatId = chatId;
  o.digest = textDigest(routedText);
}

std::unique_ptr<TurnImages> takeTurnImages(const std::string& chatId, const std::string& routedText) {
  Offer& o = offer();
  std::unique_ptr<TurnImages> got = std::move(o.imgs);
  o.imgs.reset();
  if (!got || o.chatId != chatId || o.digest != textDigest(routedText)) return nullptr;
  return got;
}

void dropTurnImages() { offer().imgs.reset(); }

TurnImages* activeTurnImages() { return active(); }

ActiveTurnImages::ActiveTurnImages(TurnImages* imgs) : prev_(active()) { active() = imgs; }
ActiveTurnImages::~ActiveTurnImages() { active() = prev_; }

}  // namespace orch
}  // namespace nimbus
