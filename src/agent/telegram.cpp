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
#include "memory_subsystem.h"   // captureMediaFile - durable voice-note sidecar
#include "adapters/image_vision.h"   // a photo enters the conversation as words
#include "files_subsystem.h"        // per-sender storage limit, checked pre-download
#include "orchestrator.h"           // roleOfChat / tenantQuotaOf - who is sending
#include "nimbus/mem_cap.h"         // utf8CapLen - never sever a character

#include "nimbus/telegram_offset.h"   // nimbus::core::nextTelegramOffset (host-tested)
#include "nimbus/tg_updates.h"        // nimbus::tg::parseUpdates (host-tested filtered parse)
#include "adapters/http_multipart.h"  // media send (sendDocument/Photo/Voice)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Esp.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>   // MALLOC_CAP_SPIRAM - queue storage on PSRAM (docs/memory-model.md)
#include "nimbus/tg_html.h"   // v4.1.1: markdown -> Telegram HTML (plain fallback below)

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
};

// A voice note seen during a poll, downloaded + transcribed AFTER the getUpdates
// TLS closes (only one TLS session at a time on the single arena).
struct PendingVoice {
  char fileId[128];
  char chatId[32];
  char from[64];
  bool set;
};
static PendingVoice g_voice = {};

// An inbound photo or document seen during a poll. Same deferral as a voice
// note: the download needs the TLS arena, which getUpdates is holding, so the
// work happens after the poll socket closes. One slot per cycle; a second
// attachment in the same batch is left un-acked and re-served next poll rather
// than dropped.
struct PendingAttach {
  char fileId[128];
  char chatId[32];
  char from[64];
  char fileName[64];
  char mime[48];
  char caption[257];
  uint32_t size;
  bool photo;      // false = document
  bool set;
};
static PendingAttach g_attach = {};

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
static PendingMedia g_media = {};
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
uint32_t        g_pollFails = 0;
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
static Pending s_pending[5];
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

// Parse a getUpdates response body (host-tested nimbus::tg::parseUpdates), invoke
// the callback per allowed message with the FULL message text (no 255-char cut, no
// escape mangling), and advance the offset ACK only for updates we actually
// accepted. The allowlist gates on message.chat.id (the authorized conversation),
// NEVER message.from.id (sender-controlled).
static int processUpdatesBody(const char* body, size_t len, int32_t offset) {
  std::vector<nimbus::tg::Update> updates;
  bool ok = false, truncatedTail = false;
  if (!nimbus::tg::parseUpdates(body, len, updates, ok, truncatedTail) || !ok) {
    alogf("telegram: poll parse failed (ok=%d trunc=%d): %.60s", ok, truncatedTail, body);
    return 0;
  }
  int count = 0;
  for (const auto& u : updates) {
    if (u.updateId <= 0) continue;
    const String chatId(u.chatId.c_str());
    const String from(u.from.c_str());
    const bool isAllowed = chatId.length() && allowed(chatId);

    if (u.attachment.kind == nimbus::tg::Attachment::Kind::Voice) {
      // Voice note: queue for STT (downloaded after the poll socket closes). One
      // slot per cycle. If the slot is already taken by an earlier voice in THIS
      // batch, STOP here WITHOUT acking this update - leave it (and everything after
      // it) for the next poll so it is re-served, never lost. The slot drains on this
      // poll cycle, so the next poll queues it.
      if (isAllowed && u.attachment.fileId.size()) {
        if (g_voice.set) break;   // slot busy -> defer this + the rest (ack-after-accept)
        strncpy(g_voice.fileId, u.attachment.fileId.c_str(), sizeof(g_voice.fileId) - 1);
        strncpy(g_voice.chatId, chatId.c_str(),               sizeof(g_voice.chatId) - 1);
        strncpy(g_voice.from,   from.c_str(),                 sizeof(g_voice.from)   - 1);
        g_voice.set = true;
        alogf("telegram: voice note from %s queued for STT", chatId.c_str());
      }
    } else if (u.attachment.kind == nimbus::tg::Attachment::Kind::Document ||
               u.attachment.kind == nimbus::tg::Attachment::Kind::Photo) {
      // Inbound file: hand to the files lane if installed, else answer honestly
      // (never a silent drop). The caption, if any, is passed along.
      const int kind = (u.attachment.kind == nimbus::tg::Attachment::Kind::Document) ? 1 : 2;
      bool handled = false;
      if (isAllowed && g_attachSink)
        handled = g_attachSink(chatId, from, String(u.attachment.fileId.c_str()),
                               String(u.attachment.fileName.c_str()),
                               String(u.attachment.mime.c_str()), u.attachment.fileSize,
                               kind, String(u.text.c_str()));
      if (isAllowed && !handled && u.attachment.fileId.size()) {
        // Queue it for the deferred lane (download + describe/store happen after
        // the poll socket closes). Slot busy = leave this and everything after it
        // un-acked, so it is re-served next poll rather than lost.
        if (g_attach.set) break;
        memset(&g_attach, 0, sizeof(g_attach));
        strncpy(g_attach.fileId,   u.attachment.fileId.c_str(),   sizeof(g_attach.fileId) - 1);
        strncpy(g_attach.chatId,   chatId.c_str(),                sizeof(g_attach.chatId) - 1);
        strncpy(g_attach.from,     from.c_str(),                  sizeof(g_attach.from) - 1);
        strncpy(g_attach.fileName, u.attachment.fileName.c_str(), sizeof(g_attach.fileName) - 1);
        strncpy(g_attach.mime,     u.attachment.mime.c_str(),     sizeof(g_attach.mime) - 1);
        // Byte-truncating a caption can split a multi-byte character, and the
        // orphaned continuation byte then rides into the vision request's JSON
        // body and 400s it - and into the captured turn text, where it persists.
        // utf8CapLen is the helper this codebase already uses in a dozen other
        // caps for exactly this; the new lane was the one that skipped it.
        {
          const int keep = nimbus::utf8CapLen(u.text.c_str(), (int)u.text.length(),
                                              (int)sizeof(g_attach.caption) - 1);
          memcpy(g_attach.caption, u.text.c_str(), (size_t)keep);
          g_attach.caption[keep] = 0;
        }
        g_attach.size  = u.attachment.fileSize;
        g_attach.photo = (kind == 2);
        g_attach.set   = true;
        handled = true;
      }
      if (isAllowed && !handled && g_cb) {
        // Nothing could take it: acknowledge, and still route any caption as text
        // so the owner's words are never lost.
        if (u.text.size())
          g_cb(from, chatId, String(u.text.c_str()));
        else
          send(chatId, "I couldn't take that file. Send the details as a text message instead.", false);
      } else if (!isAllowed) {
        pendingPush(chatId.c_str(), from.c_str(), u.text.c_str());
      }
    } else if (u.text.size()) {
      // Plain text message - the full length reaches the model now.
      if (isAllowed && g_cb) {
        alogf("telegram: msg from %s (chat %s): %.40s", from.c_str(), chatId.c_str(), u.text.c_str());
        g_cb(from, chatId, String(u.text.c_str()));
      } else if (!isAllowed && chatId.length()) {
        // P8: queue the sender for owner approval instead of silently dropping.
        alogf("telegram: unlisted chat %s (%s) -> pending approval", chatId.c_str(), from.c_str());
        pendingPush(chatId.c_str(), from.c_str(), u.text.c_str());
      }
    }
    // ACK-after-accept: advance the offset only for updates we handled here. A
    // truncated tail (partial update the parser did NOT return) is simply not in
    // this list, so Telegram re-serves it next poll - never lost.
    int32_t nextOffset = nimbus::core::nextTelegramOffset(offset, u.updateId);
    if (nextOffset > offset) { store::setTelegramOffset(nextOffset); offset = nextOffset; }
    count++;
  }
  return count;
}

// ---- getUpdates (persistent keep-alive poll socket) -------------------------
static WiFiClientSecure g_pollSc;
static bool             g_pollScOpen = false;

void closePollSocket() {
  if (!g_pollScOpen) return;
  tlsClose(g_pollSc);
  g_pollScOpen = false;
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

int doGetUpdates(int32_t offset, int longPollS) {
  if (!g_pollScOpen) {
    if (!tlsConnect(g_pollSc)) { alog("telegram: poll connect fail"); return -1; }
    g_pollScOpen = true;
  }

  char req[256];
  snprintf(req, sizeof(req),
    "GET /bot%s/getUpdates?offset=%ld&limit=%d&timeout=%d"
    "&allowed_updates=%%5B%%22message%%22%%5D HTTP/1.1\r\n"
    "Host: " TG_HOST "\r\n"
    "Connection: keep-alive\r\n\r\n",
    g_token.c_str(), (long)offset, g_limitOne ? 1 : 10, longPollS);
  if (g_pollSc.print(req) == 0) { closePollSocket(); return -1; }

  uint32_t deadline = millis() + (longPollS + 10) * 1000UL;

  char line[MAX_LINE];
  if (readLine(g_pollSc, line, sizeof(line), deadline) == 0) { closePollSocket(); return -1; }
  // 409 Conflict = ANOTHER client is long-polling this same bot token (a second
  // Nimbus, or a stray broker). The two then take turns stealing updates - the
  // owner sees replies from alternating devices and commands that "don't work"
  // (live 2026-07-24: /update answered by a different board than the one that
  // sent the update notice). Surface it ONCE per boot instead of failing silently.
  if (strstr(line, " 409")) {
    static bool s_conflictAlerted = false;
    alog("telegram: getUpdates 409 - another device/client polls this bot token");
    if (!s_conflictAlerted) {
      s_conflictAlerted = true;
      String owner = g_allowlist;
      int e = owner.indexOf(','); if (e >= 0) owner = owner.substring(0, e);
      owner.trim();
      if (owner.length())
        send(owner, String(nimbus::sys::deviceName().c_str()) +
                    ": another device appears to be using this same Telegram bot "
                    "token \xE2\x80\x94 replies will alternate between devices. Give each "
                    "device its own bot (web UI \xE2\x86\x92 Capabilities \xE2\x86\x92 Connectors "
                    "\xE2\x86\x92 Telegram).", /*block=*/false);
    }
    closePollSocket();
    return -1;
  }

  long contentLen = -1; bool serverClose = false;
  for (int i = 0; i < 64; i++) {
    int n = readLine(g_pollSc, line, sizeof(line), deadline);
    if (n == 0) break;
    if (!strncasecmp(line, "Content-Length:", 15)) contentLen = atol(line + 15);
    else if (!strncasecmp(line, "Connection:", 11) && strstr(line, "close")) serverClose = true;
  }

  char* body = ensurePollBody();
  if (!body) {   // PSRAM exhausted (should never happen) - drop this poll cycle and
    // retry. No static fallback: the 4 KB internal-SRAM buffer this replaced cost
    // memory permanently for a path that never runs (SRAM reclaim).
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
    return 0;
  }
  int stored;
  if (contentLen >= 0) {
    stored = readBodyN(g_pollSc, body, kPollBodyCap, contentLen, deadline);
    if (stored < 0) { closePollSocket(); return -1; }
  } else {
    readBody(g_pollSc, body, kPollBodyCap, deadline);
    stored = (int)strlen(body);
    serverClose = true;
  }
  g_limitOne = false;   // the body fit - resume normal batching

  // Free the single arena before the orchestrator turn runs (via the callback) -
  // or honor a server close. Idle keeps the socket open (zero re-handshake churn).
  bool hasUpdate = strstr(body, "\"update_id\"") != nullptr;
  if (serverClose || hasUpdate) closePollSocket();
  return processUpdatesBody(body, (size_t)stored, offset);
}

// ---- sendMessage ------------------------------------------------------------

// One raw sendMessage POST. htmlMode adds parse_mode=HTML (the text must then
// already BE converted HTML). Returns Telegram's ok verdict.
static bool doSendMessageRaw(const char* chatId, const char* text, bool htmlMode) {
  closePollSocket();   // free the single arena (never 2 resident TLS sessions)
  WiFiClientSecure sc;
  if (!tlsConnect(sc)) { alog("telegram: send connect fail"); return false; }

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
bool doSendMessage(const char* chatId, const char* text) {
  const std::string html = nimbus::tgHtml(text ? text : "");
  bool ok = doSendMessageRaw(chatId, html.c_str(), /*htmlMode=*/true);
  if (!ok) {
    alogf("telegram: html send rejected - resending plain to %s", chatId);
    ok = doSendMessageRaw(chatId, text, /*htmlMode=*/false);
  }
  alogf("telegram: send %s to %s", ok ? "ok" : "FAIL", chatId);
  return ok;
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

static void handleVoice() {
  if (!g_voice.set) return;
  closePollSocket();
  PendingVoice v = g_voice;
  g_voice.set = false;
  if (!tgPathSafe(v.fileId)) { alog("telegram: voice file_id unsafe - rejected"); return; }

  // If no STT sink is wired, voice is unusable here (bench audio broken + no key):
  // answer with a nudge to send text and skip the download entirely.
  if (!g_stt) {
    doSendMessage(v.chatId, "Voice messages aren't available yet. Send a text message instead.");
    return;
  }

  // 1) Resolve file_path via getFile (one retry on empty).
  char filePath[160] = {};
  for (int t = 0; t < 2; t++) {
    WiFiClientSecure sc;
    if (!tlsConnect(sc)) { alog("telegram: voice getFile connect fail"); return; }
    char req[256];
    snprintf(req, sizeof(req),
      "GET /bot%s/getFile?file_id=%s HTTP/1.0\r\n"
      "Host: " TG_HOST "\r\nConnection: close\r\n\r\n", g_token.c_str(), v.fileId);
    sc.print(req);
    uint32_t deadline = millis() + 12000;
    char line[MAX_LINE]; readLine(sc, line, sizeof(line), deadline);
    skipHeaders(sc, deadline);
    char* resp = ensureApiResp();
    if (!resp) { tlsClose(sc); break; }
    readBody(sc, resp, (int)kTgApiRespCap, deadline);
    tlsClose(sc);
    jsonStr(resp, "file_path", filePath, sizeof(filePath));
    if (filePath[0] == 0) { vTaskDelay(pdMS_TO_TICKS(400)); continue; }
    break;
  }
  if (!tgPathSafe(filePath)) {
    alog("telegram: voice file_path missing/unsafe - rejected");
    doSendMessage(v.chatId, "Couldn't receive that voice note. Send a text message instead.");
    return;
  }

  // 2) Download the audio to LittleFS (cap 64 KB; one retry on short download).
  const char* localPath = "/audio/tgvoice.ogg";
  // LittleFS refuses to open a file for write in a nonexistent directory, and
  // nothing else creates /audio - on a fresh filesystem EVERY voice note died
  // here with a silent "voice fs open fail" (field bug, Board 3 2026-07-21;
  // Board 1 only worked because legacy firmware had left the dir behind).
  if (!LittleFS.exists("/audio")) LittleFS.mkdir("/audio");
  size_t total = 0;
  for (int t = 0; t < 2; t++) {
    total = 0;
    WiFiClientSecure sc;
    if (!tlsConnect(sc)) {
      alog("telegram: voice dl connect fail");
      doSendMessage(v.chatId, "Couldn't download your voice note - please try again.");
      return;
    }
    char req[320];
    snprintf(req, sizeof(req),
      "GET /file/bot%s/%s HTTP/1.0\r\n"
      "Host: " TG_HOST "\r\nConnection: close\r\n\r\n", g_token.c_str(), filePath);
    sc.print(req);
    // 45 s window + a 1 MB cap. The old 64 KB cap silently kept only the FIRST
    // ~8-30 s of opus audio (bitrate-dependent), so long voice notes came back
    // as short partial transcripts with no error anywhere (owner-hit 2026-07-22).
    // 1 MB covers ~2 min even at high-bitrate opus; the file is deleted right
    // after STT, and LittleFS keeps a comfortable margin (~3 MB free in practice).
    uint32_t deadline = millis() + 45000;
    char line[MAX_LINE]; readLine(sc, line, sizeof(line), deadline);
    skipHeaders(sc, deadline);
    File f = LittleFS.open(localPath, FILE_WRITE);
    if (!f) {
      tlsClose(sc);
      alog("telegram: voice fs open fail");
      // Never a silent drop: the owner is waiting on a reply to their voice note.
      doSendMessage(v.chatId, "I couldn't save your voice note (storage hiccup) - please try again or send text.");
      return;
    }
    uint8_t buf[512];
    bool writeFail = false;
    while ((int32_t)(millis() - deadline) < 0 && total < 1048576) {
      if (sc.available()) {
        int r = sc.read(buf, sizeof(buf));
        if (r > 0) {
          // f.write on a full/failing FS returns short - count what LANDED, and
          // stop instead of silently transcribing a truncated file.
          if ((int)f.write(buf, r) != r) { writeFail = true; break; }
          total += r;
        }
      }
      else if (!sc.connected()) break;
      else delay(2);
    }
    f.close();
    tlsClose(sc);
    if (writeFail) {
      LittleFS.remove(localPath);
      alog("telegram: voice fs write fail (storage full?)");
      doSendMessage(v.chatId, "I couldn't save your voice note (storage full) - please try again or send text.");
      return;
    }
    if (total < 512) { vTaskDelay(pdMS_TO_TICKS(400)); continue; }
    break;
  }
  alogf("telegram: voice downloaded %u bytes", (unsigned)total);
  if (total == 0) { doSendMessage(v.chatId, "Voice note was empty."); return; }

  // 3) STT via the injected sink, then route the transcript like a text message.
  //    BENCH-BROKEN here: no sink is installed on the bench (broken audio + no
  //    Mistral key), so this path is compile-verified only, never run live.
  String transcript = g_stt(localPath, "audio/ogg");
  if (transcript.length() == 0) {           // one retry: STT flakes are usually a
    vTaskDelay(pdMS_TO_TICKS(1200));        // transient TLS/provider hiccup (owner
    transcript = g_stt(localPath, "audio/ogg");   // R5c: never silently ignore voice)
  }
  // Durable audit copy of the inbound voice note -> /mem/blobs on the SD card, BEFORE
  // the privacy-delete of the LittleFS working file (docs/orchestrator-storage.md §4).
  // SD-gated no-op: with no card, nothing is retained (unchanged privacy posture).
  agent::memory::captureMediaFile(v.chatId, "user", nimbus::orch::MsgKind::Audio,
                                  transcript, localPath, "ogg");
  LittleFS.remove(localPath);   // privacy: drop the LittleFS working copy
  if (transcript.length() == 0) {
    doSendMessage(v.chatId, "Couldn't transcribe that - please send text.");
    return;
  }
  alogf("telegram: voice -> \"%.40s\"", transcript.c_str());
  String echo = String("\xF0\x9F\x8E\xA4 ") + transcript;   // 🎤 echo what we heard
  doSendMessage(v.chatId, echo.c_str());
  if (g_cb) g_cb(String(v.from), String(v.chatId), transcript);
}

// ---- inbound attachments (photos + documents) -------------------------------
// Deferred exactly like a voice note: download after the poll socket closes, so
// only one TLS session is ever resident.
//
// ⚠ SD-GATED, by the owner's decision. Media on internal flash would compete
// with the firmware's own OTA slots for a 3.5 MB partition, and a single phone
// photo can be a sixth of it. With no card the device says so plainly instead of
// half-accepting a file it cannot keep.
//
// What enters the CONVERSATION is never the bytes. A photo is described once on
// arrival and the description is what the model reads - a few hundred characters
// that compact, summarize and re-read like any other text, instead of a blob
// that would dominate the context window for as long as it stayed in it. The
// original stays on the card, so "that photo I sent you on Tuesday" is
// answerable by looking again.
static void handleAttachment() {
  if (!g_attach.set) return;
  closePollSocket();
  PendingAttach a = g_attach;
  g_attach.set = false;
  if (!tgPathSafe(a.fileId)) { alog("telegram: attachment file_id unsafe - rejected"); return; }

  if (!agent::memory::haveSd()) {
    doSendMessage(a.chatId,
                  "I can't keep files without an SD card. Tell me what's in it and "
                  "I'll remember that instead.");
    return;
  }
  if (a.size && a.size > nimbus::tg::kPhotoBudget) {
    doSendMessage(a.chatId, "That file is too large for me to take. Send a smaller one.");
    return;
  }
  // The sender's storage limit, checked BEFORE the download. A quota enforced
  // only after the bytes have landed lets every first offender through, and on
  // this device "after" also means a minute of radio and a megabyte of flash
  // already spent. Telegram's reported size is a hint that can read low, so the
  // real size is re-checked below once the file is on disk.
  const nimbus::orch::Principal who = nimbus::orch::principalForRole(
      std::string(a.chatId), orchestrator::roleOfChat(String(a.chatId)),
      [&] { nimbus::orch::Quota q;
            orchestrator::tenantQuotaOf(std::string(a.chatId), q); return q; }());
  if (files::wouldExceedQuota(who, a.size ? a.size : 1)) {
    doSendMessage(a.chatId,
                  "You've used up your file storage on this device. Ask the owner to "
                  "raise it, or delete something first.");
    return;
  }

  // 1) Resolve file_path via getFile (one retry on empty) - same shape as voice.
  char filePath[160] = {};
  for (int t = 0; t < 2; t++) {
    WiFiClientSecure sc;
    if (!tlsConnect(sc)) { alog("telegram: attach getFile connect fail"); return; }
    char req[256];
    snprintf(req, sizeof(req),
      "GET /bot%s/getFile?file_id=%s HTTP/1.0\r\n"
      "Host: " TG_HOST "\r\nConnection: close\r\n\r\n", g_token.c_str(), a.fileId);
    sc.print(req);
    uint32_t deadline = millis() + 12000;
    char line[MAX_LINE]; readLine(sc, line, sizeof(line), deadline);
    skipHeaders(sc, deadline);
    char* resp = ensureApiResp();
    if (!resp) { tlsClose(sc); break; }
    readBody(sc, resp, (int)kTgApiRespCap, deadline);
    tlsClose(sc);
    jsonStr(resp, "file_path", filePath, sizeof(filePath));
    if (filePath[0] == 0) { vTaskDelay(pdMS_TO_TICKS(400)); continue; }
    break;
  }
  if (!tgPathSafe(filePath)) {
    alog("telegram: attach file_path missing/unsafe - rejected");
    doSendMessage(a.chatId, "Couldn't fetch that file. Try sending it again.");
    return;
  }

  // 2) Download to a LittleFS staging file. It is deleted as soon as the durable
  //    copy is on the card, so a big photo never occupies flash for long.
  const char* localPath = "/audio/tgattach.bin";
  if (!LittleFS.exists("/audio")) LittleFS.mkdir("/audio");
  size_t total = 0;
  {
    WiFiClientSecure sc;
    if (!tlsConnect(sc)) {
      alog("telegram: attach dl connect fail");
      doSendMessage(a.chatId, "Couldn't download that file - please try again.");
      return;
    }
    char req[320];
    snprintf(req, sizeof(req),
      "GET /file/bot%s/%s HTTP/1.0\r\n"
      "Host: " TG_HOST "\r\nConnection: close\r\n\r\n", g_token.c_str(), filePath);
    sc.print(req);
    uint32_t deadline = millis() + 60000;
    char line[MAX_LINE]; readLine(sc, line, sizeof(line), deadline);
    skipHeaders(sc, deadline);
    File f = LittleFS.open(localPath, FILE_WRITE);
    if (!f) {
      tlsClose(sc);
      alog("telegram: attach fs open fail");
      doSendMessage(a.chatId, "I couldn't save that file (storage hiccup) - please try again.");
      return;
    }
    uint8_t buf[512];
    bool writeFail = false;
    while ((int32_t)(millis() - deadline) < 0 && total < nimbus::tg::kPhotoBudget) {
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
    // Hitting the cap means the file was TRUNCATED, not finished. Telegram may
    // omit file_size, so the pre-check above cannot always catch an oversized
    // file - and storing a torn JPEG durably, then failing to describe it, is
    // the worst of both outcomes.
    if (writeFail || total == 0 || total >= nimbus::tg::kPhotoBudget) {
      LittleFS.remove(localPath);
      alogf("telegram: attach download failed (%u bytes)", (unsigned)total);
      doSendMessage(a.chatId, total >= nimbus::tg::kPhotoBudget
          ? "That file is too large for me to take. Send a smaller one."
          : "I couldn't save that file - please try again.");
      return;
    }
  }
  alogf("telegram: attachment downloaded %u bytes", (unsigned)total);
  // Telegram's size hint can read low; the real size is authoritative.
  if (files::wouldExceedQuota(who, (uint32_t)total)) {
    LittleFS.remove(localPath);
    doSendMessage(a.chatId,
                  "That file would put you over your storage limit. Ask the owner to "
                  "raise it, or delete something first.");
    return;
  }

  // 3) A photo becomes a description; a document becomes a note that it arrived.
  //    Vision is best-effort: if no provider can look at it, the photo is still
  //    kept and acknowledged - the conversation just does not gain a description.
  const char* ext = a.photo ? "jpg" : "bin";
  String description;
  if (a.photo && agent::vision::available()) {
    description = agent::vision::describeImage(localPath, a.mime[0] ? a.mime : "image/jpeg",
                                               nullptr, String(a.caption));
  }

  const String label = a.photo
      ? (description.length() ? String("[image] ") + description
                              : String("[image] (couldn't be described)"))
      : String("[file] ") + (a.fileName[0] ? a.fileName : "attachment");

  // Store it through the FILE STORE, not straight into a blob directory. That
  // is what makes it count against the sender's storage limit (the earlier
  // version measured a quota over an index the media never entered, so the gate
  // was inert), and it also means a photo someone sent shows up in Memory &
  // Files like any other file, and can be shared or deleted from there.
  {
    char fname[80];
    const char* base = a.fileName[0] ? a.fileName : nullptr;
    if (base) {
      snprintf(fname, sizeof(fname), "%s", base);
    } else {
      // Photos carry no filename. Name it by arrival so a chat's images sort
      // sensibly and two photos in one second cannot collide.
      snprintf(fname, sizeof(fname), "photo-%lu.%s",
               (unsigned long)(millis() / 1000), ext);
    }
    // One project per sender keeps a chat's files together and readable at a
    // glance in the web UI.
    char proj[48];
    snprintf(proj, sizeof(proj), "chat-%s", a.chatId);
    std::string ferr;
    if (!agent::files::saveStream(proj, fname, LittleFS, localPath, ferr, who.ns)) {
      LittleFS.remove(localPath);
      alogf("telegram: attach store failed: %s", ferr.c_str());
      doSendMessage(a.chatId, "I couldn't keep that file - please try again.");
      return;
    }
  }
  // The episodic row is what the MODEL reads later; the blob sidecar is what it
  // can be re-shown from.
  agent::memory::captureMediaFile(a.chatId, "user",
                                  a.photo ? nimbus::orch::MsgKind::Image
                                          : nimbus::orch::MsgKind::File,
                                  label, localPath, ext);
  LittleFS.remove(localPath);

  // 4) Route it as this chat's next turn, so the assistant answers the picture
  //    the same way it answers a sentence. The caption rides along - the sender's
  //    own words are the question they are actually asking about the image.
  String turn = label;
  if (a.caption[0]) turn += String("\n\nThey wrote: ") + a.caption;
  if (g_cb) g_cb(String(a.from), String(a.chatId), turn);
  else doSendMessage(a.chatId, a.photo ? "Got the photo." : "Got the file.");
}

// ---- media send (device -> owner; drained on the poll task) -----------------
// Map a kind to the Bot API method + form field + a filename/mime. Photos are sent
// as JPEG/PNG; documents keep their extension; voice must be OGG/Opus.
static void drainMedia() {
  PendingMedia m;
  portENTER_CRITICAL(&g_mediaMux);
  if (!g_media.set) { portEXIT_CRITICAL(&g_mediaMux); return; }
  m = g_media;
  g_media.set = false;
  portEXIT_CRITICAL(&g_mediaMux);
  closePollSocket();   // single-TLS arena
  if (g_token.length() == 0) return;

  const char* method = "sendDocument";
  const char* field  = "document";
  const char* mime   = "application/octet-stream";
  if (!strcmp(m.kind, "photo")) { method = "sendPhoto"; field = "photo"; mime = "image/jpeg"; }
  else if (!strcmp(m.kind, "voice")) { method = "sendVoice"; field = "voice"; mime = "audio/ogg"; }
  else if (!strcmp(m.kind, "audio")) { method = "sendAudio"; field = "audio"; mime = "audio/mpeg"; }

  const char* base = strrchr(m.path, '/');
  base = base ? base + 1 : m.path;

  char path[128];
  snprintf(path, sizeof(path), "/bot%s/%s", g_token.c_str(), method);
  std::vector<httpmp::Field> fields = {{"chat_id", m.chatId}};
  if (m.caption[0]) fields.push_back({"caption", m.caption});
  String resp, err;
  bool ok = httpmp::post(TG_HOST, TG_PORT, path, "", fields, field, base, mime, m.path, resp, err,
                         m.sd ? &agent::memory::dataFs() : nullptr,    // E1: SD artifacts
                         /*lockSrc=*/m.sd);                            // serialize SD reads (tg_poll)
  alogf("telegram: %s %s (%s)", method, ok ? "ok" : "FAIL", ok ? "" : err.c_str());
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

  int32_t offset = store::telegramOffset();
  if (haveToken) {
    alogf("telegram: poll start (offset=%ld) heap=%u", (long)offset, ESP.getFreeHeap());
    // SECURITY: warn loudly if a token is set but the allowlist is empty - allowed()
    // now fails CLOSED (rejects all chats), so the bot is inert until an allowlist is set.
    if (store::telegramAllowlist().length() == 0)
      alog("telegram: SECURITY - allowlist EMPTY, rejecting ALL chats; set an allowlist to use the bot");
  } else {
    alog("telegram: no token - local turn processor only (console/web injects + job polling)");
  }

  int activeJobs = 0;
  while (g_running) {
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
    // (the "changed the token but still can't communicate" failure). The old
    // 409-Conflict fight with the other device dies with the socket.
    if (s_tokenSwap) {
      s_tokenSwap = false;
      closePollSocket();
      g_token = s_newToken;
      haveToken = g_token.length() > 0;
      store::setTelegramOffset(0);
      offset = 0;
      g_polledOk = false;
      g_pollFails = 0;
      alogf("telegram: token swapped live - %s (offset reset)",
            haveToken ? "polling the new bot" : "token cleared, poll idle");
    }

    // Telegram long-poll: ONLY with a token. Without one the task still runs so
    // console/web-injected turns and background job polling execute off the main
    // loop (no watchdog reboot on a multi-second turn).
    if (haveToken) {
      // Shorten the long-poll while jobs run so the loop cycles + delivers results,
      // but not too short (each cycle's TLS churn drains heap on the no-PSRAM board).
      int longPollS = (activeJobs > 0) ? 18 : TELEGRAM_LONG_POLL_TIMEOUT_S;
      int n = doGetUpdates(offset, longPollS);
      if (n < 0) {
        g_pollFails++;
        uint32_t backoff = TG_BACKOFF_STEP_MS *
                           (g_pollFails < TG_BACKOFF_MAX_STEPS ? g_pollFails : TG_BACKOFF_MAX_STEPS);
        alogf("telegram: poll error (%u), backoff %ums", g_pollFails, backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff));
      } else {
        g_pollFails = 0;
        if (!g_polledOk) { g_polledOk = true; alog("telegram: poll ok"); }
      }
      offset = store::telegramOffset();

      // Free the single arena before ANY non-poll TLS this cycle: a voice note,
      // active-job polling, a queued reply, or an injected message.
      if (g_pollScOpen && (g_voice.set || g_activeJobs > 0 ||
                           (g_replyQ   && uxQueueMessagesWaiting(g_replyQ)   > 0) ||
                           (g_inboundQ && uxQueueMessagesWaiting(g_inboundQ) > 0)))
        closePollSocket();
    }

    // Dispatch injected inbound messages (console TURN / web / voice) - ALWAYS.
    if (g_inboundQ && g_cb) {
      // The 4 KB text buffer (grew from 1 KB with the full-length inbound fix) must
      // not sit on the tg_poll stack across the whole synchronous turn that g_cb
      // runs. This drain is single-consumer (tg_poll only), so ONE staging slot is
      // safe - and it lives in PSRAM (N7), off the scarce internal heap, not on the
      // stack. A null (no PSRAM) leaves messages queued for the next cycle.
      InboundMsg* im = ensureInboundStage();
      while (im && xQueueReceive(g_inboundQ, im, 0) == pdTRUE)
        g_cb(String(im->from[0] ? im->from : im->chatId), String(im->chatId), String(im->text));
    }

    if (haveToken) handleVoice();   // voice notes arrive via Telegram; own TLS
    if (haveToken) handleAttachment();   // photos/documents; own TLS too
    if (haveToken) drainMedia();    // outbound media (device -> owner); own TLS

    // Advance background jobs (orchestrator::pollJobs) - synchronous, this task.
    // This is where a turn (incl. the full tool loop) runs, so it is the deepest
    // this stack ever gets. Sample the high-water AFTER it and surface the WORST
    // seen, so the stack size is set from real peaks, not a guess - and so a stack
    // creeping toward the canary is visible long before it overflows (that
    // overflow is the recurring crash(panic); see POLL_STACK_BYTES).
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

    vTaskDelay(pdMS_TO_TICKS(activeJobs > 0 ? 600 : TELEGRAM_POLL_INTERVAL_MS));
  }
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

bool sendMedia(const String& chatId, const char* kind, const char* filePath,
               const String& caption) {
  return publishMedia(chatId, kind, filePath, caption, /*sd=*/false, /*refuseIfBusy=*/false);
}

bool sendMediaSd(const String& chatId, const char* kind, const char* filePath,
                 const String& caption) {
  return publishMedia(chatId, kind, filePath, caption, /*sd=*/true, /*refuseIfBusy=*/true);
}

bool injectMessage(const String& chatId, const String& text) {
  if (!g_inboundQ) return false;
  InboundMsg im{};
  chatId.toCharArray(im.chatId, sizeof(im.chatId));
  // For pseudo-channels the sender IS the channel ("web"/"voice"/"serial") - the
  // drain forwards it as `from`, so the episodic capture labels the origin honestly.
  chatId.toCharArray(im.from, sizeof(im.from));
  text.toCharArray(im.text, sizeof(im.text));
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
    int e = g_allowlist.indexOf(',');
    String firstId = (e < 0) ? g_allowlist : g_allowlist.substring(0, e);
    firstId.trim();
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

// Approve a pending sender: append to the allowlist NVS (+ display name), drop it
// from the pending ring, and hot-reload. Idempotent.
bool approvePending(const String& chatId, const String& name) {
  if (chatId.length() == 0) return false;
  String al = store::telegramAllowlist();
  // already present?
  bool present = false;
  { int s = 0; while (s < (int)al.length()) { int e = al.indexOf(',', s); if (e < 0) e = al.length();
      String t = al.substring(s, e); t.trim(); if (t == chatId) { present = true; break; } s = e + 1; } }
  if (!present) al = al.length() ? (al + "," + chatId) : chatId;
  store::setTelegramAllowlist(al);
  if (name.length()) {   // display sidecar "id:name,..."
    // The name is the sender's ATTACKER-CONTROLLED Telegram display name - strip
    // the ',' and ':' delimiters so it can't corrupt/spoof the "id:name,..."
    // parsing (prism; display-only, but keep it clean).
    String safe = name;
    safe.replace(",", " "); safe.replace(":", " ");
    if (safe.length() > 32) safe = safe.substring(0, 32);
    // Upsert, not append: the old blind append meant a re-approve with a new name
    // never renamed (the reader matched the first entry) and the blob grew forever.
    store::replaceTelegramName(chatId, safe);
  }
  pendingRemove(chatId.c_str());
  reloadAllowlist();
  return true;
}

void denyPending(const String& chatId) { pendingRemove(chatId.c_str()); }   // session tombstone

bool send(const String& chatId, const String& text, bool block) {
  if (!g_replyQ) return false;
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

uint32_t consecutiveFails() { return g_pollFails; }
int      activeJobCount()   { return g_activeJobs; }

}  // namespace telegram
}  // namespace agent
