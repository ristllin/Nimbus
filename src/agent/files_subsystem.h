#pragma once
#include <Arduino.h>

#include <FS.h>                            // fs::FS - the streaming save source
#include "nimbus/orch/tool_registry.h"   // Principal - the per-caller data boundary

#include <string>

// files_subsystem - device glue for E1 "Files + artifacts" (docs/ROADMAP.md).
//
// The SD card's /mem/files/<project>/<name> tree is the owner's PRIVATE durable
// artifact store: the browser uploads into it (web_files.cpp), the model writes
// artifacts into it (artifact.save), and both serve from it (downloads,
// files.send -> Telegram). The portable nimbus::orch::FileStore owns the index
// + path safety + quotas (host-tested); this layer owns the SD I/O, the index
// persistence (atomic .index writes), the single streaming-upload session, and
// the agent tools.
//
// Concurrency contract (matches the rest of the memory system): ALL SD I/O and
// index mutations happen under memory::Lock, held only for SHORT operations -
// per-chunk during streamed uploads/downloads, never across a network wait.
// SD-only by design: with no card the store reports absent (no silent tiny
// LittleFS quota - honest degradation, same philosophy as the memory tiering).

namespace agent::files {

// Mount/scan the store (call AFTER memory::begin resolved the SD tier). Loads
// /mem/files/.index, or rebuilds it by scanning the tree when missing. Also
// registers the agent tools (files.list / files.stat / artifact.save /
// files.send) into memory::registry(). Safe no-op without SD.
void begin();

// True when the store is usable NOW (SD mounted and not demoted).
bool available();

// Stats for /api/state + the capability manifest.
void stats(bool& present, uint16_t& count, uint64_t& bytes, uint32_t& freeBytes);

// CUM-7: the full storage truth for the files payload. `quota` = card - 512 MB
// reserve (0 when the card is unsupported); `cardTotal`/`cardFree` are the raw
// card numbers; `unsupported` is set when a mounted card is below the 1 GB
// minimum. All bytes.
struct StorageTruth {
  bool     present     = false;
  bool     unsupported = false;
  uint16_t files       = 0;
  uint64_t used        = 0;
  uint64_t quota       = 0;
  uint64_t cardTotal   = 0;
  uint64_t cardFree    = 0;
};
StorageTruth storageTruth();

// ---- streaming upload session (web_files.cpp) ----
// Single concurrent session (an ESP32 has no business running parallel SD
// uploads): beginWrite() -> false if busy/refused (reason in err - quota, path,
// no SD). Chunks stream to "<project>/.part" and the content hash accumulates;
// finishWrite(true) renames into place + commits the index + persists it.
// finishWrite(false) (or abortWrite on disconnect) deletes the partial.
uint32_t writeGen();               // id of the live upload session (0 = none)
void abortWriteGen(uint32_t gen);  // abort ONLY if `gen` still owns the session
bool beginWrite(const std::string& project, const std::string& name,
                size_t expectedBytes, std::string& err);
// Binary register seam (v4.1): open a streaming write like beginWrite, but stamp
// WHOSE file this is (ownerNs, v3.7.0 data boundary) - the provider file-fetch
// path streams a code_interpreter-produced binary straight to SD, then commits
// it via writeChunk + finishWrite. Same single-session slot as beginWrite (so it
// serializes with web uploads); pass expectedBytes=0 when the size is unknown up
// front (finishWrite does the real quota check). NOT model-reachable.
bool binaryWriteBegin(const std::string& project, const std::string& name,
                      size_t expectedBytes, const std::string& ownerNs, std::string& err);
bool writeChunk(const uint8_t* data, size_t len);
bool finishWrite(bool ok, std::string& err);
void abortWrite();
bool writeBusy();

// ---- direct ops (tools + endpoints) ----
// Persist a (bounded) text artifact from a turn. Overwrites same project/name.
// ownerNs (v3.7.0) stamps WHOSE artifact this is; empty = the device owner.
// Store a file by STREAMING it from another filesystem (the media lane's
// LittleFS staging copy), rather than holding it in memory the way saveText
// does. Goes through the same index, so an inbound photo counts against the
// sender's storage limit, appears in Memory & Files, and can be shared -
// instead of landing in a blob directory nothing accounts for.
bool saveStream(const std::string& project, const std::string& name,
                fs::FS& srcFs, const char* srcPath, std::string& err,
                const std::string& ownerNs);

bool saveText(const std::string& project, const std::string& name,
              const std::string& text, std::string& err,
              const std::string& ownerNs = "");

// Store a BINARY buffer (e.g. a generated image already decoded in PSRAM) directly
// into the artifact store, under the memory Lock. The write is brief (~1 s for ~1 MB)
// so it serializes against the non-re-entrant SD bus WITHOUT the long hold that a
// stream-during-download would need. Verifies the bytes persisted (a flaky SD can
// accept a write it doesn't keep) before committing the index.
// ---- W18 files.fetch (URL downloads under the owner trust policy) ----------
// Pump: ONE action per call, on tg_poll (single-TLS discipline): sends the
// owner-approval Telegram, or executes the next Ready request (direct download,
// or quarantine+scan+promote under policy scan). Mutex-guarded queue - the web
// approve/deny runs on the AsyncTCP task.
void tickFetch();
std::string fetchQueueJson();     // [{id,url,project,name,state,by,err,bytes}]
bool fetchApprove(uint32_t id);
bool fetchDeny(uint32_t id);
int  fetchPendingCount();

bool saveBytes(const std::string& project, const std::string& name,
               const uint8_t* data, size_t len, std::string& err,
               const std::string& ownerNs = "");
// Absolute SD path for an indexed file ("" if unknown/absent) - for downloads
// and Telegram sends. Callers stream it under memory::Lock per chunk.
std::string absPath(const std::string& project, const std::string& name);

// ---- v4.0.0 fan-out plumbing ------------------------------------------------
// Read a TEXT doc's content for attach splicing ("" on miss / non-text /
// oversize / NOT READABLE BY `who`). Device-internal seam (the JobEngine reads
// on the spawning chat's behalf) - enforces the same read boundary as the
// files.read tool, so an attach ref can't cross a tenant boundary.
std::string readDocText(const std::string& project, const std::string& name,
                        const nimbus::orch::Principal& who);
// Auto-persist a finished sub-agent's reply as <project>/<name>-<tag>.md.
// Sanitizes the agent-assigned name (validSegment, fallback = tag), enforces a
// per-project cap, stamps ownerNs. Returns the outcome line for the fresh
// result: "[saved: p/f]" or "[persist FAILED: why]".
std::string persistSubResult(const std::string& project, const std::string& name,
                             const std::string& tag, const std::string& text,
                             const std::string& ownerNs);
bool removeFile(const std::string& project, const std::string& name);
// Delete an entire project (folder) and its files. Returns the count removed,
// or -1 if the project is unknown/empty. Owner surface only (web Danger action).
int  removeProject(const std::string& project);

// Would storing `bytes` more for `ns` exceed that principal's file-storage
// limit? The media lane asks BEFORE downloading an attachment - a quota checked
// only after the bytes have landed is a quota that lets every first offender
// through. Returns false (allowed) when there is no limit or no index yet.
bool wouldExceedQuota(const nimbus::orch::Principal& who, uint32_t bytes);
// JSON array of entries (project filter optional) - for GET /api/files.
// ns/owner (v3.7.0) bound what the caller may see; the web Files browser is the
// token-authenticated owner surface, so it passes owner=true.
String listJson(const String& project, const std::string& ns = "", bool owner = true,
                bool mayReadShared = true);

}  // namespace agent::files
