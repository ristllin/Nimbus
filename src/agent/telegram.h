#pragma once
#include <Arduino.h>

// telegram - Telegram Bot API long-poll client for Nimbus, ported from
// Nuage-Solide src/telegram_client.{h,cpp} (Head Orchestrator v2).
//
// Runs a background FreeRTOS task (tg_poll, core 0, 16 KB stack - the mbedTLS
// handshake needs it; 12 KB overflowed the canary) that long-polls getUpdates():
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
void stop();

// self-heal hooks (a future net-recovery ladder can read these).
uint32_t consecutiveFails();
int      activeJobCount();

}  // namespace telegram
}  // namespace agent
