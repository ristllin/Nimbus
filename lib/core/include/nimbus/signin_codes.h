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
  static constexpr uint32_t DEFAULT_TTL_MS = 120000;  // 2 minutes (QR / scan path)
  // Hand-entry ("Show code") display path (CUM-295): a person reading a code off
  // the screen and typing it into another machine routinely needs more than the
  // 2-minute scan window, so the DISPLAYED code is minted with this longer TTL.
  // The scan path stays at DEFAULT_TTL_MS (a scan is instant). Ample for a
  // single-use, LAN-only credential.
  static constexpr uint32_t DISPLAY_TTL_MS = 600000;  // 10 minutes

  explicit SigninCodes(uint32_t ttlMs = DEFAULT_TTL_MS) : ttl_(ttlMs) {}

  // Store a freshly generated code (caller supplies device-RNG bytes as a
  // null-terminated string) with an explicit lifetime. Reuses the oldest slot when
  // full. An empty or over-long code is ignored (returns false).
  bool mint(const char* code, uint32_t now, uint32_t ttlMs) {
    if (!code) return false;
    const size_t n = ::strnlen(code, MAXLEN);
    if (n == 0 || n >= MAXLEN) return false;
    Slot& s = slots_[next_];
    next_ = (next_ + 1) % CAP;
    ::memcpy(s.code, code, n);
    s.code[n] = '\0';
    s.len = n;
    s.expiresAt = now + ttlMs;
    s.used = false;
    s.live = true;
    return true;
  }

  // Store a code with this table's default lifetime (the scan-path TTL).
  bool mint(const char* code, uint32_t now) { return mint(code, now, ttl_); }

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

// Lifecycle of the ONE code currently shown on the "Show code" (TokenDetail) screen
// (CUM-295). It carries no code bytes - the device layer holds the RNG-minted string
// and the matching slot in SigninCodes - it just tracks the displayed code's expiry
// so the panel can render an honest mm:ss countdown and re-mint the instant it
// expires, so a dead code never sits on screen as if valid. Portable, host-tested.
class SigninDisplayCode {
 public:
  // Register a freshly minted code that expires ttlMs from now (call on each mint).
  void set(uint32_t now, uint32_t ttlMs) {
    expiresAt_ = now + ttlMs;
    live_ = true;
  }
  bool live() const { return live_; }
  // Wraparound-safe: now is at/after expiry when int32_t(now - expiresAt) >= 0. An
  // unset code counts as expired so the caller mints one on first use.
  bool expired(uint32_t now) const {
    return !live_ || int32_t(now - expiresAt_) >= 0;
  }
  // Whole seconds until expiry, rounded UP so the countdown reads full at mint and
  // only hits 0 at expiry; 0 once expired or unset. Drives the on-screen mm:ss.
  uint32_t secsLeft(uint32_t now) const {
    if (expired(now)) return 0;
    const int32_t leftMs = int32_t(expiresAt_ - now);
    return leftMs <= 0 ? 0u : uint32_t((leftMs + 999) / 1000);
  }
  void clear() {
    expiresAt_ = 0;
    live_ = false;
  }

 private:
  uint32_t expiresAt_ = 0;
  bool live_ = false;
};

}  // namespace nimbus
