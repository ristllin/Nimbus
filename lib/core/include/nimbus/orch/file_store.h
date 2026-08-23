#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/vector_memory.h"   // kOwnerNs
#include <vector>

// file_store - the portable core of E1 "Files + artifacts" (docs/ROADMAP.md).
//
// The SD card is the owner's PRIVATE durable artifact store: agents write
// reports/documents into it, the owner uploads files to it, and both serve from
// it (Telegram now; email/git later). This core owns the INDEX + the rules -
// path safety, quotas, dedup identity, provider file_id caching - and is 100%
// host-tested. It never touches a filesystem: the device layer streams bytes,
// then commits metadata here and persists dump() with the same atomic-write
// pattern as the memory blobs.
//
// Layout contract (device side): /mem/files/<project>/<name>. `project` and
// `name` are SINGLE sanitized path segments - validSegment() is the one gate
// every consumer (web upload, tools, provider adapters) inherits, so a hostile
// name can never traverse out of the tree.
//
// Durability rule: artifacts are never auto-evicted. Full is full - add()
// refuses with an explicit reason the caller surfaces to the human/model.

namespace nimbus::orch {

// Mirrors the provider content-block split (document / image / audio / other) so
// tools + future Files-API routing agree on one vocabulary.
enum class FileKind : uint8_t { Doc = 0, Image = 1, Audio = 2, Data = 3 };

const char* fileKindName(FileKind k);          // "doc" / "image" / "audio" / "data"
FileKind    fileKindForName(const std::string& name);  // by extension; Data if unknown

struct FileEntry {
  std::string project;         // sanitized segment (folder under /mem/files)
  std::string name;            // sanitized segment (human name, extension kept)
  uint32_t    bytes = 0;
  uint32_t    createdAt = 0;   // epoch seconds (0 = clock unknown at write time)
  FileKind    kind = FileKind::Data;
  uint64_t    hash = 0;        // FNV-1a 64 content hash (identity/dedup; blob_store's)
  // Provider Files-API cache (A4 seam): upload-once, reference-by-id. Empty until
  // an adapter uploads this artifact; invalidated when content (hash) changes.
  std::string providerTag;     // "anthropic" | "openai" | "" (none)
  std::string providerFileId;  // the provider's file id ("" = not uploaded)
  // v3.7.0 data boundary: WHOSE artifact this is (a Principal namespace).
  // Empty = legacy, adopted as the owner's on load. Written as a NINTH index
  // field, deliberately LAST: a pre-v3.7 image parses only eight and absorbs
  // the tail into providerFileId - a disposable upload cache - so a rollback
  // keeps every file and every real column intact.
  std::string owner;
  // v3.7.0: an explicit, per-artifact, READ-ONLY grant by this file's owner -
  // "here is a document" rather than a fact that silently enters someone's
  // prompt (which is why vector memory has no equivalent). Encoded INSIDE the
  // owner field as "<ns>|shared" so the index gains no further column.
  bool shared = false;
};

class FileStore {
 public:
  struct Limits {
    size_t   maxEntries   = 256;
    size_t   maxNameLen   = 48;
    size_t   maxProjectLen = 24;
    uint64_t maxFileBytes = 8ull * 1024 * 1024;    // 8 MB per artifact
    uint64_t maxTotalBytes = 512ull * 1024 * 1024; // 512 MB indexed total
  };

  FileStore() = default;
  explicit FileStore(const Limits& l) : lim_(l) {}

  // ---- SD quota + supported-card truth (CUM-7; static + pure, host-tested) ----
  // The device recomputes the artifact-store quota from the REAL card size at
  // mount: quota = card capacity - a fixed reserve (headroom the firmware, logs,
  // and FS metadata need). A card below the minimum is UNSUPPORTED - too small to
  // be a useful bulk store once the reserve is taken - and the store stays off with
  // an explicit state rather than a silent tiny quota.
  static constexpr uint64_t kSdReserveBytes = 512ull * 1024 * 1024;   // 512 MB reserve
  static constexpr uint64_t kSdMinCardBytes = 1024ull * 1024 * 1024;  // < 1 GB => unsupported
  static bool sdCardSupported(uint64_t cardBytes) { return cardBytes >= kSdMinCardBytes; }
  // Quota for a card, in bytes: card - reserve, or 0 if the card is unsupported or
  // smaller than the reserve (never underflows).
  static uint64_t quotaForCard(uint64_t cardBytes) {
    if (!sdCardSupported(cardBytes)) return 0;
    return cardBytes > kSdReserveBytes ? (cardBytes - kSdReserveBytes) : 0;
  }

  // ---- path safety (static: usable before any store exists) ----
  // A valid segment: 1..maxLen printable-ASCII chars, no '/', '\\', control
  // chars or spaces-only, not "." / "..", no leading dot (hidden files), no ':'
  // (FAT drive-letter/ADS quirks). This is THE traversal gate.
  static bool validSegment(const std::string& s, size_t maxLen);

  // "<project>/<name>" after validating both against these limits ("" on reject).
  std::string relPath(const std::string& project, const std::string& name) const;

  // ---- mutations ----
  // Adds or REPLACES (same project+name). Enforces segment safety + all quotas.
  // On refusal: false + a short human-readable reason in err.
  // v3.7.0: may `who` (a Principal namespace; the owner sees all) touch this
  // entry? Identity here is (project,name) and add() REPLACES, so filtering
  // reads alone would still let one principal silently DESTROY another's file
  // by writing the same path.
  bool ownedBy(const FileEntry& e, const std::string& ns, bool owner) const {
    if (owner) return true;                       // the device owner sees all
    const std::string eff = e.owner.empty() ? std::string(kOwnerNs) : e.owner;
    return eff == ns;
  }
  // READ visibility = own it, or it was explicitly shared. Sharing never grants
  // write: only the owner (or an admin) may replace or delete a shared file.
  // `shared` is a grant to APPROVED people, not to the world: a revoked tenant
  // and the unattributed LAN MCP caller both have readShared=false, and neither
  // should keep reading through a share made while they were still trusted.
  bool readableBy(const FileEntry& e, const std::string& ns, bool owner,
                  bool mayReadShared = true) const {
    return ownedBy(e, ns, owner) || (e.shared && mayReadShared);
  }
  // Total bytes this namespace holds - the artifact-store quota, measured at
  // the write like the vector one.
  uint32_t bytesFor(const std::string& ns) const {
    uint32_t n = 0;
    for (const auto& e : entries_) {
      const std::string eff = e.owner.empty() ? std::string(kOwnerNs) : e.owner;
      if (eff == ns) n += e.bytes;
    }
    return n;
  }
  // Refuse a write that would overwrite an entry the caller does not own.
  bool writeAllowed(const std::string& project, const std::string& name,
                    const std::string& ns, bool owner) const {
    const FileEntry* cur = find(project, name);
    return !cur || ownedBy(*cur, ns, owner);
  }

  bool add(const FileEntry& e, std::string& err);
  bool remove(const std::string& project, const std::string& name);
  // Cache a provider file id on an existing entry (A4).
  bool setProviderId(const std::string& project, const std::string& name,
                     const std::string& tag, const std::string& id);

  // ---- queries ----
  const FileEntry* find(const std::string& project, const std::string& name) const;
  // All entries, or one project's ("" = all). Pointers valid until next mutation.
  std::vector<const FileEntry*> list(const std::string& project = "") const;
  std::vector<std::string> projects() const;    // distinct project names, sorted
  size_t   count() const { return entries_.size(); }
  uint64_t totalBytes() const;
  const Limits& limits() const { return lim_; }
  // W17: the device raises maxTotalBytes at mount from the real card size (half
  // the card, floored at the 512 MB default). Mount-time only by convention -
  // lowering it below current usage refuses new saves but never deletes.
  void setLimits(const Limits& l) { lim_ = l; }

  // Pre-check for STREAMING uploads: can `addBytes` more fit (per-file + total +
  // entry-count for a NEW name)? Refuse before the first chunk lands, not after.
  bool wouldExceed(const std::string& project, const std::string& name,
                   uint64_t addBytes, std::string& err) const;

  // ---- persistence (device stores dump() as /mem/files/.index) ----
  // Versioned, line-oriented, tab-separated (names are segment-safe so no
  // escaping is needed - tabs/newlines are rejected by validSegment).
  std::string dump() const;
  bool load(const std::string& blob);   // tolerant: bad lines skipped

 private:
  int indexOf(const std::string& project, const std::string& name) const;
  Limits lim_;
  std::vector<FileEntry> entries_;
};

}  // namespace nimbus::orch
