#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

// Portable, host-testable debug log ring buffer with secret redaction.
// Hardware glue (src/sys/logbuf.*) wraps this, feeds it formatted lines, and
// exposes it to the menu + an auth-gated /logs endpoint.
namespace core {

class LogRing {
 public:
  explicit LogRing(size_t capacity = 80) : cap_(capacity ? capacity : 1) {}

  void addSecret(const std::string& secret);  // exact value to mask (e.g. API key)
  void push(const std::string& line);          // redacted, then stored
  std::vector<std::string> lines() const;       // oldest -> newest
  size_t size() const { return buf_.size(); }
  void clear() { buf_.clear(); }

  // Pure redaction. Two layers:
  //  1) registered `secrets` (>=4 chars) masked exactly - the RELIABLE layer;
  //     glue must addSecret() the Mistral key / Wi-Fi + admin passwords at startup.
  //  2) heuristic backstop for *unregistered* secrets (case-insensitive):
  //     "Bearer <tok>", key=value / "key":"value" for password/secret/token/
  //     api_key/csid/etc., and URL-embedded user:pass@host. Key matching is
  //     boundary-guarded so it will NOT mask "monkey=" / "compass=".
  // Never rely on layer 2 for a secret you can register.
  static std::string redact(const std::string& in, const std::vector<std::string>& secrets);

 private:
  size_t cap_;
  std::deque<std::string> buf_;
  std::vector<std::string> secrets_;
};

}  // namespace core
