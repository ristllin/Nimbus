// Device seam for the Telegram archive (see tg_archive.h and nimbus/tg_archive.h).
// Three pieces: the card adapter (the engine's Fs seam over the SD data store, one
// memory::Lock per operation), the Telegram file source (getFile plus a streamed,
// resumable GET on the Telegram task's single TLS session), and the hooks plus the
// tick that telegram.cpp calls.
#include "tg_archive.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <solide/storage.h>   // cardSizeMB / freeMB - the storage-pressure inputs

#include <atomic>
#include <memory>
#include <string>

#include "../sys/agent_log.h"  // alogf; logring::g_secrets (the device's registered keys)
#include "../sys/net_util.h"   // tlsSetup / tlsClose
#include "../sys/ps_json.h"    // PsramJsonAllocator
#include "memory_subsystem.h"  // dataFs, Lock, haveSd, erasing, noteSdIoResult
#include "nimbus/fault.h"      // FAULT sd_io: a card that fails writes
#include "nimbus/tg_archive.h"
#include "store.h"             // owner chat for the one storage notice; the boot token
#include "telegram.h"          // send() for that notice, isAllowed() for the trust check

namespace agent {
namespace tgarchive {

namespace arc = nimbus::tgarc;

namespace {

constexpr const char* kHost = "api.telegram.org";
constexpr int kPort = 443;
constexpr size_t kSlice = 8 * 1024;             // card write slice (PSRAM)
constexpr uint32_t kTickBudgetMs = 8000;        // download time per tick
constexpr uint32_t kPressureEveryMs = 60000;
constexpr uint32_t kReadIdleMs = 20000;         // a silent download socket is a failed one
constexpr uint32_t kPathTtlMs = 50UL * 60 * 1000;   // getFile links last at least an hour
constexpr size_t kQueueMaxItems = 64;
constexpr size_t kQueueMaxBytes = 512 * 1024;

// ---- the card adapter -------------------------------------------------------------

std::string baseName(const char* name) {
  const char* s = strrchr(name, '/');
  return s ? std::string(s + 1) : std::string(name);
}

// Writes refuse while there is no card, while a factory erase runs (so a write can
// never recreate a swept tree), and under FAULT sd_io. Callers check it while they
// hold memory::Lock: the erase sets its barrier under the same lock.
bool writable(bool card) {
  if (memory::erasing()) return false;
  if (!card) return true;
  return memory::haveSd() && !nimbus::fault::active(nimbus::fault::SD_IO);
}

class CardWriter : public arc::Writer {
 public:
  CardWriter(File f, bool card) : f_(f), card_(card) {}
  ~CardWriter() override { close(); }
  bool write(const uint8_t* d, size_t n) override {
    if (!open_) return false;
    bool ok;
    {
      memory::Lock g;
      ok = writable(card_) && f_.write(d, n) == n;
    }
    if (card_ && !ok) memory::noteSdIoResult(false);   // feeds the demote watchdog
    return ok;
  }
  bool close() override {
    if (!open_) return false;
    open_ = false;
    memory::Lock g;
    f_.close();
    return true;
  }

 private:
  File f_;
  bool card_;
  bool open_ = true;
};

class CardReader : public arc::Reader {
 public:
  explicit CardReader(File f) : f_(f) {}
  ~CardReader() override {
    // An export ZIP's last reader dies with its response, on the web task, which never
    // waits for the card lock (nimbus/sys/web_guard.h): LoopLock tries there and blocks
    // anywhere else. A read handle's close writes nothing to the card, so it closes
    // even when the try finds the lock busy.
    memory::LoopLock g(nimbus::sys::kWebFillerWaitMs);
    f_.close();
  }
  int read(uint8_t* d, size_t cap) override {
    memory::Lock g;
    return f_.read(d, cap);
  }

 private:
  File f_;
};

class CardFs : public arc::Fs {
 public:
  CardFs(fs::FS& fs, bool card) : fs_(fs), card_(card) {}

  bool exists(const std::string& p) override {
    memory::Lock g;
    return fs_.exists(p.c_str());
  }
  bool mkdirs(const std::string& dir) override {
    memory::Lock g;
    if (!writable(card_)) return false;
    for (size_t i = 1; i <= dir.size(); ++i) {
      if (i < dir.size() && dir[i] != '/') continue;
      const std::string sub = dir.substr(0, i);
      if (!fs_.exists(sub.c_str()) && !fs_.mkdir(sub.c_str())) return false;
    }
    return true;
  }
  std::unique_ptr<arc::Writer> openWrite(const std::string& p, bool append) override {
    memory::Lock g;
    if (!writable(card_)) return nullptr;
    File f = fs_.open(p.c_str(), append ? FILE_APPEND : FILE_WRITE);
    if (!f) return nullptr;
    return std::unique_ptr<arc::Writer>(new CardWriter(f, card_));
  }
  std::unique_ptr<arc::Reader> openRead(const std::string& p) override {
    memory::Lock g;
    File f = fs_.open(p.c_str(), FILE_READ);
    if (!f) return nullptr;
    if (f.isDirectory()) {
      f.close();
      return nullptr;
    }
    return std::unique_ptr<arc::Reader>(new CardReader(f));
  }
  bool rename(const std::string& from, const std::string& to) override {
    memory::Lock g;
    return writable(card_) && fs_.rename(from.c_str(), to.c_str());
  }
  bool remove(const std::string& p) override {
    memory::Lock g;
    return fs_.remove(p.c_str());
  }
  // Removes one file per lock hold, so a big day directory never holds the card
  // lock for its whole delete.
  bool removeTree(const std::string& dir) override {
    bool ok = true;
    for (const arc::Entry& e : list(dir)) {
      const std::string child = dir + "/" + e.name;
      ok = (e.dir ? removeTree(child) : remove(child)) && ok;
    }
    memory::Lock g;
    return fs_.rmdir(dir.c_str()) && ok;
  }
  int64_t size(const std::string& p) override {
    memory::Lock g;
    File f = fs_.open(p.c_str(), FILE_READ);
    if (!f) return -1;
    const int64_t n = f.isDirectory() ? -1 : (int64_t)f.size();
    f.close();
    return n;
  }
  std::vector<arc::Entry> list(const std::string& dir) override {
    std::vector<arc::Entry> out;
    memory::Lock g;
    File d = fs_.open(dir.c_str());
    if (!d || !d.isDirectory()) {
      if (d) d.close();
      return out;
    }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      out.push_back({baseName(f.name()), f.isDirectory(), f.isDirectory() ? 0 : (uint64_t)f.size()});
      f.close();
    }
    d.close();
    return out;
  }

 private:
  fs::FS& fs_;
  bool card_;
};

// ---- the Telegram file source -----------------------------------------------------

// A file id or file path with anything outside [A-Za-z0-9._/-] could inject into
// the request line.
bool pathSafe(const std::string& s) {
  if (s.empty() || s.size() > 200) return false;
  for (char c : s)
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '/')) return false;
  return true;
}

bool connectTg(WiFiClientSecure& sc) {
  tlsSetup(sc);
  sc.setHandshakeTimeout(10);
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (sc.connect(kHost, kPort, 10000)) return true;
    tlsClose(sc);
    vTaskDelay(pdMS_TO_TICKS(400));
  }
  return false;
}

// One header line; the part past `cap` is read and dropped, so a long header never
// leaves its tail to be read as the next line.
int readLine(WiFiClientSecure& sc, char* buf, int cap, uint32_t deadline) {
  int i = 0;
  while ((int32_t)(millis() - deadline) < 0) {
    if (!sc.available()) {
      if (!sc.connected()) break;
      vTaskDelay(1);
      continue;
    }
    const char c = (char)sc.read();
    if (c == '\n') break;
    if (c != '\r' && i < cap - 1) buf[i++] = c;
  }
  buf[i] = 0;
  return i;
}

// The response head: status, Content-Length, and Content-Range ("bytes a-b/total"
// or "bytes */total"; -1 for what is absent).
struct Head {
  int     status = 0;
  int64_t length = -1;
  int64_t rangeStart = -1;
  int64_t rangeTotal = -1;
};

void parseContentRange(const char* v, Head& h) {
  while (*v == ' ') ++v;
  if (strncasecmp(v, "bytes", 5)) return;
  v += 5;
  while (*v == ' ') ++v;
  if (*v != '*') h.rangeStart = atoll(v);
  const char* slash = strchr(v, '/');
  if (slash && slash[1] != '*') h.rangeTotal = atoll(slash + 1);
}

Head readHead(WiFiClientSecure& sc, uint32_t deadline) {
  Head h;
  char line[256];
  if (readLine(sc, line, sizeof line, deadline) == 0) return h;
  const char* sp = strchr(line, ' ');
  h.status = sp ? atoi(sp + 1) : 0;
  for (int i = 0; i < 64 && readLine(sc, line, sizeof line, deadline) > 0; ++i) {
    if (!strncasecmp(line, "Content-Length:", 15)) h.length = atoll(line + 15);
    else if (!strncasecmp(line, "Content-Range:", 14)) parseContentRange(line + 14, h);
  }
  return h;
}

class TgFileSource : public arc::Source {
 public:
  explicit TgFileSource(const String& token) : token_(token) {}
  ~TgFileSource() override { close(); }

  arc::OpenResult open(const std::string& fileId, uint64_t resumeAt) override {
    arc::OpenResult r;
    close();
#ifdef NIMBUS_TEST
    std::string local;
    if (arc::localInjectPath(fileId, local)) return openLocal(local, resumeAt);
#endif
    if (!pathSafe(fileId)) {
      r.permanent = true;
      r.err = "unsafe file id";
      return r;
    }
    if ((pathFor_ != fileId || millis() - pathAt_ > kPathTtlMs) && !resolvePath(fileId, r)) return r;
    startGet(resumeAt, r);
    return r;
  }

  // Bytes as they arrive; 0 at the end of the body, -1 when the socket drops early or
  // stays silent for kReadIdleMs.
  int read(uint8_t* buf, size_t cap) override {
#ifdef NIMBUS_TEST
    if (localOpen_) {
      memory::Lock g;
      return localFile_.read(buf, cap);
    }
#endif
    if (!open_) return -1;
    if (remaining_ == 0) return 0;
    size_t want = cap;
    if (remaining_ > 0 && (int64_t)want > remaining_) want = (size_t)remaining_;
    const uint32_t start = millis();
    while (millis() - start <= kReadIdleMs) {
      const int n = sc_.available() ? sc_.read(buf, want) : 0;
      if (n > 0) {
        if (remaining_ > 0) remaining_ -= n;
        return n;
      }
      if (!sc_.available() && !sc_.connected()) return remaining_ > 0 ? -1 : 0;
      vTaskDelay(1);
    }
    return -1;
  }

  void close() override {
#ifdef NIMBUS_TEST
    if (localOpen_) {
      memory::Lock g;
      localFile_.close();
      localOpen_ = false;
    }
#endif
    if (!open_) return;
    open_ = false;
    tlsClose(sc_);
  }

 private:
#ifdef NIMBUS_TEST
  // TGINJECT media: the bytes come from a file already on the card, through the same
  // slices, resume and limits as a Telegram download.
  arc::OpenResult openLocal(const std::string& path, uint64_t resumeAt) {
    arc::OpenResult r;
    memory::Lock g;
    localFile_ = memory::dataFs().open(path.c_str(), FILE_READ);
    if (!localFile_ || localFile_.isDirectory()) {
      if (localFile_) localFile_.close();
      r.permanent = true;
      r.err = "local file missing";
      return r;
    }
    const uint64_t size = localFile_.size();
    r.resumed = resumeAt > 0 && resumeAt <= size && localFile_.seek((uint32_t)resumeAt);
    r.total = (int64_t)(size - (r.resumed ? resumeAt : 0));
    r.ok = localOpen_ = true;
    return r;
  }
  File localFile_;
  bool localOpen_ = false;
#endif
  // getFile: the file's download path. Telegram refuses files over 20 MB here; its
  // other 400s ("wrong file_id or the file is temporarily unavailable") may pass, so
  // only "too big" is final.
  bool resolvePath(const std::string& fileId, arc::OpenResult& r) {
    WiFiClientSecure sc;
    if (!connectTg(sc)) {
      r.err = "connect failed";
      return false;
    }
    String req = String("GET /bot") + token_ + "/getFile?file_id=" + fileId.c_str() +
                 " HTTP/1.0\r\nHost: " + kHost + "\r\nConnection: close\r\n\r\n";
    sc.print(req);
    const uint32_t deadline = millis() + 12000;
    const Head h = readHead(sc, deadline);
    std::string body;
    body.reserve(512);
    while ((int32_t)(millis() - deadline) < 0 && body.size() < 2048) {
      if (sc.available()) body.push_back((char)sc.read());
      else if (!sc.connected()) break;
      else vTaskDelay(1);
    }
    tlsClose(sc);
    JsonDocument d(&PsramJsonAllocator::instance());
    deserializeJson(d, body);
    const char* fp = d["result"]["file_path"] | "";
    if (h.status == 200 && (d["ok"] | false) && pathSafe(fp)) {
      pathFor_ = fileId;
      path_ = fp;
      pathAt_ = millis();
      return true;
    }
    const String desc = d["description"] | "";
    r.permanent = h.status == 400 && desc.indexOf("too big") >= 0;
    r.err = desc.length() ? std::string(desc.c_str()) : ("getFile status " + std::to_string(h.status));
    return false;
  }

  // One GET of the file, from `resumeAt` when the server agrees. A 206 must start
  // exactly there (RFC 9110 15.3.7) or the transfer starts over; a 416 whose total is
  // the bytes already held means the file is complete.
  bool startGet(uint64_t resumeAt, arc::OpenResult& r) {
    if (!connectTg(sc_)) {
      r.err = "connect failed";
      return false;
    }
    String req = String("GET /file/bot") + token_ + "/" + path_.c_str() + " HTTP/1.0\r\nHost: " +
                 kHost + "\r\nConnection: close\r\n";
    if (resumeAt) req += String("Range: bytes=") + String((unsigned long long)resumeAt) + "-\r\n";
    req += "\r\n";
    sc_.print(req);
    const Head h = readHead(sc_, millis() + 15000);
    if (resumeAt && h.status == 416 && h.rangeTotal == (int64_t)resumeAt) {
      tlsClose(sc_);
      open_ = true;      // reads report the end at once
      remaining_ = 0;
      r.ok = r.resumed = true;   // nothing left to read
      r.total = 0;
      return true;
    }
    const bool wrongStart = h.status == 206 && h.rangeStart != (int64_t)resumeAt;
    if ((h.status == 416 || wrongStart) && resumeAt) {
      tlsClose(sc_);
      return startGet(0, r);   // start over: never append at the wrong offset
    }
    if (h.status != 200 && h.status != 206) {
      tlsClose(sc_);
      if (h.status == 404) pathFor_.clear();   // the link expired: getFile again next time
      r.err = "download status " + std::to_string(h.status);
      return false;
    }
    open_ = true;
    remaining_ = h.length;
    r.ok = true;
    r.resumed = h.status == 206;
    r.total = h.length;
    return true;
  }

  String token_;
  WiFiClientSecure sc_;
  bool open_ = false;
  int64_t remaining_ = -1;
  std::string pathFor_, path_;
  uint32_t pathAt_ = 0;
};

// ---- state ------------------------------------------------------------------------

// An item handed over from another task, written by the Telegram task.
struct Pending {
  enum class What : uint8_t { Text, Media, Restored } what = What::Text;
  bool sd = false;                                   // Media: the file is on the SD card
  std::string chat, text, kind, path, caption, origin;
  arc::Restored restored;                            // Restored only
};

size_t queueCost(const Pending& p) {
  return p.text.size() + p.caption.size() + p.restored.file.size() + p.restored.role.size() + 64;
}

CardFs*        g_card = nullptr;
CardFs*        g_lfs = nullptr;
arc::Archive*  g_arc = nullptr;
uint8_t*       g_slice = nullptr;
std::atomic<TaskHandle_t> g_tgTask{nullptr};   // the Telegram task, once it has called in
bool           g_resumed = false;
uint32_t       g_lastPressure = 0;
bool           g_pressureChecked = false;
std::string    g_redactToken;
std::atomic<bool> g_importOpen{false};

// The hand-over queue: a fixed ring of pointers, so the critical section only moves
// pointers and never allocates. Everything below is touched only under g_qMux.
portMUX_TYPE   g_qMux = portMUX_INITIALIZER_UNLOCKED;
Pending*       g_ring[kQueueMaxItems] = {};
size_t         g_head = 0, g_count = 0, g_queueBytes = 0;
arc::Stats     g_statsCopy;   // the engine's counters as last published
uint32_t       g_pendingCopy = 0;
uint32_t       g_dropped = 0;       // outbound items lost to a full queue
uint32_t       g_unavailable = 0;   // items seen while the card was absent

void bumpUnavailable() {
  portENTER_CRITICAL(&g_qMux);
  ++g_unavailable;
  portEXIT_CRITICAL(&g_qMux);
}

// Copy the engine's counters for the web task (which must never read the engine).
void publishStats() {
  if (!g_arc) return;
  const arc::Stats s = g_arc->stats();
  const uint32_t pending = (uint32_t)g_arc->pending();
  portENTER_CRITICAL(&g_qMux);
  g_statsCopy = s;
  g_pendingCopy = pending;
  portEXIT_CRITICAL(&g_qMux);
}

// The exact strings never written to the card: the device's registered keys and
// bot token (agent_log's list, set at boot) plus every bot token used since boot (a
// live token swap does not re-register).
void refreshRedactions(const String& token) {
  if (!g_arc || g_redactToken == token.c_str()) return;
  g_redactToken = token.c_str();
  static std::vector<std::string> s_tokens;   // every bot token used this boot
  if (s_tokens.size() < 8) s_tokens.push_back(g_redactToken);
  std::vector<std::string> secrets = logring::g_secrets;
  secrets.insert(secrets.end(), s_tokens.begin(), s_tokens.end());
  g_arc->setRedactions(std::move(secrets));
}

// Function-local statics: their first use is serialized by the C++ runtime, so the
// web task and the Telegram task can both reach them safely.
CardFs& card() {
  static CardFs fs(memory::dataFs(), /*card=*/true);
  return fs;
}

bool ensure() {
  if (g_arc) return true;
  g_card = &card();
  static CardFs lfs(LittleFS, /*card=*/false);
  g_lfs = &lfs;
  arc::setJsonAllocator(&PsramJsonAllocator::instance());   // records stay off internal SRAM
  g_arc = new arc::Archive(*g_card);
  // Media is fetched only for chats on the allowlist (called on the Telegram task,
  // which owns the allowlist).
  g_arc->setTrust([](const std::string& chat) {
    return telegram::isAllowed(String(chat.c_str())) ? arc::Trust::Allowed : arc::Trust::Stranger;
  });
  refreshRedactions(store::telegramToken());
  return true;
}

arc::Now nowStamp() {
  arc::Now n;
  n.epoch = (int64_t)time(nullptr);
  n.ms = millis();
  return n;
}

bool onTelegramTask() {
  const TaskHandle_t t = g_tgTask.load();
  return t && xTaskGetCurrentTaskHandle() == t;
}

void writeOne(const Pending& p) {
  switch (p.what) {
    case Pending::What::Restored:
      if (!g_arc->recordRestored(p.restored))
        alogf("tg archive: restored %s not recorded", p.restored.file.c_str());
      return;
    case Pending::What::Text:
      g_arc->captureOutText(p.chat, p.text, p.origin, nowStamp());
      return;
    case Pending::What::Media: {
      arc::OutMedia om{p.chat, p.kind, p.path, p.caption, p.origin};
      if (!g_arc->captureOutMedia(om, p.sd ? *g_card : *g_lfs, nowStamp()))
        alogf("tg archive: outbound %s not fully saved", p.kind.c_str());
      return;
    }
  }
}

// From another task: hand the item to the Telegram task, never wait on the card.
// False when the queue was full (the item is dropped and counted).
bool enqueue(Pending* p) {
  const size_t cost = queueCost(*p);
  bool queued = false;
  portENTER_CRITICAL(&g_qMux);
  if (g_count < kQueueMaxItems && g_queueBytes + cost <= kQueueMaxBytes) {
    g_ring[(g_head + g_count) % kQueueMaxItems] = p;
    ++g_count;
    g_queueBytes += cost;
    queued = true;
  } else {
    ++g_dropped;
  }
  portEXIT_CRITICAL(&g_qMux);
  if (queued) return true;
  delete p;
  alog("tg archive: outbound queue full - one message not saved");
  return false;
}

// One critical section per pop, count and bytes together (queueCost only reads).
Pending* dequeue() {
  Pending* p = nullptr;
  portENTER_CRITICAL(&g_qMux);
  if (g_count) {
    p = g_ring[g_head];
    g_ring[g_head] = nullptr;
    g_head = (g_head + 1) % kQueueMaxItems;
    --g_count;
    g_queueBytes -= std::min(g_queueBytes, queueCost(*p));
  }
  portEXIT_CRITICAL(&g_qMux);
  return p;
}

// True when the item was written or handed over.
bool capture(Pending* p) {
  if (!available()) {
    delete p;
    bumpUnavailable();
    return false;
  }
  if (!onTelegramTask()) return enqueue(p);
  ensure();
  writeOne(*p);
  delete p;
  publishStats();
  return true;
}

void drainQueue() {
  for (Pending* p = dequeue(); p; p = dequeue()) {
    writeOne(*p);
    delete p;
  }
}

void discardQueue() {
  for (Pending* p = dequeue(); p; p = dequeue()) {
    delete p;
    bumpUnavailable();
  }
}

// The notice goes where every device notice goes (telegram::ownerChat: never a chat
// that was removed from the allowlist). The pressure pass runs on the poll task, which
// owns the allowlist that rule reads.
void tellOwnerOfPressure(const arc::PressureOutcome& o) {
  const String owner = telegram::ownerChat();
  if (!owner.length()) return;
  String msg = "The memory card is almost full, so the oldest saved Telegram messages and "
               "files were deleted to make room.";
  if (o.keptFrom.length()) msg += String(" Everything from ") + o.keptFrom.c_str() + " on is still saved.";
  telegram::send(owner, msg, /*block=*/false);
}

// Not while a restore upload is open: it may be writing into an old day folder, and
// FAT would let the pass unlink a file that is still being written.
void checkPressure() {
  const uint32_t now = millis();
  if (g_importOpen.load()) return;
  if (g_pressureChecked && now - g_lastPressure < kPressureEveryMs) return;
  g_pressureChecked = true;
  g_lastPressure = now;
  uint64_t total, freeB;
  {
    memory::Lock g;   // the card's free-space query is card I/O like any other
    total = solide::storage::cardSizeMB() << 20;
    freeB = solide::storage::freeMB() << 20;
  }
  if (!total) return;
  const arc::PressureOutcome o = g_arc->pressurePass(freeB, total);
  if (!o.removedDays) return;
  alogf("tg archive: card nearly full - removed %u oldest day(s), %llu bytes",
        (unsigned)o.removedDays, (unsigned long long)o.freed);
  if (o.tellOwner) tellOwnerOfPressure(o);
}

const char* stepName(arc::Step s) {
  switch (s) {
    case arc::Step::Idle: return "idle";
    case arc::Step::Done: return "saved";
    case arc::Step::Partial: return "paused";
    case arc::Step::Retry: return "will retry";
    case arc::Step::Incomplete: return "incomplete";
    case arc::Step::Failed: return "failed";
  }
  return "?";
}

// Fetch pending media for up to kTickBudgetMs: keep going after a finished job,
// stop on anything else (paused, retry later, nothing to do).
void runDownloads(const String& token, bool (*yield)(), void (*beforeTls)()) {
  if (!token.length() || !g_arc->pending()) return;
  if (!g_slice) g_slice = (uint8_t*)heap_caps_malloc(kSlice, MALLOC_CAP_SPIRAM);
  if (!g_slice) return;
  if (beforeTls) beforeTls();   // one TLS session at a time: the poll socket goes first
  TgFileSource src(token);
  const uint32_t start = millis();
  const std::function<uint32_t()> nowMs = [] { return (uint32_t)millis(); };
  const std::function<bool()> stop = [yield] { return yield && yield(); };
  while (g_arc->pending() && millis() - start < kTickBudgetMs && !stop()) {
    const uint32_t left = kTickBudgetMs - (millis() - start);
    const arc::Step s = g_arc->downloadStep(src, g_slice, kSlice, left, nowMs, stop);
    if (s == arc::Step::Idle || s == arc::Step::Partial) break;
    alogf("tg archive: media %s (%u pending)", stepName(s), (unsigned)g_arc->pending());
    if (s == arc::Step::Retry) break;
  }
}

}  // namespace

// ---- public -------------------------------------------------------------------------

bool available() { return memory::haveSd() && !memory::erasing(); }

arc::Fs& cardFs() { return card(); }

void onInboundPage(const char* body, size_t len, const String& token) {
  g_tgTask.store(xTaskGetCurrentTaskHandle());   // only the Telegram task delivers pages
  if (!body || !len || !available() || !ensure()) return;
  refreshRedactions(token);
  const uint32_t dupesBefore = g_arc->stats().dupes;
  const int added = g_arc->captureInboundPage(body, len, nowStamp());
  const uint32_t dupes = g_arc->stats().dupes - dupesBefore;
  if (added || dupes)
    alogf("tg archive: %d new update(s) saved, %u already saved", added, (unsigned)dupes);
  publishStats();
}

void onOutboundText(const String& chatId, const String& text, const char* origin) {
  // Only real Telegram chats: the web, serial and voice pseudo-chats are not archived.
  if (!text.length() || !arc::isTelegramChat(chatId.c_str())) return;
  Pending* p = new Pending();
  p->chat = chatId.c_str();
  p->text = text.c_str();
  p->origin = origin ? origin : "send";
  capture(p);
}

void onOutboundMedia(const String& chatId, const char* kind, const char* srcPath, bool sd,
                     const String& caption) {
  if (!srcPath || !srcPath[0] || !arc::isTelegramChat(chatId.c_str())) return;
  Pending* p = new Pending();
  p->what = Pending::What::Media;
  p->sd = sd;
  p->chat = chatId.c_str();
  p->kind = kind ? kind : "document";
  p->path = srcPath;
  p->caption = caption.c_str();
  p->origin = p->kind;
  capture(p);
}

bool onRestored(const arc::Restored& r) {
  Pending* p = new Pending();
  p->what = Pending::What::Restored;
  p->restored = r;
  return capture(p);
}

#ifdef NIMBUS_TEST
bool copyLocalInject(const char* fileId, const char* lfsPath, size_t& total, size_t cap) {
  std::string src;
  total = 0;
  if (!fileId || !arc::localInjectPath(fileId, src)) return false;
  std::unique_ptr<arc::Reader> r = card().openRead(src);
  const int64_t size = card().size(src);
  if (!r || size <= 0) return true;              // missing or empty: total 0
  if ((size_t)size >= cap) {                     // the caller's "too large" answer
    total = (size_t)size;
    return true;
  }
  CardFs lfs(LittleFS, /*card=*/false);
  std::unique_ptr<arc::Writer> w = lfs.openWrite(lfsPath, false);
  if (!w) return true;
  uint8_t buf[512];
  size_t got = 0;
  for (int n = r->read(buf, sizeof buf); n > 0; n = r->read(buf, sizeof buf)) {
    if (!w->write(buf, (size_t)n)) {
      got = 0;
      break;
    }
    got += (size_t)n;
  }
  w->close();
  total = got;
  return true;
}
#endif

namespace {

// Copy a card file to LittleFS; the byte count copied, or -1 when it failed.
int64_t copyToLfs(const std::string& src, const char* lfsPath) {
  std::unique_ptr<arc::Reader> r = card().openRead(src);
  CardFs lfs(LittleFS, /*card=*/false);
  std::unique_ptr<arc::Writer> w = r ? lfs.openWrite(lfsPath, false) : nullptr;
  if (!w) return -1;
  uint8_t buf[512];
  int64_t got = 0;
  for (int n = r->read(buf, sizeof buf); n > 0; n = r->read(buf, sizeof buf)) {
    if (!w->write(buf, (size_t)n)) {
      w->close();
      return -1;
    }
    got += n;
  }
  w->close();
  return got;
}

}  // namespace

bool takeFromArchive(const InboundRef& in, const char* lfsPath, size_t cap, size_t& total) {
  total = 0;
  if (!in.chat || !in.fileId || !lfsPath || !available() || !arc::isTelegramChat(in.chat)) return false;
  ensure();
  const std::string src = g_arc->mediaFor(in.chat, in.updateId, in.date, in.fileId);
  if (src.empty()) return false;
  const int64_t size = card().size(src);
  if (size <= 0) return false;
  if ((size_t)size >= cap) {   // the caller's "too large" answer, nothing copied
    total = (size_t)size;
    return true;
  }
  if (copyToLfs(src, lfsPath) != size) {
    LittleFS.remove(lfsPath);
    return false;   // the caller downloads it instead
  }
  total = (size_t)size;
  alogf("tg archive: media for update %ld reused (%u bytes), no second download", (long)in.updateId,
        (unsigned)total);
  return true;
}

void adoptInbound(const InboundRef& in, const char* lfsPath) {
  if (!in.chat || !in.fileId || !lfsPath || !available() || !arc::isTelegramChat(in.chat)) return;
  ensure();
  CardFs lfs(LittleFS, /*card=*/false);
  if (g_arc->adoptMedia(in.chat, in.updateId, in.date, in.fileId, lfs, lfsPath)) {
    alogf("tg archive: media for update %ld kept from the turn's download", (long)in.updateId);
    publishStats();
  }
}

bool queueHasRoom() {
  portENTER_CRITICAL(&g_qMux);
  const bool room = g_count + 1 < kQueueMaxItems && g_queueBytes + 1024 <= kQueueMaxBytes;
  portEXIT_CRITICAL(&g_qMux);
  return room;
}

void noteImport(bool open) { g_importOpen.store(open); }

void tick(const String& token, bool (*yield)(), void (*beforeTls)()) {
  g_tgTask.store(xTaskGetCurrentTaskHandle());
  if (!available()) {
    discardQueue();   // no card: nothing can be saved, so free the queue
    return;
  }
  ensure();
  refreshRedactions(token);
  drainQueue();
  if (!g_resumed) {
    g_resumed = true;
    const int n = g_arc->resumePending();
    if (n) alogf("tg archive: %d unfinished download(s) resumed", n);
  }
  checkPressure();
  runDownloads(token, yield, beforeTls);
  publishStats();
}

String statsJson() {
  arc::Stats s;
  uint32_t pending, queued, dropped, unavailable;
  portENTER_CRITICAL(&g_qMux);
  s = g_statsCopy;
  pending = g_pendingCopy;
  queued = (uint32_t)g_count;
  dropped = g_dropped;
  unavailable = g_unavailable;
  portEXIT_CRITICAL(&g_qMux);
  JsonDocument d(&PsramJsonAllocator::instance());
  d["available"] = available();
  d["root"] = arc::kRoot;
  d["pending"] = pending;
  d["queued"] = queued;
  d["in"] = s.inItems;
  d["out"] = s.outItems;
  d["dupes"] = s.dupes;
  d["repaired"] = s.repaired;
  d["mediaOk"] = s.mediaOk;
  d["incomplete"] = s.mediaIncomplete;
  d["failed"] = s.mediaFailed;
  d["notArchived"] = s.notArchived;
  d["writeErrors"] = s.writeErrors;
  d["dropped"] = dropped;
  d["unavailable"] = unavailable;
  d["pressureDays"] = s.pressureDays;
  d["strangerSkipped"] = s.strangerSkipped;
  String out;
  serializeJson(d, out);
  return out;
}

}  // namespace tgarchive
}  // namespace agent
