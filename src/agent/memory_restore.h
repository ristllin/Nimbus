#pragma once
// memory_restore - the PORTABLE parse + apply core of the memory restore path
// (CUM-406). It turns the JSON a backup folder holds (the exact shapes
// backup_device.py writes: vectors.json entries, episodic.jsonl rows,
// scratchpad.json) back into the in-RAM engines, idempotently.
//
// Deliberately Arduino-FREE and header-only (inline), depending only on lib/core
// engine types + ArduinoJson + the portable base64 codec, so the device seam
// (memory_subsystem / web_memory) and the native round-trip test exercise the
// SAME code. The device wrapper adds the memory Lock + persist around these; the
// functions here never touch a filesystem, a network, or a secret.
//
// Idempotency contract:
//   * vectors  - exact REPLACE-BY-ID (remove-then-add, dedup off). Re-running a
//                restore, or paging it, converges to the same set.
//   * episodic - APPEND with in-call id-dedup. The append-log store has no id
//                index to dedup against across calls, so the host tool guards a
//                non-empty store (a fresh-device restore never duplicates).
//   * scratchpad - REPLACE (active + three tiers).
//
// A restore is authoritative: nothing on the device re-clobbers it (there is no
// sync), and these writes preserve the backed-up ids/timestamps so recall,
// decay, and TTL behave exactly as they did on the source device.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/vector_memory.h"
#include "nimbus/util/b64_decode.h"

namespace agent {
namespace memory {
namespace restore {

// ---- base64 (standard alphabet, padded) -------------------------------------
// Encode is used by the device browse export (embedding -> row) and by the test
// fixture builder; decode reuses the portable, host-tested StreamDecoder.
inline std::string b64Encode(const int8_t* data, size_t n) {
  static const char* kAlpha =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 3 <= n; i += 3) {
    const uint32_t v = (uint32_t)(uint8_t)data[i] << 16 |
                       (uint32_t)(uint8_t)data[i + 1] << 8 |
                       (uint32_t)(uint8_t)data[i + 2];
    out.push_back(kAlpha[(v >> 18) & 0x3F]);
    out.push_back(kAlpha[(v >> 12) & 0x3F]);
    out.push_back(kAlpha[(v >> 6) & 0x3F]);
    out.push_back(kAlpha[v & 0x3F]);
  }
  const size_t rem = n - i;
  if (rem == 1) {
    const uint32_t v = (uint32_t)(uint8_t)data[i] << 16;
    out.push_back(kAlpha[(v >> 18) & 0x3F]);
    out.push_back(kAlpha[(v >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t v = (uint32_t)(uint8_t)data[i] << 16 |
                       (uint32_t)(uint8_t)data[i + 1] << 8;
    out.push_back(kAlpha[(v >> 18) & 0x3F]);
    out.push_back(kAlpha[(v >> 12) & 0x3F]);
    out.push_back(kAlpha[(v >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}
inline std::string b64EncodeVec(const std::vector<int8_t>& v) {
  return v.empty() ? std::string() : b64Encode(v.data(), v.size());
}
inline std::vector<int8_t> b64DecodeInt8(const char* s, size_t n) {
  std::vector<int8_t> out;
  nimbus::b64::StreamDecoder d;
  d.feed(s, n, [&](uint8_t b) { out.push_back((int8_t)b); });
  return out;
}

// ---- vectors ----------------------------------------------------------------
struct VecReport {
  int added = 0;         // ids not previously present, inserted
  int replaced = 0;      // ids that existed, overwritten (idempotent re-run)
  int skippedNoVec = 0;  // a row carried no "vec" (a pre-CUM-406 backup): no embedding to restore
  int widthErrors = 0;   // "vec" present but the wrong dimensionality for this store
  int badRows = 0;       // missing id / not an object
  int total = 0;         // store size after apply (0 in dry-run: nothing written)
};

// Parse one vectors.json row into a VecEntry, decoding the base64 "vec". Fields
// mirror the GET /api/mem/vector browse shape backup_device.py records.
//   hadVec  - the row carried a "vec" string (else there is no embedding to restore)
//   widthOk - the decoded vector matched `dims` (only meaningful when hadVec)
struct VecRowParse {
  bool haveId = false;
  bool hadVec = false;
  bool widthOk = false;
  nimbus::orch::VecEntry entry;
};
inline VecRowParse parseVectorRow(JsonObjectConst o, int dims) {
  VecRowParse p;
  const char* id = o["id"] | "";
  if (!id[0]) return p;   // an entry with no id cannot be addressed idempotently
  p.haveId = true;
  nimbus::orch::VecEntry& e = p.entry;
  e.id = id;
  e.content = (const char*)(o["content"] | "");
  e.importance = o["importance"] | 0.5f;
  e.ttlHours = o["ttlHours"] | 720;
  e.createdAtHours = o["tsHours"] | 0u;
  e.source = (const char*)(o["source"] | "system");
  e.ns = (const char*)(o["ns"] | "");
  e.permanentFlag = o["permanent"] | false;
  e.creatorFlag = o["creator"] | false;
  e.lastRecallHours = o["lastRecallHours"] | 0u;
  JsonVariantConst vv = o["vec"];
  if (vv.is<const char*>()) {
    const char* b = vv.as<const char*>();
    if (b && b[0]) {
      p.hadVec = true;
      e.vec = b64DecodeInt8(b, strlen(b));
      p.widthOk = ((int)e.vec.size() == dims);
    }
  }
  return p;
}

// Apply vectors.json entries to `vm` idempotently (replace-by-id, dedup off).
// dryRun validates + classifies every row and writes nothing.
inline VecReport applyVectors(nimbus::orch::VectorMemory& vm, JsonArrayConst entries,
                              int dims, bool dryRun) {
  VecReport r;
  for (JsonObjectConst o : entries) {
    if (o.isNull()) { r.badRows++; continue; }
    VecRowParse p = parseVectorRow(o, dims);
    if (!p.haveId) { r.badRows++; continue; }
    if (!p.hadVec) { r.skippedNoVec++; continue; }
    if (!p.widthOk) { r.widthErrors++; continue; }
    const bool existed = dryRun ? vm.idVisible(p.entry.id, {}) : vm.remove(p.entry.id);
    if (!dryRun) {
      if (!vm.add(p.entry, /*dedup=*/false)) { r.widthErrors++; continue; }
    }
    if (existed) r.replaced++; else r.added++;
  }
  r.total = dryRun ? 0 : vm.size();
  return r;
}

// ---- episodic ---------------------------------------------------------------
struct EpiReport {
  int added = 0;    // rows appended
  int dupSkipped = 0;  // a duplicate id within this call
  int badRows = 0;  // missing id / not an object
};

// Parse one episodic.jsonl row (the GET /api/mem/episodic message shape) into an
// EpisodicMessage. Returns false if it has no id.
inline bool parseEpisodicRow(JsonObjectConst o, nimbus::orch::EpisodicMessage& m) {
  const char* id = o["id"] | "";
  if (!id[0]) return false;
  m.id = id;
  m.sessionId = (const char*)(o["session"] | "");
  m.tsHours = o["ts"] | 0u;
  m.role = (const char*)(o["role"] | "");
  nimbus::orch::MsgKind k = nimbus::orch::MsgKind::Message;
  nimbus::orch::kindFromName((const char*)(o["kind"] | "message"), k);
  m.kind = k;
  m.text = (const char*)(o["text"] | "");
  m.blobPath = (const char*)(o["blob"] | "");
  m.tags = (const char*)(o["tags"] | "");
  return true;
}

// Append episodic.jsonl rows to `es`, de-duplicating ids seen within this call
// (so a fixture or a retried page can't double-add). dryRun writes nothing.
inline EpiReport applyEpisodic(nimbus::orch::EpisodicStore& es, JsonArrayConst msgs,
                               bool dryRun, std::vector<std::string>* seenIds = nullptr) {
  EpiReport r;
  std::vector<std::string> localSeen;
  std::vector<std::string>& seen = seenIds ? *seenIds : localSeen;
  for (JsonObjectConst o : msgs) {
    if (o.isNull()) { r.badRows++; continue; }
    nimbus::orch::EpisodicMessage m;
    if (!parseEpisodicRow(o, m)) { r.badRows++; continue; }
    bool dup = false;
    for (const auto& s : seen) if (s == m.id) { dup = true; break; }
    if (dup) { r.dupSkipped++; continue; }
    seen.push_back(m.id);
    if (!dryRun) es.addMessage(m);
    r.added++;
  }
  return r;
}

// ---- scratchpad -------------------------------------------------------------
struct ScratchReport { int active = 0; int shortN = 0; int midN = 0; int longN = 0; };

inline int loadTier(nimbus::orch::Scratchpad& sp, nimbus::orch::Tier t,
                    JsonArrayConst arr, bool dryRun) {
  std::vector<std::string> items;
  for (JsonVariantConst v : arr) {
    const char* s = v.as<const char*>();
    if (s && s[0]) items.push_back(s);
  }
  if (dryRun) return (int)items.size();
  return sp.replace(t, items);
}

// Replace the scratchpad's active line + three tiers from scratchpad.json.
inline ScratchReport applyScratchpad(nimbus::orch::Scratchpad& sp, JsonObjectConst obj,
                                     bool dryRun) {
  using nimbus::orch::Tier;
  ScratchReport r;
  const char* active = obj["active"] | "";
  if (!dryRun) sp.setActiveTask(active);
  r.active = active[0] ? 1 : 0;
  r.shortN = loadTier(sp, Tier::Short, obj["short"].as<JsonArrayConst>(), dryRun);
  r.midN = loadTier(sp, Tier::Mid, obj["mid"].as<JsonArrayConst>(), dryRun);
  r.longN = loadTier(sp, Tier::Long, obj["long"].as<JsonArrayConst>(), dryRun);
  return r;
}

}  // namespace restore
}  // namespace memory
}  // namespace agent
