#pragma once
#include <Arduino.h>

#include "nimbus/tg_archive.h"   // nimbus::tgarc::Fs - the card seam the web routes read through

// tg_archive - device seam for the Telegram archive on the SD card (the portable
// engine, its names and its policy are nimbus/tg_archive.h, host-tested).
//
// Owner ask: everything exchanged with the device over Telegram is kept on the
// device. Inbound updates are written as they arrived, before classification;
// media is fetched at full size with a streamed download that writes the card in
// slices; outbound messages are written whole, before splitting. SD only: with no
// card (or while it is demoted, or during a factory erase) every hook is a no-op
// and the archive reports itself unavailable.
//
// Threading: inbound capture, downloads and the resume and pressure passes run on
// the Telegram task (tg_poll), one TLS session at a time, after the poll socket is
// closed. An outbound hook called from another task (the main loop, the web task)
// is handed over through a fixed ring of pointers and written by the next tick on
// tg_poll, so no caller ever waits on the card or on a download; the engine itself is
// touched by the Telegram task only (the web task reads a copy of its counters).
// Card I/O is serialized by memory::Lock, held per operation only, never across a
// network read.
namespace agent {
namespace tgarchive {

// The raw body of one getUpdates page, called as soon as it is read, before the turn
// path parses, classifies, batches or deduplicates anything (telegram.cpp
// fetchOnePage). `token` is the bot token in use: it is never written to the card.
void onInboundPage(const char* body, size_t len, const String& token);

// Text about to go to a Telegram chat, before any splitting. origin: "send" for the
// queued path (replies, asks, routine output, notices, sub-agent results) or
// "direct" for the Telegram task's own short notices.
void onOutboundText(const String& chatId, const String& text, const char* origin);

// A media file about to go to a Telegram chat (drainMedia). sd: the file is on the
// SD data store, else on LittleFS.
void onOutboundMedia(const String& chatId, const char* kind, const char* srcPath, bool sd,
                     const String& caption);

// The restore tool's upload (web task): a file already written to the card at
// /mem/tg/<chat>/<day>/<file>. Its index line is written by the next tick. False
// when the hand-over queue was full (the upload then answers 429 and is retried).
bool onRestored(const nimbus::tgarc::Restored& r);

// For the restore upload (web task): whether the hand-over queue can take one more
// index line (when not, the upload answers 429 and the tool retries), and whether an
// upload is open (the storage-pressure pass waits for it).
bool queueHasRoom();
void noteImport(bool open);

#ifdef NIMBUS_TEST
// TGINJECT media (test builds only): when `fileId` is "LOCAL_/mem/<path>", copy that
// card file to `lfsPath` on LittleFS (the turn path's staging file) and return true,
// with `total` the bytes copied (0 when missing or a write failed; the file size
// with nothing copied when it is `cap` or more). False for any other file id.
bool copyLocalInject(const char* fileId, const char* lfsPath, size_t& total, size_t cap);
#endif

// The Telegram task's turn: write queued outbound items, run the resume pass once
// per boot, check storage pressure, and fetch pending media for up to a few seconds
// with the given bot token. `yield` (may be null) ends a long download early so a
// waiting local turn or a factory-reset quiesce is not held up; a cut transfer
// resumes on a later tick. `beforeTls` (may be null) runs before the first TLS
// connection, so the caller can close its own socket first.
void tick(const String& token, bool (*yield)(), void (*beforeTls)());

// Archive state for GET /api/tg/archive: availability plus the counters.
bool   available();
String statsJson();
// The card seam for readers on any task (each operation takes memory::Lock). Writes
// through it refuse while a factory erase runs or the card is gone.
nimbus::tgarc::Fs& cardFs();

}  // namespace tgarchive
}  // namespace agent
