// orchestrator_plain.cpp - the plain engine's device seam (engine 2). Lane E2-DEV binds the engine.
//
// An engine-2 device image (the *-e2 envs, -DNIMBUS_ENGINE=2) links this file instead of
// orchestrator_e1.cpp (tools/engine/engine1_only.txt). A person's message is one engine-2 turn:
// plain::ChatBinding (nimbus/harness/plain/chat_binding.h, a copy of nimbusd's one-chat rig) over
// the device's parts, each reused from engine 1 or an earlier lane:
//   - the platform hooks of orchestrator_e1.cpp's buildTurnDeps, plus PSRAM allocLarge/freeLarge;
//   - the shared TLS transport (transport_tls.h deviceTransport), wrapped only to read the memory
//     figures around each request;
//   - the Claude key (store::anthropicKey), the configured model (store::orchModel), the
//     summarizer (store::subModel), the person's role (roleOfChat), the day start and rounds;
//   - the transcript on the card (tx_store_device.h, /mem/tx; RAM only with no card);
//   - a part goes out once: Telegram through telegram::sendPart (no second split), the web and
//     voice chats through the common deliver();
//   - the shared registry (memory::registry: note, the file tools, calc, web_search, more), compaction (plain_fold_device's two
//     files), suppression, the money guard and one breaker, the seen-index rebuild at boot;
//   - the earlier engine's conversation imported at each boot of a chat, and its standing rules
//     copied into pinned notes once per boot (migrate_device.h, lane E2-MIG).
// One resident chat (one arena, sized for the head): a message for another chat, another model
// or another role boots the binding again over the same card (the transcript is refilled).
// Claude heads only: any other head gets one plain line. Agents are not bound yet.
// Lane E2-CHAT adds the owner's daily chat:
//   - the owner's slash commands never reach the model (commands_plain.cpp): engine 1's handlers
//     where the command does not depend on the engine; /clear, /compact and /budget act on engine 2;
//   - photos: the Telegram intake's photo set (agent::vision) as the message's image parts, the
//     bytes through the photo hook while the turn holds the set (chat_photos.h);
//   - skills: the skill files on the card (skills_device.h) in the index and, inject: context,
//     in the pinned block (a skill too large for it is listed instead; the owner is told once);
//   - usage: each turn filed once where the owner sees usage (store::recordProviderSpend);
//   - the progress line of a long Telegram turn (installProgress over turnProgressLine).
// Lane E2-SCHED binds the schedule entries (schedule_binding.h): pollJobs runs the loops tick, and
// its fire hook (injectScheduledTurn) boots the chat an entry delivers to. A reminder sends its
// stored text with no model request; a routine runs isolated on the small model through
// plain::runRoutine (its outbox and state on the card's /mem/routines), refused while the monthly
// limit holds. Engine 1's nightly upkeep record has nothing to run here.
#include "orchestrator.h"
#include "orchestrator_common.h"   // g_fabric, g_sinks, g_sessMux, deliver, the voice flags

#if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE != 2
#error "orchestrator_plain.cpp is the engine-2 seam: build it only in an env with -DNIMBUS_ENGINE=2"
#endif

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>   // memcpy, strncpy - the staged update notice
#include <time.h>     // localtime_r, gmtime_r - utcOffsetAt

#include <atomic>
#include <memory>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../sys/agent_log.h"
#include "../sys/heap_watch.h"     // heapwatch::psramFirst - the turn's PSRAM-first switch
#include "../sys/psram_static.h"   // the notice slot in PSRAM, as on engine 1
#include "files_subsystem.h"    // files::recentFiles - the file tools' recent-files list (E2-TOOLS)
#include "adapters/image_vision.h"   // the photo set the Telegram intake keeps (R1)
#include "budget_command.h"     // noteTurnEnd - /model's last reply
#include "commands_plain.h"     // the owner's slash commands
#include "loops_subsystem.h"    // checkDue, routineGrants, quietHoursEndMinute (lane E2-SCHED)
#include "file_tools_device.h"  // tools::takeOutsideCall - the call just dispatched read outside content (E2-FSAFE)
#include "memory_subsystem.h"   // memory::captureMessage, memory::registry
#include "migrate_device.h"     // migratedev::fill, importRulesOnce - the earlier engine's history and rules
#include "nimbus/fault.h"       // FAULT sd: a card that is fitted but faulted
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/chat_commands.h"   // readGeneration, writeGeneration - /clear (P7)
#include "nimbus/harness/plain/chat_photos.h"
#include "nimbus/harness/plain/schedule_binding.h"   // reminder and routine fires (lane E2-SCHED)
#include "nimbus/orch/spend_ceiling.h"               // kSpendFrozenError - a routine waits while frozen
#include "nimbus/harness/turn_memory.h"   // PsramFirstScope
#include "nimbus/harness/log.h"           // hlog::setSink - the harness lines into alog
#include "nimbus/orch/blob_store.h"        // blobPath - an earlier photo's stored copy
#include "notes_device.h"       // notesdev::pinnedNotesFor, notebook (the rails' pending shelf)
#include "schedule_device.h"    // scheduledev::fire, host - the schedule entries (lane E2-SCHED)
#include "skills_device.h"      // skillsdev::chatSkills
#include "spend_ceiling_store.h"   // spend::frozen, spend::commit, spend::limitMicro
#include "store.h"              // anthropicKey, orchModel, subModel, resolvedOrchHost
#include "telegram.h"           // telegram::ownerChat, sendPart, turnUpdateIds, seenIndex
#include "transport_tls.h"      // deviceTransport
#include "tx_store_device.h"    // txdev::cardTxFiles

namespace agent {
namespace orchestrator {

namespace {

namespace plain = agent::plain;

// Device copy (owner-facing, never model text), sent with system provenance.
constexpr const char* kNotClaudeLine =
    "This test build runs the new engine, which answers with a Claude model only. Choose Anthropic as the "
    "provider in Settings, then send the message again.";
constexpr const char* kNoKeyLine = "No Anthropic key is set. Add one in Settings, then send the message again.";
constexpr const char* kFrozenLine = "The monthly spend limit is reached. Raise it in Settings to continue.";
constexpr const char* kReadingLine =
    "The new engine is still reading the earlier conversation. Send the message again in a minute.";
constexpr const char* kNoStartLine = "The new engine could not start for this chat. Restart the device and try again.";
// Lane E2-FIX3 (ruling 116): the true cause and the owner's way out of each refusal below.
constexpr const char* kNumberUnreadLine =
    "The number of this chat's conversation could not be read from the card. Send /clear to start a new "
    "conversation; the earlier ones stay stored on the device.";
constexpr const char* kHeldElsewhereLine =
    "This chat cannot start while another conversation is held in memory because its earlier history could "
    "not be imported. Send /clear in that chat to store it and go on.";
constexpr const char* kClearNotStoredLine =
    "The new conversation was not started: the card did not store it. This conversation goes on; send /clear "
    "again later.";
// PC-0178: a context skill the fixed part cannot hold whole (lane E2-CHAT).
constexpr const char* kSkillTooLargeLine =
    "Your skill %s is too large to ride every message (%u B; notes and skills share 4,096 B). It is "
    "listed among your skills instead, and read when a message needs it.";

bool                  s_begun = false;   // tg_poll only (begin runs before the poll task starts)
std::atomic<bool>     s_inTurn{false};   // written on tg_poll, read by the web task (the spinner)
std::atomic<uint32_t> s_turns{0};        // messages answered since boot (any task reads it)
String                s_turnChat;        // turn task only, as engine 1's currentChat()

String (*s_otaInstallHook)() = nullptr;

// ---- the device's parts ---------------------------------------------------------

// orchestrator_e1.cpp buildTurnDeps' platform, plus the arena's PSRAM block.
const Platform& devicePlatform() {
  static Platform p;
  static bool made = false;
  if (!made) {
    made = true;
    p.nowMs = [] { return (uint32_t)millis(); };
    p.nowEpoch = [] {
      const time_t t = time(nullptr);
      return t > 1000000000 ? (uint64_t)t : (uint64_t)0;
    };
    p.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
    p.largestFreeBlock = [] {
      return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    };
    p.delayMs = [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); };
    p.allocLarge = [](size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); };
    p.freeLarge = [](void* q) { heap_caps_free(q); };
  }
  return p;
}

uint32_t internalLargest() {
  return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// deviceTransport, read around: the largest internal block before each request (the TLS
// handshake's need) and after it, the smallest of the turn kept.
class MeasuredTransport : public HttpTransport {
 public:
  bool exec(const HttpRequest& req, HttpResponse& out, std::string& err) override {
    note(internalLargest());
    const uint32_t t0 = millis();
    const bool ok = deviceTransport().exec(req, out, err);
    note(internalLargest());
    ++requests;
    logRequest(req, out.status, millis() - t0);
    return ok;
  }
  int execJson(const HttpRequest& req, JsonDocument& doc, const JsonDocument& filter, std::string& err) override {
    note(internalLargest());
    const int st = deviceTransport().execJson(req, doc, filter, err);
    note(internalLargest());
    ++requests;
    return st;
  }
  void reset() {
    minLargest = minPsram = UINT32_MAX;
    requests = 0;
  }
  uint32_t minLargest = UINT32_MAX;
  uint32_t minPsram = UINT32_MAX;   // PSRAM free low water across the turn's requests (lane E2-FIX4)
  uint32_t requests = 0;

 private:
  void note(uint32_t v) {
    if (v < minLargest) minLargest = v;
    const uint32_t ps = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (ps < minPsram) minPsram = ps;
  }
  // One request on one line (lane E2-FIX4, the photo measurement): its bytes on the wire, the photos
  // spliced into it and their base64 bytes, and how long the exchange took.
  static void logRequest(const HttpRequest& req, int status, uint32_t ms) {
    size_t photoB = 0;
    for (const auto& sp : req.splices) photoB += spliceLen(sp);
    alogf("e2: req bodyB=%u photos=%u photoB64=%u ms=%u status=%d", (unsigned)bodyLength(req), (unsigned)req.splices.size(),
          (unsigned)photoB, (unsigned)ms, status);
  }
};
MeasuredTransport s_transport;

bool localChat(const String& chatId) { return chatId == "web" || chatId == "voice" || chatId == "serial"; }

// One part out, once: Telegram sends it as made; the web, voice and serial chats take it through
// the common deliver() (the reply sinks, the outbound screen).
bool sendOnePart(const std::string& chat, const std::string& part) {
  const String c(chat.c_str());
  if (localChat(c)) {
    bool queued = false;
    const bool delivered = deliver(c, String(part.c_str()), /*system=*/false, &queued);
    return delivered && queued;
  }
  return telegram::sendPart(c, part);
}

std::unique_ptr<plain::ChatBinding> s_chat;   // the one resident chat (tg_poll only)

// ---- /clear: a new conversation keeps the stored one --------------------------------
// The conversation of a chat is the transcript under its key: generation 0 is the chat id itself
// (what the history import fills), generation n is "<chat>_c<n>". /clear moves the chat to the
// next generation; every earlier transcript stays on the card as it was. The generation is kept
// in /mem/tx/<chat>.gen (plain::readGeneration, written by temp and rename); with no card fitted it
// is kept in RAM until the restart. Lane E2-FIX2B (finding P7): a read that cannot tell is an error
// for that message (never generation 0) and is not kept; the next message reads again.
std::string s_genChat;   // the chat the RAM copy is for (tg_poll only)
uint32_t s_gen = 0;
const char* s_noStartWhy = nullptr;   // the owner's line when chatFor answered null (tg_poll only; E2-FIX3)

bool noCardFitted() { return !memory::haveSd() && !memory::sdLost() && !nimbus::fault::active(nimbus::fault::SD); }

// The chat's generation; false when the card could not tell.
bool generationOf(const String& chatId, uint32_t& g) {
  g = 0;
  if (s_genChat == chatId.c_str()) return g = s_gen, true;
  if (!noCardFitted()) {
    const plain::GenRead r = plain::readGeneration(txdev::cardTxFiles(), chatId.c_str(), g);
    if (r == plain::GenRead::Unknown) {
      alogf("e2: the conversation number of chat %s could not be read: tried again with the next message", chatId.c_str());
      s_noStartWhy = kNumberUnreadLine;
      return false;
    }
  }
  s_genChat = chatId.c_str();
  s_gen = g;
  return true;
}

// The chat's transcript key; "" when its generation could not be read.
std::string txKeyFor(const String& chatId) {
  uint32_t g = 0;
  if (!generationOf(chatId, g)) return std::string();
  return g ? std::string(chatId.c_str()) + "_c" + std::to_string(g) : std::string(chatId.c_str());
}

// ---- the turn's photos ------------------------------------------------------------
std::unique_ptr<nimbus::orch::TurnImages> s_photos;   // the running turn's set (tg_poll only)

// An earlier photo still in view on this turn (tx::visiblePhotos, lane E2-FIX4): its stored copy on the
// card (the intake keeps every photo at /mem/blobs/<ref>.jpg, or .bin for one sent as a file),
// read into PSRAM once per turn in slices (readPhotoSliced). Freed after the turn.
struct CarriedPhoto {
  std::string ref;
  uint8_t* bytes = nullptr;
  size_t len = 0;
  std::string mime;
};
std::vector<CarriedPhoto> s_carried;   // tg_poll only

void dropCarried() {
  for (CarriedPhoto& c : s_carried) free(c.bytes);
  s_carried.clear();
}

// An earlier photo's stored copy into PSRAM in slices of kPhotoSliceBytes, the card lock held for one
// slice at a time (lane E2-FIX4: a 200 KB photo read whole held it about 100 ms, a 512 KB one would
// hold it about 260 ms). Null with `why` when it is missing, too large or cannot be read whole.
constexpr size_t kPhotoSliceBytes = 32 * 1024;
uint8_t* readPhotoSliced(const std::string& path, size_t& len, uint32_t& longestMs, const char*& why) {
  len = 0;
  longestMs = 0;
  why = "could not be fetched";
  size_t size = 0;
  {
    memory::Lock lk;
    File f = memory::dataFs().open(path.c_str(), FILE_READ);
    if (!f) return nullptr;
    size = f.size();
    f.close();
  }
  if (!size || size > nimbus::orch::kImagePartBytes) {
    if (size) why = "too large";
    return nullptr;
  }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);   // PSRAM only, as loadToPsram
  if (!buf) {
    why = "device low on memory";
    return nullptr;
  }
  for (size_t at = 0; at < size;) {
    const size_t want = size - at < kPhotoSliceBytes ? size - at : kPhotoSliceBytes;
    size_t got = 0;
    const uint32_t t0 = millis();
    {
      memory::Lock lk;
      File f = memory::dataFs().open(path.c_str(), FILE_READ);
      if (f && f.size() == size && f.seek(at)) got = f.read(buf + at, want);
      if (f) f.close();
    }
    const uint32_t ms = millis() - t0;
    if (ms > longestMs) longestMs = ms;
    if (got != want) {   // changed or cut under us: never shown as less than it is
      free(buf);
      return nullptr;
    }
    at += got;
  }
  len = size;
  why = "";
  return buf;
}

plain::Status carriedPhoto(const std::string& ref, plain::PhotoBytes& out) {
  for (const CarriedPhoto& c : s_carried)
    if (c.ref == ref) {
      if (!c.bytes) {   // read once this turn and not there: its line, no second card read
        out = plain::PhotoBytes{};
        return plain::Status::fail(nimbus::orch::tx::Err::NotFound, "e2.carriedPhoto");
      }
      out.bytes = c.bytes;
      out.len = c.len;
      out.mime = c.mime;
      return plain::Status::success();
    }
  CarriedPhoto c;
  c.ref = ref;
  uint32_t readMs = 0, longestMs = 0;
  const std::string hash = ref.size() > 1 && ref[0] == 'p' ? ref.substr(1) : std::string();
  for (const char* ext : {"jpg", "bin"}) {
    if (hash.empty() || !memory::haveSd()) break;
    const std::string path = nimbus::orch::blobPath("/mem/blobs", hash, ext);
    const char* why = "";
    {
      memory::Lock lk;
      if (!memory::dataFs().exists(path.c_str())) continue;
    }
    readMs = millis();
    c.bytes = readPhotoSliced(path, c.len, longestMs, why);
    readMs = millis() - readMs;
    if (!c.bytes) alogf("e2: earlier photo %s not read (%s)", ref.c_str(), why);
    break;
  }
  const char* mime = c.bytes ? nimbus::orch::sniffImageMime(c.bytes, c.len) : "";
  if (!*mime) {
    free(c.bytes);
    c.bytes = nullptr;
    c.len = 0;
    s_carried.push_back(std::move(c));   // remembered: the turn's next render does not read the card again
    out = plain::PhotoBytes{};
    return plain::Status::fail(nimbus::orch::tx::Err::NotFound, "e2.carriedPhoto");
  }
  c.mime = mime;
  out.bytes = c.bytes;
  out.len = c.len;
  out.mime = c.mime;
  alogf("e2: earlier photo %s shown again (%u B, card read %u ms, longest hold %u ms)", ref.c_str(), (unsigned)c.len,
        (unsigned)readMs, (unsigned)longestMs);
  s_carried.push_back(std::move(c));
  return plain::Status::success();
}

// The photo hook: this message's photos, else an earlier one from the card.
plain::Status photoFor(const std::string& ref, plain::PhotoBytes& out) {
  if (plain::photoBytesFrom(s_photos.get(), ref, out).ok()) return plain::Status::success();
  return carriedPhoto(ref, out);
}

// ---- a context skill too large for the pinned block: told once per boot --------------
std::vector<std::string> s_skillTold;   // "<id>@<bytes>" already told (tg_poll only)
std::vector<std::string> s_skillTell;   // to tell after the running turn
void noteSkillRefused(const std::string& id, size_t bytes) {
  const std::string key = id + "@" + std::to_string(bytes);
  for (const std::string& k : s_skillTold)
    if (k == key) return;
  s_skillTold.push_back(key);
  char line[320];
  snprintf(line, sizeof(line), kSkillTooLargeLine, id.c_str(), (unsigned)bytes);
  alogf("e2: skill %s (%u B) does not fit the pinned block: listed instead", id.c_str(), (unsigned)bytes);
  s_skillTell.push_back(line);
}

// One turn's usage where the owner sees it (the Usage page, /usage: tokens and cost per provider,
// the month), once per turn; store::recordProviderSpend also counts it against the monthly limits.
nimbus::orch::TokenUsage s_lastUsage, s_sessUsage;   // the Usage panel's last turn and session (tg_poll writes)
std::mutex s_usageMx;                                 // the web task reads them
void recordUsage(const plain::TurnOutcome& out, const std::string& model) {
  const nimbus::orch::TurnSpend spend = plain::turnSpendOf(out, "anthropic", model);
  store::recordProviderSpend(spend, "turn");
  {
    std::lock_guard<std::mutex> lk(s_usageMx);
    s_lastUsage = spend.total();
    s_sessUsage += s_lastUsage;
  }
  budget::noteTurnEnd("anthropic", model, "anthropic", "owner", out.delivered);
}

// The boot's import of the earlier engine's conversation, on one line (what the bench reads).
void logImport(const plain::ChatBinding& b) {
  if (!b.migrationRan()) return;
  const nimbus::orch::tx::MigrationOutcome& o = b.migration();
  alogf("e2: import chat=%s %s@%s step=%s reentry=%d digest0=%d carried=%d verdict=%s exported=%d asks=%u turns=%u "
        "imported=%u named=%u items=%u hi=m%08x ms=%u queries=%u stackFree=%u intLargest=%u psramFree=%u",
        b.config().chatId.c_str(), nimbus::orch::tx::errSlug(b.migrationStatus().err),
        b.migrationStatus().where ? b.migrationStatus().where : "", nimbus::orch::tx::migrationStepSlug(o.step),
        (int)o.reentry, (int)o.digest0, (int)o.carried, nimbus::orch::tx::digest0VerdictSlug(o.verdict),
        (int)o.exported, (unsigned)o.asks, (unsigned)o.turns, (unsigned)o.imported, (unsigned)o.named,
        (unsigned)o.items, (unsigned)o.hi, (unsigned)b.migrationMs(), (unsigned)migratedev::lastQueries(),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)internalLargest(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// The resident binding when it serves this message (`keep`), else it is let go and `keep` is
// false. E2-STORE: a binding whose turns are held only in RAM (the card did not answer at its
// boot) is never dropped with them (ruling 81). Its own chat and role keep it, on its model and
// conversation, until the card is back; another chat or role is refused meanwhile (null, keep true).
plain::ChatBinding* residentFor(const String& chatId, const std::string& model, nimbus::orch::Role role, bool& keep) {
  keep = false;
  if (!s_chat) return nullptr;
  const plain::ChatBindingConfig& c = s_chat->config();
  const bool same = c.chatId == chatId.c_str() && c.role == role;
  const std::string key = txKeyFor(chatId);
  keep = same && (key.empty() || c.chatKey == key) && c.model == model;   // P7: an unread number keeps the resident
  if (keep) return s_chat.get();
  if (!s_chat->reload()) {
    alogf("e2: chat %s keeps its RAM-only binding until the card is back", c.chatId.c_str());
    keep = true;
    if (!same && s_chat->importStuck()) s_noStartWhy = kHeldElsewhereLine;   // E2-FIX3: its /clear lets it go
    return same ? s_chat.get() : nullptr;
  }
  s_chat.reset();   // another chat, model or role: the arena goes back before the next one
  return nullptr;
}

// The chat's binding for this model and role, booted (null when it could not start).
plain::ChatBinding* chatFor(const String& chatId, const std::string& model, nimbus::orch::Role role) {
  s_noStartWhy = nullptr;
  bool keep = false;
  plain::ChatBinding* resident = residentFor(chatId, model, role, keep);
  if (keep) return resident;
  // The binding's parts on the heap, off the tg_poll stack: a routine fire boots a chat from deep in
  // the loops tick (lane E2-FIX2B; tools/release_gate/check_tg_poll_stack.py).
  struct Parts {
    plain::ChatBindingConfig cfg;
    plain::ChatBindingDeps d;
  };
  std::unique_ptr<Parts> parts(new (std::nothrow) Parts());
  if (!parts) return nullptr;
  plain::ChatBindingConfig& cfg = parts->cfg;
  cfg.chatKey = txKeyFor(chatId);
  cfg.chatId = chatId.c_str();
  cfg.model = model;
  cfg.summarizerModel = store::subModel("anthropic").c_str();
  cfg.role = role;
  cfg.timeZone = store::deviceTz().c_str();
  cfg.dayStartMin = store::dayStartMin();
  cfg.storedRounds = store::orchLoopRounds();
  cfg.monthlyCeilingMicro = spend::limitMicro();
  if (cfg.chatKey.empty()) return nullptr;   // P7: the conversation number could not be read (logged)
  if (!nimbus::orch::tx::chatKeyOk(cfg.chatKey)) {
    alogf("e2: chat %s has no transcript key", chatId.c_str());
    return nullptr;
  }
  plain::ChatBindingDeps& d = parts->d;
  d.platform = &devicePlatform();
  d.transport = &s_transport;
  d.files = &txdev::cardTxFiles();
  d.registry = &memory::registry();
  d.sendPart = sendOnePart;
  d.key = [] {
    const String k = store::anthropicKey();
    return std::string(k.c_str(), k.length());
  };
  d.pinnedNotes = [](const nimbus::orch::Principal& who) { return notesdev::pinnedNotesFor(who); };
  d.compactCtl = deviceCompactCtlIO();
  if (!localChat(chatId)) d.seen = &telegram::seenIndex();
  d.log = [](const std::string& line) { alog(line.c_str()); };
  // Lane E2-TOOLS: the files a person may read, for the request. Lane E2-FSAFE: a call that read outside
  // content (web_search; a file from inbox/, agents/ or another person, as the tool marked it) stores an
  // outside result, so the turn is tainted; the rails then keep its note changes on the pending shelf.
  d.outside = [](const std::string&) { return tools::takeOutsideCall(); };
  d.railPending = [](const plain::TurnCtx& t, const plain::ToolCall& c) {
    if (c.name != "note" || !t.who) return plain::Status::fail(nimbus::orch::tx::Err::InvalidArgument, "e2.rails.other");
    const PsramFirstScope psram;
    JsonDocument args;
    (void)deserializeJson(args, c.argsJson, DeserializationOption::NestingLimit(64));
    const nimbus::orch::ToolResult r =
        notesdev::notebook().keep(args.as<JsonObjectConst>(), *t.who, nimbus::orch::notes::Origin::Tainted);
    return r.success ? plain::Status::success() : plain::Status::fail(nimbus::orch::tx::Err::InvalidArgument, "e2.rails.kept");
  };
  d.recentFiles = [](const nimbus::orch::Principal& who) -> nimbus::orch::tx::RecentFilesBlock::Source {
    return [who](std::vector<nimbus::orch::tx::RecentFile>& out) { return files::recentFiles(who, out); };
  };
  d.photo = photoFor;
  d.skills = [](const nimbus::orch::Principal&) { return skillsdev::chatSkills(); };
  d.skillRefused = noteSkillRefused;
  d.usage = [model](const plain::TurnOutcome& out) { recordUsage(out, model); };
  // Lane E2-CMP: the summarizer's requests count where a turn's do (the usage view and the monthly ceiling).
  d.compactUsage = [](const std::string& provider, const std::string& m, const nimbus::orch::TokenUsage& u) {
    nimbus::orch::TurnSpend spend;
    spend.add(provider.c_str(), m.c_str(), u);
    store::recordProviderSpend(spend, "compact");
  };
  if (!localChat(chatId))   // the web chat shows its own spinner; Telegram gets the line (RB-E3)
    d.progress.send = [](const std::string& chat, const std::string& text, int64_t& msgId) {
      msgId = 0;   // a new line each time: no edit on this path
      return telegram::turnProgressLine(String(chat.c_str()), String(text.c_str()));
    };
  d.oldEngine = migratedev::fill;   // E2-MIG: the earlier engine's conversation, before the log opens
  // No card fitted at all (none mounted at begin()): the chat runs from RAM by design. A card
  // that is lost, faulted or not answering is an outage: the binding reloads it (E2-STORE).
  d.noCard = noCardFitted;
  const uint32_t spiFree0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const uint32_t spiBig0 = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const uint32_t t0 = millis();
  txdev::cardTxFiles().resetHolds();
  std::unique_ptr<plain::ChatBinding> b(new (std::nothrow) plain::ChatBinding(cfg, d));
  if (!b) return nullptr;
  plain::Status st;
  {
    PsramFirstScope psramFirst;
    st = b->boot();
  }
  const nimbus::orch::tx::TxArena* a = b->arena();
  alogf("e2: boot chat=%s model=%s %s@%s arena=%u B psram free %u -> %u B, largest %u -> %u B, %u ms",
        chatId.c_str(), model.c_str(), nimbus::orch::tx::errSlug(st.err), (st.where ? st.where : ""),
        (unsigned)(a ? a->capacity() : 0), (unsigned)spiFree0, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)spiBig0, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)(millis() - t0));
  const nimbus::orch::tx::CardHolds& h = txdev::cardTxFiles().holds();
  alogf("e2: boot holdMs read=%u list=%u whole=%u rename=%u n=%u reads readTotalMs=%u readB=%u", (unsigned)h.readMs,
        (unsigned)h.listMs, (unsigned)h.wholeMs, (unsigned)h.renameMs, (unsigned)h.reads, (unsigned)h.readTotalMs,
        (unsigned)h.readBytes);
  logImport(*b);
  if (!st.ok()) return nullptr;
  s_chat = std::move(b);
  return s_chat.get();
}

// The day-stream row id "m%08x" as a number (0: none).
uint64_t rowNumber(const String& id) {
  if (id.length() < 2 || id[0] != 'm') return 0;
  return (uint64_t)strtoull(id.c_str() + 1, nullptr, 16);
}

// Effects a turn's tools asked for, after the turn. No engine-2 tool asks for one yet: each is
// named in the log; an install runs through the hook the web route set, a restart restarts.
void runDeferred(const plain::DeferredEffects& d) {
  if (!d.mask) return;
  if (d.has(plain::Deferred::HeadSwitch)) alog("e2: a head switch was asked; not bound on this build");
  if (d.has(plain::Deferred::Install) && s_otaInstallHook) alogf("e2: install: %s", s_otaInstallHook().c_str());
  if (d.has(plain::Deferred::Restart)) {
    alog("e2: restart asked by the turn");
    delay(500);
    ESP.restart();
  }
}

// One turn's figures on one log line: what the bench reads.
void logTurn(const plain::ChatBinding& b, const plain::TurnOutcome& out, const plain::Status& st, uint32_t ms) {
  const nimbus::orch::tx::TurnEndFacts& e = out.end;
  uint32_t reqBytes = 0;
  for (const auto& x : e.exchanges) reqBytes = x.requestBytes > reqBytes ? x.requestBytes : reqBytes;
  const nimbus::orch::tx::CardHolds& h = txdev::cardTxFiles().holds();
  alogf("e2: turn chat=%s model=%s %s@%s cause=%s ms=%u rounds=%u tools=%u req=%u maxReqB=%u prompt=%u out=%u "
        "cacheR=%u cacheW=%u lastPrompt=%u uUSD=%u parts=%u/%u cacheW1h=%u",
        b.config().chatId.c_str(), b.config().model.c_str(), nimbus::orch::tx::errSlug(st.err), (st.where ? st.where : ""),
        e.cause.c_str(), (unsigned)ms, (unsigned)out.rounds, (unsigned)out.toolCalls, (unsigned)out.headRequests,
        (unsigned)reqBytes, (unsigned)e.promptTokens, (unsigned)e.outputTokens, (unsigned)e.cacheReadTokens,
        (unsigned)e.cacheWriteTokens, (unsigned)e.lastPromptTokens, (unsigned)e.microUsd, (unsigned)b.sentParts(),
        (unsigned)(b.sentParts() + b.refusedParts()), (unsigned)e.cacheWrite1hTokens);
  alogf("e2: mem stackFree=%u intLargestMin=%u intLargestNow=%u psramFree=%u psramMin=%u psramLargest=%u reqs=%u "
        "holdMs append=%u whole=%u rename=%u read=%u list=%u n=%u/%u/%u/%u refused=%u/%u/%u readTotalMs=%u readB=%u",
        (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)s_transport.minLargest, (unsigned)internalLargest(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)s_transport.minPsram,
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)s_transport.requests, (unsigned)h.appendMs, (unsigned)h.wholeMs, (unsigned)h.renameMs,
        (unsigned)h.readMs, (unsigned)h.listMs, (unsigned)h.appends, (unsigned)h.wholes, (unsigned)h.renames,
        (unsigned)h.reads, (unsigned)h.refusedNoCard, (unsigned)h.refusedFloor, (unsigned)h.refusedTask,
        (unsigned)h.readTotalMs, (unsigned)h.readBytes);
}

// The line a message gets when no engine-2 turn can run for it; empty: a turn can run.
const char* refusalFor(const String& host) {
  if (host != "anthropic") return kNotClaudeLine;
  if (!store::hasAnthropicKey()) return kNoKeyLine;
  if (spend::frozen()) return kFrozenLine;
  return nullptr;
}

// The person's message into the chat's day stream (the web chat's history, a later migration's
// rows). `turn`: an engine-2 turn stores it, so it is tagged e2 and never imported (RB-MG1a);
// otherwise (a device line answered it) the import brings it in once (plain::ownerRowTags, P10).
String captureOwnerRow(const String& text, const String& fromName, const String& chatId, bool turn) {
  const std::string tags = plain::ownerRowTags(std::string(fromName.c_str(), fromName.length()), turn);
  return memory::captureMessage(chatId.c_str(), "user", nimbus::orch::MsgKind::Message, text, "", String(tags.c_str()));
}

// P10: a message a device line answered reaches the conversation through the import at the chat's
// next boot, so a resident binding that holds nothing in RAM only is let go (the next message boots it).
void leaveToImport(const String& chatId) {
  if (s_chat && s_chat->config().chatId == chatId.c_str() && !s_chat->unloaded()) s_chat.reset();
}

// One device line instead of a turn (system provenance, never model text).
bool sayInstead(const String& chatId, const char* line) {
  bool queued = false;
  const bool sent = deliver(chatId, String(line), /*system=*/true, &queued);
  return sent && queued;
}

// E2-MIG: a chat's first engine-2 start waits until the earlier engine's history is indexed (the
// chat's own key only: a conversation /clear started takes no import).
bool waitsForHistory(const String& chatId) {
  const bool resident = s_chat && s_chat->config().chatId == chatId.c_str();
  const bool baseKey = txKeyFor(chatId) == chatId.c_str();
  return !resident && baseKey && !migratedev::historyReady(chatId.c_str());
}

// One person's message as one engine-2 turn (tg_poll). Returns whether a reply went out.
bool runOne(const String& text, const String& fromName, const String& chatId) {
  const String host = store::resolvedOrchHost();
  if (const char* line = refusalFor(host)) {
    captureOwnerRow(text, fromName, chatId, false);
    leaveToImport(chatId);
    return sayInstead(chatId, line);
  }
  if (waitsForHistory(chatId)) {
    captureOwnerRow(text, fromName, chatId, false);
    return sayInstead(chatId, kReadingLine);
  }
  const std::string model(store::orchModel(host).c_str());
  plain::ChatBinding* b = chatFor(chatId, model, roleOfChat(chatId));
  if (!b) {
    captureOwnerRow(text, fromName, chatId, false);
    return sayInstead(chatId, s_noStartWhy ? s_noStartWhy : kNoStartLine);
  }
  plain::ChatMessage msg;
  msg.ch = chatId == "web" ? "web" : chatId == "voice" ? "voice" : chatId == "serial" ? "console" : "tg";
  if (!localChat(chatId)) msg.src = telegram::turnUpdateIds(chatId);
  // RB-TG1: a message the transcript already holds (re-served after a restart) was stored and
  // answered then: no second row, no second answer.
  if (b->allSeen(msg.src)) {
    alogf("e2: message to %s already taken before the restart: not answered again", chatId.c_str());
    return true;
  }
  const String row = captureOwnerRow(text, fromName, chatId, true);
  msg.text = std::string(text.c_str(), text.length());
  msg.parts = plain::photoParts(s_photos.get(), host.c_str(), model);
  msg.tzMinutes = (int16_t)(utcOffsetAt((uint32_t)time(nullptr)));
  msg.m = rowNumber(row);
  txdev::cardTxFiles().resetHolds();
  s_transport.reset();
  plain::TurnOutcome out;
  plain::Status st;
  const uint32_t t0 = millis();
  {
    PsramFirstScope psramFirst;
    st = b->turn(msg, out);
  }
  logTurn(*b, out, st, millis() - t0);
  if (!msg.parts.empty())
    alogf("e2: photos=%u bytes=%u psramFree=%u psramLargest=%u", (unsigned)msg.parts.size(),
          (unsigned)s_photos->bytes(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  if (out.delivered && !out.text.empty())
    memory::captureMessage(chatId.c_str(), "assistant", nimbus::orch::MsgKind::Message,
                           String(out.text.c_str()), "", "e2");
  else if (!out.statusText.empty())
    memory::captureMessage(chatId.c_str(), "system", nimbus::orch::MsgKind::Message,
                           String(out.statusText.c_str()), "", "e2");
  runDeferred(out.deferred);
  const bool sent = b->sentParts() > 0;
  for (const std::string& line : s_skillTell) sayInstead(chatId, line.c_str());
  s_skillTell.clear();
  return sent;
}

// ---- what the owner's commands ask of engine 2 -----------------------------------------

// The chat's binding for its configured head, booted; null when no turn could run for it.
plain::ChatBinding* bindingFor(const String& chatId) {
  const String host = store::resolvedOrchHost();
  if (refusalFor(host)) return nullptr;
  return chatFor(chatId, std::string(store::orchModel(host).c_str()), roleOfChat(chatId));
}

// /clear's next number, stored before anything uses it (P7; E2-FIX3, R2 and R4): never a number a
// transcript holds; an unreadable number is left by writing a fresh one; a write the card did not
// take did not happen, here or in RAM. False: the chat stays on its number.
bool advanceGeneration(const String& chatId) {
  uint32_t now = 0;
  const bool read = generationOf(chatId, now);
  uint32_t next = now + 1;
  if (!noCardFitted()) {
    nimbus::orch::tx::TxFiles& files = txdev::cardTxFiles();
    if (!plain::freeGeneration(files, chatId.c_str(), read ? now : 0, next) ||
        !plain::storeGeneration(files, chatId.c_str(), next)) {
      alogf("e2: /clear %s refused: conversation number %u not stored", chatId.c_str(), (unsigned)next);
      return false;
    }
  } else if (!read) {
    return false;
  }
  s_genChat = chatId.c_str();
  s_gen = next;
  return true;
}

bool clearChat(const String& chatId, String& why) {
  why = String();
  // E2-STORE with E2-CHAT: a binding whose turns are held only in RAM is never dropped with them
  // (ruling 81). The conversation stays as it is until the card is back. E2-FIX3 (finding R1): when
  // a history import that cannot finish is all that holds it, /clear stores those turns in the new
  // conversation (the earlier history stays on the card as it is).
  plain::ChatBinding* held = s_chat && s_chat->config().chatId == chatId.c_str() ? s_chat.get() : nullptr;
  const bool moveHeld = held && !held->reload();
  if (moveHeld && !held->importStuck()) {
    alogf("e2: /clear %s refused: the turns held in RAM are not stored yet", chatId.c_str());
    why = kClearNotStoredLine;
    return false;
  }
  if (!advanceGeneration(chatId)) {
    why = kClearNotStoredLine;
    return false;
  }
  const std::string key = txKeyFor(chatId);
  bool keepHeld = false;
  if (moveHeld) {
    const plain::Status st = held->storeHeldAs(key);
    keepHeld = !st.ok();   // the card refused the turns: still held, stored in `key` before the next message
    alogf("e2: /clear %s: the turns held in RAM %s %s (%s)", chatId.c_str(), keepHeld ? "not yet stored in" : "stored in",
          key.c_str(), nimbus::orch::tx::errSlug(st.err));
  }
  if (held && !keepHeld) s_chat.reset();   // the arena goes back first
  alogf("e2: /clear %s: conversation %s", chatId.c_str(), key.c_str());
  s_noStartWhy = nullptr;
  if (bindingFor(chatId)) return true;
  if (s_noStartWhy) why = s_noStartWhy;
  return false;
}

bool compactChat(const String& chatId) {
  plain::ChatBinding* b = bindingFor(chatId);
  if (!b) return false;
  s_transport.reset();
  plain::Status st;
  {
    PsramFirstScope psramFirst;
    st = b->compactNow();
  }
  alogf("e2: /compact %s: %s@%s reqs=%u", chatId.c_str(), nimbus::orch::tx::errSlug(st.err), st.where ? st.where : "",
        (unsigned)s_transport.requests);
  return true;   // the driver answered on the chat (done, nothing to do, or failed)
}

// PC-0177: the owner's budget view of engine 2 (rulings 105 and 106), in tokens.
String budgetViewText(const String& chatId) {
  plain::ChatBinding* b = bindingFor(chatId);
  plain::BudgetView v;
  if (!b || !b->budgetView(v).ok()) return String();
  char line[400];
  snprintf(line, sizeof(line),
           "New engine, in tokens: system prompt %u, tools %u, fixed part %u. Summary %u (up to %u), "
           "recent conversation %u (recent part %u). Compaction at %u, aim %u. Max rounds %u, hard time limit %u s.",
           (unsigned)v.systemTokens, (unsigned)v.toolsTokens, (unsigned)v.fixedTokens, (unsigned)v.summaryTokens,
           (unsigned)v.summaryBudgetTokens, (unsigned)v.recentTokens, (unsigned)v.recentPartTokens,
           (unsigned)v.dueTokens, (unsigned)v.aimTokens, (unsigned)v.maxRounds, (unsigned)v.hardTimeS);
  return String(line);
}

const e2cmd::EngineOps& engineOps() {
  static e2cmd::EngineOps ops;
  if (!ops.clear) {
    ops.clear = clearChat;
    ops.compact = compactChat;
    ops.budgetView = budgetViewText;
  }
  return ops;
}

// The staged post-update notice (stageUpdateNotice may run on any task): one slot in PSRAM (as on
// engine 1), copied under the spinlock into a second PSRAM buffer the poll task reads, nothing
// allocated or logged inside it, nothing large on the tg_poll stack.
constexpr size_t     kNoticeBytes = 512;
char                 (&s_noticeText)[kNoticeBytes] = ::nimbus::psramStatic<char[kNoticeBytes]>();
char                 (&s_noticeOut)[kNoticeBytes] = ::nimbus::psramStatic<char[kNoticeBytes]>();   // tg_poll only
volatile bool        s_noticePending = false;
portMUX_TYPE         s_noticeMux = portMUX_INITIALIZER_UNLOCKED;

void drainUpdateNotice() {
  if (!s_noticePending) return;
  portENTER_CRITICAL(&s_noticeMux);
  memcpy(s_noticeOut, s_noticeText, kNoticeBytes);
  s_noticePending = false;
  portEXIT_CRITICAL(&s_noticeMux);
  const String chat = telegram::ownerChat();
  if (chat.length() == 0) return;   // nowhere to deliver: dropped, as on engine 1
  const String text(s_noticeOut);
  telegram::send(chat, text, /*block=*/false);
  // Into the chat's history at once (engine_api.h), as engine 1's recordNotice does.
  memory::captureMessage(chat.c_str(), "system", nimbus::orch::MsgKind::Message, text);
}

// The owner chat's engine boots before its first message, so the seen index holds what the card
// holds before a re-served update is answered (RB-TG1). tg_poll, once.
bool s_bootTried = false;
void bootOwnerChat() {
  if (s_bootTried) return;
  if (!memory::episodicIndexComplete()) return;   // E2-MIG: tried again once the history is indexed
  s_bootTried = true;
  const String owner = telegram::ownerChat();
  const String host = store::resolvedOrchHost();
  if (owner.length() == 0 || refusalFor(host)) return;
  (void)chatFor(owner, std::string(store::orchModel(host).c_str()), roleOfChat(owner));
}

// ---- reminders and routines (lane E2-SCHED) ------------------------------------------
namespace sch = nimbus::orch::schedule;

// Engine 1's nightly upkeep record (lib/harness dream.h kLoopId, an engine-1 file): no model text.
constexpr char kUpkeepId[] = "dream";

// One outbox of routine notes per chat, on the card (/mem/routines/held-<chat>.json). tg_poll only.
struct HeldSlot {
  std::string chatId;
  std::unique_ptr<plain::HeldStore> store;
};
std::vector<HeldSlot> s_held;

plain::HeldStore* heldFor(const std::string& chatId) {
  if (!memory::haveSd()) return nullptr;   // no card: no outbox, a routine is skipped as "no card"
  for (HeldSlot& h : s_held)
    if (h.chatId == chatId) return h.store.get();
  std::unique_ptr<plain::HeldStore> st(
      new (std::nothrow) plain::HeldStore(plain::heldFileOn(txdev::cardRoutineFiles(), plain::heldPathFor(chatId))));
  if (!st) return nullptr;
  (void)st->load();
  s_held.push_back(HeldSlot{chatId, std::move(st)});
  return s_held.back().store.get();
}

plain::QuietHours quietNow() {
  plain::QuietHours q;
  q.endMinute = loops::quietHoursEndMinute();
  return q;
}

int16_t tzNow() { return (int16_t)utcOffsetAt((uint32_t)time(nullptr)); }

// The chat a fire delivers to with no binding (a head engine 2 cannot boot): the reminder's text
// still goes, through the same one-part send; nothing is appended to a transcript.
plain::OwnerSink bareSink(const std::string& chatId) {
  static Channel ch;
  ch.send = sendOnePart;
  ch.isAllowed = [](const std::string&) { return true; };
  plain::OwnerSink s;
  s.chatKey = chatId;
  s.chatId = chatId;
  s.deliver = [](const plain::DeliverInput& in, plain::DeliverOutcome& out) {
    return plain::deliverTurn(plain::DeliverDeps{&ch, nullptr, nullptr}, in, out);
  };
  s.append = [chatId](const std::string&, uint64_t) {
    alogf("e2: reminder to %s sent with no transcript (the chat could not start)", chatId.c_str());
    return plain::Status::success();
  };
  return s;
}

// A fire with no chat binding (a head engine 2 cannot boot): the reminder's text still goes; a
// routine waits as "chat could not start" (P15: the card is there, so not "no card").
void bareBinding(sch::FireBinding& fb, const std::string& chatId) {
  fb.owner = bareSink(chatId);
  if (memory::haveSd()) fb.files = &txdev::cardRoutineFiles();
  fb.tzMinutes = tzNow();
  fb.dayStartMin = store::dayStartMin();
}

// One schedule entry's fire (the loops tick, tg_poll, between turns).
nimbus::orch::FireOutcome fireEntry(const String& chatIn, const String& id) {
  nimbus::orch::FireOutcome out;
  const std::string sid(id.c_str(), id.length());
  if (sid == kUpkeepId) {   // engine 1's memory upkeep: engine 2 keeps no vector memory to tend
    out.ok = true;
    out.detail = std::to_string((unsigned long)time(nullptr));
    alog("e2: the nightly upkeep record is engine 1's: nothing to run");
    return out;
  }
  sch::Kind kind = sch::Kind::Routine;
  if (!sch::entryKind(scheduledev::host(), sid, kind)) kind = sch::Kind::Routine;   // fire() answers it
  if (kind == sch::Kind::Routine && spend::frozen()) {   // the money guard: a routine waits, a reminder goes
    out.deferred = true;
    out.error = nimbus::orch::kSpendFrozenError;
    return out;
  }
  const String chat = chatIn.length() ? chatIn : telegram::ownerChat();
  const std::string chatId(chat.c_str(), chat.length());
  const String host = store::resolvedOrchHost();
  plain::ChatBinding* b = nullptr;
  if (chat.length() && !(kind == sch::Kind::Routine && refusalFor(host)))
    b = chatFor(chat, std::string(store::orchModel(host).c_str()), roleOfChat(chat));
  // The binding of the fire on the heap, off the tg_poll stack (lane E2-FIX2B: the routine path is
  // the deepest the poll task runs).
  struct FireParts {
    sch::FireBinding fb;
    plain::ScheduleBindingDeps d;
  };
  std::unique_ptr<FireParts> parts(new (std::nothrow) FireParts());
  if (!parts) {
    out.deferred = true;   // no error text: the loops tick counts it as a memory deferral
    out.retryAfterSec = 60;
    return out;
  }
  sch::FireBinding& fb = parts->fb;
  const std::string sub(store::subModel("anthropic").c_str());
  if (b) {
    plain::ScheduleBindingDeps& d = parts->d;
    d.held = heldFor(chatId);
    d.quiet = quietNow();
    d.grants = loops::routineGrants();
    d.tzMinutes = tzNow();
    d.routineModel = sub;
    d.fired = [sub](const plain::FireReport& rep) {
      store::recordProviderSpend(plain::turnSpendOf(rep.outcome, "anthropic", sub), "routine");
    };
    fb = plain::fireBindingFor(*b, memory::haveSd() ? &txdev::cardRoutineFiles() : nullptr, d);
  } else {
    bareBinding(fb, chatId);
  }
  txdev::cardTxFiles().resetHolds();
  s_transport.reset();
  const uint32_t t0 = millis(), parts0 = b ? b->sentParts() : 0;
  {
    PsramFirstScope psramFirst;
    out = scheduledev::fire(nimbus::orch::LoopFireRequest{sid, std::string(), chatId, std::string()}, fb);
  }
  alogf("e2: fire %s %s chat=%s ok=%d deferred=%d answered=%d parts=%u reqs=%u ms=%u err=%s", sid.c_str(),
        kind == sch::Kind::Reminder ? "reminder" : "routine", chatId.c_str(), (int)out.ok, (int)out.deferred,
        (int)out.answered, (unsigned)(b ? b->sentParts() - parts0 : 0), (unsigned)s_transport.requests,
        (unsigned)(millis() - t0), out.error.c_str());
  return out;
}

// The outboxes' due work (a quiet-hours note once they end), one note a pass, every 30 s.
void drainOutboxes() {
  static uint32_t last = 0;
  if ((uint32_t)(millis() - last) < 30000) return;
  last = millis();
  const uint64_t now = (uint64_t)time(nullptr);
  if (now < 1000000000) return;   // no clock yet: a note waits for one
  const plain::QuietHours quiet = quietNow();
  const bool quietNowFlag = plain::inQuiet(quiet, now, tzNow());
  for (HeldSlot& h : s_held) {
    // Only an outbox with work due boots its chat (a held note in quiet hours never moves the resident chat).
    bool due = false;
    for (const plain::HeldNote& n : h.store->notes())
      due = due || !quietNowFlag || n.state == plain::HeldState::Sent || n.expires <= now || h.store->wasDelivered(n);
    if (!due) continue;
    const String chat(h.chatId.c_str());
    const String host = store::resolvedOrchHost();
    plain::ChatBinding* b = chatFor(chat, std::string(store::orchModel(host).c_str()), roleOfChat(chat));
    const size_t held = h.store->notes().size();
    const bool worked = b && plain::drainHeld(*h.store, b->ownerSink(), now, tzNow(), quiet, b->config().dayStartMin);
    alogf("e2: outbox %s: %u held, %s (%u left)", h.chatId.c_str(), (unsigned)held,
          !b ? "chat did not start" : worked ? "one note done" : "nothing went", (unsigned)h.store->notes().size());
    if (worked) return;
  }
}

// The outboxes a restart left on the card: loaded once, so their notes go when quiet hours end.
// Lane E2-FIX2B (finding P12): a listing that fails is tried again every 30 s until one answers.
void loadOutboxes() {
  static bool done = false;
  static uint32_t lastTry = 0;
  static bool tried = false;
  if (done || !memory::haveSd()) return;
  if (tried && (uint32_t)(millis() - lastTry) < 30000) return;
  tried = true;
  lastTry = millis();
  std::vector<std::string> names;
  if (!txdev::cardRoutineFiles().list(sch::kStateDir, names)) {
    alog("e2: the outboxes on the card could not be listed: tried again in 30 s");
    return;
  }
  done = true;
  for (const std::string& n : names) {
    const std::string pre = "held-", ext = ".json";
    if (n.size() > pre.size() + ext.size() && n.rfind(pre, 0) == 0 && n.compare(n.size() - ext.size(), ext.size(), ext) == 0)
      (void)heldFor(n.substr(pre.size(), n.size() - pre.size() - ext.size()));
  }
}

}  // namespace

// ---- lifecycle --------------------------------------------------------------

void begin(HeavyFabric* fabric, const Sinks& sinks) {
  g_fabric = fabric;
  g_sinks = sinks;
  if (!g_sessMux) g_sessMux = xSemaphoreCreateMutex();   // guards the session snapshot
  refreshSessionSnapshot();                               // no jobs: an empty snapshot
  // L24 (turn_memory.h): a turn keeps its small blocks in PSRAM, as on engine 1.
  setPsramFirstSwitch([](bool on) { nimbus::heapwatch::psramFirst(on); });
  // The harness library's log seam (compaction, the wire, the summarizer) into the device log, as engine 1
  // installs it: without a sink every such line is dropped (lane E2-BENCH: a failed /compact left no cause).
  hlog::setSink([](const char* line) { alog(line); });
  s_begun = true;
  alog("orchestrator: engine 2 ready (Claude heads)");
}

// ---- turn entry points ------------------------------------------------------

void handleMessage(const String& text, const String& fromName, const String& chatId) {
  // Hold-to-talk (CUM-456): a voice message counts as finished once its reply went out.
  const bool voice = chatId == "voice";
  if (voice) {
    s_voiceOk.store(false, std::memory_order_relaxed);
    s_inVoiceMessage = true;
  }
  // The photos the intake kept for this message (R1): held for the turn, freed after it.
  s_photos = nimbus::orch::takeTurnImages(chatId.c_str(), text.c_str());
  if (s_begun && e2cmd::handle(text, chatId, engineOps())) {
    if (voice) s_voiceOk.store(true, std::memory_order_relaxed);
  } else if (s_begun) {
    s_inTurn.store(true, std::memory_order_relaxed);
    s_turnChat = chatId;
    const bool sent = runOne(text, fromName, chatId);
    if (voice) s_voiceOk.store(sent, std::memory_order_relaxed);
    s_turns.fetch_add(1, std::memory_order_relaxed);
    s_turnChat = String();
    s_inTurn.store(false, std::memory_order_relaxed);
  }
  s_photos.reset();
  dropCarried();
  if (voice) {
    s_inVoiceMessage = false;
    s_voiceHandled.fetch_add(1, std::memory_order_release);
  }
}

// The loops fire hook (main.cpp): lane E2-SCHED's fire of a schedule entry. The record itself is
// read again by id (its text, kind and state), so the request's prompt and framing flags are unused.
nimbus::orch::FireOutcome injectScheduledTurn(const String& chatId, const String&, const String&, const String& id,
                                              bool, bool, bool, bool) {
  return fireEntry(chatId, id);
}

int pollJobs() {
  if (!s_begun) return 0;
  migratedev::importRulesOnce();   // once: the earlier engine's standing rules as pinned notes
  bootOwnerChat();        // once: the owner chat's transcript and seen index before its next message
  drainTerminates();      // the journal's staged terminates (none can exist without jobs)
  drainUpdateNotice();    // the fixed post-update notice - tg_poll only
  // Lane E2-SCHED: the loops tick (at most one due entry; 20 s-gated, heap and Wi-Fi guarded inside)
  // and the outboxes of routine notes. tg_poll, between turns.
  loadOutboxes();
  loops::checkDue((uint64_t)time(nullptr), s_inTurn.load(std::memory_order_relaxed), ESP.getFreeHeap());
  drainOutboxes();
  refreshSessionSnapshot();
  return 0;               // no jobs: the Telegram loop keeps its long poll
}

void setOtaInstallHook(String (*fn)()) { s_otaInstallHook = fn; }
String otaInstallReply() { return s_otaInstallHook ? s_otaInstallHook() : String("Updates aren't available on this build."); }

void stageUpdateNotice(const String& text) {
  portENTER_CRITICAL(&s_noticeMux);
  strncpy(s_noticeText, text.c_str(), kNoticeBytes - 1);
  s_noticeText[kNoticeBytes - 1] = 0;
  s_noticePending = true;
  portEXIT_CRITICAL(&s_noticeMux);
}

// ---- status for the web and the tools ---------------------------------------

bool turnInFlight() { return s_inTurn.load(std::memory_order_relaxed); }
uint32_t turnCount() { return s_turns.load(std::memory_order_relaxed); }
nimbus::orch::HeadPause providerPause(const std::string&, nimbus::orch::PaidSeam) { return {}; }
bool toolRidesLoop(const std::string&) { return false; }

// ---- last-turn data ---------------------------------------------------------

nimbus::orch::TokenUsage lastTurnUsage() {
  std::lock_guard<std::mutex> lk(s_usageMx);
  return s_lastUsage;
}
nimbus::orch::TokenUsage sessionUsage() {
  std::lock_guard<std::mutex> lk(s_usageMx);
  return s_sessUsage;
}
String lastFailJson() { return String("null"); }
char* lastTurnDebugPs(size_t& outLen) {
  outLen = 0;
  return nullptr;
}
String lastInstructions() { return String(); }

// ---- the turn in flight -----------------------------------------------------

bool inScheduledTurn() { return false; }
bool turnReadOutsideContent() { return false; }
bool telegramForTurn() { return false; }
bool fileSendForTurn() { return false; }
nimbus::orch::UnattendedFacts unattendedFacts() { return {}; }
String currentChat() { return s_turnChat; }
void noteTurnSpoke() {}

// ---- fold status ------------------------------------------------------------

bool stageManualFold(const char*) { return false; }
bool foldStatusJsonFor(const std::string&, std::string&) { return false; }
String foldStatusText(const char*) { return String("fold: engine 2 compacts on its own"); }
#ifdef NIMBUS_TEST
void stageFoldDailyReset() {}
#endif

// ---- clear conversation and the staged maintenance ----------------------------

String memorySnapshot() { return String(); }
void requestMemoryClear() {}
void requestConvReset() {}
void requestConvClear() {}
void noteConfigChanged() {}

// ---- what the common half asks of the engine ----------------------------------

bool engineRunning() { return false; }   // no ring arcs or sub-agents on engine 2 yet
bool engineReapStuckTurn(uint32_t) { return false; }
void engineSetHeadArc(bool) {}
String withTurnTag(const char* base) { return String(base); }
void engineNoteToolReplied(const std::string&) {}

// ---- engine-neutral helpers ---------------------------------------------------

// The local zone's offset from UTC in minutes at unixSec (DST-aware): the computation of
// lib/harness conversation.cpp's utcOffsetAtUnix, which an engine-2 image does not compile.
int32_t utcOffsetAt(uint32_t unixSec) {
  const time_t t = (time_t)unixSec;
  if (t < 1000000000) return 0;   // clock not set
  std::tm local = {}, utc = {};
  localtime_r(&t, &local);
  gmtime_r(&t, &utc);
  int days = local.tm_yday - utc.tm_yday;
  if (days > 1) days = -1;        // local is still Dec 31, UTC already Jan 1
  else if (days < -1) days = 1;   // and the reverse
  return days * 1440 + (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min);
}

}  // namespace orchestrator
}  // namespace agent
