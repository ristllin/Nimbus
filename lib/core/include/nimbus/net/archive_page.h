#pragma once
#include <cstddef>
#include <vector>

// archive_page - the Telegram archive's day page stays small (prism, wave 1 row 7):
// GET /api/tg/archive?chat=&day= built up to 200 index lines into one internal-heap
// String outside the web admission gate. A page now carries only the rows that fit
// under kArchivePageBudget bytes of JSON (always at least one, so paging moves), and
// says where the next page starts; at that size it is served like any small reply.
namespace nimbus {
namespace net {

constexpr size_t kArchivePageBudget = 14 * 1024;   // under the 16 KB small-reply line

// How many rows, of the given serialized sizes, fit with `overhead` bytes of
// envelope under `budget` (each row costs its size plus a separating comma).
inline size_t archiveRowsThatFit(const std::vector<size_t>& rowBytes, size_t overhead,
                                 size_t budget = kArchivePageBudget) {
  size_t used = overhead, n = 0;
  for (size_t b : rowBytes) {
    if (n > 0 && used + b + 1 > budget) break;
    used += b + 1;
    n++;
  }
  return n;
}

}  // namespace net
}  // namespace nimbus
