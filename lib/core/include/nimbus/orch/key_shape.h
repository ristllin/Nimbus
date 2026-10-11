#pragma once
#include <cstddef>
#include <string>

// key_shape - the shape check a posted credential passes before it is stored
// (CUM-514). A provider key, bot token or connector token is later sent raw inside
// an HTTP request it builds by hand ("Authorization: Bearer <key>\r\n", or the
// Telegram /bot<token>/ path), so a CR or LF in it would add header lines to every
// request to that provider, and any other control byte or a space breaks the
// request. Portable and host-tested (test/test_key_shape); the web handlers
// (POST /api/orch, POST /api/connectors) refuse a bad value with the plain error
// below and store nothing. A value that passes is stored exactly as posted.
namespace nimbus {
namespace orch {

struct KeyShape {
  const char* field;   // the POST /api/orch field
  const char* noun;    // owner-facing ("OpenAI key")
  const char* thing;   // "key" or "token"
  size_t minLen, maxLen;
};

// Every credential field POST /api/orch stores. Lengths are loose bounds around
// each provider's real shape (a Mistral key is 32 characters, a Telegram bot token
// about 46, an Anthropic key about 108), wide enough for format changes and proxy
// keys; a custom endpoint's key can be anything printable.
inline const KeyShape* keyShapeFor(const std::string& field) {
  static const KeyShape kShapes[] = {
      {"oaiKey", "OpenAI key", "key", 20, 512},
      {"antKey", "Anthropic key", "key", 20, 512},
      {"mistKey", "Mistral key", "key", 16, 256},
      {"zaiKey", "Z.ai key", "key", 16, 256},
      {"cumuloKey", "Cumulo key", "key", 16, 512},
      {"tavKey", "Tavily key", "key", 16, 256},
      {"custKey", "custom endpoint key", "key", 1, 1024},
      {"tgToken", "Telegram bot token", "token", 20, 128},
  };
  for (const KeyShape& s : kShapes)
    if (field == s.field) return &s;
  return nullptr;
}

// Spaces or tabs around a value are tolerated as before (HTTP drops them around a
// header value); only the value between them is checked.
inline std::string trimCredential(const std::string& v) {
  const char* ws = " \t";
  const size_t b = v.find_first_not_of(ws);
  if (b == std::string::npos) return std::string();
  return v.substr(b, v.find_last_not_of(ws) - b + 1);
}

// True when every byte is printable ASCII other than a space (what every key and
// token format uses): no CR, LF, tab, other control byte, space, DEL or non-ASCII.
inline bool credentialCharsOk(const std::string& v) {
  for (unsigned char c : v)
    if (c < 0x21 || c > 0x7E) return false;
  return true;
}

// "" when the value fits the shape, else the owner-facing error. An empty (or
// all-space) value is not checked: the handlers treat it as "leave the stored one".
inline std::string keyShapeError(const KeyShape& s, const std::string& posted) {
  const std::string value = trimCredential(posted);
  if (value.empty()) return std::string();
  const std::string that = std::string("That ") + s.noun;
  if (!credentialCharsOk(value))
    return that + " has a space or a hidden character in it. Paste just the " + s.thing +
           " and save again.";
  if (value.size() < s.minLen)
    return that + " looks too short. Paste the whole " + s.thing + " and save again.";
  if (value.size() > s.maxLen)
    return that + " looks too long. Paste just the " + s.thing + " and save again.";
  return std::string();
}

// The connector-token form (POST /api/connectors "tok"): any length, same bytes.
inline std::string connectorTokenError(const std::string& connectorName, const std::string& tok) {
  if (tok.empty() || credentialCharsOk(tok)) return std::string();
  return "The token for " + connectorName +
         " has a space or a hidden character in it. Paste just the token and save again.";
}

}  // namespace orch
}  // namespace nimbus
