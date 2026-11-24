#pragma once
// wire_shape/common.h - what every documented-shape checker shares (router design section 11,
// layer L2; lane E2-R3b). A checker answers whether one request, as the engine would send it, has
// the shape the provider documents: strict, so an unknown key, a wrong role or a wrong id form
// fails with one line naming where. No network, no lib/ code: the checkers judge bytes only.
// The Messages checker stays in test/test_plain_wire_anthropic (it reads the engine's request type).
#include <ArduinoJson.h>

#include <cctype>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wire_shape {

// One request as sent: method, path, headers in order, whether the socket is TLS, the body.
struct Request {
  std::string method = "POST";
  std::string path;
  std::vector<std::pair<std::string, std::string>> headers;
  bool tls = true;
  std::string body;
};

// The first failure wins; every check returns false through fail().
struct C {
  std::string why;
  bool fail(const std::string& w) {
    if (why.empty()) why = w;
    return false;
  }
};

inline constexpr size_t kStrGuard = 60000;   // nimbus::orch::kJsonStrGuard (wire_limits.h)

inline bool utf8Ok(const std::string& s) {
  const unsigned char* p = (const unsigned char*)s.data();
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    const unsigned char c = p[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (!len || i + len > n || (len == 2 && c < 0xC2) || (len == 4 && c > 0xF4)) return false;
    for (size_t k = 1; k < len; ++k)
      if ((p[i + k] & 0xC0) != 0x80) return false;
    if (len == 3 && c == 0xE0 && p[i + 1] < 0xA0) return false;   // overlong
    if (len == 3 && c == 0xED && p[i + 1] >= 0xA0) return false;  // surrogate
    if (len == 4 && c == 0xF0 && p[i + 1] < 0x90) return false;   // overlong
    if (len == 4 && c == 0xF4 && p[i + 1] >= 0x90) return false;  // over U+10FFFF
    i += len;
  }
  return true;
}

inline bool stringsWithinGuard(JsonVariantConst v) {
  if (v.is<const char*>()) return std::strlen(v.as<const char*>()) <= kStrGuard;
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>())
      if (!stringsWithinGuard(e)) return false;
    return true;
  }
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>())
      if (std::strlen(kv.key().c_str()) > kStrGuard || !stringsWithinGuard(kv.value())) return false;
  }
  return true;
}

// The body: no raw control byte, valid UTF-8, strict JSON nested at most 32 deep, every string
// inside the guard, an object at the top.
inline bool parseBody(const std::string& body, JsonDocument& d, C& c) {
  for (unsigned char ch : body)
    if (ch < 0x20) return c.fail("a raw control byte in the body");
  if (!utf8Ok(body)) return c.fail("the body is not valid UTF-8");
  if (deserializeJson(d, body, DeserializationOption::NestingLimit(32))) return c.fail("not JSON (or nested over 32)");
  if (!d.is<JsonObjectConst>()) return c.fail("the body is not an object");
  return stringsWithinGuard(d.as<JsonVariantConst>()) || c.fail("a string over the 60,000 B guard");
}

inline bool onlyKeys(JsonObjectConst o, std::initializer_list<const char*> keys, C& c, const std::string& where) {
  for (JsonPairConst kv : o) {
    bool known = false;
    for (const char* k : keys) known = known || kv.key() == k;
    if (!known) return c.fail(where + ": unknown key '" + kv.key().c_str() + "'");
  }
  return true;
}

inline std::string str(JsonVariantConst v) { return v.is<const char*>() ? v.as<const char*>() : ""; }

inline bool oneOf(const std::string& s, std::initializer_list<const char*> set) {
  for (const char* x : set)
    if (s == x) return true;
  return false;
}

// [A-Za-z0-9_-] plus the extra characters given, 1 to max bytes.
inline bool pattern(const std::string& s, size_t max, const char* extra = "") {
  if (s.empty() || s.size() > max) return false;
  for (char ch : s)
    if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '-' || (*extra && std::strchr(extra, ch)))) return false;
  return true;
}

// Text a provider counts as present: anything beyond ASCII white space.
inline bool visible(const std::string& s) {
  for (char ch : s)
    if (!std::isspace((unsigned char)ch)) return true;
  return false;
}

// A tool's JSON schema: an object schema.
inline bool objectSchema(JsonVariantConst v) { return v.is<JsonObjectConst>() && str(v["type"]) == "object"; }

// A JSON string that holds a JSON object (function-call arguments).
inline bool argsString(JsonVariantConst v) {
  if (!v.is<const char*>()) return false;
  JsonDocument d;
  return !deserializeJson(d, v.as<const char*>(), DeserializationOption::NestingLimit(32)) && d.is<JsonObjectConst>();
}

inline bool base64Ok(const char* s, size_t n) {
  if (!n || n % 4) return false;
  for (size_t i = 0; i < n; ++i) {
    const char ch = s[i];
    if (!(std::isalnum((unsigned char)ch) || ch == '+' || ch == '/' || (ch == '=' && i + 2 >= n))) return false;
  }
  return true;
}

// An image URL: https, or a base64 data URL of one of `formats` ("jpeg", "png", "gif", "webp").
inline bool imageUrl(const std::string& u, std::initializer_list<const char*> formats, C& c, const std::string& w) {
  if (u.rfind("https://", 0) == 0) return u.size() > 8 || c.fail(w + ": empty https URL");
  if (u.rfind("data:image/", 0) != 0) return c.fail(w + ": image_url is neither https nor a data URL");
  const size_t semi = u.find(";base64,");
  if (semi == std::string::npos) return c.fail(w + ": data URL is not base64");
  std::string fmt = u.substr(11, semi - 11);
  if (fmt == "jpg") fmt = "jpeg";
  if (!oneOf(fmt, formats)) return c.fail(w + ": image format '" + fmt + "' not accepted");
  return base64Ok(u.c_str() + semi + 8, u.size() - semi - 8) || c.fail(w + ": data is not base64");
}

// Headers by lower-cased name; a repeated header fails.
inline bool headerMap(const Request& r, std::map<std::string, std::string>& h, C& c) {
  for (const auto& kv : r.headers) {
    std::string k = kv.first;
    for (char& ch : k) ch = (char)std::tolower((unsigned char)ch);
    if (!h.emplace(k, kv.second).second) return c.fail("header repeated: " + k);
  }
  return true;
}

// POST to a path ending in `suffix`, JSON, and a Bearer key: exactly one on TLS (none allowed when
// `keyOptional`, the custom endpoint with no key), never one on a cleartext socket, and no
// Anthropic header.
inline bool bearerHeaders(const Request& r, const char* suffix, bool keyOptional, C& c) {
  if (r.method != "POST") return c.fail("method " + r.method);
  const size_t n = std::strlen(suffix);
  if (r.path.size() < n || r.path.compare(r.path.size() - n, n, suffix) != 0) return c.fail("path " + r.path);
  std::map<std::string, std::string> h;
  if (!headerMap(r, h, c)) return false;
  if (h["content-type"] != "application/json") return c.fail("content-type is not application/json");
  if (h.count("x-api-key") || h.count("anthropic-version")) return c.fail("an Anthropic header on a Bearer wire");
  const bool key = h.count("authorization") > 0;
  if (key && h["authorization"].rfind("Bearer ", 0) != 0) return c.fail("authorization is not Bearer");
  if (!r.tls) return !key || c.fail("a key on a cleartext socket");
  return key || keyOptional || c.fail("no key header");
}

}  // namespace wire_shape
