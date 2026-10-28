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
  for (auto& l : set.looked) {
    free(const_cast<uint8_t*>(l.img.bytes));
    l.img.bytes = nullptr;
  }
}

// The turn being built (one at a time, on the poll task).
std::unique_ptr<nimbus::orch::TurnImages> g_set;

// A reading's segment: its transcription (named), or why there is none.
std::string readingSegment(const std::string& tag, const CaptionResult& r) {
  nimbus::orch::MediaLabel l;
  l.kind = nimbus::orch::MediaKind::Image;
  if (!r.text.empty()) {
    l.cls = nimbus::orch::MediaClass::Transcribed;
    l.detail = r.text;
    l.by = r.provider;
  } else {
    l.cls = nimbus::orch::MediaClass::NotRead;
    l.detail = notReadWords(r);
  }
  return tag + nimbus::orch::mediaSuffix(l);
}

// A photo's fallback segment for a host that cannot see it (made only when such a
// host renders the turn): its transcription, or why there is none. A reading that
// failed for a passing reason (a refusal, a closed provider, no answer) is read again
// once a provider is open (the turn's end queues it).
std::string transcribeFor(nimbus::orch::TurnImage& it) {
  const CaptionResult r = read(it.bytes, it.len, it.mime.c_str(), String(it.context.c_str()), CaptionKind::Fallback);
  it.retryRead = r.text.empty() && rereadLater(r.fail);
  return readingSegment(it.tag, r);
}

// A blind host's reading of a photo image.look showed again: the answer to the head's
// question (one caption request, made on first use).
std::string answerFor(const nimbus::orch::TurnImage& it, const std::string& question) {
  const CaptionResult r =
      read(it.bytes, it.len, it.mime.c_str(), String(), CaptionKind::Question, String(question.c_str()));
  return readingSegment(it.tag, r);
}

// Queue the re-read of photo `it` (its turn's reading failed for a passing reason).
void queueReread(const nimbus::orch::TurnImages& set, const nimbus::orch::TurnImage& it) {
  PendingRecord r;
  r.chatId = nimbus::orch::workingCopy(set.chatId);
  r.ref = nimbus::orch::workingCopy(it.ref);
  r.mime = nimbus::orch::workingCopy(it.mime);
  r.tag = nimbus::orch::workingCopy(it.tag);
  r.context = nimbus::orch::workingCopy(it.context);
  r.len = it.len;
  recordQueue().push(r);
}

// The turn is over (R1): each photo's stored row says what the head had of it (its own
// note, a blind host's reading, or "seen in its turn"), written before the row is ever
// sent as history, so no caption call is made for a photo the head saw and no history
// message changes after it was sent. A photo the turn never showed or read keeps the
// row intake stored.
void settleRows(nimbus::orch::TurnImages& set) {
  for (size_t i = 0; i < set.items.size(); ++i) {
    const nimbus::orch::TurnImage& it = set.items[i];
    if (nimbus::orch::settledNeedsReread(set, i)) queueReread(set, it);
    const std::string row = nimbus::orch::settledRow(set, i);
    if (row.empty() || set.chatId.empty()) continue;
    const bool ok = memory::replaceMediaRowText(set.chatId.c_str(), it.ref, String(row.c_str()));
    alogf("vision: photo %d/%d row %s (%s)", it.k, it.n, ok ? "settled" : "has no row to settle",
          nimbus::orch::settledHow(set, i));
  }
}

// The set's end (its release hook, run from a destructor): the rows, then the bytes,
// which are freed whatever the rows' card work met (nothing may escape a destructor).
void endOfTurn(nimbus::orch::TurnImages& set) {
  try {
    settleRows(set);
  } catch (...) {
    alogf("vision: the photo rows were not settled (out of memory) - they keep their stored text");
  }
  freeBytes(set);
}

std::string newNonce() {
  char nonce[17];
  snprintf(nonce, sizeof nonce, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  return nonce;
}

// The device's hooks on a turn's set (the photo set, or the empty set of a turn that
// brought none, on its first image.look).
void adopt(nimbus::orch::TurnImages& set) {
  if (set.nonce.empty()) set.nonce = newNonce();
  if (!set.release) set.release = endOfTurn;
  if (!set.transcribe) set.transcribe = transcribeFor;
  if (!set.answer) set.answer = answerFor;
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
// The conversation rows are keyed by the routing chat id; the principal's ns is
// the DATA namespace ("chat:<id>" for a member, nsForChat). Translate before the
// lookup or a member's own photos would never be found ("no photo with that ref
// in this chat" for a photo this chat sent - found on hardware by the IMG lane's
// look-again leg). The owner's check is skipped upstream (owner=true), so only
// the member form needs it.
std::string chatIdOfNs(const std::string& ns) {
  return ns.rfind("chat:", 0) == 0 ? ns.substr(5) : ns;
}

bool chatHasPhoto(const std::string& ns, const std::string& ref) {
  nimbus::orch::MsgQuery q;
  q.sessionId = chatIdOfNs(ns);
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
  return cur && cur->shown && cur->holds(ref);   // shown: the host in use sees them (and did not refuse)
}

// image.look on a turn's set (R1): the photo joins the set's looked photos and every
// later request of the turn shows it to a host that sees images (a blind host reads the
// caption step's answer to `question`, made on first use). Takes `buf` (freed with the
// set) and returns the result text, or "" (buf untouched) when it cannot join.
static std::string showAgain(nimbus::orch::TurnImages& set, const std::string& ref, const std::string& question,
                             uint8_t* buf, size_t len) {
  const char* why = "";
  const char* real = nimbus::orch::admitLook(set, buf, len, why);
  if (!*real) {
    alogf("vision: image.look %s not shown as an image (%s) - the caption step reads it", ref.c_str(), why);
    return std::string();
  }
  adopt(set);
  nimbus::orch::LookedImage l;
  l.img.ref = ref;
  l.img.mime = real;
  l.img.bytes = buf;
  l.img.len = len;
  l.img.tag = "[image ref=" + ref + "]";
  l.question = question;
  set.looked.push_back(l);
  alogf("vision: image.look %s goes to the head as an image (%u bytes)", ref.c_str(), (unsigned)len);
  return nimbus::orch::lookResultText(ref);
}

// The stored copy of photo `ref` read into PSRAM: a photo ("jpg") or an image sent as a
// file ("bin"); `ext` "" tries both. The card lock is held only around the read.
static uint8_t* loadStoredPhoto(const std::string& ref, const std::string& ext, size_t& len, const char*& why) {
  why = "";
  for (const char* e : {"jpg", "bin"}) {
    if (!ext.empty() && ext != e) continue;
    const std::string path = nimbus::orch::blobPath("/mem/blobs", ref, e);
    memory::Lock lk;
    if (!memory::dataFs().exists(path.c_str())) continue;
    return loadToPsram(path.c_str(), memory::dataFs(), nimbus::orch::kImagePartBytes, len, why);
  }
  return nullptr;
}

// May this caller look at photo `ref`? A card is needed (earlier photos live there); a
// caller that is not the owner looks only at its own chat's photos, and an unnamed chat
// (empty ns) is the most restrictive case, never "any chat".
static bool lookAllowed(const std::string& ref, const std::string& ns, bool owner, std::string& err) {
  if (!memory::haveSd()) err = "no memory card: earlier photos are not kept";
  else if (!owner && (ns.empty() || !chatHasPhoto(ns, ref))) err = "no photo with that ref in this chat";
  return err.empty();
}

// One caption request answering `question` about the photo in `buf` (freed here).
static std::string captionAnswer(uint8_t* buf, size_t len, const char* mime, const std::string& question,
                                 std::string& err) {
  const CaptionResult r = read(buf, len, mime, String(), CaptionKind::Question, String(question.c_str()));
  free(buf);
  if (r.text.empty()) err = "the photo could not be read: " + r.words;
  return r.text;
}

std::string look(const std::string& ref, const std::string& question, const std::string& ns, bool owner,
                 std::string& err) {
  if (!lookArgsValid(ref, question, err)) return std::string();
  if (shownThisTurn(ref))
    return "This photo came with the current message and is already in front of you as an image: "
           "answer from what you see in it.";
  nimbus::orch::TurnImages* set = nimbus::orch::activeTurnImages();
  if (set && set->lookedIndex(ref) >= 0) return nimbus::orch::lookResultText(ref);   // already shown again
  if (!lookAllowed(ref, ns, owner, err)) return std::string();
  size_t len = 0;
  const char* why = "";
  uint8_t* buf = loadStoredPhoto(ref, std::string(), len, why);
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
  // The pixels, when a turn holds a set (the owner's turns): the head views the photo
  // itself. Otherwise (a routine, the turn's limits reached) one caption request.
  const std::string shown = set && g_native ? showAgain(*set, ref, question, buf, len) : std::string();
  return shown.empty() ? captionAnswer(buf, len, mime, question, err) : shown;
}

std::string note(const std::string& ref, const std::string& text, std::string& err) {
  nimbus::orch::TurnImages* set = nimbus::orch::activeTurnImages();
  if (!nimbus::orch::validRef(ref)) {
    err = "ref must be the 16 hex characters in the photo's [image ... ref=...] label";
    return std::string();
  }
  if (!set || !set->holds(ref)) {
    err = "image.note is only for a photo of the current message; an earlier photo keeps its note";
    return std::string();
  }
  if (!set->setNote(ref, text)) {
    err = "the note is empty";
    return std::string();
  }
  return "noted";
}

nimbus::orch::TurnImages* turnSet(bool create) {
  if (!g_set && create) {
    g_set.reset(new nimbus::orch::TurnImages());
    g_set->uploadOnce = g_uploadOnce;
    adopt(*g_set);
  }
  return g_set.get();
}

void offerTurnSet(const String& chatId, const String& routedText) {
  if (!g_set || g_set->empty()) {
    g_set.reset();
    return;
  }
  g_set->chatId = chatId.c_str();
  alogf("vision: %u photo(s), %u bytes, kept for the head (images to a host that sees them)",
        (unsigned)g_set->items.size(), (unsigned)g_set->bytes());
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
  const uint32_t now = (uint32_t)millis();
  if (providers::pendingUploadDeletes() || providers::keptUploadsDue(now)) return true;
  return recordQueue().due(now, available()) != nullptr;
}

void idleStep() {
  const uint32_t now = (uint32_t)millis();
  if (providers::keptUploadsDue(now)) {   // a kept photo unused for an hour: queue its deletion
    providers::tendKeptUploads(now);
    return;
  }
  if (providers::pendingUploadDeletes()) {
    providers::deletePendingUploads(deviceProviderDeps(), 1);
    return;
  }
  const PendingRecord* due = recordQueue().due((uint32_t)millis(), available());
  if (!due) return;
  const PendingRecord rec = *due;   // settle() reorders the queue
  size_t len = 0;
  const char* why = "";
  uint8_t* buf = loadStoredPhoto(nimbus::orch::plainCopy(rec.ref), nimbus::orch::plainCopy(rec.ext), len, why);
  if (!buf) {
    alogf("vision: record for %s skipped: %s", rec.ref.c_str(), why[0] ? why : "could not be fetched");
    recordQueue().settle(rec.ref, true, (uint32_t)millis());   // nothing to read: give up
    return;
  }
  // Only a photo a blind host could not read gets here (the head's own photos are
  // settled at their turn's end, with no caption call): its fallback reading again.
  const CaptionResult r = read(buf, len, rec.mime.c_str(), String(rec.context.c_str()), CaptionKind::Fallback);
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
