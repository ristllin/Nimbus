#pragma once
// nvs_inventory - what fills the settings storage (NVS): every key of the default NVS
// partition, in every namespace, with its type, size and entry count. Never a value.
//
// Serves GET /api/nvs/stats (owner sign-in only). On 14 Oct 2026 a desk device sat at
// 0 free entries with about 495 entries used and only about 110 of them explained;
// this listing names the rest without a restart, a USB dump or any value leaving the
// device. Read-only: it opens namespaces read-only and asks NVS for sizes only.
// The entry arithmetic is portable and host-tested (nimbus/store/nvs_diag.h).

#include <cstddef>
#include <cstdint>
#include <functional>

#include "nimbus/store/nvs_diag.h"

namespace nimbus::sys {

struct NvsItem {
  const char*          ns;        // namespace name
  const char*          key;       // key name
  nimbus::store::NvsKind kind;
  uint32_t             bytes;     // value size (a string counts its NUL); 0 for an int
  uint32_t             entries;   // 32-byte entries it takes (see nvsEntriesFor)
};

// NVS partition totals in entries, from nvs_get_stats. False when the call fails.
struct NvsEntryTotals {
  uint32_t used = 0, free = 0, total = 0, namespaces = 0;
};
bool nvsEntryTotals(NvsEntryTotals& out);

// Calls fn once per key (at most maxKeys). Returns how many were listed; *truncated is
// set when the partition held more.
size_t nvsForEachKey(const std::function<void(const NvsItem&)>& fn, size_t maxKeys,
                     bool* truncated);

}  // namespace nimbus::sys
