#pragma once
#include <string>

// tg_owner - which Telegram chat the device's own notices go to (storage full, the
// duplicate and ceiling notices, the archive's card-pressure notice). One rule, used by
// every notice: the first explicit owner that is still on the allowlist, else (no
// owners set) the first allowlist entry; "" when there is none. A chat removed from the
// allowlist never receives a notice, even while the owners list still names it first.
// Portable and host-tested (test_tg_hooks).
namespace nimbus::net {

namespace detail {
inline std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}
}  // namespace detail

// `owners` and `allowlist` are comma-separated chat ids; `allowed(chat)` is the
// device's allowlist check.
template <class AllowedFn>
std::string ownerChatOf(const std::string& owners, const std::string& allowlist, AllowedFn allowed) {
  const std::string own = detail::trimmed(owners);
  if (own.empty()) return detail::trimmed(allowlist.substr(0, allowlist.find(',')));
  size_t start = 0;
  while (start <= own.size()) {
    size_t end = own.find(',', start);
    if (end == std::string::npos) end = own.size();
    const std::string entry = detail::trimmed(own.substr(start, end - start));
    if (!entry.empty() && allowed(entry)) return entry;
    start = end + 1;
  }
  return std::string();
}

}  // namespace nimbus::net
