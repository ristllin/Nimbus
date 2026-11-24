// orchestrator_plain.cpp - the plain engine's device seam (engine 2). Lane E2-DEV binds the engine.
//
// An engine-2 device image (the *-e2 envs, -DNIMBUS_ENGINE=2) links this file instead of
// orchestrator_e1.cpp (tools/engine/engine1_only.txt). A person's message is one engine-2 turn:
// plain::ChatBinding (nimbus/harness/plain/chat_binding.h, a copy of nimbusd's one-chat rig) over
// the device's parts, each reused from engine 1 or an earlier lane:
//   - the platform hooks of orchestrator_e1.cpp's buildTurnDeps, plus PSRAM allocLarge/freeLarge;
//   - the shared TLS transport (transport_tls.h deviceTransport), wrapped only to read the memory
//     figures around each request;
//   - the head on its provider slot (lane E2-R8a, plain/head_slot.h: store::resolvedOrchHost, its key,
//     store::orchModel), the summarizer ladder (store::subModel of the head's provider, then of each
//     keyed provider of store::subPriority), the person's role (roleOfChat), the day start and rounds;
//   - the transcript on the card (tx_store_device.h, /mem/tx; RAM only with no card);
//   - a part goes out once: Telegram through telegram::sendPart (no second split), the web and
//     voice chats through the common deliver();
//   - the shared registry (memory::registry: note, the file tools, calc, web_search, more), compaction (plain_fold_device's two
//     files), suppression, the money guard and one breaker, the seen-index rebuild at boot;
//   - the earlier engine's conversation imported at each boot of a chat, and its standing rules
//     copied into pinned notes once per boot (migrate_device.h, lane E2-MIG).
// One resident chat (one arena, sized for the head): a message for another chat, another model
// or another role boots the binding again over the same card (the transcript is refilled).
// Every provider slot the released engine has: Anthropic, OpenAI, Mistral, Z.ai, a custom endpoint and
// the Cumulo router (lane E2-R8a); spend, park and price on the head's own provider.
// Lane E2-P-SUB binds sub-agents (plain/agents.h) over engine 1's job fabric: the shared journal on
// flash (orch_persist.h FlashJournalStore, g_journal) and the portable JobEngine with this engine's
// closures; agent_start, agent_list and agent_stop on the shared registry; pollJobs pumps the jobs
// between turns; a finished job's report is saved whole as agents/<id>-<name>.md and runs as one
// report turn in the chat that started it (never inside another turn).
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
#include "../sys/config_nvs.h"      // deviceName - the resume's plain crash notice (E2-FIX12)
#include "../sys/crash_report.h"    // the crash journal of a person's turn, the kept notice (E2-FIX12)
#include "../sys/heap_watch.h"     // heapwatch::psramFirst - the turn's PSRAM-first switch
#include "../sys/psram_static.h"   // the notice slot in PSRAM, as on engine 1
#include "files_subsystem.h"    // files::recentFiles - the file tools' recent-files list (E2-TOOLS)
#include <LittleFS.h>           // the jobs' stop list on flash (lane E2-P-SUB)
#include "agent_config.h"       // AGENT_MAX_ACTIVE_INFLIGHT, ORCH_DISPATCH_MIN_HEAP, AGENT_POLL_INTERVAL_MS
#include "../hw/selftest.h"     // localTimeStr - a job's finished record
#include "fs_atomic.h"          // writeFileAtomic - the jobs' stop list (lane E2-P-SUB)
#include "orch_persist.h"       // FlashJournalStore - the sub-agent journal (lane E2-P-SUB)
#include "nimbus/harness/plain/agents.h"   // the agent tools and the report turn (lane E2-P-SUB)
#include "nimbus/orch/model_choices.h"     // effectiveModelChoices - a job's model (lane E2-P-SUB)
#include "nimbus/orch/model_selectable.h"  // PickRole
#include "adapters/image_vision.h"   // the photo set the Telegram intake keeps (R1)
#include "budget_command.h"     // noteTurnEnd - /model's last reply
#include "commands_plain.h"     // the owner's slash commands
#include "loops_subsystem.h"    // checkDue, routineGrants, quietHoursEndMinute (lane E2-SCHED)
#include "tools_plain_connectors.h"  // connectorsdev::beforeTurn (E2-P-CONN1)
#include "file_tools_device.h"  // tools::takeOutsideCall - the call just dispatched read outside content (E2-FSAFE)
#include "memory_subsystem.h"   // memory::captureMessage, memory::registry
#include "migrate_device.h"     // migratedev::fill, importRulesOnce - the earlier engine's history and rules
#include "history_tools_device.h"   // E2-SRCH: search and view
#include "nimbus/fault.h"       // FAULT sd: a card that is fitted but faulted
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/plain/chat_commands.h"   // readGeneration, writeGeneration - /clear (P7)
#include "nimbus/harness/plain/chat_photos.h"
#include "nimbus/harness/plain/head_gate.h"   // engine 1's gates: inbound screen, provider limit, park (E2-FIX14)
#include "nimbus/harness/plain/schedule_binding.h"   // reminder and routine fires (lane E2-SCHED)
#include "nimbus/harness/plain/surface.h"           // the web pages' asks, answered here (lane E2-P-SURF)
#include "safety_activity_store.h"   // safety::record - a call the rails refused (lane E2-P-SURF)
#include "nimbus/harness/plain/waiting.h"            // the messages kept during the read-in (E2-FIX13)
#include "nimbus/orch/turn_journal.h"              // kTurnSourceOwner - the crash journal (E2-FIX12)
#include "nimbus/orch/spend_ceiling.h"               // kSpendFrozenError - a routine waits while frozen
#include "nimbus/harness/turn_memory.h"   // PsramFirstScope
#include "nimbus/harness/log.h"           // hlog::setSink - the harness lines into alog
#include "nimbus/orch/blob_store.h"        // blobPath - an earlier photo's stored copy
#include "../sys/config_nvs.h"   // sys::deviceName - engine 1's inbound-block line (E2-FIX14)
#include "nimbus/orch/moderation.h"   // ModGate, ModAction (E2-FIX14)
#include "nimbus/orch/no_dash.h"      // applyOutputFilters - the owner's output rules (E2-FIX14)
#include "prefs_subsystem.h"          // filtersForRole (E2-FIX14)
#include "version.h"                  // NIMBUS_FW_VERSION (E2-FIX14)
#include "notes_device.h"       // notesdev::pinnedNotesFor, notebook (the rails' pending shelf)
#include "tools_plain_skills.h"  // E2-P-SKILL: skill_save on the rails, the held notes' answers
#include "schedule_device.h"    // scheduledev::fire, host - the schedule entries (lane E2-SCHED)
#include "skills_device.h"      // skillsdev::chatSkills
#include "spend_ceiling_store.h"   // spend::frozen, spend::commit, spend::limitMicro
#include "store.h"              // the slot keys, orchModel, subModel, resolvedOrchHost
#include "provider_verify.h"    // customToolsVerified - a custom endpoint's tools (E2-R8a)
#include "nimbus/harness/plain/head_slot.h"   // headSlotFor - the head on its provider slot (E2-R8a)
#include "nimbus/orch/model_caps.h"           // caps::slotName - a slot's key (E2-R8a)
#include "nimbus/orch/model_catalog.h"        // parseModelStore - an unknown model marked unusable (E2-R8a)
#include "telegram.h"           // telegram::ownerChat, sendPart, turnUpdateIds, seenIndex
#include "transport_tls.h"      // deviceTransport
#include "tx_store_device.h"    // txdev::cardTxFiles
#include "tools_plain_device.h"  // tools::switchHead - device_control's deferred head switch (E2-P-TOOLS)
#include "nimbus/harness/jobs.h"  // keyFromTag - the head arc's ring key (E2-P-TOOLS)

namespace agent {
namespace orchestrator {

namespace {

namespace plain = agent::plain;

// Device copy (owner-facing, never model text), sent with system provenance.
constexpr const char* kNoKeyLine = "No key is set for the chosen provider. Add one in Settings, then send the message again.";
constexpr const char* kTextModeLine =
    "The custom endpoint answers without tools: its window or tool support is not known. Verify it in Settings.";
constexpr const char* kFrozenLine = "The monthly spend limit is reached. Raise it in Settings to continue.";
// Lane E2-P-TOOLS: after a head switch the owner asked for, the model that answers from now on.
constexpr const char* kHeadSwitchedLine = "Now answering with ";
// Lane E2-FIX13: a message that arrives while the earlier conversation is read in is kept and answered
// by itself when the reading is done. The person is told once per chat (the first message kept).
constexpr const char* kReadingLine =
    "The device is reading the earlier conversation in. It answers your messages by itself when that is done, "
    "in the order they came.";
// The card did not store the kept message: it is still answered, unless the device restarts before.
constexpr const char* kReadingRamLine =
    "The device is reading the earlier conversation in. Your message could not be stored on the card: it is "
    "answered when the reading is done, unless the device restarts before then.";
// Not kept (ruling 116: the true cause and the way out): the list is full, or the card did not read it.
constexpr const char* kReadingFullLine =
    "The device is still reading the earlier conversation in, and 32 messages are waiting for it already. This "
    "one is not kept: send it again once those are answered.";
constexpr const char* kReadingUnreadLine =
    "The device is reading the earlier conversation in, and its list of waiting messages could not be read from "
    "the card. This message is not kept: send it again in a few minutes.";
constexpr const char* kNoStartLine = "The new engine could not start for this chat. Restart the device and try again.";
// Lane E2-FIX3 (ruling 116): the true cause and the owner's way out of each refusal below.
constexpr const char* kNumberUnreadLine =
    "The number of this chat's conversation could not be read from the card. Send /clear to start a new "
    "conversation; the earlier ones stay stored on the device.";
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

// ---- the head on its provider slot (lane E2-R8a) -----------------------------------------
// A slot's key, read when a request is rendered (never stored by the engine); each slot is named by
// the facts table's own slot name.
std::string keyOf(const std::string& p) {
  namespace caps = nimbus::orch::caps;
  String k;
  if (p == caps::slotName(caps::Slot::Anthropic)) k = store::anthropicKey();
  else if (p == caps::slotName(caps::Slot::OpenAI)) k = store::openaiKey();
  else if (p == caps::slotName(caps::Slot::Mistral)) k = store::mistralKey();
  else if (p == caps::slotName(caps::Slot::Zai)) k = store::zaiKey();
  else if (p == "custom") k = store::customKey();
  else if (p == "cumulo") k = store::cumuloKey();
  return std::string(k.c_str(), k.length());
}

// What the device knows about its slots (plain/head_slot.h).
plain::SlotSettings slotSettings() {
  plain::SlotSettings set;
  set.key = keyOf;
  set.customBase = store::customBase().c_str();
  set.customConv = store::customConv().c_str();
  set.customTools = provider_verify::customToolsVerified();
  set.zaiHost = store::zaiBase().c_str();
  const String router = store::cumuloBase();
  set.routerHost = router.length() ? router.c_str() : CUMULO_HOST_DEFAULT;
  return set;
}

// The compaction ladder (the released engine's: the head's provider first, then each keyed provider
// of the sub-agent priority), each rung its provider's small model, at most three.
std::vector<plain::SummarizerRung> summarizerLadder(const std::string& head, const plain::SlotSettings& set) {
  std::vector<std::string> order{head};
  const std::string pri(store::subPriority().c_str());
  for (size_t at = 0; at <= pri.size();) {
    size_t end = pri.find(',', at);
    if (end == std::string::npos) end = pri.size();
    std::string p = pri.substr(at, end - at);
    while (!p.empty() && p.front() == ' ') p.erase(0, 1);
    while (!p.empty() && p.back() == ' ') p.pop_back();
    if (!p.empty() && std::find(order.begin(), order.end(), p) == order.end() && store::providerHasKey(String(p.c_str())))
      order.push_back(p);
    at = end + 1;
  }
  std::vector<plain::SummarizerRung> rungs;
  for (const std::string& p : order) {
    plain::HeadSlot slot;
    if (rungs.size() < 3 && plain::headSlotFor(set, p, std::string(store::subModel(String(p.c_str())).c_str()), slot).ok())
      rungs.push_back(plain::rungOf(slot));
  }
  return rungs;
}

// Lane E2-P-FO: the heads a failed round may move to (engine 1's fallbackOrder, plain::fallbackSlots):
// none with midTurnFailover off; else by the owner's rule set over the provider priority, each provider
// keyed and not parked, paused or over its monthly limit, on its own head model.
void fallbacksFor(const plain::SlotSettings& set, const plain::FailoverAsk& ask, std::vector<plain::HeadSlot>& out) {
  if (!store::midTurnFailover()) return;
  plain::FallbackPlan plan;
  plan.rulesJson = store::fallbackRulesJson().c_str();
  const std::string pri(store::providerPriority().c_str());
  for (size_t at = 0; at <= pri.size();) {
    size_t end = pri.find(',', at);
    if (end == std::string::npos) end = pri.size();
    std::string p = pri.substr(at, end - at);
    while (!p.empty() && p.front() == ' ') p.erase(0, 1);
    while (!p.empty() && p.back() == ' ') p.pop_back();
    if (!p.empty()) plan.priority.push_back(p);
    at = end + 1;
  }
  plan.open = [](const std::string& p) {
    const String ps(p.c_str());
    std::string held;
    return store::providerHasKey(ps) && !plain::headHeld(p, keyOf(p), store::providerOverBudget(ps), (uint32_t)millis(), held);
  };
  plan.model = [](const std::string& p) { return std::string(store::orchModel(String(p.c_str())).c_str()); };
  plain::fallbackSlots(set, plan, ask, out);
}

// The provider said it does not know the model (lane E2-R8a, design section 9): its row in the provider's
// model store is marked unusable, so the pickers stop offering it (as the verify's usability probe does).
void markUnusable(const std::string& provider, const std::string& model) {
  const String p(provider.c_str());
  nimbus::orch::ModelStoreMeta meta;
  std::vector<nimbus::orch::ModelInfo> rows;
  if (!nimbus::orch::parseModelStore(provider.c_str(), store::modelStoreText(p), meta, &rows)) return;
  for (nimbus::orch::ModelInfo& m : rows) {
    if (m.id != model || !m.usable) continue;
    m.usable = false;
    m.probed = true;
    const bool kept = store::setModelStoreText(p, nimbus::orch::serializeModelStore(meta, rows));
    alogf("e2: %s does not know %s: marked unusable%s", provider.c_str(), model.c_str(), kept ? "" : " (not stored)");
    return;
  }
}

// One turn's usage where the owner sees it (the Usage page, /usage: tokens and cost per provider,
// the month), once per turn; store::recordProviderSpend also counts it against the monthly limits.
nimbus::orch::TokenUsage s_lastUsage, s_sessUsage;   // the Usage panel's last turn and session (tg_poll writes)
std::mutex s_usageMx;                                 // the web task reads them
void recordUsage(const plain::TurnOutcome& out, const std::string& provider, const std::string& model) {
  const nimbus::orch::TurnSpend spend = plain::turnSpendOf(out, provider, model);
  store::recordProviderSpend(spend, "turn");
  {
    std::lock_guard<std::mutex> lk(s_usageMx);
    s_lastUsage = spend.total();
    s_sessUsage += s_lastUsage;
  }
  budget::noteTurnEnd(provider, model, provider, "owner", out.delivered);
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
plain::ChatBinding* residentFor(const String& chatId, const plain::HeadSlot& head, nimbus::orch::Role role, bool& keep) {
  keep = false;
  if (!s_chat) return nullptr;
  const plain::ChatBindingConfig& c = s_chat->config();
  const bool same = c.chatId == chatId.c_str() && c.role == role;
  const std::string key = txKeyFor(chatId);
  keep = same && (key.empty() || c.chatKey == key) && c.model == head.model && c.provider == head.provider &&
         c.host == head.endpoint.host && c.dialect == head.dialect;   // P7: an unread number keeps the resident
  if (keep) return s_chat.get();
  if (!s_chat->reload()) {
    alogf("e2: chat %s keeps its RAM-only binding until the card is back", c.chatId.c_str());
    keep = true;
    if (!same) s_noStartWhy = s_chat->elsewhereLine();   // E2-FIX3, E2-FIX5: the true cause, never a restart
    return same ? s_chat.get() : nullptr;
  }
  s_chat.reset();   // another chat, model or role: the arena goes back before the next one
  return nullptr;
}

void onResumed(const std::string& chat, const plain::ResumeReport& r);   // E2-FIX12, below

// Lane E2-P-SURF: a call the rails refused or put on the shelf is an entry on the Safety tab.
void noteRail(const plain::TurnCtx& t, const plain::ToolCall& c, bool kept) {
  agent::safety::record(kept ? nimbus::orch::SafetyVerdict::Suspected : nimbus::orch::SafetyVerdict::Blocked,
                        "outside-content-rail", localChat(String(t.chatId.c_str())) ? "web" : "telegram", t.chatId,
                        kept ? "rails: kept for approval" : "rails: refused", c.name + " " + c.argsJson);
}

// RailsDeps::pending: a note change goes to the notes' pending shelf; skill_save is kept waiting in the
// skill store (lane E2-P-SKILL); any other call is refused.
plain::Status railKeep(const plain::TurnCtx& t, const plain::ToolCall& c) {
  plain::Status held;
  if (tools::skillsplain::railHeld(t, c, held)) return held;
  if (c.name != "note" || !t.who) return plain::Status::fail(nimbus::orch::tx::Err::InvalidArgument, "e2.rails.other");
  const PsramFirstScope psram;
  JsonDocument args;
  (void)deserializeJson(args, c.argsJson, DeserializationOption::NestingLimit(64));
  const nimbus::orch::ToolResult r =
      notesdev::notebook().keep(args.as<JsonObjectConst>(), *t.who, nimbus::orch::notes::Origin::Tainted);
  return r.success ? plain::Status::success() : plain::Status::fail(nimbus::orch::tx::Err::InvalidArgument, "e2.rails.kept");
}

bool sayInstead(const String& chatId, const char* line);   // below

// The configured head on its provider slot (lane E2-R8a); false (logged) when no slot reaches it.
bool headNow(const plain::SlotSettings& set, plain::HeadSlot& head) {
  const String host = store::resolvedOrchHost();
  const std::string provider(host.c_str()), model(store::orchModel(host).c_str());
  if (plain::headSlotFor(set, provider, model, head).ok()) return true;
  alogf("e2: the head %s/%s names no provider slot engine 2 can reach", provider.c_str(), model.c_str());
  return false;
}

// A custom chat endpoint whose verify found no window or no tool call runs in text mode: told once a boot.
void tellTextMode(const String& chatId, const plain::HeadSlot& head) {
  static bool told = false;
  if (head.dialect != plain::Dialect::Flat || told) return;
  told = true;
  sayInstead(chatId, kTextModeLine);
}

// The chat's binding for the configured head (its provider slot and model) and this role, booted
// (null when it could not start).
plain::ChatBinding* chatFor(const String& chatId, nimbus::orch::Role role) {
  s_noStartWhy = nullptr;
  const plain::SlotSettings set = slotSettings();
  plain::HeadSlot head;
  if (!headNow(set, head)) return nullptr;
  const std::string provider = head.provider, model = head.model;
  bool keep = false;
  plain::ChatBinding* resident = residentFor(chatId, head, role, keep);
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
  plain::useSlot(head, cfg);
  cfg.summarizers = summarizerLadder(provider, set);
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
  d.key = [provider] { return keyOf(provider); };
  d.pinnedNotes = [](const nimbus::orch::Principal& who) { return notesdev::pinnedNotesFor(who); };
  d.compactCtl = deviceCompactCtlIO();
  if (!localChat(chatId)) d.seen = &telegram::seenIndex();
  d.log = [](const std::string& line) { alog(line.c_str()); };
  // Lane E2-TOOLS: the files a person may read, for the request. Lane E2-FSAFE: a call that read outside
  // content (web_search; a file from inbox/, agents/ or another person, as the tool marked it) stores an
  // outside result, so the turn is tainted; the rails then keep its note changes on the pending shelf.
  d.outside = [](const std::string&) { return tools::takeOutsideCall(); };
  d.railPending = railKeep;
  d.railNoted = noteRail;
  d.recentFiles = [](const nimbus::orch::Principal& who) -> nimbus::orch::tx::RecentFilesBlock::Source {
    return [who](std::vector<nimbus::orch::tx::RecentFile>& out) { return files::recentFiles(who, out); };
  };
  d.photo = photoFor;
  d.turnTool = historydev::answer;   // E2-SRCH: search and view read this turn's own conversation
  d.skills = [](const nimbus::orch::Principal&) { return skillsdev::chatSkills(); };
  d.skillRefused = noteSkillRefused;
  d.usage = [provider, model](const plain::TurnOutcome& out) { recordUsage(out, provider, model); };
  // Lane E2-CMP: the summarizer's requests count where a turn's do (the usage view and the monthly ceiling).
  d.compactUsage = [](const std::string& provider, const std::string& m, const nimbus::orch::TokenUsage& u) {
    nimbus::orch::TurnSpend spend;
    spend.add(provider.c_str(), m.c_str(), u);
    store::recordProviderSpend(spend, "compact");
  };
  // E2-P-TOOLS: at each step of a turn a message that arrived meanwhile gets engine 1's short
  // acknowledgement (telegram::turnAckQueued peeks without advancing; at most one peek per 10 s).
  if (!localChat(chatId)) d.onProgress = [](const plain::ProgressEvent&) { (void)telegram::turnAckQueued(); };
  if (!localChat(chatId))   // the web chat shows its own spinner; Telegram gets the line (RB-E3)
    d.progress.send = [](const std::string& chat, const std::string& text, int64_t& msgId) {
      msgId = 0;   // a new line each time: no edit on this path
      return telegram::turnProgressLine(String(chat.c_str()), String(text.c_str()));
    };
  d.oldEngine = migratedev::fill;   // E2-MIG: the earlier engine's conversation, before the log opens
  // No card fitted at all (none mounted at begin()): the chat runs from RAM by design. A card
  // that is lost, faulted or not answering is an outage: the binding reloads it (E2-STORE).
  d.noCard = noCardFitted;
  // Lane E2-FIX12: the boot's crash resume (plain/resume.h) names the device in its plain notice and
  // hands back what it sent, for the chat's history rows.
  d.deviceName = nimbus::sys::deviceName().c_str();
  d.resumed = [chat = std::string(chatId.c_str())](const plain::ResumeReport& r) { onResumed(chat, r); };
  // Lane E2-FIX14: engine 1's money gate after each turn (the shared park), and its deliver()'s
  // rules on every reply: the owner's output rules, then the outbound screen (a member's chat only).
  d.parkHead = true;
  d.replyFilter = [](const std::string& chat, const std::string& text) {
    const uint32_t mask = prefs::filtersForRole(roleOfChat(String(chat.c_str())) == nimbus::orch::Role::Admin);
    return mask ? nimbus::orch::applyOutputFilters(text, mask) : text;
  };
  d.replyScreen = [](const std::string& chat, const std::string& text) {
    return moderateGate(nimbus::orch::ModGate::OutboundReply, String(chat.c_str()), String(text.c_str())) !=
           nimbus::orch::ModAction::Block;
  };
  d.overBudget = [](const std::string& p) { return store::providerOverBudget(String(p.c_str())); };   // E2-FIX14: the resume
  d.fallbacks = [set](const plain::FailoverAsk& ask, std::vector<plain::HeadSlot>& out) { fallbacksFor(set, ask, out); };   // E2-P-FO
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
  tellTextMode(chatId, head);
  return s_chat.get();
}

// The day-stream row id "m%08x" as a number (0: none).
uint64_t rowNumber(const String& id) {
  if (id.length() < 2 || id[0] != 'm') return 0;
  return (uint64_t)strtoull(id.c_str() + 1, nullptr, 16);
}

// Effects a turn's tools asked for, after the turn: a head switch device_control accepted (lane
// E2-P-TOOLS: the head setting written by engine 1's executor, the chat told which model answers
// now), an install through the hook the web route set, a restart.
void runDeferred(const plain::DeferredEffects& d, const String& chatId) {
  if (!d.mask) return;
  if (d.has(plain::Deferred::HeadSwitch)) {
    const std::string& to = d.arg[static_cast<size_t>(plain::Deferred::HeadSwitch)];
    const bool ok = tools::switchHead(to);
    alogf("e2: head switch to %s: %s", to.c_str(), ok ? "done" : "not a provider/model");
    if (ok) sayInstead(chatId, (String(kHeadSwitchedLine) + (to.c_str() + to.find('/') + 1) + ".").c_str());
  }
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
  if (!store::providerHasKey(host)) return kNoKeyLine;
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
// chat's own key only: a conversation /clear started takes no import). `quiet`: no log line (the
// poll task's check of the kept messages, every few seconds).
bool waitsForHistory(const String& chatId, bool quiet = false) {
  const bool resident = s_chat && s_chat->config().chatId == chatId.c_str();
  const bool baseKey = txKeyFor(chatId) == chatId.c_str();
  return !resident && baseKey && !migratedev::historyReady(chatId.c_str(), quiet);
}

const char* channelOf(const String& chatId) {
  return chatId == "web" ? "web" : chatId == "voice" ? "voice" : chatId == "serial" ? "console" : "tg";
}

// The crash journal of a person's turn (E2-FIX12): its chat and kind for the next boot.
// A sub-agent's report turn (lane E2-P-SUB) is journaled as a turn no person started: a restart inside it
// is closed at the chat's next boot without a resume (plain/resume.h), its report already stored.
void journalTurn(const String& chatId, const plain::ChatMessage& msg) {
  const char* head = !msg.parts.empty() ? "[image]" : !msg.stored.empty() ? "[file]" : "";
  const int source = msg.trigger == plain::Trigger::AgentResult ? nimbus::orch::kTurnSourceSynthesis
                     : chatId == "serial"                        ? nimbus::orch::kTurnSourceSerial
                                                                 : nimbus::orch::kTurnSourceOwner;
  nimbus::crash::turnStarted(source, chatId.c_str(), head);
}

// Lane E2-FIX14: engine 1's gates before a turn (plain/head_gate.h), each over the shared code engine 1
// calls: the inbound screen (moderateGate, InboundText; engine 1's line), the provider's own monthly
// limit (store::providerOverBudget) and the shared provider park (its flash file is registered by
// provider_gate.cpp).
std::string headKeyText() { return keyOf(std::string(store::resolvedOrchHost().c_str())); }

plain::MessageGateDeps gateDeps() {
  plain::MessageGateDeps g;
  g.provider = store::resolvedOrchHost().c_str();
  g.key = headKeyText;
  g.nowMs = [] { return (uint32_t)millis(); };
  g.inbound = [](const std::string& chat, const std::string& text) -> std::string {
    if (moderateGate(nimbus::orch::ModGate::InboundText, String(chat.c_str()), String(text.c_str())) !=
        nimbus::orch::ModAction::Block)
      return std::string();
    return std::string(nimbus::sys::deviceName().c_str()) + " \xC2\xB7 " NIMBUS_FW_VERSION
                                                              ": That message couldn't be processed here.";
  };
  g.overBudget = [](const std::string& p) { return store::providerOverBudget(String(p.c_str())); };
  return g;
}

// The park's re-check handed to a message that then sent nothing goes back (E2-FIX14).
void handBack(bool probe) {
  if (probe) nimbus::orch::providerPark().releaseProbe(store::resolvedOrchHost().c_str());
}

// After the turn: a turn refused before its first request hands the re-check back (E2-FIX14).
void afterTurn(bool probe, const plain::Status& st, const plain::TurnOutcome& out) {
  if (!st.ok() && out.headRequests == 0) handBack(probe);
}

// The reply as it left (after the owner's output rules), else the model's text (E2-FIX14).
String historyText(const plain::ChatBinding& b, const plain::TurnOutcome& out) {
  return String((b.shownReply().empty() ? out.text : b.shownReply()).c_str());
}

// Lane E2-P-SURF: the tags of a turn's answer row: e2, and "turn:m<row>" (the message's row), the id the chat's
// Turn anatomy button asks /api/trace for.
String turnTags(uint64_t row) {
  if (!row) return String("e2");
  char b[32];
  snprintf(b, sizeof b, "e2,turn:m%08x", (unsigned)row);
  return String(b);
}

// One turn of `b` for `msg` (its day-stream row already written) and what follows it: the figures,
// the answer's history row, the deferred effects, the skill lines. Returns whether a part went out;
// `status` (optional) is the binding's answer (Busy: the turn did not start, nothing stored).
bool turnFor(plain::ChatBinding& b, const String& chatId, const plain::ChatMessage& msg, plain::Status* status = nullptr) {
  txdev::cardTxFiles().resetHolds();
  s_transport.reset();
  plain::TurnOutcome out;
  plain::Status st;
  const uint32_t t0 = millis();
  // The crash journal (E2-FIX12): a restart inside this turn (a kept message's too) names its chat at the
  // next boot, where the engine resumes it once (bootLostChat).
  journalTurn(chatId, msg);
  engineSetHeadArc(true);   // E2-P-TOOLS: the ring shows the turn working, as engine 1's TurnGuard does
  {
    PsramFirstScope psramFirst;
    st = b.turn(msg, out);
  }
  engineSetHeadArc(false);
  nimbus::crash::turnEnded();
  if (status) *status = st;
  if (out.modelUnusable) markUnusable(b.config().provider, b.config().model);
  afterTurn(msg.probe, st, out);   // E2-FIX14: a turn refused before its first request gives the re-check back
  logTurn(b, out, st, millis() - t0);
  if (!msg.parts.empty())
    alogf("e2: photos=%u bytes=%u psramFree=%u psramLargest=%u", (unsigned)msg.parts.size(),
          (unsigned)(s_photos ? s_photos->bytes() : 0), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  if (out.delivered && !out.text.empty())   // E2-FIX14: the reply as it left (after the owner's output rules)
    memory::captureMessage(chatId.c_str(), "assistant", nimbus::orch::MsgKind::Message, historyText(b, out), "",
                           turnTags(msg.m));
  else if (!out.statusStored.empty())   // the line without a provider's reason (E2-FIX11): a model may read this row
    memory::captureMessage(chatId.c_str(), "system", nimbus::orch::MsgKind::Message,
                           String(out.statusStored.c_str()), "", "e2");
  runDeferred(out.deferred, chatId);
  const bool sent = b.sentParts() > 0;
  for (const std::string& line : s_skillTell) sayInstead(chatId, line.c_str());
  s_skillTell.clear();
  return sent;
}

// ---- messages kept while the earlier conversation is read in (lane E2-FIX13) -------------------
// A chat's list (plain/waiting.h) is on the card at /mem/tx/<chat>.wait.json. Each kept message has
// its day-stream row at once, tagged e2 (its turn stores it later, so the import never takes it).
static_assert(plain::kWaitingMax == 32, "kReadingFullLine names the bound");
std::vector<std::unique_ptr<plain::WaitingInbox>> s_waiting;   // one list per chat (tg_poll only)
bool s_answeringKept = false;     // a kept message's turn runs (tg_poll; the web sink reads it there)
uint32_t s_keptRetryAt = 0;       // a chat that could not start: the next try (millis; 0 = none)
std::vector<std::string> s_keptStallTold;   // chats told they could not start while messages wait

// The chat's list as known now (null: none made this boot).
plain::WaitingInbox* existingWaiting(const std::string& chat) {
  for (auto& w : s_waiting)
    if (w->chatId() == chat) return w.get();
  return nullptr;
}

// The chat's list, read from the card (a list not read yet is read again on each call). A list made
// before the card was mounted, still empty, is made again on the card.
plain::WaitingInbox* waitingFor(const std::string& chat) {
  plain::WaitingInbox* in = existingWaiting(chat);
  const bool remake = in && in->ramOnly() && in->empty() && memory::haveSd();
  if (!in || remake) {
    std::unique_ptr<plain::WaitingInbox> w(
        new (std::nothrow) plain::WaitingInbox(memory::haveSd() ? &txdev::cardTxFiles() : nullptr, chat));
    if (!w) return nullptr;
    for (auto& old : s_waiting)
      if (old->chatId() == chat) old.swap(w);
    if (!remake) s_waiting.push_back(std::move(w));
    in = existingWaiting(chat);
  }
  if (!in->loaded() && !in->load().ok()) alogf("e2: the waiting messages of %s could not be read: read again later", chat.c_str());
  return in;
}

bool hasKept(const String& chatId) {
  const plain::WaitingInbox* in = existingWaiting(chatId.c_str());
  return in && !in->empty();
}

// The lists a restart left on the card, read once (a listing that fails is tried again in 30 s).
void loadWaitingLists() {
  static bool done = false;
  static uint32_t lastTry = 0;
  if (done || !memory::haveSd() || (lastTry && (uint32_t)(millis() - lastTry) < 30000)) return;
  lastTry = millis() | 1;
  std::vector<std::string> names;
  if (!txdev::cardTxFiles().list(nimbus::orch::tx::kTxRoot, names)) return;
  done = true;
  const std::string ext = ".wait.json";
  for (const std::string& n : names)
    if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) {
      const std::string chat = n.substr(0, n.size() - ext.size());
      if (nimbus::orch::tx::chatKeyOk(chat)) (void)waitingFor(chat);
    }
}

// The message, kept until the read-in ends (or joined to the chat's earlier kept messages, so they
// are answered in the order they came). The person is told once: with the first message kept.
bool keepMessage(const String& text, const String& fromName, const String& chatId) {
  plain::WaitingInbox* in = waitingFor(chatId.c_str());
  plain::WaitingMessage w;
  w.ch = channelOf(chatId);
  if (!localChat(chatId)) {
    w.src = telegram::turnUpdateIds(chatId);
    w.stored = telegram::turnStoredPaths(chatId);
  }
  if (in && !w.src.empty() && in->holds(w.src)) {
    alogf("e2: message to %s kept already (served again): not kept twice", chatId.c_str());
    return true;
  }
  const char* refused = !in || !in->loaded() ? kReadingUnreadLine
                        : in->messages().size() >= plain::kWaitingMax ? kReadingFullLine : nullptr;
  if (refused) {   // not kept: the import brings its row into the conversation (P10)
    captureOwnerRow(text, fromName, chatId, false);
    return sayInstead(chatId, refused);
  }
  const bool first = in->empty();
  const String host = store::resolvedOrchHost();
  w.text = std::string(text.c_str(), text.length());
  w.from = std::string(fromName.c_str(), fromName.length());
  w.parts = plain::photoParts(s_photos.get(), host.c_str(), std::string(store::orchModel(host).c_str()));
  w.tzMinutes = (int16_t)(utcOffsetAt((uint32_t)time(nullptr)));
  w.m = rowNumber(captureOwnerRow(text, fromName, chatId, true));
  const plain::HoldVerdict v = in->hold(w);
  const char* line = v == plain::HoldVerdict::KeptInRam ? kReadingRamLine : first ? kReadingLine : nullptr;
  alogf("e2: message to %s kept until the earlier conversation is read in (%s, %u waiting, %u photos, told=%d)",
        chatId.c_str(), v == plain::HoldVerdict::Kept ? "stored" : "RAM only", (unsigned)in->messages().size(),
        (unsigned)w.parts.size(), line ? 1 : 0);
  return line ? sayInstead(chatId, line) : true;
}

// The binding a kept message is answered on (null: no turn can run; `line` is the person's line and
// `setting` says whether a setting is the cause: no key, not Claude, the monthly limit).
plain::ChatBinding* keptBinding(const String& chatId, const char*& line, bool& setting) {
  const String host = store::resolvedOrchHost();
  line = refusalFor(host);
  setting = line != nullptr;
  if (line) return nullptr;
  plain::ChatBinding* b = chatFor(chatId, roleOfChat(chatId));
  if (!b) line = s_noStartWhy ? s_noStartWhy : kNoStartLine;
  return b;
}

// The chat could not start (the card, memory, a held conversation): the messages stay kept and are
// tried again in a minute; the person is told the cause once while it lasts (ruling 116).
plain::WaitingAnswer keptStalled(const String& chatId, const char* line) {
  s_keptRetryAt = millis() | 1;
  for (const std::string& c : s_keptStallTold)
    if (c == chatId.c_str()) return plain::WaitingAnswer::Later;
  s_keptStallTold.push_back(chatId.c_str());
  alogf("e2: chat %s could not start: its kept messages wait and are tried again in a minute", chatId.c_str());
  sayInstead(chatId, line);
  return plain::WaitingAnswer::Later;
}

// One kept message, answered once the read-in ended: its turn, or one true line instead when a
// setting holds every turn back (as for a new message).
plain::WaitingAnswer answerKept(const plain::WaitingMessage& w, const String& chatId) {
  const char* line = nullptr;
  bool setting = false;
  plain::ChatBinding* b = keptBinding(chatId, line, setting);
  if (!b && setting) {
    sayInstead(chatId, line);
    return plain::WaitingAnswer::Done;
  }
  if (!b) return keptStalled(chatId, line);
  s_keptStallTold.clear();
  // E2-FIX14: a kept message's turn passes engine 1's money gates as a new message does (its inbound screen
  // ran when it came): over the limit, parked or paused, it gets the same line and no request.
  plain::MessageGateDeps g = gateDeps();
  g.inbound = nullptr;
  const plain::MessageGate gate = plain::gateMessage(g, chatId.c_str(), w.text, /*personWrote=*/true);
  if (gate.verdict != plain::GateVerdict::Run) {
    alogf("e2: a message to %s kept during the read-in: the head is held (%s), no request", chatId.c_str(), gate.why);
    if (!gate.line.empty()) sayInstead(chatId, gate.line.c_str());
    return plain::WaitingAnswer::Done;
  }
  plain::ChatMessage msg;
  msg.probe = gate.probe;
  msg.text = w.text;
  msg.ch = w.ch;
  msg.src = w.src;
  msg.stored = w.stored;
  msg.parts = w.parts;   // the photos' bytes come from the card's copy (the photo hook)
  msg.tzMinutes = w.tzMinutes;
  msg.m = w.m;
  alogf("e2: a message to %s kept during the read-in is answered now", chatId.c_str());
  (void)turnFor(*b, chatId, msg);
  dropCarried();   // the earlier photos this turn read from the card, freed with it (as handleMessage does)
  s_turns.fetch_add(1, std::memory_order_relaxed);
  return plain::WaitingAnswer::Done;
}

// Did a turn take this message before a restart (its row or update ids are in the conversation)?
bool keptTaken(const plain::WaitingMessage& w, const String& chatId) {
  const char* line = nullptr;
  bool setting = false;
  plain::ChatBinding* b = keptBinding(chatId, line, setting);
  return b && b->holdsMessage(w.src, w.m);
}

// Every chat's kept messages, answered in order once its read-in ended (tg_poll, between turns;
// looked at every 5 s while any wait).
void answerKeptMessages() {
  static uint32_t last = 0;
  if ((uint32_t)(millis() - last) < 5000) return;
  if (s_keptRetryAt && (uint32_t)(millis() - s_keptRetryAt) < 60000) return;
  last = millis();
  s_keptRetryAt = 0;
  for (size_t i = 0; i < s_waiting.size(); ++i) {
    const std::string chat = s_waiting[i]->chatId();
    plain::WaitingInbox* in = waitingFor(chat);   // read again when the card did not answer before
    const String chatId(chat.c_str());
    if (!in || !in->loaded() || in->empty() || waitsForHistory(chatId, /*quiet=*/true)) continue;
    plain::WaitingDrainFns f;
    f.taken = [&chatId](const plain::WaitingMessage& w) { return keptTaken(w, chatId); };
    f.answer = [&chatId](const plain::WaitingMessage& w) { return answerKept(w, chatId); };
    s_inTurn.store(true, std::memory_order_relaxed);
    s_turnChat = chatId;
    s_answeringKept = true;
    const size_t n = plain::answerWaiting(*in, f);
    s_answeringKept = false;
    s_turnChat = String();
    s_inTurn.store(false, std::memory_order_relaxed);
    alogf("e2: %u kept message(s) of %s answered after the read-in, %u still waiting", (unsigned)n, chatId.c_str(),
          (unsigned)in->messages().size());
  }
}

// Lane E2-FIX14: true when the message was answered before the gates (a re-served update) or by them
// (a block, the limit, the park); `sent` then says whether a line went out. `gate` holds the verdict.
bool answeredBeforeTurn(const String& text, const String& fromName, const String& chatId, const String& host,
                        plain::MessageGate& gate, bool& sent) {
  if (const char* line = refusalFor(host)) {
    captureOwnerRow(text, fromName, chatId, false);
    leaveToImport(chatId);
    sent = sayInstead(chatId, line);
    return true;
  }
  sent = true;
  // RB-TG1 before the gates: an update the resident chat already took (re-served after a restart) is
  // neither answered nor counted again, and never spends the park's one short answer.
  if (!localChat(chatId) && s_chat && s_chat->config().chatId == chatId.c_str() &&
      s_chat->allSeen(telegram::turnUpdateIds(chatId))) {
    alogf("e2: message to %s already taken before the restart: not answered again", chatId.c_str());
    return true;
  }
  gate = plain::gateMessage(gateDeps(), chatId.c_str(), text.c_str(), /*personWrote=*/true);
  if (gate.verdict == plain::GateVerdict::Blocked) {   // engine 1: no turn, nothing kept, its one line
    alogf("moderation: inbound blocked (chat=%s)", chatId.c_str());
    sent = sayInstead(chatId, gate.line.c_str());
    return true;
  }
  if (gate.verdict != plain::GateVerdict::Held) return false;
  // No request: the limit, the park or a pause.
  alogf("e2: %s holds the head (%s): no request, %s", host.c_str(), gate.why,
        gate.line.empty() ? "the person was told this episode" : "the person is told");
  captureOwnerRow(text, fromName, chatId, false);
  leaveToImport(chatId);
  sent = !gate.line.empty() && sayInstead(chatId, gate.line.c_str());
  return true;
}

// One person's message as one engine-2 turn (tg_poll). Returns whether a reply went out.
bool runOne(const String& text, const String& fromName, const String& chatId) {
  const String host = store::resolvedOrchHost();
  plain::MessageGate gate;
  bool gated = false;
  if (answeredBeforeTurn(text, fromName, chatId, host, gate, gated)) return gated;
  // E2-FIX13: kept while the earlier conversation is read in, and after it behind the messages kept
  // before (answered in order on this task's next pass; each kept turn passes the gates again).
  if (hasKept(chatId) || waitsForHistory(chatId)) {
    handBack(gate.probe);
    return keepMessage(text, fromName, chatId);
  }
  tools::connectorsdev::beforeTurn();   // E2-P-CONN1: engine 1's MCP reconcile and discovery, before the turn
  const std::string model(store::orchModel(host).c_str());
  plain::ChatBinding* b = chatFor(chatId, roleOfChat(chatId));
  if (!b) {
    handBack(gate.probe);
    captureOwnerRow(text, fromName, chatId, false);
    return sayInstead(chatId, s_noStartWhy ? s_noStartWhy : kNoStartLine);
  }
  plain::ChatMessage msg;
  msg.ch = channelOf(chatId);
  if (!localChat(chatId)) {
    msg.src = telegram::turnUpdateIds(chatId);
    msg.stored = telegram::turnStoredPaths(chatId);   // E2-FIX10: the files stored for this message
  }
  // RB-TG1: a message the transcript already holds (re-served after a restart) was stored and
  // answered then: no second row, no second answer.
  if (b->allSeen(msg.src)) {
    alogf("e2: message to %s already taken before the restart: not answered again", chatId.c_str());
    handBack(gate.probe);
    return true;
  }
  msg.probe = gate.probe;
  const String row = captureOwnerRow(text, fromName, chatId, true);
  // E2-P-PHOTO: a head without image input reads each photo's one description (engine 1's caption walk,
  // made only for such a head) as its line; a seeing head gets the pixels and no caption call.
  msg.photosBlind = !nimbus::orch::headTakesImages(host.c_str(), model);
  msg.text = plain::photoText(s_photos.get(), host.c_str(), model, std::string(text.c_str(), text.length()));
  msg.parts = plain::photoParts(s_photos.get(), host.c_str(), model);
  msg.tzMinutes = (int16_t)(utcOffsetAt((uint32_t)time(nullptr)));
  msg.m = rowNumber(row);
  return turnFor(*b, chatId, msg);
}

// ---- what the owner's commands ask of engine 2 -----------------------------------------

// The chat's binding for its configured head, booted; null when no turn could run for it.
plain::ChatBinding* bindingFor(const String& chatId) {
  const String host = store::resolvedOrchHost();
  if (refusalFor(host)) return nullptr;
  return chatFor(chatId, roleOfChat(chatId));
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

// Lane E2-FIX14: the web page's "Clear conversation", staged by requestConvClear (any task) and run on
// tg_poll as the web chat's /clear (clearChat): a new conversation, the earlier one kept on the card.
// A clear that could not start is told on the web chat with its true cause.
std::atomic<bool> s_convClearReq{false};
void drainConvClear() {
  if (!s_convClearReq.exchange(false, std::memory_order_relaxed)) return;
  String why;
  const bool started = clearChat("web", why);
  alogf("e2: the web page's clear conversation: %s", started ? "a new conversation" : "not started");
  if (!started) sayInstead("web", why.length() ? why.c_str() : kNoStartLine);
}

void bootOwnerChat() {
  if (s_bootTried) return;
  if (!memory::episodicIndexComplete()) return;   // E2-MIG: tried again once the history is indexed
  s_bootTried = true;
  const String owner = telegram::ownerChat();
  const String host = store::resolvedOrchHost();
  if (owner.length() == 0 || refusalFor(host)) return;
  (void)chatFor(owner, roleOfChat(owner));
}

// Lane E2-FIX12: what the boot's crash resume sent goes where a turn's reply goes in the chat's
// history (runOne's rows), and a resumed turn's deferred effects run as after any turn.
void onResumed(const std::string& chat, const plain::ResumeReport& r) {
  using BootAction = nimbus::orch::intents::BootAction;
  const plain::TurnOutcome& out = r.outcome;
  std::string assistant, system;
  if (r.action == BootAction::Resume) {
    if (out.delivered && !out.text.empty()) assistant = out.text;
    else system = out.statusStored;
  } else if (r.action == BootAction::SendStored) {
    assistant = r.text;
  } else if (r.action == BootAction::Notice) {
    system = r.text;
  }
  if (!assistant.empty())
    memory::captureMessage(chat.c_str(), "assistant", nimbus::orch::MsgKind::Message, String(assistant.c_str()), "", "e2");
  else if (!system.empty())
    memory::captureMessage(chat.c_str(), "system", nimbus::orch::MsgKind::Message, String(system.c_str()), "", "e2");
  if (r.action == BootAction::Resume) runDeferred(out.deferred, String(chat.c_str()));
}

// Lane E2-FIX12: a person's message a restart cut (the crash journal names its chat) is taken up
// at boot by that chat's engine (plain/resume.h: one resume, else the plain crash notice), before
// the owner chat boots. The boot is journaled like a turn, so a cut inside the resume is named at
// the next boot. A chat that cannot boot gets the crash notice the device kept (a crash only).
// tg_poll, once (tried again while the chat waits for its history import).
bool s_lostTried = false;
void bootLostChat() {
  if (s_lostTried) return;
  std::string chat, kind;
  if (!nimbus::crash::lostOwnerTurn(chat, kind)) {
    s_lostTried = true;
    return;
  }
  const String chatId(chat.c_str());
  if (waitsForHistory(chatId)) return;
  s_lostTried = true;
  const String host = store::resolvedOrchHost();
  if (refusalFor(host)) {
    alogf("e2: the restart cut a message to %s; its engine cannot start", chatId.c_str());
    nimbus::crash::releaseOwnerNotice();
    return;
  }
  alogf("e2: the restart cut a message to %s: its chat boots now", chatId.c_str());
  nimbus::crash::turnStarted(kind == "console" ? nimbus::orch::kTurnSourceSerial : nimbus::orch::kTurnSourceOwner,
                             chat, kind == "photo" ? "[image]" : kind == "file" ? "[file]" : "");
  s_inTurn.store(true, std::memory_order_relaxed);
  plain::ChatBinding* b = chatFor(chatId, roleOfChat(chatId));
  s_inTurn.store(false, std::memory_order_relaxed);
  nimbus::crash::turnEnded();
  // Dropped only when the engine sent the person a line (the answer, the stored answer or its
  // notice); a boot that found no open turn, or whose send was refused, leaves the person the
  // device's own notice (a crash only).
  using BootAction = nimbus::orch::intents::BootAction;
  const plain::ResumeReport* r = b ? &b->lastResume() : nullptr;
  const bool told = r && r->status.ok() &&
                    (r->action == BootAction::Resume || r->action == BootAction::SendStored || r->action == BootAction::Notice);
  if (told) nimbus::crash::dropOwnerNotice();
  else nimbus::crash::releaseOwnerNotice();
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

// The log's word for an entry's kind.
const char* kindWord(sch::Kind k) {
  if (k == sch::Kind::Reminder) return "reminder";
  return k == sch::Kind::Wakeup ? "wakeup" : "routine";
}

// Lane E2-FIX14: a routine or a wake-up (E2-P-MEM) sends no request while the head's provider is over its
// limit or held by the park (a refused key, an empty credit, a pause); it waits for its next slot. A
// reminder goes (it sends its stored text, no request). The overall monthly limit first, as before.
bool routineHeld(sch::Kind kind, const String& host, nimbus::orch::FireOutcome& out) {
  if (kind == sch::Kind::Reminder) return false;
  if (spend::frozen()) {
    out.deferred = true;
    out.error = nimbus::orch::kSpendFrozenError;
    return true;
  }
  std::string held;
  const std::string provider(host.c_str());
  if (!plain::headHeld(provider, keyOf(provider), store::providerOverBudget(host), (uint32_t)millis(), held))
    return false;
  out.deferred = true;
  out.error = "paid requests are held: " + held;
  return true;
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
  const String chat = chatIn.length() ? chatIn : telegram::ownerChat();
  const std::string chatId(chat.c_str(), chat.length());
  const String host = store::resolvedOrchHost();
  if (routineHeld(kind, host, out)) return out;   // the money guard: a routine or wake-up waits, a reminder goes
  plain::ChatBinding* b = nullptr;
  if (chat.length() && !(kind != sch::Kind::Reminder && refusalFor(host)))
    b = chatFor(chat, roleOfChat(chat));
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
  // The routine runs on the head provider's small model; its spend and park are that provider's.
  const std::string provider(host.c_str());
  const std::string sub(store::subModel(host).c_str());
  if (b) {
    plain::ScheduleBindingDeps& d = parts->d;
    d.held = heldFor(chatId);
    d.quiet = quietNow();
    d.grants = loops::routineGrants();
    d.tzMinutes = tzNow();
    d.routineModel = sub;
    d.fired = [provider, sub](const plain::FireReport& rep) {
      store::recordProviderSpend(plain::turnSpendOf(rep.outcome, provider, sub), "routine");
      // E2-FIX14: the park learns what the routine's head answered; nobody was told, so the owner's next
      // message still gets the episode's notice (engine 1: a routine may carry it, never take it).
      plain::noteHeadOutcome(provider, keyOf(provider), rep.outcome, /*probe=*/false, /*told=*/false,
                             (uint32_t)millis());
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
        kindWord(kind), chatId.c_str(), (int)out.ok, (int)out.deferred,
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
    plain::ChatBinding* b = chatFor(chat, roleOfChat(chat));
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

// ---- sub-agents (lane E2-P-SUB) ----------------------------------------------------------
constexpr const char* kStopListPath = "/data/jobs_stopping.txt";   // the stop list, as on engine 1
std::string s_reportsFor;   // the chat whose parked reports the last pump named (tg_poll only)

// One report turn in `chat` (plain::deliverReports, from the job pump between turns). Busy, with
// nothing stored, when no turn can run for that chat now: the report waits in the JobEngine.
plain::Status reportTurnIn(const std::string& chatId, const std::string& note) {
  const plain::Status busy = plain::Status::fail(nimbus::orch::tx::Err::Busy, "e2.reportTurn");
  const String chat(chatId.c_str());
  const String host = store::resolvedOrchHost();
  if (s_inTurn.load(std::memory_order_relaxed) || chat.length() == 0 || hasKept(chat) || refusalFor(host) ||
      waitsForHistory(chat, true))
    return busy;
  // E2-FIX14: a report turn sends no request while the head is over its limit, parked or paused: the report
  // waits in the JobEngine (it never takes the park's re-check; a person's message does).
  std::string held;
  if (plain::headHeld(host.c_str(), keyOf(host.c_str()), store::providerOverBudget(host), (uint32_t)millis(), held)) {
    alogf("e2: an agent report for %s waits: %s", chatId.c_str(), held.c_str());
    return busy;
  }
  plain::ChatBinding* b = chatFor(chat, roleOfChat(chat));
  if (!b) return busy;
  plain::ChatMessage msg;
  msg.trigger = plain::Trigger::AgentResult;
  msg.text = note;
  msg.ch = channelOf(chat);
  msg.tzMinutes = (int16_t)(utcOffsetAt((uint32_t)time(nullptr)));
  plain::Status st = busy;
  s_inTurn.store(true, std::memory_order_relaxed);
  s_turnChat = chat;
  (void)turnFor(*b, chat, msg, &st);   // the shared turn: journaled, figures, history row, deferred effects
  s_turns.fetch_add(1, std::memory_order_relaxed);
  s_turnChat = String();
  s_inTurn.store(false, std::memory_order_relaxed);
  return st;
}

nimbus::orch::Principal chatPrincipal(const std::string& chatId) {
  nimbus::orch::Quota q;
  tenantQuotaOf(chatId, q);
  return nimbus::orch::principalForRole(chatId, roleOfChat(String(chatId.c_str())), q);
}

// The job engine's model and key reads (engine 1's buildJobDeps, the same store calls).
void jobModelDeps(JobEngine::Deps& d) {
  d.subPriority = [] { return std::string(store::subPriority().c_str()); };   // engine 1's order (E2-R8a)
  d.providerHasKey = [](const std::string& p) { return store::providerHasKey(String(p.c_str())); };
  d.overBudget = [](const std::string& p) { return store::providerOverBudget(String(p.c_str())); };
  d.subModel = [](const std::string& p) { return std::string(store::subModel(String(p.c_str())).c_str()); };
  d.modelIsValid = [](const std::string& p, const std::string& m) {
    return store::modelSelectable(String(p.c_str()), String(m.c_str()), nimbus::orch::PickRole::Any);
  };
  d.modelChoices = [](const std::string& p) {
    return nimbus::orch::effectiveModelChoices(p.c_str(), std::string(store::modelChoices(String(p.c_str())).c_str()));
  };
  d.recordSpawnUsage = [](const std::string& backend, const std::string& model, const nimbus::orch::TokenUsage& u) {
    const String be(backend.c_str());
    const String resolved = (model.empty() || model == "default") ? store::subModel(be) : String(model.c_str());
    nimbus::orch::meterSeamUsage(nimbus::orch::SpendSeam::Sub, backend.c_str(), resolved.c_str(), u);
  };
}

// The job engine's file reads and writes: a brief's files, the whole report, the stop list.
void jobFileDeps(JobEngine::Deps& d) {
  d.readDoc = [](const std::string& chatId, const std::string& project, const std::string& name) {
    return files::readDocText(project, name, nimbus::orch::principalForRole(chatId, roleOfChat(String(chatId.c_str()))));
  };
  d.persistResult = [](const std::string& project, const std::string& name, const std::string& tag,
                       const std::string& text, const std::string& chatId) -> std::string {
    std::string path, err;
    if (project != plain::kAgentsProject || !plain::agentReportPath(tag, name, path).ok()) return std::string();
    const std::string file = path.substr(project.size() + 1);
    if (!files::saveText(project, file, text, err, nimbus::orch::nsForChat(chatId, /*admin=*/false)))
      return "[persist FAILED: " + files::refusalForHead(err) + "]";
    return "[saved: " + path + "]";
  };
  d.loadStopping = [] {
    std::string out;
    File f = LittleFS.open(kStopListPath, "r");
    if (!f) return out;
    while (f.available() && out.size() < 8192) out += (char)f.read();
    f.close();
    return out;
  };
  d.saveStopping = [](const std::string& text) {
    LittleFS.mkdir("/data");
    return agent::writeFileAtomic(kStopListPath, text, "jobs_stopping", false);
  };
}

JobEngine::Deps buildJobDeps() {
  JobEngine::Deps d;
  d.principalFor = chatPrincipal;
  d.platform.nowMs = [] { return (uint32_t)millis(); };
  d.platform.freeHeap = [] { return (uint32_t)ESP.getFreeHeap(); };
  d.fabric = g_fabric;
  d.journal = &g_journal;
  // The job engine's own lines (a failure with nothing else running, a stop) to the person, and into
  // the chat's history tagged as job deliveries (never read back by a model), as engine 1's
  // capturingDelivery does; one lambda, no std::function of the same signature inside it (the tg_poll
  // stack gate resolves such a call to every handler of that signature, itself included).
  d.deliver = [](const std::string& chatId, const std::string& text) {
    const String body(text.c_str());
    if (!deliver(String(chatId.c_str()), body, /*system=*/true)) return;   // screened out: never captured
    memory::captureMessage(chatId.c_str(), "tool", nimbus::orch::MsgKind::Message, body, "", agent::kJobDeliveryTag);
  };
  jobModelDeps(d);
  jobFileDeps(d);
  d.nowString = [] { return std::string(nimbus::hw::localTimeStr().c_str()); };
  d.nowEpoch = [] {
    const time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0u;
  };
  d.turnInFlight = [] { return s_inTurn.load(std::memory_order_relaxed); };
  // The pump only names the chat whose reports are parked; pumpJobs runs them after the pump returns
  // (a direct call: the report turn's frames never sit under the pump's, nor under a std::function a
  // turn also calls, which the tg_poll stack gate would read as a turn inside a turn).
  d.synthesize = [](const std::string& chatId) { s_reportsFor = chatId; };
  return d;
}

// The journal from flash, the job engine over it and the agent tools on the shared registry.
void beginAgents() {
  static FlashJournalStore& journalStore = ::nimbus::psramObject<FlashJournalStore>();
  journalStore.begin();
  g_journal.begin(&journalStore);
  static JobEngine& engine = ::nimbus::psramObject<JobEngine>(
      buildJobDeps(), JobEngine::Tuning{AGENT_MAX_ACTIVE_INFLIGHT, ORCH_DISPATCH_MIN_HEAP, AGENT_POLL_INTERVAL_MS});
  g_jobs = &engine;
  plain::AgentsDeps a;
  a.jobs = g_jobs;
  a.chat = [] { return turnInFlight() ? std::string(s_turnChat.c_str()) : std::string(); };
  a.dispatchNow = [] { (void)g_jobs->dispatchQueued(); };   // one request on this task, between the turn's rounds
  const plain::Status st = plain::registerAgentTools(memory::registry(), a);
  alogf("e2: agents %s (%d open job%s)", nimbus::orch::tx::errSlug(st.err), g_journal.count(), g_journal.count() == 1 ? "" : "s");
}

// Lane E2-P-SUB: the jobs (dispatch, poll, the stop list) and a finished one's report turn; the active
// count keeps the Telegram loop pumping. noinline: its frame stays off pollJobs' (the tg_poll stack gate).
__attribute__((noinline)) int pumpJobs() {
  if (!g_jobs) return 0;
  const int n = g_jobs->pump() + g_jobs->stoppingCount();
  if (s_reportsFor.empty() || s_inTurn.load(std::memory_order_relaxed)) return n;   // never inside a turn
  const std::string chatId = std::move(s_reportsFor);
  s_reportsFor.clear();
  int ran = 0;
  const plain::Status st = plain::deliverReports(*g_jobs, chatId, reportTurnIn, ran);
  if (ran || !st.ok()) alogf("e2: agent reports for %s: %d ran, %s", chatId.c_str(), ran, nimbus::orch::tx::errSlug(st.err));
  return n;
}

}  // namespace

// ---- lifecycle --------------------------------------------------------------

void begin(HeavyFabric* fabric, const Sinks& sinks) {
  g_fabric = fabric;
  g_sinks = sinks;
  if (!g_sessMux) g_sessMux = xSemaphoreCreateMutex();   // guards the session snapshot
  if (g_fabric) beginAgents();                            // lane E2-P-SUB
  refreshSessionSnapshot();                               // from the restored journal
  // L24 (turn_memory.h): a turn keeps its small blocks in PSRAM, as on engine 1.
  setPsramFirstSwitch([](bool on) { nimbus::heapwatch::psramFirst(on); });
  // The harness library's log seam (compaction, the wire, the summarizer) into the device log, as engine 1
  // installs it: without a sink every such line is dropped (lane E2-BENCH: a failed /compact left no cause).
  hlog::setSink([](const char* line) { alog(line); });
  nimbus::crash::ownerTurnsResumed();   // E2-FIX12: a person's cut turn is this engine's to answer
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

// Lane E2-P-SURF: the web pages' asks (/api/lastturn, /api/trace, /api/fold/status). The engine's arena is
// this task's, so a page stages its ask and this answers it between turns (plain/surface.h).
void serveWebAsks() {
  plain::surface::AskReq q;
  if (!plain::surface::askBox().pending(q)) return;
  std::string body;
  {
    const PsramFirstScope psramFirst;   // a rendered request is large: it goes to PSRAM
    plain::PlainEngine* e = s_chat ? s_chat->engine() : nullptr;
    const std::string chat = s_chat ? s_chat->config().chatId : std::string();
    body = plain::surface::answerAsk(
        q, e, chat, (uint64_t)time(nullptr),
        [](size_t n) -> void* { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); },
        [](void* p) { heap_caps_free(p); });
  }
  plain::surface::askBox().answer(q.id, std::move(body));
}

int pollJobs() {
  if (!s_begun) return 0;
  serveWebAsks();         // E2-P-SURF: the web pages' asks, between turns
  migratedev::importRulesOnce();   // the earlier engine's standing rules as pinned notes, again after a change
  tools::skillsplain::pump();       // E2-P-SKILL: the Rules page's answers to held notes
  bootLostChat();         // once: a person's message a restart cut is resumed (E2-FIX12)
  bootOwnerChat();        // once: the owner chat's transcript and seen index before its next message
  loadWaitingLists();     // E2-FIX13: the messages a restart left kept for the read-in
  answerKeptMessages();   // and those kept, answered in order once their chat's read-in ended
  drainTerminates();      // the journal's staged terminates (web stops), on the journal's writer task
  drainUpdateNotice();    // the fixed post-update notice - tg_poll only
  drainConvClear();       // the web page's "Clear conversation" (E2-FIX14)
  // Lane E2-SCHED: the loops tick (at most one due entry; 20 s-gated, heap and Wi-Fi guarded inside)
  // and the outboxes of routine notes. tg_poll, between turns.
  loadOutboxes();
  loops::checkDue((uint64_t)time(nullptr), s_inTurn.load(std::memory_order_relaxed), ESP.getFreeHeap());
  drainOutboxes();
  if (!s_inTurn.load(std::memory_order_relaxed)) files::tickFetch();   // E2-P-OUT2: fetch_url, as engine 1 pumps it
  const int n = pumpJobs();
  refreshSessionSnapshot();
  return n;
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
bool answeringKept() { return s_answeringKept; }
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
void requestMemoryClear() {}   // engine 2 keeps no assistant memory (the web route says so: E2-FIX14)
void requestConvReset() {}     // engine 2 keeps no provider-side conversation to reset
// Lane E2-FIX14: the web page's "Clear conversation" is the web chat's /clear on engine 2, staged here
// (the web task) and run by tg_poll's next pollJobs (drainConvClear).
void requestConvClear() { s_convClearReq.store(true, std::memory_order_relaxed); }
void noteConfigChanged() {}

// ---- what the common half asks of the engine ----------------------------------

bool engineRunning() { return false; }   // no ring arcs or sub-agents on engine 2 yet
bool engineReapStuckTurn(uint32_t) { return false; }
// The head's ring arc (E2-P-TOOLS): engine 1's event on the attention Router (blue comet, then off).
void engineSetHeadArc(bool on) {
  if (!g_sinks.event) return;
  nimbus::attn::Event e;
  e.type = nimbus::attn::Event::Type::JobState;
  e.key = keyFromTag("head");
  e.status = (uint8_t)(on ? solide::ring::Status::Running : solide::ring::Status::Offline);
  g_sinks.event(e);
}
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
