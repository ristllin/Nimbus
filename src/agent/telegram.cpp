// Hand-rolled Telegram Bot API client for Nimbus. Ported from Nuage-Solide
// src/telegram_client.cpp (Head Orchestrator v2). Single FreeRTOS task (tg_poll,
// core 0, 16 KB stack) interleaves long-poll GET and outbound POST sendMessage so
// only one TLS session is open at any moment. No UniversalTelegramBot; inbound is
// hand-scanned (no ArduinoJson) so the mbedTLS handshake keeps its stack.
//
// Dropped from the Nuage original (Nimbus doesn't have these modules): the
// heaptrace tracer, provider_verify gate, and the netheal self-heal ladder. What
// remains is the full poll/send/voice machinery + a simple consecutive-fail
// backoff. The RST-close (net_util tlsClose) and the persistent poll socket are
// kept - they survive the S3/PSRAM relaxation (plan §3.7).
//
// LIVE-GATED: needs a bot token + allowlist + STA WiFi. VOICE is BENCH-BROKEN +
// key-gated - the STT sink (setSttSink) is unset here so voice notes are answered
// "please send text"; the download/getFile machinery is ported compile-clean.
#include "telegram.h"
#include "../sys/config_nvs.h"   // sys::deviceName - self-identifying 409 alert
#include "../sys/net_util.h"
#include "agent_config.h"
#include "store.h"
#include "../sys/agent_log.h"
#include "../sys/heap_watch.h"   // activity tag: Telegram downloads
#include "../sys/psram_static.h" // pending-slot buffers live in PSRAM, not internal .bss
#include <esp_task_wdt.h>       // quiesce() feeds the main-task WDT while it waits
#include "memory_subsystem.h"   // captureMediaFile - durable voice-note sidecar
#include "nimbus/harness/turn_inbox.h"   // histV2: what the sender sent, for the turn's history row
#include "adapters/image_vision.h"   // a photo enters the conversation as words
#include "files_subsystem.h"        // per-sender storage limit, checked pre-download
#include "tg_archive.h"             // lane R: every update and every send is archived on SD
#include "orchestrator.h"           // roleOfChat / tenantQuotaOf - who is sending
#include "nimbus/mem_cap.h"         // utf8CapLen - never sever a character

#include "nimbus/telegram_offset.h"   // nimbus::core::TgOffsetCursor (host-tested)
#include "nimbus/tg_updates.h"        // nimbus::tg::parseUpdates (host-tested filtered parse)
#include "nimbus/orch/msg_batch.h"    // CUM-398 rapid-message batcher + drainPages (host-tested)
#include "nimbus/orch/media_label.h"  // the one inbound media label (host-tested)
#include "nimbus/orch/blob_store.h"   // BlobHasher: the content hash of an inbound file
#include "nimbus/net/tg_receive.h"    // the receive cycle: settle, breakers, turns (host-tested)
#include "nimbus/net/tg_owner.h"      // which chat the device's notices go to (host-tested)
#include "nimbus/net/tg_media_memo.h" // blob-hash dedupe of the description step (host-tested)
#include <new>                        // placement new: the memo lives in PSRAM
#include "nimbus/sys/breadcrumb.h"    // crc32: the firmware build id of the guard record
#include "nimbus/net/telegram_auth.h" // nimbus::net::TelegramAuthFailDetector (host-tested)
#include "nimbus/net/tg_poll_sched.h" // CUM-462 idle waits yield to local turns (host-tested)
#include "nimbus/net/tg_backoff.h"    // getUpdates backoff + retry_after (host-tested)
#include "adapters/http_multipart.h"  // media send (sendDocument/Photo/Voice)
#include "nimbus/net/tg_receipt.h"     // Telegram's answer -> a delivery receipt (host-tested)

#include <algorithm>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Esp.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>   // MALLOC_CAP_SPIRAM - queue storage on PSRAM (docs/memory-model.md)
#include <esp_attr.h>        // RTC_NOINIT_ATTR - the storm breakers' record
#include <esp_app_desc.h>    // the running image's ELF hash: the guard record's build id
#include <time.h>
#include "nimbus/tg_html.h"   // v4.1.1: markdown -> Telegram HTML (plain fallback below)
#include "nimbus/orch/reply_markup.h"   // lane L: cleanReplyMarkup at the Telegram seam

#define TG_HOST "api.telegram.org"
static const int TG_PORT = 443;

namespace agent {
namespace telegram {

namespace {

struct PendingReply {
  char    chatId[32];
  char    text[4097];   // ONE Telegram message: 4096 chars max + NUL. Long turn
                        // replies / sub-agent results are split into numbered (k/N)
                        // parts by send() BEFORE they reach this buffer - never truncated.
  uint8_t tries;        // resend attempts so far (transient TLS failures)
};

struct InboundMsg {
  char chatId[32];
  char from[32];   // honest origin for the unified chat view (was hardcoded "web"
                   // in the drain - a voice/serial inject showed as web-sent)
  char text[4097]; // one full Telegram message (4096 + NUL) - lifts the old 1023
                   // cap on injected (web/serial/voice) messages. The queue slots
                   // live in PSRAM (xQueueCreateWithCaps), so this costs no internal
                   // heap - only the drain's stack local (tg_poll, 16 KB).
  uint32_t queuedAt;   // millis() at injectMessage: the drain logs how long the
                       // message waited for tg_poll (the CUM-462 bench seam)
};

// One inbound photo, document or voice note of the page being drained: what the
// receive cycle needs to fetch it after the poll socket closes, inside its turn (only
// one TLS session at a time). The caption is not here: it rides the batcher at full
// length. One slot per item, kBatchMaxMediaPerDrain per drain (an album of 10 fits),
// in a lazily allocated PSRAM array (never a static: 10 slots in internal .bss would
// cost the turn's heap), freed for the next page when the page ends.
struct MediaSlot {
  char     fileId[128];
  char     fileUniqueId[48];   // Telegram's stable id: the duplicate key and the files-store hand-off
  char     chatId[32];
  char     from[64];
  char     fileName[64];
  char     mime[48];
  uint32_t size;               // Telegram's size hint (0 = unknown)
  uint32_t width, height;      // the chosen photo rendition (0 when absent)
  int32_t  updateId;
  int64_t  date;               // Telegram's send time
  uint8_t  kind;               // nimbus::tg::Attachment::Kind (Photo, Document, Voice)
  bool     used;
  TurnMedia media;             // filled once processed: the turn's ordered segment list
  const char* why;             // a failed item's reason (a static string), else nullptr
};
static constexpr size_t kMediaSlots = nimbus::orch::kBatchMaxMediaPerDrain;
static MediaSlot* g_slots = nullptr;   // PSRAM, allocated on the first media item
static MediaSlot* ensureSlots() {
  if (!g_slots) {
    g_slots = (MediaSlot*)heap_caps_calloc(kMediaSlots, sizeof(MediaSlot), MALLOC_CAP_SPIRAM);
  }
  return g_slots;
}

// The device's best epoch: the clock when set, else the newest Telegram date seen
// plus the uptime since, else uptime (the hourly ceiling and the answer times need a
// time base before the clock syncs).
static int64_t  s_serverDate = 0;
static uint32_t s_serverDateMs = 0;
static constexpr time_t kClockSet = 1577836800;   // 2020-01-01

static void noteServerDate(int64_t date) {
  if (date <= s_serverDate) return;
  s_serverDate = date;
  s_serverDateMs = millis();
}

static uint32_t deviceEpoch() {
  const time_t t = time(nullptr);
  if (t >= kClockSet) return (uint32_t)t;
  if (s_serverDate > 0) return (uint32_t)(s_serverDate + (millis() - s_serverDateMs) / 1000);
  return 1 + millis() / 1000;
}


// A media file to SEND (device -> owner), drained on the poll task after the poll
// socket closes so only one TLS session is ever resident. One slot: the next
// send overwrites a still-pending one (media sends are rare + owner-facing).
struct PendingMedia {
  char chatId[32];
  char kind[12];     // "document" | "photo" | "voice"
  char path[96];     // source file (LittleFS, or SD /mem/... when sd=true - E1)
  char caption[256];
  bool sd;           // stream from memory::dataFs() (SD) instead of LittleFS
  bool set;
};
static PendingMedia& g_media = ::nimbus::psramStatic<PendingMedia>();   // PSRAM; g_mediaMux stays internal
// sendMedia() may be called from another task/core (console TTSTG/TGSEND on the
// main loop) while the poll task drains g_media - publish/consume the slot under a
// spinlock so the drain never sees a half-written struct (torn 'set').
static portMUX_TYPE g_mediaMux = portMUX_INITIALIZER_UNLOCKED;

// A single orchestrator turn runs to completion on THIS poll task before the reply
// queue is drained (drain is at the bottom of the loop, after g_cb/g_tick return), so
// every message the turn emits synchronously must fit: reply + ask (applyTurn), an
// optional failover notice (runTurn), plus pollJobs' dispatch note + finished-job
// results in the same iteration. Depth 2 dropped the 3rd+ (send() blocked 100ms then
// returned false, and deliver() ignores that) - size it to hold a full turn's burst.
// A single long reply now ALSO fans out into up to TG_MAX_PARTS entries (see send()),
// so the depth must hold that plus the turn burst; sized to 16.
static constexpr int  REPLY_QUEUE_DEPTH    = 16;
// Was 2 with a zero-timeout send: a third rapid message while a turn ran was
// SILENTLY DISCARDED (no error, no reply). Turns are synchronous on this task
// and a media turn is slower than a text turn, so the window is real - and the
// multi-principal tests inject many chats at once. 8 slots x ~4.2 KB lives in
// PSRAM (xQueueCreateWithCaps), so the cost is PSRAM, not internal SRAM.
static constexpr int  INBOUND_QUEUE_DEPTH   = 8;
static constexpr int  MAX_LINE             = 512;
static constexpr int  MAX_RESP             = 4096;
// Long-reply splitting (Telegram caps one message at 4096 chars). send() breaks a
// longer reply / sub-agent result into numbered "(k/N) " parts of TG_CHUNK content
// chars each; TG_MAX_PARTS bounds one logical reply (~32 KB) so a runaway output
// can't flood the queue - the overflow is flagged in-message, never silently cut.
static constexpr int  TG_CHUNK             = 4000;
static constexpr int  TG_MAX_PARTS         = 8;
static constexpr uint32_t CONNECT_TIMEOUT  = 10000;  // ms

String          g_token;
String          g_allowlist;
String          g_owners;      // OWNER chat ids (subset of allow). Empty => first allow entry is owner.
MessageCallback g_cb    = nullptr;
TickCallback    g_tick  = nullptr;
SttSink         g_stt   = nullptr;
TaskHandle_t    g_task  = nullptr;
bool            g_polledOk = false;
// Every non-200 getUpdates (and a 200 that does not parse) backs off, honoring
// Telegram's retry_after; no poll goes out before the deadline, even when a local
// turn ends the pause early (nimbus/net/tg_backoff.h). Poll-task-owned.
nimbus::net::TgPollBackoff g_backoff(TG_BACKOFF_STEP_MS, TG_BACKOFF_MAX_STEPS);
// The getUpdates offset: RAM is the truth while the device runs, seeded once from
// NVS when the poll loop starts; the stored copy only matters across a restart
// (nimbus/telegram_offset.h). Poll-task-owned; /api/state reads the published copy.
nimbus::core::TgOffsetCursor g_cursor;
volatile bool     g_pubOffsetSaved = true;
volatile uint32_t g_pubOffsetWrites = 0;
volatile uint32_t g_pubOffsetWriteFails = 0;
volatile uint32_t g_pubStaleDrops = 0;
volatile uint32_t g_pubBackoffEnd = 0;     // millis() when the backoff ends (in the
                                           // past when none: one word, never torn)
// The inbound storm breakers' record (nimbus/net/tg_dup_guard.h: duplicate breaker,
// hourly ceiling, poison guard) in RTC no-init memory: it survives a restart, a panic,
// a watchdog and a brownout, never a power cycle, and costs no settings write. Magic,
// build id and CRC reject garbage; the poll task boots it before the first poll.
RTC_NOINIT_ATTR nimbus::net::TgGuardStore s_guardRtc;
nimbus::net::TgGuard g_guard(s_guardRtc);
int g_resetReason = 0;   // this boot's esp_reset_reason (main.cpp, setResetReason)
volatile uint32_t g_pubDupDrops = 0;
volatile uint32_t g_pubCeilingDrops = 0;
volatile uint32_t g_pubPoisonAcks = 0;
volatile uint8_t  g_pubTries = 0;
volatile bool     g_pubHolding = false;
volatile bool     g_pubBooted = false;   // the record was read this boot (begin())
volatile bool     g_pubKept = false;     // ...and found intact (false: rebuilt)
// CUM-308: a token verified once (getMe) can be REVOKED later while the poll loop
// keeps running - Telegram then 401s every getUpdates while the cached verdict
// still reads "verified". This debounces consecutive poll auth failures (401/403,
// NOT a 409 conflict or a transient network error) into an honest "token rejected"
// verdict, cleared immediately by the next successful poll. Poll-task-owned; the
// verdict is published to g_authRejected for the web/health surface to read.
nimbus::net::TelegramAuthFailDetector g_authDetect;
volatile bool   g_authRejected = false;
volatile int    g_activeJobs = 0;
volatile bool   g_running = false;
QueueHandle_t   g_replyQ   = nullptr;
QueueHandle_t   g_inboundQ = nullptr;

// Worst (smallest) free stack the tg_poll task has ever had, in bytes. Updated
// each poll cycle after a turn runs; surfaced in /api/state (mem.pollStackMin) so
// the deepest tool-loop chain's real headroom is visible over HTTP without serial.
volatile uint32_t g_pollStackMinFree = 0xFFFFFFFFu;

// Stack in BYTES. This task runs BOTH the mbedTLS handshake (deep: ECDHE / bignum /
// cert) AND the whole orchestrator turn on top of it. 16 KB was validated for the
// SINGLE-SHOT turn (12 KB overflowed the canary back then), but that predates the
// multi-round tool loop becoming the default path: the loop adds the head-loop
// controller + the per-round step closure + the accumulating message build ON TOP
// of the same handshake+parse chain, and 16 KB overflowed the canary during a
// tool-loop round - captured live on Board 1 2026-07-31 ("Stack canary watchpoint
// triggered (tg_poll)" at headloop round=0, heap healthy at 30 KB, so it is DEPTH
// not heap). This is the true cause of the recurring crash(panic) reboots since
// v3.5.0 (when the loop became the default). +8 KB over the old 16 KB gives real
// headroom over the deepest tool-loop chain; the actual peak is surfaced in
// /api/state (mem.pollStackMin) so the size is set from data, not guessed. The
// internal cost is paid out of internal heap (an early attempt funded it by
// halving the AsyncTCP stack, but that task ALSO runs mbedTLS inline - POST /mcp
// memory.write embeds - so halving just moved the overflow; async_tcp is now a
// measured 12288, see platformio.ini).
//
// Sized from MEASUREMENT, not a round number: a 4-round OpenAI tool-loop turn
// peaked at 16156 B used, and a FAN-OUT turn peaked at 17312 B (min free 3168 at
// an interim 20480 - that size overflowed in the field, do not go back). 24576
// clears the deepest observed peak by ~7.2 KB. The resting-heap cost is covered
// by the display-path reclaim from removing the old renderer, keeping free internal above
// the 30000 B tool-loop gate. If mem.pollStackMin ever trends
// under ~2 KB free, raise this AND reclaim bytes elsewhere (the 8 KB pverify
// task is the known candidate) rather than eating the turn budget.
static constexpr int POLL_STACK_BYTES = 24576;

// ---- helpers ----------------------------------------------------------------

// P8: public-mode mirror + a first-message approval ring, both updated live from
// the web task without a reboot. g_allowlist stays POLL-TASK-OWNED (read in
// allowed()); the web task never mutates it directly - it writes NVS + raises
// s_allowReload, and the poll task re-reads at its loop top (drainReload). The
// pending ring IS shared (poll pushes, web snapshots/approves), so it takes the
// spinlock.
static volatile bool g_public = false;
static volatile bool s_allowReload = false;
// Live TOKEN swap (owner 2026-07-16: two devices were fighting one bot with 409
// Conflicts - separating them onto two bots must not need a reboot). Staged by the
// web task, drained on the poll task at its loop top: g_token, the offset and the
// poll socket are ALL poll-task-owned, so the swap happens where they live. Same
// single-producer/single-consumer char-buffer+flag handoff as provider_verify.
static char          s_newToken[80] = {};
static volatile bool s_tokenSwap = false;
static portMUX_TYPE  s_tgMux = portMUX_INITIALIZER_UNLOCKED;

// Oversized-safe getUpdates body: a lazily-allocated PSRAM arena replaces the old
// 4 KB static, so a full 4096-char message (or a batch) parses intact. If a batch
// ever exceeds the arena, we re-poll with limit=1 (Telegram re-serves) so a single
// update always fits - nothing is lost or truncated.
static constexpr int kPollBodyCap = 128 * 1024;
static char*         g_pollBody   = nullptr;
static bool          g_limitOne   = false;   // narrow the next poll after an oversized batch
static char* ensurePollBody() {
  if (!g_pollBody) g_pollBody = (char*)heap_caps_malloc(kPollBodyCap, MALLOC_CAP_SPIRAM);
  return g_pollBody;
}

// SRAM headroom (N7): the two staging buffers below used to sit in the scarce
// internal SRAM as .bss statics - the InboundMsg drain slot (~4.1 KB) and the
// three per-call API response scratches (~2.5 KB). tg_poll is the single consumer
// of both and its TLS work is fully serialized (one slot at a time), so a batch
// never needs two of either live at once. They move to the abundant PSRAM pool -
// the same place the poll body and the reply/inbound queues already live - off the
// internal heap that a live turn's TLS handshake is contending for. Lazy-allocated;
// a null (PSRAM exhausted, never seen on an 8 MB board) falls through to each
// caller's existing soft-failure path - the inbound drain leaves messages queued
// (lossless), and the already-best-effort media fetches just skip that pass.
static InboundMsg* g_inboundStage = nullptr;
static InboundMsg* ensureInboundStage() {
  if (!g_inboundStage)
    g_inboundStage = (InboundMsg*)heap_caps_malloc(sizeof(InboundMsg), MALLOC_CAP_SPIRAM);
  return g_inboundStage;
}

// CUM-462: a local turn (voice / web / serial inject) is waiting AND this cycle's
// inbound drain can take it (callback set, staging slot allocated - begin() does
// that up front). The idle waits below end early on this, so it must never stay
// true for a message the drain cannot consume: that would turn every pause into a
// spin and every long-poll into a reconnect. Poll-task-only (g_inboundStage).
static bool localTurnWaiting() {
  return g_cb && g_inboundStage && g_inboundQ && uxQueueMessagesWaiting(g_inboundQ) > 0;
}

// Work another module staged for this task (setStagedWork: an OTA check or install
// whose own task did not fit). Set once at boot, before the task starts; read here.
static bool (*g_stagedPending)() = nullptr;
static void (*g_stagedRun)() = nullptr;
static bool stagedWorkWaiting() { return g_stagedPending && g_stagedRun && g_stagedPending(); }

// The idle waits (the inter-cycle pause, a poll-error backoff, an idle long-poll)
// end for a waiting local turn (CUM-462), for staged work, and for a quiesce
// (!g_running): each means "stop idling, let the loop come around".
static bool idleWaitEnds() { return localTurnWaiting() || stagedWorkWaiting() || !g_running; }

// Sleep an idle pause (the inter-cycle pause, a poll-error backoff) in short
// slices, ending it as soon as idleWaitEnds() says so (nimbus/net/tg_poll_sched.h).
static void idlePause(uint32_t pauseMs) {
  const uint32_t start = millis();
  for (;;) {
    // Without the quiesce leg the factory-reset quiesce only ever joined an idle
    // task by luck (release-gate verifier, 2026-10-04).
    const uint32_t ms = nimbus::net::tgIdleSleepMs(idleWaitEnds(), millis() - start, pauseMs);
    if (ms == 0) return;
    vTaskDelay(pdMS_TO_TICKS(ms));
  }
}

// Whether each cycle's Telegram poll runs or yields to a waiting local turn (never
// two cycles running). Poll-task-owned.
static nimbus::net::TgPollScheduler g_pollSched;

// CUM-398 rapid-message batcher (portable, host-tested). Reused across drains;
// cleared between them. Its accumulated payload rides the WorkingAllocator, which
// main.cpp routes to PSRAM at boot, so a batch never touches internal SRAM. Only
// ever touched on the tg_poll task (both the Telegram drain and the inbound-queue
// drain run there, sequentially - no new task, no second TLS slot).
static nimbus::orch::MessageBatcher g_batch;

// One shared response scratch for the mutually-exclusive Telegram API helpers
// that parse a small JSON body: doSendMessageRaw and the two getFile calls (voice
// + attachment). Each reads its HTTP body here then extracts what it needs before
// the next helper runs, so a single PSRAM buffer replaces three internal statics.
// (The file DOWNLOAD paths stream to LittleFS through their own local buffer, not
// this one.)
static constexpr size_t kTgApiRespCap = 1024;
static char* g_apiResp = nullptr;
static char* ensureApiResp() {
  if (!g_apiResp) g_apiResp = (char*)heap_caps_malloc(kTgApiRespCap, MALLOC_CAP_SPIRAM);
  return g_apiResp;
}

// Files-lane seam (inbound documents/photos). Unset in R1 -> such messages get a
// deterministic "can't store files yet" reply instead of being silently dropped.
static AttachmentSink g_attachSink = nullptr;
struct Pending { char chatId[24]; char name[40]; char preview[64]; };
static Pending (&s_pending)[5] = ::nimbus::psramStatic<Pending[5]>();   // PSRAM; s_tgMux stays internal
static int     s_pendCount = 0;

static void pendingPush(const char* chatId, const char* name, const char* text) {
  portENTER_CRITICAL(&s_tgMux);
  for (int i = 0; i < s_pendCount; i++)
    if (strcmp(s_pending[i].chatId, chatId) == 0) { portEXIT_CRITICAL(&s_tgMux); return; }  // dedup
  int idx;
  if (s_pendCount < 5) idx = s_pendCount++;
  else { for (int i = 1; i < 5; i++) s_pending[i - 1] = s_pending[i]; idx = 4; }  // LRU drop oldest
  strncpy(s_pending[idx].chatId, chatId, sizeof(s_pending[idx].chatId) - 1);
  s_pending[idx].chatId[sizeof(s_pending[idx].chatId) - 1] = 0;
  strncpy(s_pending[idx].name, name && name[0] ? name : "?", sizeof(s_pending[idx].name) - 1);
  s_pending[idx].name[sizeof(s_pending[idx].name) - 1] = 0;
  strncpy(s_pending[idx].preview, text, sizeof(s_pending[idx].preview) - 1);
  s_pending[idx].preview[sizeof(s_pending[idx].preview) - 1] = 0;
  portEXIT_CRITICAL(&s_tgMux);
}

static void pendingRemove(const char* chatId) {
  portENTER_CRITICAL(&s_tgMux);
  for (int i = 0; i < s_pendCount; i++)
    if (strcmp(s_pending[i].chatId, chatId) == 0) {
      for (int j = i + 1; j < s_pendCount; j++) s_pending[j - 1] = s_pending[j];
      s_pendCount--;
      break;
    }
  portEXIT_CRITICAL(&s_tgMux);
}

bool allowed(const String& chatId) {
  // SECURITY: FAIL CLOSED. The allowlist IS the device's Telegram auth boundary, and
  // bots are discoverable by username, so an empty allowlist must reject everyone (was
  // fail-OPEN = allow-all, letting any stranger drive live orchestrator turns). A
  // properly provisioned device has tgAllow set; the poll task warns if it doesn't.
  // (Only real inbound Telegram chats reach here; console/web/voice injects bypass this.)
  if (g_public) return true;   // P8: owner opted into public mode (danger)
  if (g_allowlist.length() == 0) return false;
  int start = 0;
  while (start < (int)g_allowlist.length()) {
    int end = g_allowlist.indexOf(',', start);
    if (end < 0) end = (int)g_allowlist.length();
    String entry = g_allowlist.substring(start, end);
    entry.trim();
    if (entry == chatId) return true;
    start = end + 1;
  }
  return false;
}

// The first entry of a comma-separated id list, trimmed ("" for an empty list).
static String firstListEntry(const String& list) {
  const int e = list.indexOf(',');
  String first = (e < 0) ? list : list.substring(0, e);
  first.trim();
  return first;
}

}  // namespace

// The chat that device notices go to, by the same rule as isOwner: the first explicit
// owner that is also allow-listed, else (no owners set) the first allow-list entry.
// "" when there is none. Poll-task-owned reads (g_owners/g_allowlist), as allowed():
// call it from the poll task (the archive's pressure notice runs there too). The rule
// itself is portable and host-tested (nimbus/net/tg_owner.h).
String ownerChat() {
  return String(nimbus::net::ownerChatOf(g_owners.c_str(), g_allowlist.c_str(),
                                         [](const std::string& c) { return allowed(String(c.c_str())); })
                    .c_str());
}

namespace {

static inline bool urlSafe(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static size_t urlEncodedLen(const char* s) {
  size_t n = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++)
    n += (*p == ' ' || urlSafe(*p)) ? 1 : 3;
  return n;
}

// Write `s` url-encoded to the socket in small stack-buffered chunks (avoids a
// large contiguous String alloc on the fragmented heap).
static void writeUrlEncoded(WiFiClientSecure& sc, const char* s) {
  static const char* hexd = "0123456789ABCDEF";
  char buf[96];
  int bi = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    unsigned char c = *p;
    if (bi > (int)sizeof(buf) - 3) { sc.write((const uint8_t*)buf, bi); bi = 0; }
    if (c == ' ')        { buf[bi++] = '+'; }
    else if (urlSafe(c)) { buf[bi++] = (char)c; }
    else { buf[bi++] = '%'; buf[bi++] = hexd[(c >> 4) & 0xF]; buf[bi++] = hexd[c & 0xF]; }
  }
  if (bi) sc.write((const uint8_t*)buf, bi);
}

// Open TLS to api.telegram.org. Retries the handshake: rapid alternating-host
// handshakes on the single arena intermittently fail; a fresh socket + settle
// usually succeeds.
bool tlsConnect(WiFiClientSecure& sc) {
  tlsSetup(sc);
  sc.setHandshakeTimeout(CONNECT_TIMEOUT / 1000);   // seconds
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (sc.connect(TG_HOST, TG_PORT, (int32_t)CONNECT_TIMEOUT)) return true;
    tlsClose(sc);
    if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(400));
  }
  char err[64] = {};
  sc.lastError(err, sizeof(err));
  alogf("telegram: TLS fail (2x): %s heap=%u", err[0] ? err : "(none)", ESP.getFreeHeap());
  return false;
}

int readLine(WiFiClientSecure& sc, char* buf, int bufLen, uint32_t deadline) {
  int i = 0;
  while ((int32_t)(millis() - deadline) < 0 && i < bufLen - 1) {
    if (!sc.available()) {
      if (!sc.connected()) break;
      vTaskDelay(1);
      continue;
    }
    char c = sc.read();
    if (c == '\n') break;
    if (c != '\r') buf[i++] = c;
  }
  buf[i] = 0;
  return i;
}

int readBody(WiFiClientSecure& sc, char* buf, int bufLen, uint32_t deadline) {
  int n = 0;
  while ((int32_t)(millis() - deadline) < 0 && n < bufLen - 1) {
    if (sc.available()) buf[n++] = sc.read();
    else { if (!sc.connected()) break; vTaskDelay(1); }
  }
  buf[n] = 0;
  return n;
}

bool skipHeaders(WiFiClientSecure& sc, uint32_t deadline) {
  char line[MAX_LINE];
  for (int i = 0; i < 64; i++) {
    int n = readLine(sc, line, sizeof(line), deadline);
    if (n == 0) return true;  // blank line = headers done
  }
  return false;
}

// ---- JSON field extractor (flat; no nested objects) -------------------------
bool jsonStr(const char* json, const char* key, char* val, int valLen) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = strstr(json, needle);
  if (!p) return false;
  p += strlen(needle);
  while (*p == ' ' || *p == ':') p++;
  if (*p == '"') {
    p++;
    int i = 0;
    while (*p && *p != '"' && i < valLen - 1) {
      if (*p == '\\' && *(p+1)) { p++; }  // skip escape prefix
      val[i++] = *p++;
    }
    val[i] = 0;
    return true;
  }
  int i = 0;
  while (*p && *p != ',' && *p != '}' && *p != ']' && i < valLen - 1) val[i++] = *p++;
  val[i] = 0;
  return i > 0;
}

// ---- classification ---------------------------------------------------------
// One getUpdates page is classified into the receive cycle's per-update actions
// (nimbus/net/tg_receive.h runs them): text from an allowed chat is batched into one
// turn per chat; a photo or document becomes a media segment of that turn (its caption
// with it, at full length), fetched and described inside the turn; a voice note is a
// turn of its own; pending-approval, disallowed and inline-handled updates are AckOnly
// (the offset advances past them but they join no turn). A media item with no free
// slot this drain is StopNoAck: the drain stops WITHOUT acking it, and it and the rest
// are served again next cycle. The allowlist gates on message.chat.id (the authorized
// conversation), NEVER message.from.id (sender-controlled).

// The send time of a late or redelivered item in the device's local time ("09:58", or
// "21 Oct 09:58" on another day); "" when it is on time or the clock is unset.
static std::string sentAtOf(int64_t date) {
  const time_t now = time(nullptr);
  if (!nimbus::orch::lateItem((int64_t)now, date)) return std::string();
  struct tm d = {}, n = {};
  const time_t t = (time_t)date;
  localtime_r(&t, &d);
  localtime_r(&now, &n);
  char buf[24];
  const bool sameDay = d.tm_yday == n.tm_yday && d.tm_year == n.tm_year;
  strftime(buf, sizeof buf, sameDay ? "%H:%M" : "%d %b %H:%M", &d);
  return std::string(buf);
}

static void copyField(char* dst, size_t cap, const std::string& src) {
  strncpy(dst, src.c_str(), cap - 1);
  dst[cap - 1] = 0;
}

// Take a free media slot for an inbound photo, document or voice note: -1 when none is
// free this drain (the caller defers the update) or PSRAM is short.
static int takeSlot(const nimbus::tg::Update& u) {
  MediaSlot* slots = ensureSlots();
  if (!slots) return -1;
  for (size_t i = 0; i < kMediaSlots; ++i) {
    MediaSlot& m = slots[i];
    if (m.used) continue;
    memset(&m, 0, sizeof m);
    copyField(m.fileId, sizeof m.fileId, u.attachment.fileId);
    copyField(m.fileUniqueId, sizeof m.fileUniqueId, u.attachment.fileUniqueId);
    copyField(m.chatId, sizeof m.chatId, u.chatId);
    copyField(m.from, sizeof m.from, u.from);
    copyField(m.fileName, sizeof m.fileName, u.attachment.fileName);
    copyField(m.mime, sizeof m.mime, u.attachment.mime);
    m.size = u.attachment.fileSize;
    m.width = u.attachment.width;
    m.height = u.attachment.height;
    m.updateId = u.updateId;
    m.date = u.date;
    m.kind = (uint8_t)u.attachment.kind;
    m.used = true;
    return (int)i;
  }
  return -1;
}

// The page ended (its turns ran, or it was acked unprocessed): every slot is free.
static void freeSlots() {
  if (!g_slots) return;
  for (size_t i = 0; i < kMediaSlots; ++i) g_slots[i].used = false;
}

// The processed media of the turn about to run, in album order (turnMediaAt).
static int8_t s_turnSlots[kMediaSlots];
static size_t s_turnMedia = 0;
// histV2: the blob ref (16 hex) of the voice note processVoice just stored ("" = none).
static char s_voiceRef[17] = {};

// Route a text field into cu as a batched turn segment (grouped into one turn per
// chat by the drain driver). Model sees the full length; a late one carries its send
// time. A forwarded "/command" is someone else's words: it enters labeled and never runs.
static void batchTextInto(const nimbus::tg::Update& u, nimbus::orch::ClassifiedUpdate& cu) {
  cu.action = nimbus::orch::ClassifiedUpdate::Action::BatchText;
  cu.chatId = u.chatId;
  cu.from   = u.from;
  cu.text   = u.forwarded ? nimbus::orch::forwardedText(u.text) : u.text;
  cu.sentAt = sentAtOf(u.date);
}

// Voice note: a turn of its own (its transcript), in its place among the chat's
// messages. No free slot: StopNoAck, so it and the rest are served next poll.
static void classifyVoice(const nimbus::tg::Update& u, bool isAllowed, nimbus::orch::ClassifiedUpdate& cu) {
  if (!(isAllowed && u.attachment.fileId.size())) return;
  const int slot = takeSlot(u);
  if (slot < 0) { cu.action = nimbus::orch::ClassifiedUpdate::Action::StopNoAck; return; }
  cu.action = nimbus::orch::ClassifiedUpdate::Action::BatchVoice;
  cu.chatId = u.chatId;
  cu.from = u.from;
  cu.slot = slot;
  alogf("telegram: voice note from %s queued (update %ld)", u.chatId.c_str(), (long)u.updateId);
}

// Inbound file (document/photo): hand to the files lane if installed, else a media
// segment of its chat's turn (fetched and described inside the turn), else route any
// caption as text - never a silent drop. No free slot: StopNoAck.
static void classifyFile(const nimbus::tg::Update& u, const String& chatId, const String& from,
                         bool isAllowed, nimbus::orch::ClassifiedUpdate& cu) {
  const int kind = (u.attachment.kind == nimbus::tg::Attachment::Kind::Document) ? 1 : 2;
  bool handled = false;
  if (isAllowed && g_attachSink)
    handled = g_attachSink(chatId, from, String(u.attachment.fileId.c_str()),
                           String(u.attachment.fileName.c_str()),
                           String(u.attachment.mime.c_str()), u.attachment.fileSize,
                           kind, String(u.text.c_str()));
  if (isAllowed && !handled && u.attachment.fileId.size()) {
    const int slot = takeSlot(u);
    if (slot < 0) { cu.action = nimbus::orch::ClassifiedUpdate::Action::StopNoAck; return; }
    cu.action = nimbus::orch::ClassifiedUpdate::Action::BatchMedia;
    cu.chatId = u.chatId;
    cu.from = u.from;
    cu.text = u.forwarded ? nimbus::orch::forwardedText(u.text) : u.text;   // the caption, at full length
    cu.slot = slot;
    alogf("telegram: media queued (update %ld, group %s, %ux%u)", (long)u.updateId,
          u.mediaGroupId.size() ? u.mediaGroupId.c_str() : "-", (unsigned)u.attachment.width,
          (unsigned)u.attachment.height);
    return;
  }
  if (isAllowed && !handled && g_cb) {
    // Nothing could take it: still route any caption as text (batched with this
    // chat's other messages), else say so - the owner's words are never lost.
    if (u.text.size()) batchTextInto(u, cu);
    else send(chatId, "I couldn't take that file. Send the details as a text message instead.", false);
  } else if (!isAllowed) {
    pendingPush(chatId.c_str(), from.c_str(), u.text.c_str());
  }
}

// Plain text message: batch it (a burst becomes ONE turn per chat), or queue an
// unlisted sender for owner approval instead of silently dropping.
static void classifyText(const nimbus::tg::Update& u, const String& chatId,
                         const String& from, bool isAllowed,
                         nimbus::orch::ClassifiedUpdate& cu) {
  if (isAllowed && g_cb) {
    alogf("telegram: msg from %s (chat %s): %.40s", from.c_str(), chatId.c_str(), u.text.c_str());
    batchTextInto(u, cu);
  } else if (!isAllowed && chatId.length()) {
    alogf("telegram: unlisted chat %s (%s) -> pending approval", chatId.c_str(), from.c_str());
    pendingPush(chatId.c_str(), from.c_str(), u.text.c_str());
  }
}

// Parse a getUpdates page body into whole updates (NO side effects - a truncated
// tail update the parser drops is simply re-served next poll). Returns whether the
// body was a well-formed ok:true response (the device folds this into the CUM-308
// auth-fail debounce).
static bool parsePageBody(const char* body, size_t len, std::vector<nimbus::tg::Update>& out) {
  out.clear();
  bool ok = false, truncatedTail = false;
  if (!nimbus::tg::parseUpdates(body, len, out, ok, truncatedTail) || !ok) {
    alogf("telegram: poll parse failed (ok=%d trunc=%d): %.60s", ok, truncatedTail, body);
    return false;
  }
  return true;   // a well-formed ok:true body - this poll cycle authenticated
}

// Classify ONE update and run its inline side effect (take a media slot / hand a file
// to the files lane / push an unlisted sender for approval / route a caption as text).
// Returns the drain action. The receive cycle calls this LAZILY, in order, and only for
// updates it actually reaches, after its duplicate breaker - so a side effect never runs
// for an update that is then left un-acked and re-served (which would run it twice).
// `polled` is false for an injected test page, whose ids are not Telegram's: the
// replay guard (which compares ids with the getUpdates offset) does not apply to it.
static nimbus::orch::ClassifiedUpdate handleUpdate(const nimbus::tg::Update& u, bool polled) {
  using Kind = nimbus::tg::Attachment::Kind;
  nimbus::orch::ClassifiedUpdate cu;
  cu.updateId = u.updateId;
  cu.action   = nimbus::orch::ClassifiedUpdate::Action::AckOnly;   // default: handled/ignored
  if (u.updateId <= 0) return cu;   // defensive: never side-effect on a bogus id
  // Replay guard: an update below the offset cursor was already confirmed, so it must
  // not run again. Never expected (the cursor only moves forward); counted, not handled.
  if (polled && !g_cursor.admit(u.updateId)) {
    alogf("telegram: dropped update %ld below the offset cursor %ld", (long)u.updateId,
          (long)g_cursor.offset());
    return cu;
  }

  const String chatId(u.chatId.c_str());
  const String from(u.from.c_str());
  const bool isAllowed = chatId.length() && allowed(chatId);
  if (u.attachment.kind == Kind::Voice)
    classifyVoice(u, isAllowed, cu);
  else if (u.attachment.kind == Kind::Document || u.attachment.kind == Kind::Photo)
    classifyFile(u, chatId, from, isAllowed, cu);
  else if (u.text.size())
    classifyText(u, chatId, from, isAllowed, cu);
  return cu;
}

// ---- getUpdates (persistent keep-alive poll socket) -------------------------
static WiFiClientSecure g_pollSc;
static bool             g_pollScOpen = false;

void closePollSocket() {
  if (!g_pollScOpen) return;
  tlsClose(g_pollSc);
  g_pollScOpen = false;
}

// Every TLS session of this task other than the poll socket opens here: the poll
// socket closes first, so at most one session is ever resident. That is what lets a
// held page keep the keep-alive socket open between its re-polls (tg_settle.h).
static bool workConnect(WiFiClientSecure& sc) {
  closePollSocket();
  return tlsConnect(sc);
}

// Read exactly `want` keep-alive body bytes: store up to bufLen-1, DRAIN the rest
// (so the next response isn't corrupted). Returns stored count, or -1 on a short
// read / disconnect (caller must closePollSocket() + reconnect).
static int readBodyN(WiFiClientSecure& sc, char* buf, int bufLen, long want, uint32_t deadline) {
  int stored = 0; long got = 0;
  while (got < want && (int32_t)(millis() - deadline) < 0) {
    if (sc.available()) {
      char c = sc.read();
      got++;
      if (stored < bufLen - 1) buf[stored++] = c;
    } else if (!sc.connected()) { buf[stored] = 0; return -1; }
    else vTaskDelay(1);
  }
  buf[stored] = 0;
  return (got >= want) ? stored : -1;
}

// Numeric HTTP status from a "HTTP/1.1 NNN reason" line (0 if not found). Used to
// tell an auth failure (401/403) from a conflict (409) or a normal 200.
static int httpStatusCode(const char* statusLine) {
  const char* sp = strchr(statusLine, ' ');
  return sp ? atoi(sp + 1) : 0;
}

// Classify a getUpdates HTTP status that is not 200 for the CUM-308 debounce. 409 =
// another client polls this same bot token (surface it once; the token is still
// valid); 401/403 = the token was rejected (the revoked-token case); anything else (a
// 429 flood-control answer, a 5xx, a 404 from a malformed token) stays Transient.
// Every one of them is a fetch error the loop backs off from (failPollStatus).
static void classifyPollStatus(int status, nimbus::net::TgPollOutcome* outcome) {
  auto setOutcome = [&](nimbus::net::TgPollOutcome o) { if (outcome) *outcome = o; };
  if (status == 409) {
    setOutcome(nimbus::net::TgPollOutcome::Conflict);
    static bool s_conflictAlerted = false;
    alog("telegram: getUpdates 409 - another device/client polls this bot token");
    const String owner = ownerChat();
    if (!s_conflictAlerted && owner.length()) {
      s_conflictAlerted = true;
      send(owner, String(nimbus::sys::deviceName().c_str()) +
                  ": another device seems to be using this Telegram bot token, so "
                  "replies will alternate between them. Give each device its own bot: "
                  "on the device web page, open Assistant, then Connectors, then Telegram.",
           /*block=*/false);
    }
    return;
  }
  if (status == 401 || status == 403) {
    setOutcome(nimbus::net::TgPollOutcome::AuthFail);
    alogf("telegram: getUpdates %d - bot token rejected (revoked or invalid)", status);
  }
}

// Read the response header block: Content-Length, a Connection: close hint, and,
// when `retryAfterS` is given, a Retry-After header (seconds; left at -1 when absent).
static void readPollHeaders(uint32_t deadline, long& contentLen, bool& serverClose,
                            int* retryAfterS = nullptr) {
  char line[MAX_LINE];
  for (int i = 0; i < 64; i++) {
    int n = readLine(g_pollSc, line, sizeof(line), deadline);
    if (n == 0) break;
    if (!strncasecmp(line, "Content-Length:", 15)) contentLen = atol(line + 15);
    else if (!strncasecmp(line, "Connection:", 11) && strstr(line, "close")) serverClose = true;
    else if (retryAfterS && !strncasecmp(line, "Retry-After:", 12))
      *retryAfterS = nimbus::net::tgRetryAfterHeaderS(line + 12);
  }
}

// A getUpdates answer other than 200: classify it, read Telegram's retry_after (the
// Retry-After header, else parameters.retry_after in the error body), close the
// socket and report a fetch error, so the loop backs off (honoring retry_after)
// instead of polling again a second later. Only the head of the error body is read.
static void failPollStatus(int status, uint32_t deadline, nimbus::orch::RawPage& rp,
                           nimbus::net::TgPollOutcome* outcome) {
  static constexpr long kErrBodyCap = 1024;
  classifyPollStatus(status, outcome);
  long contentLen = -1;
  bool serverClose = false;
  int retryAfter = -1;
  readPollHeaders(deadline, contentLen, serverClose, &retryAfter);
  char* body = ensurePollBody();
  if (retryAfter < 0 && body && contentLen > 0) {
    const long want = contentLen < kErrBodyCap ? contentLen : kErrBodyCap;
    const int n = readBodyN(g_pollSc, body, (int)want + 1, want, deadline);
    if (n > 0) retryAfter = nimbus::net::tgRetryAfterS(body, (size_t)n);
  }
  closePollSocket();
  rp.retryAfterS = retryAfter;
  rp.fetchError = true;
  alogf("telegram: getUpdates HTTP %d (retry_after %d)", status, retryAfter);
}

// Read the response body into the PSRAM arena. Returns 0 with `body`/`stored` set on
// success; 1 when the batch is larger than the arena (re-poll limit=1 next cycle,
// pg.rawCount left 0 so the drain ends this cycle - nothing lost); -1 on an alloc or
// socket error. The poll socket is closed on 1 and -1.
static int readPollBodyOrDefer(long contentLen, uint32_t deadline, bool& serverClose,
                               char*& body, int& stored, nimbus::orch::RawPage& rp) {
  body = ensurePollBody();
  if (!body) {   // PSRAM exhausted (should never happen) - drop this cycle and retry.
    alog("telegram: poll body alloc failed; skipping cycle");
    closePollSocket();
    return -1;
  }
  // A batch larger than the arena: re-poll narrow (limit=1) at the SAME offset so a
  // single update always fits - Telegram re-serves the whole batch. Nothing lost.
  if (contentLen > kPollBodyCap - 1) {
    alogf("telegram: getUpdates body %ld B > %d cap - re-polling limit=1", contentLen, kPollBodyCap);
    g_limitOne = true;
    closePollSocket();
    rp.count = 0;   // count(0) < limit -> drainPages stops this cycle
    return 1;
  }
  if (contentLen >= 0) {
    stored = readBodyN(g_pollSc, body, kPollBodyCap, contentLen, deadline);
    if (stored < 0) { closePollSocket(); return -1; }
  } else {
    readBody(g_pollSc, body, kPollBodyCap, deadline);
    stored = (int)strlen(body);
    serverClose = true;
  }
  g_limitOne = false;   // the body fit - resume normal batching
  return 0;
}

// CUM-462: wait for the first byte of a getUpdates response that asked for
// `timeoutS`, giving an idle long-poll up for a waiting local turn - never once a
// decrypted response byte is available, never a Check (timeout 0); see
// nimbus/net/tg_poll_sched.h. Read and GiveUp both hand over to readLine, which
// reads the status line or reports the failure exactly as before. The socket is
// probed only while silent, as readLine does.
static nimbus::net::TgWait awaitPollResponse(uint32_t deadline, int timeoutS) {
  for (;;) {
    const bool bytes = g_pollSc.available() > 0;
    // A quiesce or staged work ends the long-poll wait through the same path as a
    // waiting turn (socket closed, offset uncommitted - Telegram re-sends; nothing
    // is lost), instead of sitting out up to 30s of getUpdates while the factory
    // reset or the update check waits.
    const nimbus::net::TgWait w = nimbus::net::tgWaitStep(
        bytes, bytes || g_pollSc.connected(), (int32_t)(millis() - deadline) >= 0,
        idleWaitEnds(), timeoutS);
    if (w != nimbus::net::TgWait::Keep) return w;
    vTaskDelay(1);
  }
}

// Fetch ONE getUpdates page at `offset`: HTTP + parse the whole updates into `out`
// and fill `rp` (count/limit/fetchError) for the drain driver. Runs NO per-update
// side effects - the driver calls handleUpdate lazily per reached update. Does NOT
// move the offset (the drain advances the cursor AFTER the turns run, CUM-398 crash-
// safety). `outcome` (optional) receives this fetch's auth classification for the
// CUM-308 debounce: AuthFail on 401/403, Conflict on 409, Ok on a clean ok:true body,
// Transient for a connect/socket/parse hiccup or any other status. rp.fetchError marks
// a poll error that drives the backoff: any non-200 status (its retry_after lands in
// rp.retryAfterS), a connect or socket failure, or a 200 body that does not parse. An
// oversized batch leaves rp.count 0 (re-polled limit=1 next).
// `yielded` is set when the idle wait was given up for a waiting local turn (CUM-462).
static void fetchOnePage(int32_t offset, int longPollS, std::vector<nimbus::tg::Update>& out,
                         nimbus::orch::RawPage& rp, bool& yielded,
                         nimbus::net::TgPollOutcome* outcome = nullptr) {
  auto setOutcome = [&](nimbus::net::TgPollOutcome o) { if (outcome) *outcome = o; };
  setOutcome(nimbus::net::TgPollOutcome::Transient);   // default until classified
  const int usedLimit = g_limitOne ? 1 : 10;   // the getUpdates limit for THIS fetch
  rp.limit = usedLimit;
  if (!g_pollScOpen) {
    if (!tlsConnect(g_pollSc)) { alog("telegram: poll connect fail"); rp.fetchError = true; return; }
    g_pollScOpen = true;
  }

  char req[256];
  snprintf(req, sizeof(req),
    "GET /bot%s/getUpdates?offset=%ld&limit=%d&timeout=%d"
    "&allowed_updates=%%5B%%22message%%22%%5D HTTP/1.1\r\n"
    "Host: " TG_HOST "\r\n"
    "Connection: keep-alive\r\n\r\n",
    g_token.c_str(), (long)offset, usedLimit, longPollS);
  if (g_pollSc.print(req) == 0) { closePollSocket(); rp.fetchError = true; return; }

  uint32_t deadline = millis() + (longPollS + 10) * 1000UL;
  if (awaitPollResponse(deadline, longPollS) == nimbus::net::TgWait::Yield) {
    // The socket carries an unanswered request, so it cannot be reused: drop it. The
    // offset is untouched, so anything Telegram held is re-served by the next poll
    // (at-least-once). Neither success nor failure: the outcome stays Transient
    // (neutral for CUM-308) and rp.count stays 0, so nothing is batched or acked.
    closePollSocket();
    yielded = true;
    alogi("telegram: long-poll cut short for a local turn");
    return;
  }

  char line[MAX_LINE];
  if (readLine(g_pollSc, line, sizeof(line), deadline) == 0) { closePollSocket(); rp.fetchError = true; return; }
  const int status = httpStatusCode(line);
  // Every status but 200 is a fetch error that backs off. A conflict (409) means the
  // token is VALID (Telegram authenticated it), so it is NOT an auth failure; 401/403
  // is (CUM-308 keeps them apart); classifyPollStatus sets the outcome.
  if (status != 200) { failPollStatus(status, deadline, rp, outcome); return; }

  long contentLen = -1; bool serverClose = false;
  readPollHeaders(deadline, contentLen, serverClose);

  char* body = nullptr;
  int stored = 0;
  const int br = readPollBodyOrDefer(contentLen, deadline, serverClose, body, stored, rp);
  if (br < 0) { rp.fetchError = true; return; }   // alloc/socket error (socket closed inside)
  if (br == 1) return;                            // oversized: deferred to a limit=1 re-poll

  // Honor a server close. Otherwise the keep-alive socket stays open: an idle answer
  // re-polls on it with zero re-handshake churn, and so does a page held for the rest
  // of an album (tg_settle.h). That is safe only because every other TLS user of this
  // task closes it first (doSendMessageRaw, the file fetches, the typing indicator, a
  // turn, the archive tick, drainMedia, the staged work); test_tg_hooks pins it.
  if (serverClose) closePollSocket();
  // Archive hook (inbound): the raw page as read, before the turn path parses,
  // classifies, batches or deduplicates it (whole updates are kept even when the
  // rest of the page will not parse). A replay, or a held page seen again, is
  // deduplicated by update_id inside (this boot's ring costs no card read).
  agent::tgarchive::onInboundPage(body, (size_t)stored, g_token);
  const bool parsedOk = parsePageBody(body, (size_t)stored, out);
  rp.count = (int)out.size();
  for (const nimbus::tg::Update& u : out) noteServerDate(u.date);
  // A 200 whose body is not an ok:true page backs off like any failed fetch, instead
  // of polling the same unreadable answer again a second later.
  rp.fetchError = !parsedOk;
  // A clean ok:true body (empty batch or real updates) is proof the token works;
  // an unparseable/ok:false body on a non-error status stays neutral (Transient),
  // never counted as an auth failure.
  setOutcome(parsedOk ? nimbus::net::TgPollOutcome::Ok : nimbus::net::TgPollOutcome::Transient);
}

// The one write of the stored offset (the cursor checks its verdict and counts it).
static bool writeOffset(int32_t v) { return store::setTelegramOffset(v); }

// After each bucket of a page that holds a command: store the offset now, while writes
// land (TgOffsetCursor::checkpoint), so a restart re-runs at most the next bucket.
static void checkpointOffset() { g_cursor.checkpoint(millis(), writeOffset); }

#if defined(NIMBUS_TEST)
// The test seam's page (injectRawUpdates; the console's TGINJECT via injectRawPage):
// one raw getUpdates body in PSRAM, handed to the poll task under the spinlock. From
// then on the poll task owns it (and the two cursors below) until the page has run,
// and frees it there. Test builds only: an injected page can name any chat, the
// owner's included.
static constexpr size_t kRawInjectMax = 64 * 1024;
static char*        s_rawInject = nullptr;
static size_t       s_rawInjectLen = 0;
static portMUX_TYPE s_rawMux = portMUX_INITIALIZER_UNLOCKED;
static size_t       s_injNext = 0;           // poll task: first update not yet taken
static bool         s_injArchived = false;   // poll task: the page passed the archive hook
#endif  // NIMBUS_TEST

// Publish the poll health /api/state reads (telegram::pollStatus) from the poll task.
static void publishPollStatus() {
  g_pubOffsetSaved = g_cursor.saved();
  g_pubOffsetWrites = g_cursor.writeAttempts();
  g_pubOffsetWriteFails = g_cursor.writeFails();
  g_pubStaleDrops = g_cursor.staleDropped();
  const uint32_t now = millis();
  g_pubBackoffEnd = now + g_backoff.remainingMs(now);   // == now (already past) when none
}

// Fold one drain's result into the getUpdates backoff. A poll error backs off: the
// cycle plan skips every poll until the deadline (tgPlanWithBackoff), so the loop itself
// keeps cycling at its normal pace - jobs advance, replies and local turns go out -
// even through a long retry_after; only getUpdates waits. A yielded poll proves
// nothing either way; any other answer clears the backoff.
static void noteDrainResult(int n, bool yielded, int retryAfterS) {
  if (n < 0) {
    const uint32_t backoff = g_backoff.onError(millis(), retryAfterS);
    alogf("telegram: poll error (%u), next poll in %ums", (unsigned)g_backoff.fails(),
          (unsigned)backoff);
    publishPollStatus();
  } else if (!yielded) {
    g_backoff.onOk();
    if (!g_polledOk) { g_polledOk = true; alog("telegram: poll ok"); }
  }
}

// Keep the stored offset in step with the RAM cursor: one write per newly acked page,
// and while a write is owed and failing (a full NVS), at most one retry a minute. The
// result is checked here; a failure is already recorded by name in the persist ledger
// (nvs.recentFails, the device log) and leaves tgOffsetSaved false in /api/state.
static void persistOffset() {
  g_cursor.persist(millis(), writeOffset);
  publishPollStatus();
}

// The once-per-boot "settings storage is almost full" notice (raised by any failed
// NVS write, nimbus::sys::takeStorageNotice), sent here on the poll task's own
// outbound path - never from the writer's task. Waits for an owner chat to exist.
static void sendStorageNotice() {
  const String owner = ownerChat();
  if (!owner.length() || !nimbus::sys::takeStorageNotice()) return;
  String name = nimbus::sys::deviceName();
  if (!name.length()) name = "Nimbus";
  send(owner, name + ": " + nimbus::store::kStorageNoticeText, /*block=*/false);
  alog("telegram: storage-full notice sent to the owner");
}

// ---- sendMessage ------------------------------------------------------------

// One raw sendMessage POST. htmlMode adds parse_mode=HTML (the text must then
// already BE converted HTML). Returns Telegram's ok verdict.
static bool doSendMessageRaw(const char* chatId, const char* text, bool htmlMode) {
  WiFiClientSecure sc;   // workConnect frees the single arena first (never 2 resident TLS sessions)
  if (!workConnect(sc)) { alog("telegram: send connect fail"); return false; }

  char prefix[72];
  int prefixLen = snprintf(prefix, sizeof(prefix), "chat_id=%s%s&text=", chatId,
                           htmlMode ? "&parse_mode=HTML" : "");
  size_t bodyLen = (size_t)prefixLen + urlEncodedLen(text);

  char req[256];
  snprintf(req, sizeof(req),
    "POST /bot%s/sendMessage HTTP/1.0\r\n"
    "Host: " TG_HOST "\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: %u\r\n"
    "Connection: close\r\n\r\n",
    g_token.c_str(), (unsigned)bodyLen);
  sc.print(req);
  sc.print(prefix);
  writeUrlEncoded(sc, text);

  uint32_t deadline = millis() + 10000;
  char line[MAX_LINE];
  readLine(sc, line, sizeof(line), deadline);   // status line
  skipHeaders(sc, deadline);
  char* resp = ensureApiResp();
  if (!resp) { tlsClose(sc); return false; }
  readBody(sc, resp, 512, deadline);
  tlsClose(sc);

  return strstr(resp, "\"ok\":true") != nullptr;
}

// v4.1.1: messages go out as parse_mode=HTML (the model writes markdown; the
// portable nimbus::tgHtml converter maps **bold** / `code` / ``` / headings /
// links and escapes everything else) - previously every reply reached the owner
// with literal asterisks. FALLBACK: if Telegram rejects the entities, the
// ORIGINAL text is resent plain. Formatting may degrade; a reply is never lost.
// Delivery only: the reply-queue drain calls this for parts send() already observed
// and archived whole; every other text goes through sendNow.
bool doSendMessage(const char* chatId, const char* text) {
  // Defense in depth (lane L, formatting F4): every Telegram send - turn replies,
  // reply.telegram, loop deliveries, notices - passes the reply seam, so no
  // reasoning, tool-call markup or wrapper tag reaches the chat even from a path
  // the engine did not clean. Idempotent: an engine-cleaned reply is unchanged.
  const std::string clean = nimbus::orch::cleanReplyMarkup(text ? text : "");
  const std::string html = nimbus::tgHtml(clean);
  bool ok = doSendMessageRaw(chatId, html.c_str(), /*htmlMode=*/true);
  if (!ok) {
    alogf("telegram: html send rejected - resending plain to %s", chatId);
    ok = doSendMessageRaw(chatId, clean.c_str(), /*htmlMode=*/false);
  }
  alogf("telegram: send %s to %s", ok ? "ok" : "FAIL", chatId);
  return ok;
}

// A short notice the poll task sends at once, bypassing the reply queue (the voice
// note path). Archived like every outbound send.
static bool sendNow(const char* chatId, const char* text) {
  agent::tgarchive::onOutboundText(String(chatId), String(text ? text : ""), "direct");   // archive hook
  return doSendMessage(chatId, text);
}

// ---- voice notes (LIVE-GATED + BENCH-BROKEN) --------------------------------
// Reject a Telegram file_id / file_path with chars outside [A-Za-z0-9._/-] so an
// unexpected value can't inject into the HTTP request line (CRLF / header inject).
static bool tgPathSafe(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; ++p) {
    char ch = *p;
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == '/'))
      return false;
  }
  return true;
}

// TGINJECT media (test builds only): a "LOCAL_<path>" file id names a file already on
// the card, copied to the LittleFS staging file instead of downloaded. In a
// production build this is always false and the prefix means nothing.
static bool injectedLocal(const char* fileId, const char* lfsPath, size_t& total, size_t cap) {
#ifdef NIMBUS_TEST
  return agent::tgarchive::copyLocalInject(fileId, lfsPath, total, cap);
#else
  (void)fileId; (void)lfsPath; (void)total; (void)cap;
  return false;
#endif
}

// Step 1 of a voice note or file: resolve its download path via getFile (one retry
// on an empty answer). Connect: no TLS session (the caller stays silent, as before);
// NoPath: Telegram gave no usable path (the caller tells the sender).
enum class TgPath : uint8_t { Ok, Connect, NoPath };
static TgPath resolveFilePath(const char* fileId, char* filePath, size_t cap, const char* what) {
  filePath[0] = 0;
  for (int t = 0; t < 2; t++) {
    WiFiClientSecure sc;
    if (!workConnect(sc)) {
      alogf("telegram: %s getFile connect fail", what);
      return TgPath::Connect;
    }
    char req[256];
    snprintf(req, sizeof(req),
      "GET /bot%s/getFile?file_id=%s HTTP/1.0\r\n"
      "Host: " TG_HOST "\r\nConnection: close\r\n\r\n", g_token.c_str(), fileId);
    sc.print(req);
    uint32_t deadline = millis() + 12000;
    char line[MAX_LINE]; readLine(sc, line, sizeof(line), deadline);
    skipHeaders(sc, deadline);
    char* resp = ensureApiResp();
    if (!resp) { tlsClose(sc); break; }
    readBody(sc, resp, (int)kTgApiRespCap, deadline);
    tlsClose(sc);
    jsonStr(resp, "file_path", filePath, (int)cap);
    if (filePath[0] == 0) { vTaskDelay(pdMS_TO_TICKS(400)); continue; }
    break;
  }
  if (tgPathSafe(filePath)) return TgPath::Ok;
  alogf("telegram: %s file_path missing/unsafe - rejected", what);
  return TgPath::NoPath;
}

// Step 2: stream the file at `filePath` to `localPath` on LittleFS, up to `cap`
// bytes within `timeoutMs`. `total` counts what LANDED (a short write on a full or
// failing FS stops the copy rather than keeping a silently truncated file).
enum class TgDl : uint8_t { Done, Connect, Open, Write };
static TgDl downloadToLfs(const char* filePath, const char* localPath, size_t cap,
                          uint32_t timeoutMs, size_t& total) {
  total = 0;
  WiFiClientSecure sc;
  if (!workConnect(sc)) return TgDl::Connect;
  char req[320];
  snprintf(req, sizeof(req),
    "GET /file/bot%s/%s HTTP/1.0\r\n"
    "Host: " TG_HOST "\r\nConnection: close\r\n\r\n", g_token.c_str(), filePath);
  sc.print(req);
  uint32_t deadline = millis() + timeoutMs;
  char line[MAX_LINE]; readLine(sc, line, sizeof(line), deadline);
  skipHeaders(sc, deadline);
  File f = LittleFS.open(localPath, FILE_WRITE);
  if (!f) { tlsClose(sc); return TgDl::Open; }
  uint8_t buf[512];
  bool writeFail = false;
  while ((int32_t)(millis() - deadline) < 0 && total < cap) {
    if (sc.available()) {
      int r = sc.read(buf, sizeof(buf));
      if (r > 0) {
        if ((int)f.write(buf, r) != r) { writeFail = true; break; }
        total += r;
      }
    } else if (!sc.connected()) break;
    else delay(2);
  }
  f.close();
  tlsClose(sc);
  return writeFail ? TgDl::Write : TgDl::Done;
}

// A file copied from the card instead of downloaded: "" when it landed whole, else why.
static const char* localCopyVerdict(const char* localPath, size_t cap, size_t total) {
  if (total > 0 && total < cap) return "";
  LittleFS.remove(localPath);
  return total >= cap ? "too large" : "download failed";
}

// Steps 1-2 of any inbound file: bring it to `localPath` on LittleFS (the staging
// file, deleted as soon as the durable copies exist), at most `cap` bytes. One
// download per Telegram file: when the archive already holds this very file (same
// update, same file id) it is copied from the card instead; a test build's
// "LOCAL_<path>" id reads a card file. Returns "" on success, else the plain reason it
// could not ("too large", "download failed", ...), which goes into the turn's label.
static const char* fetchToLfs(const MediaSlot& m, const char* localPath, size_t cap, size_t& total) {
  // LittleFS refuses to open a file for write in a nonexistent directory, and nothing
  // else creates /audio - on a fresh filesystem EVERY voice note died here with a
  // silent "voice fs open fail" (field bug, Board 3 2026-07-21).
  if (!LittleFS.exists("/audio")) LittleFS.mkdir("/audio");
  total = 0;
  const agent::tgarchive::InboundRef ref{m.chatId, m.updateId, m.date, m.fileId};
  if (agent::tgarchive::takeFromArchive(ref, localPath, cap, total) ||
      injectedLocal(m.fileId, localPath, total, cap))
    return localCopyVerdict(localPath, cap, total);
  char filePath[160];
  const TgPath fp = resolveFilePath(m.fileId, filePath, sizeof(filePath), "media");
  if (fp != TgPath::Ok) return "download failed";
  // One retry on a short download (voice notes: a long note came back as a short
  // partial transcript with no error anywhere, owner-hit 2026-07-22).
  for (int t = 0; t < 2; t++) {
    const TgDl d = downloadToLfs(filePath, localPath, cap, 60000, total);
    if (d == TgDl::Connect) return "download failed";
    if (d == TgDl::Open) return "storage hiccup";
    // Hitting the cap means the file was TRUNCATED, not finished. Telegram may omit
    // file_size, so the pre-check cannot always catch an oversized file - and storing a
    // torn JPEG durably, then failing to describe it, is the worst of both outcomes.
    if (d == TgDl::Write || total >= cap) {
      LittleFS.remove(localPath);
      return d == TgDl::Write ? "storage full" : "too large";
    }
    if (total >= 512) return "";
    vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (total > 0) return "";
  LittleFS.remove(localPath);
  return "download failed";
}

// The content hash of a staged file (the blob store's 16-hex FNV-1a digest): the
// label's ref, the blob sidecar's name and the files store's dedupe key.
static std::string hashFile(const char* localPath) {
  File f = LittleFS.open(localPath, FILE_READ);
  if (!f) return std::string();
  nimbus::orch::BlobHasher h;
  uint8_t buf[512];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) h.update(buf, n);
  f.close();
  return h.hex();
}

// What the file store needs from the receive boundary: the staged file, its content
// hash and Telegram's file_unique_id (the files lane dedupes on them), and the name it
// is kept under: every stored attachment is named by its update id, so a photo can
// never replace an older one after a restart and two documents with the same name
// never overwrite each other.
struct InboundFile {
  const MediaSlot& slot;
  const char* lfsPath;
  std::string hash;
  bool photo;
};

static bool storeInbound(const InboundFile& in, const nimbus::orch::Principal& who) {
  char fname[96];
  if (in.photo || !in.slot.fileName[0])
    snprintf(fname, sizeof(fname), "%s-%ld.%s", in.photo ? "photo" : "file", (long)in.slot.updateId,
             in.photo ? "jpg" : "bin");
  else
    snprintf(fname, sizeof(fname), "%ld-%s", (long)in.slot.updateId, in.slot.fileName);
  // One project per sender keeps a chat's files together and readable at a glance in
  // the web UI. The hash and in.slot.fileUniqueId are the files store's dedupe keys.
  char proj[48];
  snprintf(proj, sizeof(proj), "chat-%s", in.slot.chatId);
  std::string ferr;
  if (agent::files::saveStream(proj, fname, LittleFS, in.lfsPath, ferr, who.ns)) return true;
  alogf("telegram: attach store failed: %s", ferr.c_str());
  return false;
}

static nimbus::orch::Principal senderOf(const char* chatId) {
  return nimbus::orch::principalForRole(
      std::string(chatId), orchestrator::roleOfChat(String(chatId)),
      [&] { nimbus::orch::Quota q;
            orchestrator::tenantQuotaOf(std::string(chatId), q); return q; }());
}

// Why a photo or document cannot be taken before it is fetched ("" when it can).
//
// SD-GATED, by the owner's decision: media on internal flash would compete with the
// firmware's own update slots for a 3.5 MB partition, and one phone photo can be a
// sixth of it. With no card the turn says so plainly instead of half-accepting a file
// the device cannot keep. The sender's storage limit is checked BEFORE the download
// (a quota enforced only after the bytes landed lets every first offender through);
// Telegram's size hint can read low, so the real size is checked again after.
static const char* refusalOf(const MediaSlot& m, const nimbus::orch::Principal& who) {
  if (!tgPathSafe(m.fileId)) return "unreadable file id";
  if (!agent::memory::haveSd()) return "no memory card in the device";
  if (m.size && m.size > nimbus::tg::kPhotoBudget) return "too large";
  if (files::wouldExceedQuota(who, m.size ? m.size : 1)) return "over the sender's storage limit";
  return "";
}

// The vision call's caption context for photo k of n of a turn (the captioner reads it
// after "The sender wrote: "): the album's caption (the first one among its media) and
// the item's place in it, so every photo of an album is described with its caption.
static String visionContext(size_t turn, size_t k, size_t n) {
  String cap;
  for (size_t j = 0; j < n && !cap.length(); ++j) cap = g_batch.mediaCaptionAt(turn, j);
  if (n <= 1) return cap;
  const String place = String("(photo ") + (unsigned)(k + 1) + " of " + (unsigned)n + " sent together)";
  return cap.length() ? cap + " " + place : place;
}

// The typing indicator while a turn's media are fetched: at most once per photo, and
// only kTypingGapMs or more after the last one (Telegram shows it for about 5 s; each
// is one short TLS session on this task, so an album never turns into a burst of them).
static constexpr uint32_t kTypingGapMs = 4000;
static uint32_t s_lastTypingMs = 0;
static bool     s_typedOnce = false;

static void maybeTyping(const char* chatId) {
  if (!g_token.length() || !chatId[0]) return;
  for (const char* p = chatId; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || *p == '-')) return;   // a real Telegram chat id only
  const uint32_t now = millis();
  if (s_typedOnce && now - s_lastTypingMs < kTypingGapMs) return;
  char body[64];
  const int len = snprintf(body, sizeof(body), "chat_id=%s&action=typing", chatId);
  if (len <= 0 || len >= (int)sizeof(body)) return;   // never a Content-Length the body cannot meet
  s_typedOnce = true;
  s_lastTypingMs = now;
  WiFiClientSecure sc;
  if (!workConnect(sc)) return;
  char req[256];
  snprintf(req, sizeof(req),
           "POST /bot%s/sendChatAction HTTP/1.0\r\nHost: " TG_HOST "\r\n"
           "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n"
           "Connection: close\r\n\r\n",
           g_token.c_str(), len);
  sc.print(req);
  sc.print(body);
  char line[MAX_LINE];
  readLine(sc, line, sizeof(line), millis() + 5000);
  tlsClose(sc);
}

// Photo or document k of n of turn `turn` (slot `slot`): fetched, hashed, handed to the
// archive (one download), described (a photo), kept in the file store and the episodic
// record, and returned as its segment text - the one label (media_label.h) and the
// description, or why there is none. Never a separate reply: a failure is part of the
// turn, so the assistant answers it with everything else the owner sent.
// The blob-hash dedupe of the paid description step (nimbus/net/tg_media_memo.h): a
// photo sent again with the same bytes and caption is described once. Allocated in
// PSRAM on first use (its strings ride PSRAM too), never a static.
static nimbus::net::MediaMemo* g_memo = nullptr;
static nimbus::net::MediaMemo* memo() {
  if (!g_memo) {
    void* p = heap_caps_malloc(sizeof(nimbus::net::MediaMemo), MALLOC_CAP_SPIRAM);
    if (p) g_memo = new (p) nimbus::net::MediaMemo();
  }
  return g_memo;
}

// Record photo k of n of the turn about to run in its ordered segment list.
static void noteTurnMedia(int slot, const nimbus::orch::MediaLabel& label, bool fetched) {
  MediaSlot& m = g_slots[slot];
  TurnMedia& t = m.media;
  t.k = label.k;
  t.n = label.n;
  t.photo = label.kind == nimbus::orch::MediaKind::Image;
  t.fetched = fetched;
  copyField(t.ref, sizeof t.ref, fetched ? label.ref : std::string());
  copyField(t.mime, sizeof t.mime, m.mime);
  t.sentAt = m.date;
  t.kept = false;   // set once its media row is stored
  if (s_turnMedia < kMediaSlots) s_turnSlots[s_turnMedia++] = (int8_t)slot;
}

// The segment of an item that could not be taken or fetched (false: not fetched). The
// reason stays with the item for a reply that has nothing else to answer.
static bool failedSegment(nimbus::orch::MediaLabel& label, const char* why, int slot, std::string& seg) {
  label.cls = nimbus::orch::MediaClass::FetchFailed;
  label.detail = why;
  seg = nimbus::orch::mediaSegment(label);
  noteTurnMedia(slot, label, false);
  g_slots[slot].why = why;
  alogf("telegram: media %d/%d not taken (update %ld): %s", label.k, label.n, (long)g_slots[slot].updateId, why);
  return false;
}

// Steps 3-5 once the file is staged: the label's ref (the content hash), the archive's
// copy (one download), the description (a photo) or the name (a document).
static void describeStaged(const InboundFile& in, size_t turn, size_t k, size_t n, nimbus::orch::MediaLabel& label) {
  const MediaSlot& m = in.slot;
  label.ref = in.hash;
  agent::tgarchive::adoptInbound({m.chatId, m.updateId, m.date, m.fileId}, in.lfsPath);
  // Vision is best-effort: if no provider can look at it, the photo is still kept and
  // the label says so. The same bytes and caption already described: no second call.
  String description;
  const String ctx = in.photo ? visionContext(turn, k, n) : String();
  std::string known;
  nimbus::net::MediaMemo* mm = in.photo ? memo() : nullptr;
  if (mm && mm->find(m.chatId, in.hash, ctx.c_str(), known)) {
    description = known.c_str();
    alogf("telegram: media %d/%d described before (same photo and caption): no second call", label.k, label.n);
  } else if (in.photo && agent::vision::available()) {
    alogf("telegram: media %d/%d describing", label.k, label.n);
    description = agent::vision::describeImage(in.lfsPath, m.mime[0] ? m.mime : "image/jpeg", nullptr, ctx);
    if (mm) mm->put(m.chatId, in.hash, ctx.c_str(), description.c_str());
  }
  if (!in.photo) {
    label.cls = nimbus::orch::MediaClass::Stored;
    label.detail = m.fileName;
    return;
  }
  label.cls = description.length() ? nimbus::orch::MediaClass::Described : nimbus::orch::MediaClass::NotDescribed;
  label.detail = description.c_str();
}

static bool processMedia(size_t turn, size_t k, size_t n, int slot, std::string& seg) {
  const MediaSlot& m = g_slots[slot];
  const bool photo = m.kind == (uint8_t)nimbus::tg::Attachment::Kind::Photo;
  nimbus::orch::MediaLabel label;
  label.kind = photo ? nimbus::orch::MediaKind::Image : nimbus::orch::MediaKind::File;
  label.k = (int)k + 1;
  label.n = (int)n;
  label.sentAt = sentAtOf(m.date);
  const nimbus::orch::Principal who = senderOf(m.chatId);
  const char* why = refusalOf(m, who);
  if (why[0]) return failedSegment(label, why, slot, seg);
  if (photo) maybeTyping(m.chatId);
  closePollSocket();
  nimbus::heapwatch::Scope downloading(nimbus::heapwatch::kTgDownload);
  const char* localPath = "/audio/tgattach.bin";
  size_t total = 0;
  alogf("telegram: media %d/%d fetch start (update %ld)", label.k, label.n, (long)m.updateId);
  why = fetchToLfs(m, localPath, nimbus::tg::kPhotoBudget, total);
  if (!why[0] && files::wouldExceedQuota(who, (uint32_t)total)) {
    LittleFS.remove(localPath);
    why = "over the sender's storage limit";
  }
  if (why[0]) return failedSegment(label, why, slot, seg);
  alogf("telegram: attachment downloaded %u bytes", (unsigned)total);
  downloading.end();
  const InboundFile in{m, localPath, hashFile(localPath), photo};
  describeStaged(in, turn, k, n, label);
  seg = nimbus::orch::mediaSegment(label);
  noteTurnMedia(slot, label, true);
  // Store it through the FILE STORE: that is what makes it count against the sender's
  // storage limit, and a photo someone sent shows up in Memory & Files like any other
  // file. The episodic row is what the MODEL reads later, stamped with the original
  // send time; the blob sidecar is what it can be re-shown from.
  storeInbound(in, who);
  g_slots[slot].media.kept = agent::memory::captureMediaFile(
      m.chatId, "user", photo ? nimbus::orch::MsgKind::Image : nimbus::orch::MsgKind::File,
      String(seg.c_str()), localPath, photo ? "jpg" : "bin", m.date);
  LittleFS.remove(localPath);
  return true;
}

// A turn of failed media only (no text, no caption, nothing fetched): one plain reply
// to the sender with the reason and the next step, instead of a paid turn.
static void mediaFailed(const char* chatId, size_t count) {
  const char* why = "";
  for (size_t i = 0; i < s_turnMedia && !why[0]; ++i) why = g_slots[s_turnSlots[i]].why ? g_slots[s_turnSlots[i]].why : "";
  const bool many = count > 1;
  String text;
  if (!strcmp(why, "no memory card in the device"))
    text = "This device can't keep photos or files without a memory card. Describe it in words instead.";
  else if (!strcmp(why, "too large"))
    text = many ? "Those files are too large to take. Send smaller ones." : "That file is too large to take. Send a smaller one.";
  else if (!strcmp(why, "over the sender's storage limit"))
    text = "Your file storage on this device is full. Ask the owner to raise it, or delete something first.";
  else
    text = many ? "Couldn't fetch those files. Try sending them again." : "Couldn't fetch that file. Try sending it again.";
  sendNow(chatId, text.c_str());
}

// An item the poison guard skips (fetching or describing it crashed the device twice):
// its label says so, and the turn runs with everything else that was sent.
static void skipMedia(size_t k, size_t n, int slot, std::string& seg) {
  const MediaSlot& m = g_slots[slot];
  nimbus::orch::MediaLabel label;
  label.kind = m.kind == (uint8_t)nimbus::tg::Attachment::Kind::Photo ? nimbus::orch::MediaKind::Image
                                                                       : nimbus::orch::MediaKind::File;
  label.k = (int)k + 1;
  label.n = (int)n;
  label.sentAt = sentAtOf(m.date);
  failedSegment(label, "it made the device restart twice", slot, seg);
}

// A voice note's turn text: downloaded (one download, shared with the archive),
// transcribed, kept, and echoed. False when it already told the sender why there is
// no transcript (no speech provider, a failed download, an empty note): no turn runs.
static bool processVoice(int slot, std::string& transcript) {
  const MediaSlot& v = g_slots[slot];
  if (!tgPathSafe(v.fileId)) { alog("telegram: voice file_id unsafe - rejected"); return false; }
  // With no STT sink wired, voice is unusable here: answer with a nudge to send text
  // and skip the download entirely.
  if (!g_stt) {
    sendNow(v.chatId, "Voice messages aren't available yet. Send a text message instead.");
    return false;
  }
  closePollSocket();
  nimbus::heapwatch::Scope downloading(nimbus::heapwatch::kTgDownload);
  const char* localPath = "/audio/tgvoice.ogg";
  size_t total = 0;
  // 1 MB: the old 64 KB cap silently kept only the first 8-30 s of opus audio.
  const char* why = fetchToLfs(v, localPath, 1048576, total);
  if (why[0]) {
    alogf("telegram: voice note not fetched: %s", why);
    sendNow(v.chatId, "Couldn't receive that voice note. Send it again, or send a text message instead.");
    return false;
  }
  alogf("telegram: voice downloaded %u bytes", (unsigned)total);
  downloading.end();
  agent::tgarchive::adoptInbound({v.chatId, v.updateId, v.date, v.fileId}, localPath);
  String text = g_stt(localPath, "audio/ogg");
  if (text.length() == 0) {           // one retry: STT flakes are usually a transient
    vTaskDelay(pdMS_TO_TICKS(1200));  // TLS/provider hiccup (owner R5c: never silently
    text = g_stt(localPath, "audio/ogg");   // ignore voice)
  }
  // Durable audit copy of the inbound voice note -> /mem/blobs on the SD card, BEFORE
  // the privacy-delete of the LittleFS working file (docs/orchestrator-storage.md
  // section 4). SD-gated no-op: with no card, nothing is retained. histV2: the
  // transcript is what the sender said, so it lives in the turn's owner row and the
  // audio row keeps the blob only (stored once); the turn names it by its ref.
  const bool histV2 = agent::history::v2Enabled();
  std::string ref;
  agent::memory::captureMediaFile(v.chatId, "user", nimbus::orch::MsgKind::Audio,
                                  histV2 ? String() : text, localPath, "ogg", v.date, &ref);
  copyField(s_voiceRef, sizeof s_voiceRef, ref);
  LittleFS.remove(localPath);   // privacy: drop the LittleFS working copy
  if (text.length() == 0) {
    sendNow(v.chatId, "Couldn't transcribe that - please send text.");
    return false;
  }
  alogf("telegram: voice -> \"%.40s\"", text.c_str());
  sendNow(v.chatId, (String("\xF0\x9F\x8E\xA4 ") + text).c_str());   // echo what was heard
  transcript = text.c_str();
  return true;
}

// One plain owner notice from the storm breakers, on this task's own outbound path.
static void sendGuardNotice(const nimbus::net::TgNotice& n) {
  const String owner = ownerChat();
  if (!owner.length()) return;
  String name = nimbus::sys::deviceName();
  if (!name.length()) name = "Nimbus";
  String text;
  if (n.kind == nimbus::net::TgNotice::Kind::Duplicate) {
    text = n.verdict == nimbus::net::DupVerdict::Replay
        ? "Telegram delivered a message again that was already answered, so the repeat was skipped."
        : String("The same message arrived twice within ") + (unsigned)(nimbus::net::kRepeatWindowS / 60) +
              " minutes, before the first was answered, so the repeat was skipped. Send it again if you "
              "meant to.";
  } else if (n.kind == nimbus::net::TgNotice::Kind::Ceiling) {
    char when[32] = "";
    const time_t at = (time_t)n.resumeAt;
    if (time(nullptr) >= kClockSet) {
      struct tm tm = {};
      localtime_r(&at, &tm);
      strftime(when, sizeof(when), "until %H:%M", &tm);
    } else {
      const uint32_t mins = (n.resumeAt > deviceEpoch() ? n.resumeAt - deviceEpoch() : 0) / 60 + 1;
      snprintf(when, sizeof(when), "for about %u minutes", (unsigned)mins);
    }
    if (n.owner)
      text = String("Replies are paused ") + when + ": " + (unsigned)nimbus::net::kInboundTurnsPerHour +
             " messages were answered in the last hour, the most it answers in an hour. Messages sent "
             "before then are not answered; send them again after that.";
    else
      text = String("Replies to other chats are paused ") + when + ": they used their share of the " +
             (unsigned)nimbus::net::kInboundTurnsPerHour + " answers an hour. Your own messages still get "
             "answers.";
  } else if (n.items == 1) {
    text = "Something you sent could not be processed: the device restarted twice while working on it. "
           "Send it again.";
  } else {
    text = String("The last ") + n.items +
           " items you sent could not be processed: the device restarted twice while working on them. "
           "Send them again.";
  }
  send(owner, name + ": " + text, /*block=*/false);
  alogf("telegram: guard notice (%s) queued for the owner",
        n.kind == nimbus::net::TgNotice::Kind::Duplicate ? "duplicate"
        : n.kind == nimbus::net::TgNotice::Kind::Ceiling ? "ceiling" : "poison");
}

// The running image's id for the guard record: a record written by another build
// (its RTC layout may differ) is rebuilt, never read.
static uint32_t guardBuildId() {
  const esp_app_desc_t* d = esp_app_get_description();
  return nimbus::sys::crc32(d->app_elf_sha256, sizeof(d->app_elf_sha256));
}

static void publishGuard() {
  const nimbus::net::TgGuard::Counts c = g_guard.counts();
  g_pubDupDrops = c.dupDrops;
  g_pubCeilingDrops = c.ceilingDrops;
  g_pubPoisonAcks = c.poisonAcks;
  g_pubTries = c.tries;
  g_pubBooted = true;
}

// The storm breakers' record (RTC memory), read once per boot in begin(), before the
// poll task exists, and published at once: kept across a restart, a panic, the
// watchdogs, a brownout (a crash counts a try against work it cut short), a deep-sleep
// wake and a USB reset; rebuilt after a power-on, from another build, or when neither
// copy reads as intact. Once per boot: a second begin() must not count a crash twice.
static void bootGuard() {
  if (g_pubBooted) return;
  const bool kept = g_guard.boot(nimbus::net::bootKindOf(g_resetReason), guardBuildId());
  g_pubKept = kept;
  publishGuard();
  alogf("telegram: storm guard %s (reset %d, page tries %u, duplicates %u)", kept ? "kept" : "fresh",
        g_resetReason, (unsigned)g_guard.tries(), (unsigned)g_guard.dupDrops());
}

static void noteHistory(const nimbus::orch::PsString& text);

// The receive cycle's I/O on this task (nimbus/net/tg_receive.h): one TLS session at a
// time, every fetch and turn after the poll socket closed.
struct TgIo {
  bool polled = true;                                  // false: an injected test page
  nimbus::net::TgPollOutcome* outcome = nullptr;       // CUM-308 classification of the fetch

  nimbus::net::TgFetch fetch(int32_t offset, int timeoutS, std::vector<nimbus::tg::Update>& out) {
    nimbus::net::TgFetch f;
    fetchOnePage(offset, timeoutS, out, f.rp, f.yielded, outcome);
    return f;
  }
  uint32_t nowMs() const { return millis(); }
  int64_t nowEpoch() const {
    const time_t t = time(nullptr);
    return t >= kClockSet ? (int64_t)t : 0;
  }
  uint32_t bestEpoch() const { return deviceEpoch(); }
  bool allowed(const std::string& chat) const { return telegram::allowed(String(chat.c_str())); }
  bool isOwnerChat(const std::string& chat) const { return ownerChat() == chat.c_str(); }
  nimbus::orch::ClassifiedUpdate classify(const nimbus::tg::Update& u) { return handleUpdate(u, polled); }
  bool processMedia(size_t turn, size_t k, size_t n, int slot, std::string& seg) {
    return telegram::processMedia(turn, k, n, slot, seg);
  }
  void mediaFailed(const std::string& chat, size_t turn) {
    telegram::mediaFailed(chat.c_str(), g_batch.mediaCountAt(turn));
    s_turnMedia = 0;
  }
  void skipMedia(size_t, size_t k, size_t n, int slot, std::string& seg) { telegram::skipMedia(k, n, slot, seg); }
  bool processVoice(int slot, std::string& transcript) { return telegram::processVoice(slot, transcript); }
  // One turn per bucket, on this task, after the poll socket closed: the whole tool
  // loop runs here synchronously. The rendered text lives in PSRAM; one at a time.
  void runTurn(const std::string& from, const std::string& chat, const nimbus::orch::PsString& text, bool) {
    closePollSocket();
    noteHistory(text);   // histV2: the turn's source and stored media, for its history row
    if (g_cb) g_cb(String(from.c_str()), String(chat.c_str()), String(text.c_str()));
    s_turnMedia = 0;   // the ordered segment list belonged to that turn
  }
  void notice(const nimbus::net::TgNotice& n) { sendGuardNotice(n); }
  void checkpoint() { checkpointOffset(); }
  void endPage() {
    s_turnMedia = 0;
    freeSlots();
    publishGuard();
  }
};

// CUM-398 batching, albums as one turn (R2) and the storm breakers: one getUpdates page
// per call, at the RAM offset cursor, through the portable receive cycle. A page that
// may still grow (an album uploading, a burst being typed) is held: re-polled with
// timeout 0 next cycle, nothing classified, acked or stored meanwhile. A dispatched
// page runs its turns (each photo fetched and described inside its turn) and only then
// moves the cursor past itself, so the NEXT poll confirms it server-side whether or not
// NVS took the stored copy. A reset mid-drain re-serves the whole page (at-least-once;
// the duplicate breaker skips what was already answered). Still fully serial on
// tg_poll - no new task, one TLS session at a time. Returns the message count handled
// this cycle, or -1 on a poll error (backoff, with Telegram's retry_after in
// `retryAfterS`). `yielded`: the poll was given up for a waiting local turn (CUM-462;
// returns 0, nothing acked). A page that holds a command is committed bucket by bucket
// (checkpointOffset), so a restart re-runs at most the bucket in flight.
static nimbus::net::TgReceiver g_receiver(g_cursor, g_batch, g_guard);

// histV2 (lib/harness turn_inbox.h): what the turn about to run hands its history row -
// its source (the bucket's newest update id and message count, so a re-served page is
// captured once), the refs of the media rows its items left (album order) and what the
// sender sent with each stored item cut back to its label (the media row keeps the
// description). Noted just before the turn is routed; the engine takes it at capture.
static void noteHistory(const nimbus::orch::PsString& text) {
  const std::string voiceRef = s_voiceRef;
  s_voiceRef[0] = 0;
  if (!agent::history::v2Enabled()) return;
  const size_t i = g_receiver.currentTurn();
  if (i >= g_batch.turnCount()) return;
  std::vector<std::string> refs(g_batch.mediaCountAt(i));
  for (size_t j = 0; j < s_turnMedia; ++j) {
    const TurnMedia& t = g_slots[s_turnSlots[j]].media;
    if (t.kept && t.k >= 1 && (size_t)t.k <= refs.size()) refs[(size_t)t.k - 1] = t.ref;
  }
  agent::history::Inbound in = agent::history::inboundForBucket(g_batch, i, refs);
  if (g_batch.voiceAt(i) && !voiceRef.empty()) in.refs.push_back(voiceRef);
  agent::history::noteInbound(in, std::string(text.c_str(), text.size()));
}

static int drainAndDispatch(int longPollS, nimbus::net::TgPollOutcome* outcome, bool& yielded,
                            int& retryAfterS) {
  TgIo io;
  io.outcome = outcome;
  const nimbus::net::TgCycle c = g_receiver.cycle(io, longPollS);
  g_pubHolding = g_receiver.holding();
  publishGuard();
  yielded = c.kind == nimbus::net::TgCycle::Kind::Yielded;
  retryAfterS = c.retryAfterS;
  const nimbus::net::TgSettle& st = g_receiver.settle();
  if (c.kind == nimbus::net::TgCycle::Kind::Held)
    alogif("telegram: settle hold (re-poll %d, %s)", st.repolls(), nimbus::net::settleWhyName(st.why()));
  else if (c.kind == nimbus::net::TgCycle::Kind::Ran && st.lastHeldMs())
    alogf("telegram: settle dispatch n=%d after %u ms (%s)", c.messages, (unsigned)st.lastHeldMs(),
          nimbus::net::settleWhyName(st.why()));
  if (c.duplicates)
    alogf("telegram: %d duplicate%s dropped (%u since the guard was cleared)", c.duplicates,
          c.duplicates == 1 ? "" : "s", (unsigned)g_guard.dupDrops());
  if (c.ceilingSkips)
    alogf("telegram: hourly ceiling: %d turn%s skipped", c.ceilingSkips, c.ceilingSkips == 1 ? "" : "s");
  if (c.kind == nimbus::net::TgCycle::Kind::Poisoned || c.poisonSkips)
    alog("telegram: work that crashed the device twice was skipped unprocessed; the owner is told");
  return c.kind == nimbus::net::TgCycle::Kind::FetchError ? -1 : c.messages;
}

#if defined(NIMBUS_TEST)
// Run an injected page through the same receive path as a polled one (the archive
// hook, the duplicate breaker, handleUpdate, the batcher, the media processing, one turn
// per bucket), without fetching and without touching the offset cursor (its ids are not
// Telegram's). A page that stops on a cap (more media than this drain's slots) resumes
// at the update that was not taken on the next cycle, as Telegram would re-serve it;
// the archive sees the page once.
static void drainInjectedPage() {
  portENTER_CRITICAL(&s_rawMux);
  char* body = s_rawInject;
  const size_t len = s_rawInjectLen;
  portEXIT_CRITICAL(&s_rawMux);
  if (!body) return;
  if (!s_injArchived) agent::tgarchive::onInboundPage(body, len, g_token);   // archive hook
  s_injArchived = true;
  std::vector<nimbus::tg::Update> ups;
  const bool ok = parsePageBody(body, len, ups);   // parsePageBody logs a failure
  if (ok) {
    TgIo io;
    io.polled = false;
    nimbus::net::TgCycle c;
    const size_t next = g_receiver.injected(io, ups, s_injNext, c);
    alogf("telegram: injected page: %d message%s batched into %d turn%s%s", c.messages,
          c.messages == 1 ? "" : "s", c.turns, c.turns == 1 ? "" : "s",
          next < ups.size() ? " (stopped on a cap: resumes next cycle)" : "");
    if (c.duplicates)
      alogf("telegram: %d duplicate%s dropped (%u since the guard was cleared)", c.duplicates,
            c.duplicates == 1 ? "" : "s", (unsigned)g_guard.dupDrops());
    if (next < ups.size()) {
      s_injNext = next;   // the update that was not taken runs again next cycle
      return;
    }
  }
  s_injNext = 0;
  s_injArchived = false;
  portENTER_CRITICAL(&s_rawMux);
  s_rawInject = nullptr;
  portEXIT_CRITICAL(&s_rawMux);
  heap_caps_free(body);
}
#endif  // NIMBUS_TEST

// ---- media send (device -> owner; drained on the poll task) -----------------
// Map a kind to the Bot API method + form field + a filename/mime. Photos are sent
// as JPEG/PNG; documents keep their extension; voice must be OGG/Opus.
struct MediaMethod {
  const char* method;
  const char* field;
  const char* mime;
};
static MediaMethod mediaMethod(const char* kind) {
  if (!strcmp(kind, "photo")) return {"sendPhoto", "photo", "image/jpeg"};
  if (!strcmp(kind, "voice")) return {"sendVoice", "voice", "audio/ogg"};
  if (!strcmp(kind, "audio")) return {"sendAudio", "audio", "audio/mpeg"};
  return {"sendDocument", "document", "application/octet-stream"};
}

// The size of a file to upload, for the receipt's log line (SD under the card lock).
static size_t mediaFileSize(const char* path, bool sd) {
  if (sd) {
    agent::memory::Lock g;
    File f = agent::memory::dataFs().open(path, FILE_READ);
    const size_t n = f ? (size_t)f.size() : 0;
    if (f) f.close();
    return n;
  }
  File f = LittleFS.open(path, FILE_READ);
  const size_t n = f ? (size_t)f.size() : 0;
  if (f) f.close();
  return n;
}

// One media upload from this task, Telegram's answer read into a receipt and logged
// ("telegram: sendAudio ok (message_id N, B bytes)" or FAIL with the reason).
// `uploadName` (nullptr = the file's own name) and `title` (an audio's title) are what
// the chat shows. Closes the poll socket first: one TLS session at a time.
struct MediaUpload {
  const char* chatId;
  const char* kind;
  const char* path;
  const char* caption;
  bool sd;
  const char* uploadName;
  const char* title;
};
static nimbus::net::TgReceipt uploadMedia(const MediaUpload& u, size_t* bytesOut) {
  closePollSocket();   // single-TLS arena
  const MediaMethod mm = mediaMethod(u.kind);
  const char* base = strrchr(u.path, '/');
  base = base ? base + 1 : u.path;
  char path[128];
  snprintf(path, sizeof(path), "/bot%s/%s", g_token.c_str(), mm.method);
  std::vector<httpmp::Field> fields = {{"chat_id", u.chatId}};
  if (u.caption && u.caption[0]) fields.push_back({"caption", u.caption});
  if (u.title && u.title[0]) fields.push_back({"title", u.title});
  const size_t bytes = mediaFileSize(u.path, u.sd);
  if (bytesOut) *bytesOut = bytes;
  String resp, err;
  httpmp::Options opt;
  opt.srcFs = u.sd ? &agent::memory::dataFs() : nullptr;   // E1: SD artifacts
  opt.lockSrc = u.sd;                                      // serialize SD reads (tg_poll)
  const bool ok = httpmp::post(TG_HOST, TG_PORT, path, "", fields, mm.field,
                               u.uploadName && u.uploadName[0] ? u.uploadName : base, mm.mime,
                               u.path, resp, err, opt);
  const nimbus::net::TgReceipt r = nimbus::net::parseTgSendAnswer(ok, std::string(err.c_str()),
                                                                  std::string(resp.c_str()));
  alog(nimbus::net::tgSendLogLine(mm.method, r, bytes).c_str());
  return r;
}

static void drainMedia() {
  PendingMedia m;
  portENTER_CRITICAL(&g_mediaMux);
  if (!g_media.set) { portEXIT_CRITICAL(&g_mediaMux); return; }
  m = g_media;
  g_media.set = false;
  portEXIT_CRITICAL(&g_mediaMux);
  closePollSocket();   // single-TLS arena
  if (g_token.length() == 0) return;

  const char* base = strrchr(m.path, '/');
  base = base ? base + 1 : m.path;
  agent::tgarchive::onOutboundMedia(String(m.chatId), m.kind, m.path, m.sd, String(m.caption));   // archive hook
  const bool ok = uploadMedia({m.chatId, m.kind, m.path, m.caption, m.sd, nullptr, nullptr}, nullptr).ok;
  // A stored file that could not go out stays under Files: say so in the chat, so a
  // "queued for Telegram delivery" never ends in silence (one short notice through
  // the archived direct send, on this task, after the failed upload closed its socket).
  if (!ok && m.sd) sendNow(m.chatId, (String("Couldn't send ") + base + ". It's saved under Files.").c_str());
}

// ---- pollTask ---------------------------------------------------------------

// Check connectivity by IP - WiFi.status() can stay at 0 (WL_IDLE_STATUS) if the
// CONNECTED-event malloc failed while the device actually has an IP.
static bool wifiReady() {
  // Cast IP -> uint32 before comparing (arduino-esp32 3.1.x IPAddress operator!=
  // made `!= (uint32_t)0` ambiguous).
  return (uint32_t)WiFi.localIP() != 0u || WiFi.status() == WL_CONNECTED;
}

void pollTask(void*) {
  bool haveToken = g_token.length() > 0;   // non-const: a live token swap updates it
  while (g_running && !wifiReady()) vTaskDelay(pdMS_TO_TICKS(500));
  vTaskDelay(pdMS_TO_TICKS(3000));   // let the association RX burst drain before first TLS

  // The ONE read of the stored offset: the RAM cursor is the truth from here on, and
  // the drain never re-reads NVS (the re-read after every drain is what turned a full
  // NVS into the 14 Oct replay loop; test_nvs_full pins this single read).
  g_cursor.seed(store::telegramOffset());
  publishPollStatus();
  if (haveToken) {
    alogf("telegram: poll start (offset=%ld) heap=%u", (long)g_cursor.offset(), ESP.getFreeHeap());
    // SECURITY: warn loudly if a token is set but the allowlist is empty - allowed()
    // now fails CLOSED (rejects all chats), so the bot is inert until an allowlist is set.
    if (store::telegramAllowlist().length() == 0)
      alog("telegram: SECURITY - allowlist EMPTY, rejecting ALL chats; set an allowlist to use the bot");
  } else {
    alog("telegram: no token - local turn processor only (console/web injects + job polling)");
  }

  int activeJobs = 0;
  while (g_running) {
    // Work staged for this task by another module (setStagedWork: an OTA check or
    // install whose own task did not fit). Here at the loop top every turn, job
    // dispatch and reply send of the previous cycle has returned, so it never runs
    // inside a turn or a helper dispatch and holds no TLS work slot from them. It
    // runs before the Wi-Fi wait below: without Wi-Fi it fails fast and frees the
    // OTA claim (USB updates take the same claim), as its own task would. One run
    // per cycle, and this cycle's Telegram poll is then a Check (an immediate
    // getUpdates, read whole): Telegram is served between runs however steady the
    // staged work, and what a run queued (an update notice) goes out with this
    // cycle's replies before the next run can start. Nothing pauses, stops or
    // restarts this task for it; turns and Telegram simply wait while it runs.
    const bool ranStaged = stagedWorkWaiting();
    if (ranStaged) {
      closePollSocket();   // one TLS session at a time: the staged work opens its own
      g_stagedRun();
      if (!g_running) break;   // a quiesce arrived during the run: start nothing new
    }

    if (!wifiReady()) { closePollSocket(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

    // P8: pick up a live allowlist / public-mode change (owner approved a pending
    // sender, edited a chip, or flipped public mode) WITHOUT a reboot. Poll-task
    // owned; the web task only sets NVS + this flag.
    if (s_allowReload) {
      s_allowReload = false;
      g_allowlist = store::telegramAllowlist();
      g_owners = store::telegramOwners();
      g_public = store::telegramPublic();
    }

    // Live token swap: close the OLD bot's long-poll socket, adopt the new token,
    // and RESET the getUpdates offset - the stored offset belongs to the old bot,
    // and a stale high offset on a fresh bot silently skips every message forever
    // (the "changed the token but still can't communicate" failure). The reset goes
    // through the cursor; its stored 0 is an erase, which lands on a full NVS too.
    // The old 409-Conflict fight with the other device dies with the socket, and the
    // old bot's backoff with it.
    if (s_tokenSwap) {
      s_tokenSwap = false;
      closePollSocket();
      g_token = s_newToken;
      haveToken = g_token.length() > 0;
      g_cursor.reset();
      persistOffset();
      g_polledOk = false;
      g_backoff.onOk();
      // The new token is unproven: the old bot's auth-fail streak must not carry
      // over (nor claim the new token rejected before it has even been polled).
      g_authDetect.reset();
      g_authRejected = false;
      alogf("telegram: token swapped live - %s (offset reset)",
            haveToken ? "polling the new bot" : "token cleared, poll idle");
    }

    // Telegram long-poll: ONLY with a token. Without one the task still runs so
    // console/web-injected turns and background job polling execute off the main
    // loop (no watchdog reboot on a multi-second turn).
    // CUM-462: a waiting local turn (voice / web / serial) sets this cycle's plan
    // (tg_poll_sched.h): Skip the poll and run the turn below at once - or, right
    // after a skip, Check Telegram first with an immediate getUpdates, so Telegram
    // is served at least every other cycle.
    nimbus::net::TgPoll plan =
        haveToken ? g_pollSched.plan(localTurnWaiting()) : nimbus::net::TgPoll::LongPoll;
    if (haveToken && ranStaged) plan = nimbus::net::TgPoll::Check;   // Telegram's turn first
    // Inside a getUpdates backoff the cycle does not poll (a local turn still runs).
    if (haveToken) plan = nimbus::net::tgPlanWithBackoff(plan, g_backoff.mayPoll(millis()));
    if (haveToken && plan == nimbus::net::TgPoll::Skip) {
      closePollSocket();   // a skipped poll (a local turn, or a backoff) frees the TLS session
    } else if (haveToken) {
      // Shorten the long-poll while jobs run so the loop cycles + delivers results,
      // but not too short (each cycle's TLS churn drains heap on the no-PSRAM board).
      // While an offset write is owed (a full NVS) it ends when the retry is due. A
      // Check asks for timeout 0.
      int longPollS = nimbus::net::tgPollTimeoutS(
          plan, g_cursor.longPollS((activeJobs > 0) ? 18 : TELEGRAM_LONG_POLL_TIMEOUT_S, millis()));
      nimbus::net::TgPollOutcome outcome = nimbus::net::TgPollOutcome::Transient;
      bool yielded = false;   // CUM-462: the wait was given up for a waiting local turn
      int retryAfterS = -1;
      int n = drainAndDispatch(longPollS, &outcome, yielded, retryAfterS);
      // CUM-308: fold this cycle's auth classification into the debounce and
      // publish the honest verdict. A revoked token (repeated 401/403) flips
      // g_authRejected within a few cycles; a 409 conflict or a transient network
      // error never does; the next successful poll clears it immediately.
      const bool wasRejected = g_authRejected;
      g_authRejected = g_authDetect.update(outcome);
      if (g_authRejected != wasRejected)
        alogf("telegram: token %s", g_authRejected ? "REJECTED (auth failing) - set a new bot token"
                                                   : "auth recovered - token accepted again");
      noteDrainResult(n, yielded, retryAfterS);

      // Free the single arena before ANY non-poll TLS this cycle: active-job polling, a
      // queued reply, or an injected message. A held page keeps it open otherwise.
      if (g_pollScOpen && (g_activeJobs > 0 ||
                           (g_replyQ   && uxQueueMessagesWaiting(g_replyQ)   > 0) ||
                           (g_inboundQ && uxQueueMessagesWaiting(g_inboundQ) > 0)))
        closePollSocket();
    }

    // The stored offset follows the RAM cursor (a newly acked page now; while a write
    // is owed, at most one retry a minute - also on cycles that skipped the poll). Never
    // while a page is held: a hold stores nothing (it acked nothing either).
    if (haveToken && !g_receiver.holding()) persistOffset();

    // Quiesce: once asked to stop, start NO new work this cycle (an injected local
    // turn, a voice note or file, job polling, reply sends). It all stays queued and
    // the factory reset discards it with the restart. Without this the reset's own
    // quiesce could start a turn and then time out on it.
    if (!g_running) break;

#if defined(NIMBUS_TEST)
    drainInjectedPage();   // a test build's TGINJECT page, if one is waiting
#endif

    // Dispatch injected inbound messages (console TURN / web / voice) - ALWAYS.
    // CUM-398: batch them the same way as Telegram - a burst of injects for one chat
    // becomes ONE turn per chat. The staging slot is single-consumer (tg_poll only)
    // and lives in PSRAM (N7), off the scarce internal heap; a null (no PSRAM) leaves
    // messages queued for the next cycle. The accumulated batch also rides PSRAM
    // (g_batch / WorkingAllocator), so this adds no internal-SRAM pressure.
    if (g_inboundQ && g_cb) {
      InboundMsg* im = ensureInboundStage();
      if (im) {
        g_batch.clear();
        bool runStagedDirectly = false;   // the deferred message could not be re-queued
        while (xQueueReceive(g_inboundQ, im, 0) == pdTRUE) {
          const char* from = im->from[0] ? im->from : im->chatId;
          if (g_batch.add(im->chatId, from, im->text) == nimbus::orch::BatchAdd::Deferred) {
            // A cap fired: put this message back at the FRONT (order preserved) to
            // handle next cycle. This usually has room (we just received a slot), but a
            // concurrent producer (injectMessage from web/serial/voice) can refill the
            // queue in between, so the send CAN fail - do NOT ignore it and drop the
            // message. On a full queue, run this staged message as its own turn below
            // (after the batch's turns), so it is never silently lost.
            if (xQueueSendToFront(g_inboundQ, im, 0) != pdTRUE) {
              alog("telegram: inbound queue full on re-serve; running the staged message now");
              runStagedDirectly = true;
            }
            break;
          }
          // Batched into this drain's turns. The CUM-462 seam: inject -> drain time.
          alogif("telegram: local turn from %s waited %lu ms", from,
                 (unsigned long)(millis() - im->queuedAt));
        }
        for (size_t i = 0; i < g_batch.turnCount(); ++i)
          g_cb(String(g_batch.turnFromAt(i)), String(g_batch.chatIdAt(i)),
               String(g_batch.render(i).c_str()));
        g_batch.clear();
        // im still holds the un-requeued message (the loop broke without overwriting
        // it); run it as its own turn now that the batch's turns have freed the caps.
        if (runStagedDirectly)
          g_cb(String(im->from[0] ? im->from : im->chatId), String(im->chatId), String(im->text));
      }
    }

    if (haveToken) drainMedia();    // outbound media (device -> owner); own TLS

    // Advance background jobs (orchestrator::pollJobs) - synchronous, this task.
    // This is where a turn (incl. the full tool loop) runs, so it is the deepest
    // this stack gets (the staged OTA run at the loop top is shallower, and the
    // lifetime mark sampled here covers it too). Sample the high-water AFTER it and
    // surface the WORST seen, so the stack size is set from real peaks, not a guess -
    // and so a stack creeping toward the canary is visible long before it overflows
    // (that overflow is the recurring crash(panic); see POLL_STACK_BYTES).
    activeJobs = g_tick ? g_tick() : 0;
    g_activeJobs = activeJobs;
    {
      // uxTaskGetStackHighWaterMark returns the MINIMUM free stack ever seen, in
      // StackType_t units - which is uint8_t on the Xtensa ESP32-S3 port (see
      // portmacro.h portSTACK_TYPE), i.e. already BYTES, the same unit as
      // POLL_STACK_BYTES. g_pollStackMinFree keeps the smallest (worst) headroom.
      uint32_t freeBytes = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
      if (freeBytes < g_pollStackMinFree) {
        g_pollStackMinFree = freeBytes;
        alogf("tg_poll stack: min free %u B of %u (worst)", (unsigned)freeBytes,
              (unsigned)POLL_STACK_BYTES);
      }
    }

    if (haveToken) sendStorageNotice();   // once per boot, after a failed NVS write

    // Drain reply queue. With a token, deliver to Telegram (retry a few times on a
    // transient TLS failure). Without a token the reply was already mirrored to
    // serial by the send sink, so drain-and-discard to keep the queue from backing up.
    PendingReply reply;
    while (xQueueReceive(g_replyQ, &reply, 0) == pdTRUE) {
      if (!haveToken) continue;
      if (!doSendMessage(reply.chatId, reply.text) && ++reply.tries < 6) {
        alogf("telegram: send retry %u for %s", reply.tries, reply.chatId);
        xQueueSend(g_replyQ, &reply, 0);
        break;   // don't hot-loop; let WiFi settle (heap recovers as TIME_WAIT drains)
      }
    }

    // Telegram archive: queued outbound items, unfinished media downloads (on this
    // task's single TLS session, after the replies above went out), storage pressure.
    agent::tgarchive::tick(g_token, &idleWaitEnds, &closePollSocket);

    // Quiesce tighten (verifier round 2): a stop that ended the long-poll above
    // should not run another drain/turn cycle before the exit check - the
    // factory reset is waiting on this task.
    if (!g_running) break;
    // A held page re-polls after the settle pace; otherwise the normal pause. Either
    // ends for a local turn (CUM-462).
    idlePause(g_receiver.holding() ? nimbus::net::kSettleRepollMs
                                   : (activeJobs > 0 ? 600 : TELEGRAM_POLL_INTERVAL_MS));
  }
  // Publish "gone" BEFORE self-deleting so quiesce() can poll the handle without
  // touching a dead task's state. Queues are deliberately left alive (reboot or
  // a fresh begin() follows; deleting them here is the stop()-crash in reverse).
  g_task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

// ---- public API -------------------------------------------------------------

void begin(const String& token, const String& allowlist, MessageCallback cb) {
  // The turn task ALWAYS runs in Orchestrator mode, even with no token: it is the
  // one place off the watchdog'd main loop where console/web-injected turns run
  // and background jobs poll. The token gates only the Telegram getUpdates
  // long-poll / voice / outbound sends (see pollTask). This is what lets a turn
  // (5-30 s of TLS) execute without rebooting the board.
  g_token     = token;   // may be empty
  g_allowlist = allowlist;
  g_owners    = store::telegramOwners();    // roles (owner vs member)
  g_public    = store::telegramPublic();   // P8
  g_cb        = cb;
  // Queue storage on PSRAM, not the scarce internal heap: the reply item is 4130 B
  // (a full 4 KB Telegram message) x depth 16 = ~66 KB - a big chunk of internal RAM
  // for a task-only (no-ISR) queue on the reply path where PSRAM latency is moot.
  // See docs/memory-model.md. Deleted with vQueueDeleteWithCaps in stop().
  g_replyQ    = xQueueCreateWithCaps(REPLY_QUEUE_DEPTH, sizeof(PendingReply), MALLOC_CAP_SPIRAM);
  g_inboundQ  = xQueueCreateWithCaps(INBOUND_QUEUE_DEPTH, sizeof(InboundMsg), MALLOC_CAP_SPIRAM);
  // The inbound drain slot (PSRAM) up front, before the task starts: the idle waits
  // end early only for a message the drain can take (localTurnWaiting, CUM-462), so
  // the very first local turn after boot must find it allocated. A null here keeps
  // the old behavior (waits run full; the drain still retries the alloc lazily).
  ensureInboundStage();
  bootGuard();   // before the task: its counters are right from the first moment
  g_running   = true;
  BaseType_t ok = xTaskCreatePinnedToCore(pollTask, "tg_poll", POLL_STACK_BYTES,
                                          nullptr, 3, &g_task, 0);
  if (ok != pdPASS) {
    g_task = nullptr;
    alogf("telegram: FAILED to create poll task (heap=%u)", ESP.getFreeHeap());
    return;
  }
  alogf("telegram: turn task up (%s)",
        token.length() ? "Telegram long-poll ON" : "local turns only, no token");
}

void setTick(TickCallback cb)   { g_tick = cb; }
void setSttSink(SttSink cb)     { g_stt = cb; }
void setAttachmentSink(AttachmentSink cb) { g_attachSink = cb; }
void setResetReason(int reason) { g_resetReason = reason; }

static bool publishMedia(const String& chatId, const char* kind, const char* filePath,
                         const String& caption, bool sd, bool refuseIfBusy) {
  if (g_token.length() == 0 || !filePath || !filePath[0]) return false;
  if (strcmp(kind, "document") && strcmp(kind, "photo") && strcmp(kind, "voice") &&
      strcmp(kind, "audio")) return false;
  portENTER_CRITICAL(&g_mediaMux);
  if (refuseIfBusy && g_media.set) {   // E1 files.send: never silently DROP a queued
    portEXIT_CRITICAL(&g_mediaMux);    // artifact (the legacy overwrite is fine for
    return false;                      // rare owner-facing TTS/console sends)
  }
  chatId.toCharArray(g_media.chatId, sizeof(g_media.chatId));
  strncpy(g_media.kind, kind, sizeof(g_media.kind) - 1); g_media.kind[sizeof(g_media.kind) - 1] = 0;
  strncpy(g_media.path, filePath, sizeof(g_media.path) - 1); g_media.path[sizeof(g_media.path) - 1] = 0;
  caption.toCharArray(g_media.caption, sizeof(g_media.caption));
  g_media.sd = sd;
  g_media.set = true;   // published LAST - picked up by drainMedia() next poll cycle
  portEXIT_CRITICAL(&g_mediaMux);
  return true;
}

bool onTurnTask() { return g_task != nullptr && xTaskGetCurrentTaskHandle() == g_task; }

MediaReceipt sendMediaNow(const String& chatId, const char* kind, const char* filePath,
                          const MediaOptions& opt) {
  MediaReceipt out;
  if (!onTurnTask()) {
    out.error = "the upload can run only on the Telegram task, inside a turn";
  } else if (g_token.length() == 0) {
    out.error = "no Telegram bot token is set";
  } else if (!filePath || !filePath[0] || !kind ||
             (strcmp(kind, "document") && strcmp(kind, "photo") && strcmp(kind, "voice") &&
              strcmp(kind, "audio"))) {
    out.error = "nothing to upload";
  }
  if (out.error.length()) {
    alogf("telegram: media upload refused (%s)", out.error.c_str());
    return out;
  }
  size_t bytes = 0;
  const nimbus::net::TgReceipt r =
      uploadMedia({chatId.c_str(), kind, filePath, opt.caption, false, opt.uploadName, opt.title}, &bytes);
  out.ok = r.ok;
  out.messageId = r.messageId;
  out.bytes = bytes;
  out.error = r.error.c_str();
  // Archive what was delivered. On this task the hook copies the file before it
  // returns, so the caller may delete it afterwards.
  if (r.ok)
    agent::tgarchive::onOutboundMedia(chatId, kind, filePath, false,
                                      String(opt.caption ? opt.caption : ""));
  return out;
}

bool sendMedia(const String& chatId, const char* kind, const char* filePath,
               const String& caption) {
  return publishMedia(chatId, kind, filePath, caption, /*sd=*/false, /*refuseIfBusy=*/false);
}

bool sendMediaSd(const String& chatId, const char* kind, const char* filePath,
                 const String& caption) {
  return publishMedia(chatId, kind, filePath, caption, /*sd=*/true, /*refuseIfBusy=*/true);
}

#if defined(NIMBUS_TEST)
bool injectRawUpdates(const char* body, size_t len) {
  if (!body || len == 0 || len > kRawInjectMax) return false;
  char* copy = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
  if (!copy) return false;
  memcpy(copy, body, len);   // outside the lock: a 64 KB copy must not run with interrupts off
  copy[len] = 0;
  portENTER_CRITICAL(&s_rawMux);
  const bool busy = s_rawInject != nullptr;
  if (!busy) {
    s_rawInject = copy;
    s_rawInjectLen = len;
  }
  portEXIT_CRITICAL(&s_rawMux);
  if (busy) heap_caps_free(copy);
  return !busy;
}

// TGINJECT (the console verb): the same seam, with the counts read here by the same
// parser so the console line says what was staged.
String injectRawPage(const String& body) {
  std::vector<nimbus::tg::Update> ups;
  bool ok = false, trunc = false;
  if (!nimbus::tg::parseUpdates(body.c_str(), body.length(), ups, ok, trunc) || !ok)
    return String("TGINJECT parse FAILED ok=") + (ok ? 1 : 0) + " trunc=" + (trunc ? 1 : 0);
  if (body.length() > kRawInjectMax) return "TGINJECT refused: page larger than 64 KB";
  if (!injectRawUpdates(body.c_str(), body.length()))
    return "TGINJECT busy: the previous page is still running (or no PSRAM)";
  int media = 0, text = 0;
  for (const auto& u : ups) {
    if (u.attachment.kind != nimbus::tg::Attachment::Kind::None) media++;
    else if (u.text.size()) text++;
  }
  return String("TGINJECT queued parsed=") + (unsigned)ups.size() + " text=" + text + " media=" + media;
}
#endif  // NIMBUS_TEST

bool injectMessage(const String& chatId, const String& text) {
  if (!g_inboundQ) return false;
  InboundMsg im{};
  chatId.toCharArray(im.chatId, sizeof(im.chatId));
  // For pseudo-channels the sender IS the channel ("web"/"voice"/"serial") - the
  // drain forwards it as `from`, so the episodic capture labels the origin honestly.
  chatId.toCharArray(im.from, sizeof(im.from));
  text.toCharArray(im.text, sizeof(im.text));
  im.queuedAt = millis();
  // Report a full queue instead of dropping in silence: the caller decides what
  // the sender is told (the web/serial/voice surfaces surface it; a drop that
  // nobody can see is the bug this replaces).
  if (xQueueSend(g_inboundQ, &im, 0) != pdTRUE) {
    alogf("telegram: inbound queue FULL - dropped a message for %s", chatId.c_str());
    return false;
  }
  return true;
}

// "Enabled" = the turn task is up AND a token is configured, i.e. Telegram is
// actually usable for send/receive. The task also runs token-less (local turns),
// but that is not "Telegram enabled" for the capability manifest's purposes.
bool enabled() { return g_task != nullptr && g_token.length() > 0; }

void applyToken(const String& token) {
  token.toCharArray(s_newToken, sizeof(s_newToken));
  s_tokenSwap = true;   // drained on the poll task's next loop (within ~one poll cycle)
}

// ---- P8: live allowlist + first-message approval (web surface) --------------
// Hand-built JSON (this TU deliberately avoids ArduinoJson to protect the poll
// task's mbedTLS stack). Only "/\ and control chars need escaping in our fields.
static void jsonEsc(String& out, const char* s) {
  for (; *s; ++s) {
    char c = *s;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if ((unsigned char)c < 0x20) { /* drop other control chars */ }
    else out += c;
  }
}
String pendingJson() {
  // Snapshot under the lock (no heap work inside the critical section), then
  // serialize outside it.
  Pending snap[5];
  int n;
  portENTER_CRITICAL(&s_tgMux);
  n = s_pendCount;
  for (int i = 0; i < n; i++) snap[i] = s_pending[i];
  portEXIT_CRITICAL(&s_tgMux);
  String out = "[";
  for (int i = 0; i < n; i++) {
    if (i) out += ',';
    out += "{\"chatId\":\""; jsonEsc(out, snap[i].chatId);
    out += "\",\"name\":\"";  jsonEsc(out, snap[i].name);
    out += "\",\"preview\":\""; jsonEsc(out, snap[i].preview);
    out += "\"}";
  }
  out += "]";
  return out;
}

bool isAllowed(const String& chatId) { return allowed(chatId); }   // for the web list badge

// Role check: is `chatId` an OWNER (may change settings / trigger OTA), vs a plain
// allow-listed MEMBER (conversational turns only)? An owner MUST first be allow-
// listed. If the owners list is empty (the common single-account setup), the FIRST
// allow-list entry is the owner - so an existing device keeps working and only its
// primary account can do sensitive things until the owner explicitly assigns roles
// in the web UI. Poll-task-owned reads (g_owners/g_allowlist), same as allowed().
bool isOwner(const String& chatId) {
  if (!allowed(chatId)) return false;          // must be allow-listed at all
  String owners = g_owners; owners.trim();
  if (owners.length() == 0) {                  // default: first allow entry is owner
    const String firstId = firstListEntry(g_allowlist);
    return firstId.length() > 0 && firstId == chatId;
  }
  int start = 0;                               // walk the explicit owners list
  while (start < (int)owners.length()) {
    int end = owners.indexOf(',', start);
    if (end < 0) end = (int)owners.length();
    String entry = owners.substring(start, end); entry.trim();
    if (entry == chatId) return true;
    start = end + 1;
  }
  return false;
}


// Signal the poll task to re-read the allowlist + public flag from NVS (after the
// web task has written them). Applied within one poll cycle - no reboot.
void reloadAllowlist() { s_allowReload = true; }
uint32_t pollStackMinFree() { return g_pollStackMinFree; }
bool pollEverOk() { return g_polledOk; }

// Approve a pending sender: append to the allowlist NVS (+ display name), drop it
// from the pending ring, and hot-reload. Idempotent. False when the allowlist did not
// store (a full NVS): the sender stays pending, nothing is half-approved, and the
// route's SaveScope answers 507.
bool approvePending(const String& chatId, const String& name) {
  if (chatId.length() == 0) return false;
  String al = store::telegramAllowlist();
  // already present?
  bool present = false;
  { int s = 0; while (s < (int)al.length()) { int e = al.indexOf(',', s); if (e < 0) e = al.length();
      String t = al.substring(s, e); t.trim(); if (t == chatId) { present = true; break; } s = e + 1; } }
  if (!present) al = al.length() ? (al + "," + chatId) : chatId;
  if (!store::setTelegramAllowlist(al)) return false;
  if (name.length()) {   // display sidecar "id:name,..."
    // The name is the sender's ATTACKER-CONTROLLED Telegram display name - strip
    // the ',' and ':' delimiters so it can't corrupt/spoof the "id:name,..."
    // parsing (prism; display-only, but keep it clean).
    String safe = name;
    safe.replace(",", " "); safe.replace(":", " ");
    if (safe.length() > 32) safe = safe.substring(0, 32);
    // Upsert, not append: the old blind append meant a re-approve with a new name
    // never renamed (the reader matched the first entry) and the blob grew forever.
    // A name that did not store is display-only; the ledger reports it (507).
    (void)store::replaceTelegramName(chatId, safe);
  }
  pendingRemove(chatId.c_str());
  reloadAllowlist();
  return true;
}

void denyPending(const String& chatId) { pendingRemove(chatId.c_str()); }   // session tombstone

bool send(const String& chatId, const String& text, bool block) {
  if (!g_replyQ) return false;
  // Archive hook (outbound): the whole text, before it is split into parts.
  if (enabled()) agent::tgarchive::onOutboundText(chatId, text, "send");
  // block=true: wait up to 100 ms so a turn's reply isn't dropped on a full
  // queue. block=false: never wait - best-effort for the main render loop.
  const TickType_t wait = block ? pdMS_TO_TICKS(100) : 0;

  const int n = (int)text.length();
  if (n <= TG_CHUNK) {                      // fits one Telegram message - the common path
    PendingReply r{};
    chatId.toCharArray(r.chatId, sizeof(r.chatId));
    text.toCharArray(r.text, sizeof(r.text));
    return xQueueSend(g_replyQ, &r, wait) == pdTRUE;
  }

  // Long reply (sub-agent result / big turn): split into numbered "(k/N) " parts so
  // nothing is silently truncated. Bounded by TG_MAX_PARTS; if the output would need
  // more, the final part carries a visible "(truncated)" marker rather than a silent cut.
  int parts = (n + TG_CHUNK - 1) / TG_CHUNK;
  bool overflow = parts > TG_MAX_PARTS;
  if (overflow) parts = TG_MAX_PARTS;
  bool allOk = true;
  for (int k = 0; k < parts; k++) {
    PendingReply r{};
    chatId.toCharArray(r.chatId, sizeof(r.chatId));
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "(%d/%d) ", k + 1, parts);
    String body = String(hdr) + text.substring(k * TG_CHUNK, (k + 1) * TG_CHUNK);
    if (overflow && k == parts - 1)
      body += "\n…(truncated - ask for the rest, or have it sent as a file)";
    body.toCharArray(r.text, sizeof(r.text));   // body <= 4000 + short prefix/suffix < 4096
    if (xQueueSend(g_replyQ, &r, wait) != pdTRUE) { allOk = false; break; }
  }
  return allOk;
}

// ⚠ UNSAFE while tg_poll is live: this deletes g_replyQ but does NOT join the
// poll task, so a still-running pollTask loop can call xQueueReceive on the now
// -NULL g_replyQ -> configASSERT(pxQueue) -> abort/reboot (caught live wiring an
// OTA-install "free heap" hook). No caller today. Before calling it again, make
// it join the task first (signal g_running, wait for the task to self-delete,
// THEN delete the queues) - or don't stop the poller at all.
void stop() {
  g_running = false;
  if (g_replyQ) { vQueueDeleteWithCaps(g_replyQ); g_replyQ = nullptr; }
}

bool quiesce(uint32_t timeoutMs) {
  g_running = false;   // idlePause AND the long-poll wait end on this (the GiveUp
                       // path), so an idle task - token or not - joins within a
                       // slice or two; only a turn mid-provider-call holds it.
  const uint32_t t0 = millis();
  while (g_task != nullptr) {
    if (millis() - t0 >= timeoutMs) break;
    esp_task_wdt_reset();   // this runs ON the watchdog'd main task: a 7s wait
                            // without feeding starved the 8s loop WDT (verifier)
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return g_task == nullptr;
}

bool taskAlive() { return g_task != nullptr; }

void setStagedWork(bool (*pending)(), void (*run)()) {
  g_stagedPending = pending;
  g_stagedRun = run;
}

uint32_t consecutiveFails() { return g_backoff.fails(); }
int      activeJobCount()   { return g_activeJobs; }

PollStatus pollStatus() {
  PollStatus st;
  st.offsetSaved = g_pubOffsetSaved;
  st.offsetWrites = g_pubOffsetWrites;
  st.offsetWriteFails = g_pubOffsetWriteFails;
  st.staleDrops = g_pubStaleDrops;
  const int32_t left = (int32_t)(g_pubBackoffEnd - millis());
  st.backoffMs = left > 0 ? (uint32_t)left : 0;
  return st;
}

size_t turnMediaCount() { return s_turnMedia; }
const TurnMedia* turnMediaAt(size_t i) {
  return (i < s_turnMedia && g_slots) ? &g_slots[s_turnSlots[i]].media : nullptr;
}

GuardStatus guardStatus() {
  GuardStatus g;
  g.dupDrops = g_pubDupDrops;
  g.ceilingDrops = g_pubCeilingDrops;
  g.poisonAcks = g_pubPoisonAcks;
  g.tries = g_pubTries;
  g.holding = g_pubHolding;
  g.booted = g_pubBooted;
  g.kept = g_pubKept;
  return g;
}

// CUM-308: the debounced "the bot token was rejected" verdict. True only after a
// run of consecutive poll auth failures (401/403); a 409 conflict or a transient
// network error never sets it, and a successful poll clears it. The web/health
// surface reads this so a revoked token no longer reports "verified/live".
bool authRejected() { return g_authRejected; }

}  // namespace telegram
}  // namespace agent
