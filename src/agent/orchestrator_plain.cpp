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
//     files), suppression, the money guard and one breaker, the seen-index rebuild at boot.
// One resident chat (one arena, sized for the head): a message for another chat, another model
// or another role boots the binding again over the same card (the transcript is refilled).
// Claude heads only: any other head gets one plain line. Owner commands are not parsed yet (they
// reach the model as text); routines, agents and photos are not bound yet.
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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../sys/agent_log.h"
#include "../sys/heap_watch.h"     // heapwatch::psramFirst - the turn's PSRAM-first switch
#include "../sys/psram_static.h"   // the notice slot in PSRAM, as on engine 1
#include "files_subsystem.h"    // files::recentFiles - the file tools' recent-files list (E2-TOOLS)
#include "memory_subsystem.h"   // memory::captureMessage, memory::registry
#include "nimbus/harness/plain/chat_binding.h"
#include "nimbus/harness/turn_memory.h"   // PsramFirstScope
#include "notes_device.h"       // notesdev::pinnedNotesFor
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
constexpr const char* kNoStartLine = "The new engine could not start for this chat. Restart the device and try again.";

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
    const bool ok = deviceTransport().exec(req, out, err);
    note(internalLargest());
    ++requests;
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
    minLargest = UINT32_MAX;
    requests = 0;
  }
  uint32_t minLargest = UINT32_MAX;
  uint32_t requests = 0;

 private:
  void note(uint32_t v) {
    if (v < minLargest) minLargest = v;
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

// The chat's binding for this model and role, booted (null when it could not start).
plain::ChatBinding* chatFor(const String& chatId, const std::string& model, nimbus::orch::Role role) {
  if (s_chat) {
    const plain::ChatBindingConfig& c = s_chat->config();
    if (c.chatId == chatId.c_str() && c.model == model && c.role == role) return s_chat.get();
    s_chat.reset();   // another chat, model or role: the arena goes back before the next one
  }
  plain::ChatBindingConfig cfg;
  cfg.chatKey = chatId.c_str();
  cfg.chatId = chatId.c_str();
  cfg.model = model;
  cfg.summarizerModel = store::subModel("anthropic").c_str();
  cfg.role = role;
  cfg.timeZone = store::deviceTz().c_str();
  cfg.dayStartMin = store::dayStartMin();
  cfg.storedRounds = store::orchLoopRounds();
  cfg.monthlyCeilingMicro = spend::limitMicro();
  if (!nimbus::orch::tx::chatKeyOk(cfg.chatKey)) {
    alogf("e2: chat %s has no transcript key", chatId.c_str());
    return nullptr;
  }
  plain::ChatBindingDeps d;
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
  d.chatsum = deviceChatsumIO();
  if (!localChat(chatId)) d.seen = &telegram::seenIndex();
  d.log = [](const char* line) { alog(line); };
  // Lane E2-TOOLS: web_search reads outside content; the files a person may read, for the request.
  d.outside = [](const std::string& tool) { return tool == "web_search"; };
  d.recentFiles = [](const nimbus::orch::Principal& who) -> nimbus::orch::tx::RecentFilesBlock::Source {
    return [who](std::vector<nimbus::orch::tx::RecentFile>& out) { return files::recentFiles(who, out); };
  };
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
  alogf("e2: boot holdMs read=%u list=%u whole=%u rename=%u n=%u reads", (unsigned)h.readMs, (unsigned)h.listMs,
        (unsigned)h.wholeMs, (unsigned)h.renameMs, (unsigned)h.reads);
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
        "cacheR=%u cacheW=%u lastPrompt=%u uUSD=%u parts=%u/%u",
        b.config().chatId.c_str(), b.config().model.c_str(), nimbus::orch::tx::errSlug(st.err), (st.where ? st.where : ""),
        e.cause.c_str(), (unsigned)ms, (unsigned)out.rounds, (unsigned)out.toolCalls, (unsigned)out.headRequests,
        (unsigned)reqBytes, (unsigned)e.promptTokens, (unsigned)e.outputTokens, (unsigned)e.cacheReadTokens,
        (unsigned)e.cacheWriteTokens, (unsigned)e.lastPromptTokens, (unsigned)e.microUsd, (unsigned)b.sentParts(),
        (unsigned)(b.sentParts() + b.refusedParts()));
  alogf("e2: mem stackFree=%u intLargestMin=%u intLargestNow=%u psramFree=%u psramLargest=%u reqs=%u "
        "holdMs append=%u whole=%u rename=%u read=%u list=%u n=%u/%u/%u/%u refused=%u/%u/%u",
        (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)s_transport.minLargest, (unsigned)internalLargest(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)s_transport.requests, (unsigned)h.appendMs, (unsigned)h.wholeMs, (unsigned)h.renameMs,
        (unsigned)h.readMs, (unsigned)h.listMs, (unsigned)h.appends, (unsigned)h.wholes, (unsigned)h.renames,
        (unsigned)h.reads, (unsigned)h.refusedNoCard, (unsigned)h.refusedFloor, (unsigned)h.refusedTask);
}

// The line a message gets when no engine-2 turn can run for it; empty: a turn can run.
const char* refusalFor(const String& host) {
  if (host != "anthropic") return kNotClaudeLine;
  if (!store::hasAnthropicKey()) return kNoKeyLine;
  if (spend::frozen()) return kFrozenLine;
  return nullptr;
}

// The person's message into the chat's day stream (the web chat's history, a later migration's
// rows), tagged e2 so a migration never imports it again (RB-MG1a). Its row id.
String captureOwnerRow(const String& text, const String& fromName, const String& chatId) {
  const String tags = fromName.length() ? "from:" + fromName + ",e2" : String("e2");
  return memory::captureMessage(chatId.c_str(), "user", nimbus::orch::MsgKind::Message, text, "", tags);
}

// One device line instead of a turn (system provenance, never model text).
bool sayInstead(const String& chatId, const char* line) {
  bool queued = false;
  const bool sent = deliver(chatId, String(line), /*system=*/true, &queued);
  return sent && queued;
}

// One person's message as one engine-2 turn (tg_poll). Returns whether a reply went out.
bool runOne(const String& text, const String& fromName, const String& chatId) {
  const String host = store::resolvedOrchHost();
  if (const char* line = refusalFor(host)) {
    captureOwnerRow(text, fromName, chatId);
    return sayInstead(chatId, line);
  }
  const std::string model(store::orchModel(host).c_str());
  plain::ChatBinding* b = chatFor(chatId, model, roleOfChat(chatId));
  if (!b) {
    captureOwnerRow(text, fromName, chatId);
    return sayInstead(chatId, kNoStartLine);
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
  const String row = captureOwnerRow(text, fromName, chatId);
  msg.text = std::string(text.c_str(), text.length());
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
  const spend::Part part{"anthropic", (uint64_t)out.end.microUsd};
  spend::commit(&part, 1, /*turn=*/true);
  if (out.delivered && !out.text.empty())
    memory::captureMessage(chatId.c_str(), "assistant", nimbus::orch::MsgKind::Message,
                           String(out.text.c_str()), "", "e2");
  else if (!out.statusText.empty())
    memory::captureMessage(chatId.c_str(), "system", nimbus::orch::MsgKind::Message,
                           String(out.statusText.c_str()), "", "e2");
  runDeferred(out.deferred);
  return b->sentParts() > 0;
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
  s_bootTried = true;
  const String owner = telegram::ownerChat();
  const String host = store::resolvedOrchHost();
  if (owner.length() == 0 || refusalFor(host)) return;
  (void)chatFor(owner, std::string(store::orchModel(host).c_str()), roleOfChat(owner));
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
  if (s_begun) {
    s_inTurn.store(true, std::memory_order_relaxed);
    s_turnChat = chatId;
    const bool sent = runOne(text, fromName, chatId);
    if (voice) s_voiceOk.store(sent, std::memory_order_relaxed);
    s_turns.fetch_add(1, std::memory_order_relaxed);
    s_turnChat = String();
    s_inTurn.store(false, std::memory_order_relaxed);
  }
  if (voice) {
    s_inVoiceMessage = false;
    s_voiceHandled.fetch_add(1, std::memory_order_release);
  }
}

nimbus::orch::FireOutcome injectScheduledTurn(const String&, const String&, const String&, const String&,
                                              bool, bool, bool, bool) {
  nimbus::orch::FireOutcome out;
  out.error = "engine 2: no scheduled turns yet";
  return out;
}

int pollJobs() {
  if (!s_begun) return 0;
  bootOwnerChat();        // once: the owner chat's transcript and seen index before its next message
  drainTerminates();      // the journal's staged terminates (none can exist without jobs)
  drainUpdateNotice();    // the fixed post-update notice - tg_poll only
  refreshSessionSnapshot();
  return 0;               // no jobs: the Telegram loop keeps its long poll
}

void setOtaInstallHook(String (*fn)()) { s_otaInstallHook = fn; }

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

nimbus::orch::TokenUsage lastTurnUsage() { return {}; }
nimbus::orch::TokenUsage sessionUsage() { return {}; }
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
