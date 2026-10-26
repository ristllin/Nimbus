#pragma once
#include <Arduino.h>


// telegram - Telegram Bot API long-poll client for Nimbus, ported from
// Nuage-Solide src/telegram_client.{h,cpp} (Head Orchestrator v2).
//
// Runs a background FreeRTOS task (tg_poll, core 0, 24 KB stack - the mbedTLS
// handshake plus the whole tool loop; see POLL_STACK_BYTES) that long-polls getUpdates():
// each call blocks server-side up to ~18-30 s (shorter while jobs run), then a
// short inter-cycle delay before the next poll. Only one resident TLS session is
// open at a time (the persistent keep-alive poll socket), torn down before ANY
// other TLS (a turn / a reply send / a voice download / job polling). Invokes the
// MessageCallback for each incoming message that passes the allowlist.
//
// Nimbus wiring (plan §3.6): begin(token, allowlist, handleMessage) +
// setTick(pollJobs). send() is thread-safe (queue). handleMessage runs the
// orchestrator turn INSIDE the poll task (Telegram TLS closed) so only one TLS
// session is ever open.
//
// LIVE-GATED: dead without a Telegram bot token + allowlist (web UI / NVS) and
// provisioned STA WiFi. VOICE is additionally BENCH-BROKEN + key-gated - see
// setSttSink() and the note in telegram.cpp.

namespace agent {
namespace telegram {

// from = sender display name, chatId = int64 as string, text = message text.
using MessageCallback = void (*)(const String& from, const String& chatId, const String& text);

// Called once per poll cycle (in the poll task, Telegram TLS closed) so the
// orchestrator can advance background jobs without its own task/stack. Returns the
// active job count so the poll loop can shorten its long-poll while jobs run.
using TickCallback = int (*)();

// STT seam (LIVE-GATED + BENCH-BROKEN): transcribe a downloaded voice note file.
// Nuage used audio::sttFromFile (Mistral STT). Nimbus injects the impl so the
// Telegram port is compile-clean with no audio dependency; unset => voice notes
// are answered "please send text". Signature: (localPath, mime) -> transcript ("").
using SttSink = String (*)(const char* localPath, const char* mime);

// Inbound-file seam (files lane): installed to capture Telegram documents/photos.
// Given the attachment metadata, returns true if it handled storage (owns the
// download); false/unset => ingress sends a deterministic "can't store files yet"
// reply instead of silently dropping. kind: 1=document, 2=photo.
using AttachmentSink = bool (*)(const String& chatId, const String& from,
                                const String& fileId, const String& fileName,
                                const String& mime, uint32_t fileSize,
                                int kind, const String& caption);

void begin(const String& token, const String& allowlist, MessageCallback cb);
void setTick(TickCallback cb);
void setSttSink(SttSink cb);   // optional; voice is unverified here
void setAttachmentSink(AttachmentSink cb);   // optional; files lane plugs in here
// This boot's esp_reset_reason(), set by main.cpp before begin(): the poison guard
// counts a panic, watchdog or brownout reset against a page whose processing it cut
// short, and a power-on clears its record (nimbus/net/tg_dup_guard.h).
void setResetReason(int reason);

#if defined(NIMBUS_TEST)
// Test seam (the test build's TGINJECT console verb): queue ONE raw getUpdates
// response body to run through the normal receive path on the poll task - the same
// parser, the archive hook, the duplicate breaker and the hourly ceiling, the
// allowlist, classification, the batcher with its command and voice boundaries, the
// media processing inside each turn, and one turn per bucket - exactly as a polled
// page, except that it never moves the getUpdates offset (its ids are not Telegram's)
// and runs at once. A body that opens with {"ok":true,"settle":true, is a settle page:
// it is held by the settle rule like a polled page, and later settle pages join it (up
// to 4) until the rule dispatches them as one page. Returns false when a page is
// already waiting (a settle page: while a plain page runs, while held pages run, or
// with 4 held), the body is empty or over 64 KB, or PSRAM is short.
// Test builds only: an injected page can name any chat, the owner's included.
bool injectRawUpdates(const char* body, size_t len);
// TGINJECT (the console verb): the same seam, returning the console's one-line
// summary. The page also passes the Telegram archive hook once; a page that stops on a
// cap (more media than one drain's slots) resumes where it stopped on the next cycle.
// Media whose file_id is "LOCAL_<path>" is read from that file on the card instead of
// fetched.
String injectRawPage(const String& body);
#endif

// The chat the device's own notices go to: the first explicit owner that is still on
// the allowlist, else the first allowlist entry; "" when there is none (one rule for
// every notice, nimbus/net/tg_owner.h). Call it on the poll task, like isOwner().
String ownerChat();

// The media of the turn the MessageCallback is running, in the order they were sent:
// the ordered segment list the image path uses (each photo can go to the head as an
// image part, in this order, beside the text the turn carries). Valid only inside the
// callback, on the poll task; empty for a turn with no photo or file.
struct TurnMedia {
  int     k;          // 1-based place among the turn's media (the label's k)
  int     n;          // the turn's media count (the label's N)
  bool    photo;      // false: a document
  bool    fetched;    // false: it could not be fetched (its label says why)
  char    ref[17];    // the content hash (16 hex; the blob sidecar's name), "" when not fetched
  char    mime[48];   // Telegram's MIME type ("" when absent; a photo is image/jpeg)
  int64_t sentAt;     // Telegram's send time
  bool    kept;       // its media row was stored: the turn's history row names it by ref
};
size_t turnMediaCount();
const TurnMedia* turnMediaAt(size_t i);

// The inbound storm breakers since the record was last cleared (it survives restarts,
// not a power cycle): duplicates dropped, turns skipped at the hourly ceiling, pages
// acked unprocessed by the poison guard, and the crash resets counted against the page
// in flight. Published by the poll task; any task may read it.
struct GuardStatus {
  uint32_t dupDrops;
  uint32_t ceilingDrops;
  uint32_t poisonAcks;
  uint8_t  tries;
  bool     holding;   // a page is being held for the rest of an album or burst, or for
                      // the photo sent after a text
  bool     booted;    // the record was read this boot (false: Notifier mode, no counts)
  bool     kept;      // this boot found the record intact (false: rebuilt, a fresh count)
};
GuardStatus guardStatus();

// Inject a message as if it arrived from Telegram - enqueued + dispatched by the
// poll task (correct stack + TLS context). Used by the web test endpoint.
// Queue a message as if it arrived on `chatId` (the web/serial/voice pseudo-
// channels and the test seams). Returns FALSE when the inbound queue is full -
// the caller must tell the sender rather than let the message vanish.
bool injectMessage(const String& chatId, const String& text);
bool enabled();

// Live token swap: adopt a NEW bot token without a reboot. Staged here (any task)
// and applied on the poll task's next loop - it closes the old bot's long-poll
// socket and RESETS the getUpdates offset (the stored offset belongs to the old
// bot; reusing it on a fresh bot silently skips messages). "" clears the token
// (poll idles). The caller persists the token to NVS separately (store::).
void applyToken(const String& token);

// P8: live allowlist management + first-message approval (the web surface).
String pendingJson();                          // [{chatId,name,preview}] awaiting approval
bool   isAllowed(const String& chatId);        // is a chat currently allowed?
bool   isOwner(const String& chatId);          // is a chat an OWNER (settings/OTA) vs member (chat only)
void   reloadAllowlist();                      // re-read allowlist + owners + public flag (no reboot)
// Worst (smallest) free stack the poll task - which runs the whole turn + tool
// loop - has ever had, in bytes; UINT32_MAX until the first cycle. Surfaced in
// /api/state (mem.pollStackMin) so a stack creeping toward a canary overflow (the
// recurring crash(panic)) is visible before it happens.
uint32_t pollStackMinFree();
bool   pollEverOk();   // a getUpdates poll has succeeded since boot (the relay's first dial waits on it)
bool   approvePending(const String& chatId, const String& name);  // allow + drop from pending
void   denyPending(const String& chatId);      // drop from pending (session tombstone)
// Enqueue a reply for the poll task to deliver. Blocks up to 100 ms when the
// reply queue is full so an orchestrator turn doesn't silently drop a reply.
// Pass block=false for best-effort, latency-sensitive callers (e.g. the main
// render loop's low-battery ping) that must never stall on a full queue.
bool send(const String& chatId, const String& text, bool block = true);

// Send a media file (device -> owner) from LittleFS. kind = "document" | "photo" |
// "voice"; the send is queued and delivered on the poll task (single-TLS safe).
// Returns false if no token is set or kind is unknown. One outstanding send at a
// time (a new call overwrites a still-pending one).
bool sendMedia(const String& chatId, const char* kind, const char* filePath,
               const String& caption = "");
// E1 artifact variant: streams from the SD data store (memory::dataFs()) instead
// of LittleFS, and REFUSES (returns false) when a send is already pending rather
// than overwriting it - an artifact delivery must never be silently dropped.
bool sendMediaSd(const String& chatId, const char* kind, const char* filePath,
                 const String& caption = "");

// A media upload that runs NOW and waits for Telegram's answer: the voice reply path,
// where the tool must say whether the message arrived. Runs only on the poll task
// (every turn runs there, and it owns the TLS arena between its own requests); from any
// other task it refuses. `messageId` is Telegram's id for the delivered message (> 0
// only when ok); `error` says why nothing arrived. A delivered file is archived before
// this returns, so the caller may delete it afterwards. LittleFS files only.
struct MediaReceipt {
  bool     ok = false;
  int64_t  messageId = 0;
  size_t   bytes = 0;       // the uploaded file's size
  String   error;
};
struct MediaOptions {
  const char* caption = nullptr;
  const char* uploadName = nullptr;   // the file name the chat shows (nullptr: the file's own)
  const char* title = nullptr;        // an audio's title in the chat
};
MediaReceipt sendMediaNow(const String& chatId, const char* kind, const char* filePath,
                          const MediaOptions& opt = MediaOptions());
// True when the caller runs on the poll task (the turn task).
bool onTurnTask();
void stop();

// Factory-reset quiesce (release-gate finding 2026-10-04): signal the poll task
// to exit and WAIT for it to self-delete, deleting NOTHING out from under it
// (stop() while the task lives crashes on the freed queue - see stop()'s
// warning). Bounded: a turn mid-provider-call can hold the task past the
// timeout; the caller then proceeds behind the erase barriers instead of
// waiting out a TLS exchange. Returns true when the task is gone.
bool quiesce(uint32_t timeoutMs);

// True while the poll task exists. Unlike enabled(), a bot token is not required:
// the task is also the local turn processor (web / voice / serial injects).
bool taskAlive();

// Work another module stages for this task to run BETWEEN its own work: an OTA
// check or install whose own task did not fit (src/sys/ota_update, wired by main.cpp
// before begin()). `pending` is cheap and safe from any task; `run` runs on this task,
// at its loop top, never inside a turn (the full rule at the call site in pollTask).
// nullptr = none (the default).
void setStagedWork(bool (*pending)(), void (*run)());

// self-heal hooks (a future net-recovery ladder can read these).
uint32_t consecutiveFails();
int      activeJobCount();

// Poll health for /api/state, published by the poll task each cycle: the getUpdates
// offset cursor (nimbus::core::TgOffsetCursor: RAM is the truth while the device
// runs, NVS keeps a copy for restarts) and the getUpdates backoff.
struct PollStatus {
  bool     offsetSaved;        // the stored offset matches RAM (false while a write is owed)
  uint32_t offsetWrites;       // offset write attempts since boot: one per new page,
                               // and at most one retry a minute while a write is owed
  uint32_t offsetWriteFails;   // of those, the ones that did not land
  uint32_t staleDrops;         // updates below the cursor that were dropped (expected 0)
  uint32_t backoffMs;          // time left before the next getUpdates may go out
};
PollStatus pollStatus();

// CUM-308: the debounced "the bot token was rejected" verdict. A token verified
// once (getMe) can be revoked later while the poll loop keeps running - Telegram
// then 401s every getUpdates while the cached verify result still reads
// "verified". This returns true only after a run of consecutive poll auth
// failures (401/403); a 409 conflict (another poller) and transient network
// errors never set it, and the next successful poll clears it. The web
// (/api/orch tgAuthFail, and tgVerify) and Health surfaces read this so a
// revoked token stops reporting "verified/live".
bool authRejected();

}  // namespace telegram
}  // namespace agent
