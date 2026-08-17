#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// blob_store - content-addressed naming for the media/artifact sidecars on the SD
// card (docs/orchestrator-storage.md §4). A blob's filename is the hex hash of its
// bytes, so identical bytes (a re-sent voice note, a repeated TTS reply) dedup to one
// file, and an episodic row references it by blobPath. Portable + Arduino-free: the
// hashing + path + reference-count-prune LOGIC is host-tested; the device does the
// actual SD read/write/delete against these names.
namespace nimbus {
namespace orch {

// 64-bit FNV-1a hex digest of the bytes (16 lowercase hex chars). Stable, fast, and
// collision-safe enough for the handful-to-thousands of blobs a desk device keeps.
std::string blobHash(const std::string& bytes);

// Streaming form of blobHash: feed a blob in chunks via update(), then hex() returns
// the SAME 16-char digest as blobHash() over the concatenation. Lets the device
// content-address a large file (e.g. a ~480 KB voice recording) by streaming it off
// the filesystem in a small buffer, never holding all the bytes in RAM.
class BlobHasher {
 public:
  void        update(const void* data, size_t len);
  std::string hex() const;   // == blobHash(all bytes fed)
 private:
  uint64_t h_ = 1469598103934665603ULL;   // FNV-1a offset basis
};

// Canonical blob path: "<dir>/<hash>.<ext>" (ext WITHOUT a leading dot; ""->no ext).
// e.g. blobPath("/mem/blobs", "1a2b…", "ogg") -> "/mem/blobs/1a2b….ogg".
std::string blobPath(const std::string& dir, const std::string& hash, const std::string& ext);

// The hash embedded in a blob filename or path: the basename (after the last '/')
// truncated at the first '.'. "/mem/blobs/1a2b.ogg" -> "1a2b"; "" if none.
std::string blobHashOf(const std::string& pathOrName);

// Maintenance: given every blob filename present on the card and the set of hashes
// still referenced by episodic rows, return the filenames that are UNREFERENCED and
// therefore safe to delete. A file is referenced iff blobHashOf(name) is in `referenced`.
std::vector<std::string> unreferencedBlobs(const std::vector<std::string>& present,
                                           const std::set<std::string>& referenced);

}  // namespace orch
}  // namespace nimbus
