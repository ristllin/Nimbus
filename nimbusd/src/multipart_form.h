#pragma once
// multipart_form - the web app's FormData posts, read into the urlencoded field list
// every /api/* handler parses (CUM-543).
//
// The page the device and a hosted instance share posts some settings as
// `new FormData(...)` (multipart/form-data): the Updates and battery toggles to
// /api/config, the cloud and sign-in forms. The device's web server parses both
// encodings; nimbusd parsed only urlencoded bodies, so a toggle's boundary line
// ("------WebKitFormBoundary...") was read as a field name and refused as one. The
// HTTP layer now hands a multipart body here first: its text parts become the same
// "name=value&..." body a urlencoded post carries (each name and value
// percent-encoded, so a value can never add a field), and every handler reads it
// unchanged. Bounded: at most kMaxParts parts, names of at most kMaxNameBytes and
// values of at most kMaxValueBytes (the whole body is already capped at 1 MB by
// the HTTP layer). A file part is refused here: only /api/files/upload takes one,
// and it reads the raw body itself.
#include <cctype>
#include <cstddef>
#include <string>

namespace nimbusd {
namespace form {

inline constexpr size_t kMaxParts = 64;
inline constexpr size_t kMaxNameBytes = 64;
inline constexpr size_t kMaxValueBytes = 64 * 1024;
inline constexpr size_t kMaxBoundary = 70;   // RFC 2046

enum class Result { Ok, Malformed, FilePart, TooLarge };

inline std::string lower(std::string s) {
  for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

// The boundary of a "multipart/form-data; boundary=..." Content-Type value (quoted
// or not, any case), or "" when the value is not multipart/form-data or carries no
// usable boundary.
inline std::string boundaryOf(const std::string& contentType) {
  const std::string low = lower(contentType);
  if (low.compare(0, 19, "multipart/form-data") != 0) return std::string();
  const size_t at = low.find("boundary=");
  if (at == std::string::npos) return std::string();
  size_t b = at + 9;
  size_t e;
  if (b < contentType.size() && contentType[b] == '"') {
    e = contentType.find('"', ++b);
    if (e == std::string::npos) return std::string();
  } else {
    e = contentType.find_first_of("; \t", b);
    if (e == std::string::npos) e = contentType.size();
  }
  const std::string bnd = contentType.substr(b, e - b);
  return bnd.size() <= kMaxBoundary ? bnd : std::string();
}

inline bool isMultipart(const std::string& contentType) {
  return lower(contentType).compare(0, 19, "multipart/form-data") == 0;
}

// Percent-encode everything but the unreserved characters, so the decoded field is
// byte-identical to the part's value and no value can carry a '&' or '=' of its own.
inline std::string pctEncode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string o;
  o.reserve(s.size());
  for (unsigned char ch : s) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      o += (char)ch;
    } else {
      o += '%';
      o += kHex[ch >> 4];
      o += kHex[ch & 15];
    }
  }
  return o;
}

// A quoted parameter of a Content-Disposition line (name="..." / filename="..."),
// matched as a whole parameter so `name` never matches inside `filename`.
inline bool dispositionParam(const std::string& line, const char* key, std::string& out) {
  const std::string low = lower(line);
  const std::string want = std::string(key) + "=\"";
  for (size_t p = low.find(want); p != std::string::npos; p = low.find(want, p + 1)) {
    if (p > 0 && low[p - 1] != ' ' && low[p - 1] != ';' && low[p - 1] != '\t') continue;
    const size_t v = p + want.size();
    const size_t q = line.find('"', v);
    if (q == std::string::npos) return false;
    out = line.substr(v, q - v);
    return true;
  }
  return false;
}

// One part's header block: its field name, and whether it is a file.
inline Result partName(const std::string& headers, std::string& name) {
  size_t at = 0;
  while (at < headers.size()) {
    size_t eol = headers.find("\r\n", at);
    if (eol == std::string::npos) eol = headers.size();
    const std::string line = headers.substr(at, eol - at);
    at = eol + 2;
    if (lower(line).compare(0, 20, "content-disposition:") != 0) continue;
    std::string file;
    if (dispositionParam(line, "filename", file)) return Result::FilePart;
    if (!dispositionParam(line, "name", name) || name.empty()) return Result::Malformed;
    return name.size() <= kMaxNameBytes ? Result::Ok : Result::TooLarge;
  }
  return Result::Malformed;
}

// The text parts of a multipart body as a urlencoded body ("k=v&k2=v2"). Anything
// but Ok leaves `out` unspecified.
inline Result toUrlencoded(const std::string& body, const std::string& boundary, std::string& out) {
  out.clear();
  if (boundary.empty()) return Result::Malformed;
  const std::string delim = "--" + boundary;
  if (body.compare(0, delim.size(), delim) != 0) return Result::Malformed;
  size_t at = delim.size();
  for (size_t parts = 0;; ++parts) {
    if (body.compare(at, 2, "--") == 0) return Result::Ok;   // the closing delimiter
    if (body.compare(at, 2, "\r\n") != 0) return Result::Malformed;
    if (parts == kMaxParts) return Result::TooLarge;
    const size_t hdrEnd = body.find("\r\n\r\n", at);
    if (hdrEnd == std::string::npos) return Result::Malformed;
    std::string name;
    const Result r = partName(body.substr(at + 2, hdrEnd - at - 2), name);
    if (r != Result::Ok) return r;
    const size_t start = hdrEnd + 4;
    const size_t end = body.find("\r\n" + delim, start);
    if (end == std::string::npos) return Result::Malformed;
    if (end - start > kMaxValueBytes) return Result::TooLarge;
    if (!out.empty()) out += '&';
    out += pctEncode(name) + "=" + pctEncode(body.substr(start, end - start));
    at = end + 2 + delim.size();
  }
}

}  // namespace form
}  // namespace nimbusd
