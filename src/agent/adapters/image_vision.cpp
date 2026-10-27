#include "image_vision.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>   // esp_random - the per-turn marker nonce

#include "../../sys/agent_log.h"
#include "../../sys/heap_watch.h"   // activity tag: vision
#include "../agent_config.h"        // CUMULO_HOST_DEFAULT
#include "../memory_subsystem.h"    // dataFs, the photo's stored copy and its row
#include "../orchestrator.h"        // inScheduledTurn, firstAllowedChat - the billing notice
#include "../store.h"
#include "../telegram.h"            // send - the billing notice
#include "../transport_tls.h"       // deviceTransport, deviceProviderDeps - the one TLS path
#include "nimbus/harness/providers.h"   // deletePendingUploads - the idle step
#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/media_label.h"
#include "nimbus/orch/provider_park.h"

namespace agent {
namespace vision {

namespace {

String g_failWords;
bool g_uploadOnce = true;
bool g_native = true;

// The router's bare host ("" without a Cumulo key).
std::string routerHost() {
  if (!store::cumuloKey().length()) return std::string();
  String base = store::cumuloBase();
  if (!base.length()) base = CUMULO_HOST_DEFAULT;
  const int sch = base.indexOf("://");
  if (sch >= 0) base = base.substring(sch + 3);
  const int sl = base.indexOf('/');
  if (sl >= 0) base = base.substring(0, sl);
  return std::string(base.c_str());
}

std::string keyOf(const std::string& provider) {
  if (provider == "openai") return store::openaiKey().c_str();
  if (provider == "mistral") return store::mistralKey().c_str();
  if (provider == "anthropic") return store::anthropicKey().c_str();
  if (provider == "cumulo") return store::cumuloKey().c_str();
  return std::string();
}

CaptionDeps deps() {
  CaptionDeps d;
  d.http = &deviceTransport();
  d.key = keyOf;
  d.priority = [] { return std::string(store::providerPriority().c_str()); };
  d.nowMs = [] { return (uint32_t)millis(); };
  d.routerHost = routerHost;
  d.routerSelection = [] { return std::string(store::orchModel("cumulo").c_str()); };
  // The caption model: the role default from the provider's catalog (lane DEF).
  d.model = [](const std::string& p) { return std::string(store::captionModel(String(p.c_str())).c_str()); };
  // One owner notice per billing episode, shared with every seam; a quiet background
  // turn leaves it for the next owner turn.
  d.ownerReachable = [] {
    return !orchestrator::inScheduledTurn() && orchestrator::firstAllowedChat().length() > 0;
  };
  d.notifyOwner = [](const std::string& words) {
    const String owner = orchestrator::firstAllowedChat();
    if (owner.length()) agent::telegram::send(owner, String(words.c_str()), /*block=*/false);
  };
  return d;
}

void freeBytes(nimbus::orch::TurnImages& set) {
  for (auto& it : set.items) {
    free(const_cast<uint8_t*>(it.bytes));
    it.bytes = nullptr;
  }
}

// The turn being built (one at a time, on the poll task).
std::unique_ptr<nimbus::orch::TurnImages> g_set;

// A photo's fallback segment for a head that cannot see it: its transcription, or
// why there is none (the typed label).
std::string transcribeFor(const nimbus::orch::TurnImage& it) {
  const CaptionResult r = read(it.bytes, it.len, it.mime.c_str(), String(), CaptionKind::Fallback);
  nimbus::orch::MediaLabel l;
  l.kind = nimbus::orch::MediaKind::Image;
  if (!r.text.empty()) {
    l.cls = nimbus::orch::MediaClass::Transcribed;
    l.detail = r.text;
    l.by = r.provider;
  } else {
    l.cls = nimbus::orch::MediaClass::NotRead;
    l.detail = r.words;
  }
  return it.tag + nimbus::orch::mediaSuffix(l);
}

}  // namespace

bool available() {
  const uint32_t now = (uint32_t)millis();
  for (const char* p : {"openai", "mistral", "anthropic", "cumulo"}) {
    const std::string key = keyOf(p);
    if (key.empty() || !*captionModel(p == std::string("cumulo") ? "openai" : p)) continue;
    if (!strcmp(p, "cumulo") && routerHost().empty()) continue;
    if (nimbus::orch::providerPark()
            .available(p, nimbus::orch::PaidSeam::Vision, nimbus::orch::parkKeyFingerprint(key), now)
            .open)
      return true;
  }
  return false;
}

CaptionResult read(const uint8_t* bytes, size_t len, const char* mime, const String& context, CaptionKind kind,
                   const String& question) {
  nimbus::heapwatch::Scope describing(nimbus::heapwatch::kVision);
  CaptionRequest in;
  in.bytes = bytes;
  in.len = len;
  in.mime = mime && *mime ? nimbus::orch::imagePartMime(mime) : "image/jpeg";
  if (in.mime.empty()) in.mime = "image/jpeg";
  in.context = context.c_str();
  in.question = question.c_str();
  in.kind = kind;
  CaptionResult r = caption(deps(), in);
  g_failWords = r.text.empty() ? String(r.words.c_str()) : String();
  // T's per-turn spend: what the caption step billed rides the next turn's record, each
  // provider tried on its own price and limit (lane QW4).
  for (const CaptionResult::Spend& s : r.spends)
    nimbus::orch::addVisionSpend(s.provider.c_str(), s.model.c_str(), s.usage);
  return r;
}

uint8_t* loadToPsram(const char* path, ::fs::FS& fsys, size_t maxBytes, size_t& len, const char*& why) {
  len = 0;
  why = "";
  File f = fsys.open(path, FILE_READ);
  if (!f) { why = "could not be fetched"; return nullptr; }
  const size_t n = f.size();
  if (!n || n > maxBytes) {
    f.close();
    why = n ? "too large" : "could not be fetched";
    return nullptr;
  }
  // PSRAM only: a photo on the internal heap would take the memory a TLS handshake
  // needs moments later. No PSRAM, no photo (the label says the device was low).
  uint8_t* buf = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!buf) {
    f.close();
    why = "device low on memory";
    return nullptr;
  }
  const size_t got = f.read(buf, n);
  f.close();
  if (got != n) {
    free(buf);
    why = "could not be fetched";
    return nullptr;
  }
  len = n;
  return buf;
}

String describeImage(const char* path, const char* mime, ::fs::FS* sourceFs, const String& caption) {
  g_failWords = "";
  if (!path || !*path) return String();
  size_t len = 0;
  const char* why = "";
  uint8_t* buf = loadToPsram(path, sourceFs ? *sourceFs : (::fs::FS&)LittleFS, nimbus::orch::kImagePartBytes, len, why);
  if (!buf) {
    g_failWords = why;
    alogf("vision: %s: %s", path, why);
    return String();
  }
  const CaptionResult r = read(buf, len, mime, caption, CaptionKind::Fallback);
  free(buf);
  return String(r.text.c_str());
}

String failWords() { return g_failWords; }

namespace {
// Is the photo `ref` in chat `ns`'s recent rows (the photos and files it sent)?
bool chatHasPhoto(const std::string& ns, const std::string& ref) {
  nimbus::orch::MsgQuery q;
  q.sessionId = ns;
  q.haveKind = true;
  q.kind = nimbus::orch::MsgKind::Image;
  q.alsoKinds = {nimbus::orch::MsgKind::File};
  q.limit = 200;
  memory::Lock lk;
  for (const auto& m : memory::episodic().query(q))
    if (nimbus::orch::blobHashOf(m.blobPath) == ref) return true;
  return false;
}
}  // namespace

// Never both: a photo of this turn the head already has as an image is not read a
// second time (on 26 Oct such a second read held the turn for two minutes and failed).
static bool shownThisTurn(const std::string& ref) {
  const nimbus::orch::TurnImages* cur = nimbus::orch::activeTurnImages();
  return cur && cur->shown && !cur->rejected && cur->holds(ref);
}

std::string look(const std::string& ref, const std::string& question, const std::string& ns, bool owner,
                 std::string& err) {
  if (!lookArgsValid(ref, question, err)) return std::string();
  if (shownThisTurn(ref))
    return "This photo came with the current message and is already in front of you as an image: "
           "answer from what you see in it.";
  if (!memory::haveSd()) {
    err = "no memory card: earlier photos are not kept";
    return std::string();
  }
  // A caller that is not the owner looks only at its own chat's photos; an unnamed
  // chat (empty ns) is the most restrictive case, never "any chat".
  if (!owner && (ns.empty() || !chatHasPhoto(ns, ref))) {
    err = "no photo with that ref in this chat";
    return std::string();
  }
  size_t len = 0;
  const char* why = "";
  uint8_t* buf = nullptr;
  for (const char* ext : {"jpg", "bin"}) {
    const std::string path = nimbus::orch::blobPath("/mem/blobs", ref, ext);
    memory::Lock lk;
    if (!memory::dataFs().exists(path.c_str())) continue;
    buf = loadToPsram(path.c_str(), memory::dataFs(), nimbus::orch::kImagePartBytes, len, why);
    break;
  }
  if (!buf) {
    err = why[0] ? std::string("the photo could not be read: ") + why : "no stored photo with that ref";
    return std::string();
  }
  const char* mime = nimbus::orch::sniffImageMime(buf, len);
  if (!*mime) {
    free(buf);
    err = "that ref is not a photo";
    return std::string();
  }
  const CaptionResult r = read(buf, len, mime, String(), CaptionKind::Question, String(question.c_str()));
  free(buf);
  if (r.text.empty()) err = "the photo could not be read: " + r.words;
  return r.text;
}

nimbus::orch::TurnImages* turnSet(bool create) {
  if (!g_set && create) {
    g_set.reset(new nimbus::orch::TurnImages());
    char nonce[17];
    snprintf(nonce, sizeof nonce, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    g_set->nonce = nonce;
    g_set->uploadOnce = g_uploadOnce;
    g_set->release = freeBytes;
    g_set->transcribe = transcribeFor;
  }
  return g_set.get();
}

void offerTurnSet(const String& chatId, const String& routedText) {
  if (!g_set || g_set->empty()) {
    g_set.reset();
    return;
  }
  alogf("vision: %u photo(s), %u bytes, go to the head as images", (unsigned)g_set->items.size(),
        (unsigned)g_set->bytes());
  nimbus::orch::offerTurnImages(chatId.c_str(), routedText.c_str(), std::move(g_set));
}

void dropTurnSet() {
  g_set.reset();
  nimbus::orch::dropTurnImages();
}

bool nativeHead() {
  if (!g_native) return false;
  const String h = store::resolvedOrchHost();
  return nimbus::orch::headTakesImages(h.c_str(), store::orchModel(h).c_str());
}
void setNative(bool on) { g_native = on; }
bool native() { return g_native; }

void setUploadOnce(bool on) { g_uploadOnce = on; }
bool uploadOnce() { return g_uploadOnce; }

bool idleDue() {
  if (providers::pendingUploadDeletes()) return true;
  return recordQueue().due((uint32_t)millis(), available()) != nullptr;
}

void idleStep() {
  if (providers::pendingUploadDeletes()) {
    providers::deletePendingUploads(deviceProviderDeps(), 1);
    return;
  }
  const PendingRecord* due = recordQueue().due((uint32_t)millis(), available());
  if (!due) return;
  const PendingRecord rec = *due;   // settle() reorders the queue
  const std::string path = nimbus::orch::blobPath("/mem/blobs", nimbus::orch::plainCopy(rec.ref),
                                                  rec.ext.empty() ? "jpg" : nimbus::orch::plainCopy(rec.ext));
  size_t len = 0;
  const char* why = "";
  uint8_t* buf = nullptr;
  {
    memory::Lock lk;   // the card: one reader at a time (the idle step holds no turn)
    buf = loadToPsram(path.c_str(), memory::dataFs(), nimbus::orch::kImagePartBytes, len, why);
  }
  if (!buf) {
    alogf("vision: record for %s skipped: %s", rec.ref.c_str(), why);
    recordQueue().settle(rec.ref, true, (uint32_t)millis());   // nothing to read: give up
    return;
  }
  const CaptionResult r = read(buf, len, rec.mime.c_str(), String(rec.context.c_str()), CaptionKind::Memory);
  free(buf);
  if (r.text.empty()) {
    alogf("vision: record for %s not written (%s)", rec.ref.c_str(), r.words.c_str());
    recordQueue().settle(rec.ref, false, (uint32_t)millis());
    return;
  }
  nimbus::orch::MediaLabel l;
  l.cls = nimbus::orch::MediaClass::Transcribed;
  l.detail = r.text;
  l.by = r.provider;
  const std::string text = nimbus::orch::plainCopy(rec.tag) + nimbus::orch::mediaSuffix(l);
  const bool ok =
      memory::replaceMediaRowText(rec.chatId.c_str(), nimbus::orch::plainCopy(rec.ref), String(text.c_str()));
  alogf("vision: record for %s %s", rec.ref.c_str(), ok ? "written" : "has no row to write to");
  recordQueue().settle(rec.ref, true, (uint32_t)millis());
}

String lastErrJson() {
  const LastError e = lastError();
  if (!e.atMs) return String();
  String out = "{\"atS\":" + String((unsigned long)(e.atMs / 1000)) + ",\"provider\":\"" + e.provider.c_str() +
               "\",\"class\":\"" + readFailSlug(e.fail) + "\",\"status\":" + String(e.status) + "}";
  return out;
}

}  // namespace vision
}  // namespace agent
