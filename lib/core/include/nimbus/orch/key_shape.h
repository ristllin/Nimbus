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
  // Sent inside the request path (Telegram's /bot<token>/...), where no space is
  // ever harmless; every other credential rides a header value.
  bool inPath;
};

// Every credential field POST /api/orch stores. Lengths are loose bounds around
// each provider's real shape (a Mistral key is 32 characters, a Telegram bot token
// about 46, an Anthropic key about 108), wide enough for format changes and proxy
// keys; a custom endpoint's key can be anything printable.
inline const KeyShape* keyShapeFor(const std::string& field) {
  static const KeyShape kShapes[] = {
      {"oaiKey", "OpenAI key", "key", 20, 512, false},
      {"antKey", "Anthropic key", "key", 20, 512, false},
      {"mistKey", "Mistral key", "key", 16, 256, false},
      {"zaiKey", "Z.ai key", "key", 16, 256, false},
      {"cumuloKey", "Cumulo key", "key", 16, 512, false},
      {"tavKey", "Tavily key", "key", 16, 256, false},
      {"custKey", "custom endpoint key", "key", 1, 1024, false},
      {"tgToken", "Telegram bot token", "token", 20, 128, true},
  };
  for (const KeyShape& s : kShapes)
    if (field == s.field) return &s;
  return nullptr;
}

// Spaces around a header value are tolerated as before (HTTP drops them as
// optional whitespace); only the value between them is checked. A tab is never
// tolerated, and a path credential gets no trim at all.
inline std::string trimSpaces(const std::string& v) {
  const size_t b = v.find_first_not_of(' ');
  if (b == std::string::npos) return std::string();
  return v.substr(b, v.find_last_not_of(' ') - b + 1);
}

// True when every byte is printable ASCII other than a space (what every key and
// token format uses): no CR, LF, tab, other control byte, space, DEL or non-ASCII.
inline bool credentialCharsOk(const std::string& v) {
  for (unsigned char c : v)
    if (c < 0x21 || c > 0x7E) return false;
  return true;
}

// "" when the posted value fits the shape, else the owner-facing error. Only an
// empty value is unchecked (the handlers treat it as "leave the stored one"); a
// value of spaces alone would replace the stored key, so it is refused.
inline std::string keyShapeError(const KeyShape& s, const std::string& posted) {
  if (posted.empty()) return std::string();
  const std::string value = s.inPath ? posted : trimSpaces(posted);
  const std::string that = std::string("That ") + s.noun;
  if (value.empty())
    return that + " is blank. Paste the whole " + s.thing + " and save again.";
  if (!credentialCharsOk(value))
    return that + " has a space or a hidden character in it. Paste just the " + s.thing +
           " and save again.";
  if (value.size() < s.minLen)
    return that + " looks too short. Paste the whole " + s.thing + " and save again.";
  if (value.size() > s.maxLen)
    return that + " looks too long. Paste just the " + s.thing + " and save again.";
  return std::string();
}

// The connector-token form (POST /api/connectors "tok"): any length, same bytes;
// it rides a header value, so spaces around it are tolerated like a key's.
inline std::string connectorTokenError(const std::string& connectorName, const std::string& tok) {
  if (tok.empty()) return std::string();
  const std::string value = trimSpaces(tok);
  if (value.empty())
    return "The token for " + connectorName + " is blank. Paste the whole token and save again.";
  if (credentialCharsOk(value)) return std::string();
  return "The token for " + connectorName +
         " has a space or a hidden character in it. Paste just the token and save again.";
}

}  // namespace orch
}  // namespace nimbus
