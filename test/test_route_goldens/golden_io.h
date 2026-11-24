#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

// golden_io.h - reading, writing and comparing the route goldens (lane E2-R2a). A golden is
// compared as whole bytes; a difference is reported with the golden's path, the first differing
// byte offset, its line and column, and both sides around it, so a one-byte change in the wire
// names the fixture and the place. ROUTE_GOLDENS_WRITE=1 (tools/router/refresh_goldens.py)
// writes the rendered bytes instead of comparing them.
namespace route_goldens {

inline constexpr const char* kRoot = "test/support/goldens/wire/";

inline bool writeMode() {
  const char* e = std::getenv("ROUTE_GOLDENS_WRITE");
  return e && std::strcmp(e, "1") == 0;
}

inline bool readFile(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

// Write `bytes` to `path`, making its folders first (write mode only).
inline bool writeFile(const std::string& path, const std::string& bytes) {
  for (size_t i = path.find('/'); i != std::string::npos; i = path.find('/', i + 1)) ::mkdir(path.substr(0, i).c_str(), 0755);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

// FNV-1a, 64 bit: pins bytes too long to keep whole in a text golden (signed thinking).
inline uint64_t fnv64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

inline std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
  return b;
}

// Bytes as one printable line: quotes, backslash and every byte outside 0x20..0x7E escaped.
inline std::string quoted(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += (char)c;
    } else if (c >= 0x20 && c < 0x7F) {
      o += (char)c;
    } else {
      char b[5];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    }
  }
  return o + "\"";
}

// The first offset where two byte strings differ; npos when equal.
inline size_t firstDiff(const std::string& a, const std::string& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i)
    if (a[i] != b[i]) return i;
  return a.size() == b.size() ? std::string::npos : n;
}

inline std::string around(const std::string& s, size_t at) {
  const size_t from = at > 40 ? at - 40 : 0;
  return quoted(s.substr(from, at - from)) + " >>> " + quoted(s.substr(at, 40));
}

// The report for a golden that differs: which golden, where, and both sides.
inline std::string diffReport(const std::string& path, const std::string& golden, const std::string& rendered) {
  const size_t at = firstDiff(golden, rendered);
  if (at == std::string::npos) return std::string();
  size_t line = 1, col = 1;
  for (size_t i = 0; i < at; ++i) {
    col = golden[i] == '\n' ? 1 : col + 1;
    line += golden[i] == '\n';
  }
  return "golden " + path + " differs at byte " + std::to_string(at) + " (line " + std::to_string(line) + ", column " +
         std::to_string(col) + "; golden " + std::to_string(golden.size()) + " B, rendered " +
         std::to_string(rendered.size()) + " B)\n  golden:   " + around(golden, at) + "\n  rendered: " +
         around(rendered, at);
}

// Compare `rendered` with the golden at `path` (or write it in write mode). Empty: it matches.
inline std::string checkGolden(const std::string& path, const std::string& rendered) {
  if (writeMode()) return writeFile(path, rendered) ? std::string() : "cannot write golden " + path;
  std::string golden;
  if (!readFile(path, golden))
    return "golden " + path + " is missing (tools/router/refresh_goldens.py writes it)";
  return diffReport(path, golden, rendered);
}

}  // namespace route_goldens
