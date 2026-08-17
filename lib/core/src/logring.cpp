#include "nimbus/logring.h"

namespace core {

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Characters that may legitimately precede a credential key.
static bool isBoundary(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '&' || c == '?' ||
         c == ';' || c == ',' || c == '{' || c == '(' || c == '[' || c == '\'' ||
         c == '"' || c == ':' || c == '=' || c == '/' || c == '|' || c == '-';
}
// Characters that terminate a masked value.
static bool isValueDelim(char c) {
  return c == '"' || c == '\'' || c == ' ' || c == '\t' || c == '&' || c == ';' ||
         c == '}' || c == ',' || c == '\r' || c == '\n';
}

static size_t ciFind(const std::string& s, const std::string& m, size_t from) {
  if (m.empty() || m.size() > s.size()) return std::string::npos;
  for (size_t i = from; i + m.size() <= s.size(); i++) {
    bool ok = true;
    for (size_t j = 0; j < m.size(); j++)
      if (lc(s[i + j]) != lc(m[j])) { ok = false; break; }
    if (ok) return i;
  }
  return std::string::npos;
}

// Mask the token following `marker` (case-insensitive), e.g. "bearer ".
static void maskAfterCI(std::string& s, const std::string& marker) {
  if (marker.empty()) return;
  size_t pos = 0;
  while ((pos = ciFind(s, marker, pos)) != std::string::npos) {
    size_t v = pos + marker.size();
    size_t end = v;
    while (end < s.size() && !isValueDelim(s[end])) end++;
    if (end > v) { s.replace(v, end - v, "***"); pos = v + 3; } else { pos = v + 1; }
  }
}

// Mask the value of key=value or "key":"value" (case-insensitive). The key must
// be preceded by a boundary (so "monkey"/"compass" don't match "key"/"pass") and
// followed by '=' or ':' (so "password" doesn't match the shorter "pass" key).
static void maskKeyValue(std::string& s, const std::string& key) {
  if (key.empty()) return;
  size_t pos = 0;
  while (true) {
    size_t k = ciFind(s, key, pos);
    if (k == std::string::npos) break;
    if (k > 0 && !isBoundary(s[k - 1])) { pos = k + 1; continue; }
    size_t v = k + key.size();
    if (v < s.size() && s[v] == '"') v++;                      // optional closing quote of a JSON key
    while (v < s.size() && (s[v] == ' ' || s[v] == '\t')) v++;
    if (v >= s.size() || (s[v] != '=' && s[v] != ':')) { pos = k + key.size(); continue; }
    v++;                                                       // consume = or :
    while (v < s.size() && (s[v] == ' ' || s[v] == '\t' || s[v] == '"')) v++;  // skip ws + opening quote
    size_t end = v;
    while (end < s.size() && !isValueDelim(s[end])) end++;
    if (end > v) { s.replace(v, end - v, "***"); pos = v + 3; } else { pos = v + 1; }
  }
}

// Mask credentials embedded in URLs: scheme://user:pass@host
static void maskUrlCreds(std::string& s) {
  size_t pos = 0;
  while ((pos = s.find("://", pos)) != std::string::npos) {
    size_t start = pos + 3;
    size_t at = s.find('@', start);
    size_t slash = s.find('/', start);
    if (at == std::string::npos || (slash != std::string::npos && slash < at)) { pos = start; continue; }
    if (at > start) { s.replace(start, at - start, "***"); pos = start + 4; } else { pos = at + 1; }
  }
}

std::string LogRing::redact(const std::string& in, const std::vector<std::string>& secrets) {
  std::string s = in;
  // Layer 1: exact registered secrets - reliable regardless of format.
  for (const auto& sec : secrets) {
    if (sec.size() < 4) continue;
    size_t p = 0;
    while ((p = s.find(sec, p)) != std::string::npos) { s.replace(p, sec.size(), "***"); p += 3; }
  }
  // Layer 2: heuristic backstop.
  maskAfterCI(s, "bearer ");
  static const char* kKeys[] = {
      "password", "passwd", "secret", "access_token", "refresh_token",
      "api_key", "api-key", "apikey", "token", "csid", "pass", "key"};
  for (const char* k : kKeys) maskKeyValue(s, k);
  maskUrlCreds(s);
  return s;
}

void LogRing::addSecret(const std::string& secret) {
  if (!secret.empty()) secrets_.push_back(secret);
}

void LogRing::push(const std::string& line) {
  buf_.push_back(redact(line, secrets_));
  while (buf_.size() > cap_) buf_.pop_front();
}

std::vector<std::string> LogRing::lines() const {
  return std::vector<std::string>(buf_.begin(), buf_.end());
}

}  // namespace core
