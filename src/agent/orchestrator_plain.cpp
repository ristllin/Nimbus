// orchestrator_plain.cpp - the plain engine's device seam (engine 2). Lane RB-I0: a STUB.
//
// An engine-2 device image (the *-e2 envs, -DNIMBUS_ENGINE=2) links this file instead of
// orchestrator_e1.cpp (tools/engine/engine1_only.txt). It defines every call of engine_api.h and
// binds none of the engine-2 library: each message is answered with one fixed line, no provider
// is called, no tool is registered (tools_plain.cpp), no sub-agent, routine, fold or memory turn
// runs. It exists so every later device lane links and is measured on engine 2 (image bytes, the
// tg_poll stack, static RAM); RB-I1 and RB-I2 replace the stub with the engine.
// The engine-neutral half (orchestrator_common.cpp) is shared with engine 1 unchanged: what it
// asks of a seam is honoured here (begin creates g_sessMux and primes the session snapshot;
// pollJobs drains the staged terminates and refreshes the snapshot; engineRunning() is false,
// so the ring arcs and the mid-loop reply note stay off).
#include "orchestrator.h"
#include "orchestrator_common.h"   // g_fabric, g_sinks, g_sessMux, deliver, the voice flags

#include <time.h>   // localtime_r, gmtime_r - utcOffsetAt

#include <freertos/FreeRTOS.h>

#include "../sys/agent_log.h"
#include "memory_subsystem.h"   // memory::captureMessage - the chat's history rows
#include "telegram.h"           // telegram::ownerChat, telegram::send - the staged update notice

namespace agent {
namespace orchestrator {

namespace {

// The one line every message gets on the stub. Device copy, never model text.
constexpr const char* kStubLine =
    "This test build runs engine 2, which is not connected yet, so every message gets this one line.";

bool     s_begun = false;
bool     s_inTurn = false;        // tg_poll only
uint32_t s_turns = 0;             // messages answered since boot
String   s_turnChat;              // tg_poll only: the chat of the message being answered

String (*s_otaInstallHook)() = nullptr;

// The staged post-update notice (stageUpdateNotice may run on any task): one slot, copied under the
// spinlock, nothing allocated or logged inside it.
char                 s_noticeText[512] = {};
volatile bool        s_noticePending = false;
portMUX_TYPE         s_noticeMux = portMUX_INITIALIZER_UNLOCKED;

void drainUpdateNotice() {
  if (!s_noticePending) return;
  char text[sizeof(s_noticeText)];
  portENTER_CRITICAL(&s_noticeMux);
  memcpy(text, s_noticeText, sizeof(text));
  s_noticePending = false;
  portEXIT_CRITICAL(&s_noticeMux);
  const String chat = telegram::ownerChat();
  if (chat.length() == 0) return;   // nowhere to deliver: dropped, as on engine 1
  telegram::send(chat, String(text), /*block=*/false);
}

}  // namespace

// ---- lifecycle --------------------------------------------------------------

void begin(HeavyFabric* fabric, const Sinks& sinks) {
  g_fabric = fabric;
  g_sinks = sinks;
  if (!g_sessMux) g_sessMux = xSemaphoreCreateMutex();   // guards the session snapshot
  refreshSessionSnapshot();                               // no jobs: an empty snapshot
  s_begun = true;
  alog("orchestrator: engine 2 stub ready (answers one fixed line)");
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
    s_inTurn = true;
    s_turnChat = chatId;
    String tags;
    if (fromName.length()) tags = "from:" + fromName;
    memory::captureMessage(chatId.c_str(), "user", nimbus::orch::MsgKind::Message, text, "", tags);
    // Device copy (system provenance), like engine 1's command replies: the web sink records it in the
    // web chat's history; no assistant row of the engine's is written (no engine answered).
    const bool sent = deliver(chatId, String(kStubLine), /*system=*/true);
    if (voice) s_voiceOk.store(sent, std::memory_order_relaxed);
    ++s_turns;
    s_turnChat = String();
    s_inTurn = false;
  }
  if (voice) {
    s_inVoiceMessage = false;
    s_voiceHandled.fetch_add(1, std::memory_order_release);
  }
}

nimbus::orch::FireOutcome injectScheduledTurn(const String&, const String&, const String&, const String&,
                                              bool, bool, bool, bool) {
  nimbus::orch::FireOutcome out;
  out.error = "engine 2 stub: no scheduled turns";
  return out;
}

int pollJobs() {
  if (!s_begun) return 0;
  drainTerminates();      // the journal's staged terminates (none can exist without jobs)
  drainUpdateNotice();    // the fixed post-update notice - tg_poll only
  refreshSessionSnapshot();
  return 0;               // no jobs: the Telegram loop keeps its long poll
}

void setOtaInstallHook(String (*fn)()) { s_otaInstallHook = fn; }

void stageUpdateNotice(const String& text) {
  portENTER_CRITICAL(&s_noticeMux);
  strncpy(s_noticeText, text.c_str(), sizeof(s_noticeText) - 1);
  s_noticeText[sizeof(s_noticeText) - 1] = 0;
  s_noticePending = true;
  portEXIT_CRITICAL(&s_noticeMux);
}

// ---- status for the web and the tools ---------------------------------------

bool turnInFlight() { return s_inTurn; }
uint32_t turnCount() { return s_turns; }
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
String foldStatusText(const char*) { return String("fold: none (engine 2 stub)"); }
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

bool engineRunning() { return false; }
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
