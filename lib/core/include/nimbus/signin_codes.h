#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Single-use, TTL-bounded sign-in codes (CUM-45: access token out of URLs).
//
// The web app must never carry the durable access token in a URL: a URL is
// committed to browser history before any script runs, and browsers sync history
// across machines while localStorage is not, so a token-bearing link silently
// authenticates a machine you never used. Instead a Sign-in QR / Wi-Fi-handoff
// link carries a SHORT, SINGLE-USE, short-lived code; the page exchanges it once
// for the durable token over POST (see POST /api/signin/exchange). A copy of the
// link sitting in synced history is inert the moment the code is used or expires.
//
// This is portable logic (no Arduino), so it is host-unit-tested. It lives on the
// web server's single task (the firmware has no on-device concurrency, a frozen
// invariant), so it needs no locking. Fixed capacity, no heap.
namespace nimbus {

class SigninCodes {
 public:
  static constexpr size_t CAP = 8;                    // a few concurrent scans/links
  static constexpr size_t MAXLEN = 16;                // code buffer incl. null
  static constexpr uint32_t DEFAULT_TTL_MS = 120000;  // 2 minutes

  explicit SigninCodes(uint32_t ttlMs = DEFAULT_TTL_MS) : ttl_(ttlMs) {}

  // Store a freshly generated code (caller supplies device-RNG bytes as a
  // null-terminated string). Reuses the oldest slot when full. An empty or
  // over-long code is ignored (returns false).
  bool mint(const char* code, uint32_t now) {
    if (!code) return false;
    const size_t n = ::strnlen(code, MAXLEN);
    if (n == 0 || n >= MAXLEN) return false;
    Slot& s = slots_[next_];
    next_ = (next_ + 1) % CAP;
    ::memcpy(s.code, code, n);
    s.code[n] = '\0';
    s.len = n;
    s.expiresAt = now + ttl_;
    s.used = false;
    s.live = true;
    return true;
  }

  // Redeem a code exactly once. Compares against every live slot without an
  // early-out on the byte loop so a wrong guess is not timed against a right one.
  // Returns true (and consumes the slot) only for a live, unused, unexpired match.
  bool redeem(const char* code, uint32_t now) {
    if (!code) return false;
    const size_t n = ::strnlen(code, MAXLEN);
    if (n == 0 || n >= MAXLEN) return false;
    int hit = -1;
    for (size_t i = 0; i < CAP; i++) {
      Slot& s = slots_[i];
      const bool valid = s.live && !s.used && !expired(s, now) && s.len == n;
      uint8_t diff = 0;
      for (size_t j = 0; j < n; j++) diff |= uint8_t(s.code[j] ^ code[j]);
      if (valid && diff == 0) hit = int(i);   // record, keep scanning (constant work)
    }
    if (hit < 0) return false;
    slots_[hit].used = true;                   // single use
    return true;
  }

  // Live, unused, unexpired codes right now (test/introspection helper).
  size_t liveCount(uint32_t now) const {
    size_t c = 0;
    for (size_t i = 0; i < CAP; i++)
      if (slots_[i].live && !slots_[i].used && !expired(slots_[i], now)) c++;
    return c;
  }

  void clear() {
    for (size_t i = 0; i < CAP; i++) slots_[i] = Slot{};
    next_ = 0;
  }

 private:
  struct Slot {
    char code[MAXLEN] = {0};
    size_t len = 0;
    uint32_t expiresAt = 0;
    bool used = false;
    bool live = false;
  };
  // Wraparound-safe expiry: now is at/after expiry when (now - expiresAt) >= 0.
  static bool expired(const Slot& s, uint32_t now) {
    return int32_t(now - s.expiresAt) >= 0;
  }
  Slot slots_[CAP]{};
  size_t next_ = 0;
  uint32_t ttl_;
};

}  // namespace nimbus
