#pragma once
// nvs_diag - what the device tells about its settings storage (NVS) when it fills.
//
// On 14 Oct 2026 a desk device ran out of NVS and nobody could see why: the failure
// log line printed "key=***" (the log redactor masks the value after any "key="), the
// state page showed only the last failing key, and nothing listed what filled the
// partition. This header holds the portable half of the visibility fix, host-tested
// in test/test_logring and test/test_nvs_full:
//
//   - the one persist-failure log line, with the key in brackets so redaction keeps it;
//   - a small ring of recent failures (key, sequence number, time) for /api/state
//     nvs.recentFails, so a bench or the owner can count which writes fail and how often;
//   - a once-per-boot latch for the owner notice: any task's failed write raises it,
//     and only the tg_poll task sends the notice, on its own outbound path (no new
//     task, no TLS from the writer's task);
//   - the entry arithmetic behind GET /api/nvs/stats (names and entry counts, never
//     values).
//
// No Arduino: the device seam is src/sys/config_nvs.cpp (failures) and
// src/sys/nvs_inventory.cpp (the listing).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nimbus {
namespace store {

// The persist-failure log line: key, free bytes, total bytes. The key sits in
// brackets, never after "key=": the redactor (core::LogRing::redact) masks the value
// of any key=/key: pair, so the old "key=%s" form logged every failure as "key=***".
inline constexpr char kNvsWriteFailedFmt[] =
    "nvs: write FAILED [%s] - storage full? (%u B free of %u B)";

// ---- recent failures ------------------------------------------------------------
// The newest kRing failed writes, for /api/state. Every failure is recorded (the log
// line is rate limited per key, this ring is not), each with a monotonic sequence
// number, so a reader polling it can tell how many failures happened between two
// reads and which keys they hit. Not internally locked: the device records and reads
// under the persist ledger's spinlock.
class RecentFails {
 public:
  static constexpr size_t kRing   = 8;
  static constexpr size_t kKeyMax = 16;   // NVS keys are <= 15 chars + NUL

  struct Entry {
    uint32_t seq;            // 1-based, monotonic since boot
    uint32_t ms;             // millis() when it failed
    char     key[kKeyMax];
  };

  void note(const char* key, uint32_t nowMs) {
    Entry& e = ring_[seq_ % kRing];
    e.seq = ++seq_;
    e.ms = nowMs;
    std::strncpy(e.key, key ? key : "?", kKeyMax - 1);
    e.key[kKeyMax - 1] = 0;
  }

  // Copies up to `max` entries, newest first; returns how many.
  size_t snapshot(Entry* out, size_t max) const {
    size_t n = 0;
    const size_t have = seq_ < kRing ? seq_ : kRing;
    for (size_t i = 0; i < have && n < max; ++i) out[n++] = ring_[(seq_ - 1 - i) % kRing];
    return n;
  }

  uint32_t total() const { return seq_; }

 private:
  Entry    ring_[kRing] = {};
  uint32_t seq_ = 0;
};

// ---- the owner notice -----------------------------------------------------------
// Raised by any failed write (any task), taken once by the task that sends it. take()
// is true exactly once per boot and only after a raise(); every later raise is a no-op.
// A 32-bit word, so the compare-and-swap is a native lock-free instruction on the
// ESP32-S3 (no library lock behind a sub-word atomic).
class OnceNotice {
 public:
  void raise() {
    uint32_t idle = kIdle;
    state_.compare_exchange_strong(idle, kRaised);
  }
  bool take() {
    uint32_t raised = kRaised;
    return state_.compare_exchange_strong(raised, kSent);
  }
  bool pending() const { return state_.load() == kRaised; }
  bool sent() const { return state_.load() == kSent; }

 private:
  static constexpr uint32_t kIdle = 0, kRaised = 1, kSent = 2;
  std::atomic<uint32_t> state_{kIdle};
};

// What the owner reads (AGENTS.md section 6: what happened, then the one next step;
// plain words; the device is "it"). The device name is prefixed by the sender.
inline constexpr char kStorageNoticeText[] =
    "settings storage is almost full, so some changes may not be saved. To free "
    "space, open the device web page and remove connectors or provider keys you "
    "don't use.";

// ---- the NVS listing --------------------------------------------------------------
// One NVS entry is 32 bytes. An int of any width takes one entry. A string takes one
// header entry plus its bytes (NUL included) in 32-byte data entries. A blob takes an
// index entry plus, per chunk, a header entry and its data entries; the listing counts
// one chunk, so a blob split across pages reads one or two entries low.
enum class NvsKind : uint8_t { Int, Str, Blob, Other };

inline constexpr size_t kNvsEntryDataBytes = 32;

inline size_t nvsEntriesFor(NvsKind kind, size_t bytes) {
  const size_t data = (bytes + kNvsEntryDataBytes - 1) / kNvsEntryDataBytes;
  switch (kind) {
    case NvsKind::Int:  return 1;
    case NvsKind::Str:  return 1 + data;         // `bytes` includes the NUL
    case NvsKind::Blob: return 2 + data;         // index + one chunk header + data
    default:            return 1;
  }
}

inline const char* nvsKindName(NvsKind kind) {
  switch (kind) {
    case NvsKind::Int:  return "int";
    case NvsKind::Str:  return "str";
    case NvsKind::Blob: return "blob";
    default:            return "other";
  }
}

// The listing caps how many keys one response carries (a full partition holds about
// 500 entries, so a real device lists far fewer keys than this).
inline constexpr size_t kNvsListMaxKeys = 512;

}  // namespace store
}  // namespace nimbus
