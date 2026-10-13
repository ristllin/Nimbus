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

// The media type multipart/form-data (the type token ends at ';', a space or the end).
inline bool isMultipart(const std::string& contentType) {
  const std::string low = lower(contentType);
  if (low.compare(0, 19, "multipart/form-data") != 0) return false;
  return low.size() == 19 || low[19] == ';' || low[19] == ' ' || low[19] == '\t';
}

// Where the value of parameter `key` starts in the lowercased header value `low`:
// just past "key=" where key is a whole parameter name (after ';', a space or a
// tab, so "name" never matches inside "filename"), or npos.
inline size_t paramValueAt(const std::string& low, const std::string& key) {
  const std::string want = key + "=";
  for (size_t p = low.find(want); p != std::string::npos; p = low.find(want, p + 1))
    if (p > 0 && (low[p - 1] == ';' || low[p - 1] == ' ' || low[p - 1] == '\t')) return p + want.size();
  return std::string::npos;
}

// The parameter value at `v` of the raw header value: quoted, or a bare token up to
// ';' or whitespace. False for an unterminated quote.
inline bool paramValue(const std::string& raw, size_t v, std::string& out) {
  if (v < raw.size() && raw[v] == '"') {
    const size_t q = raw.find('"', v + 1);
    if (q == std::string::npos) return false;
    out = raw.substr(v + 1, q - v - 1);
    return true;
  }
  const size_t e = raw.find_first_of("; \t", v);
  out = raw.substr(v, (e == std::string::npos ? raw.size() : e) - v);
  return true;
}

// The boundary of a "multipart/form-data; boundary=..." Content-Type value (quoted
// or not, any case), or "" when the value is not multipart/form-data or carries no
// usable boundary.
inline std::string boundaryOf(const std::string& contentType) {
  if (!isMultipart(contentType)) return std::string();
  const size_t v = paramValueAt(lower(contentType), "boundary");
  std::string bnd;
  if (v == std::string::npos || !paramValue(contentType, v, bnd)) return std::string();
  return bnd.size() <= kMaxBoundary ? bnd : std::string();
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

// One header line of a part: a file marker (a filename in any form, or a content
// type other than text/plain: browsers send one with every file part and none with a
// text field), the field name, or neither.
inline Result partHeader(const std::string& line, bool& named, std::string& name) {
  const std::string low = lower(line);
  if (low.compare(0, 13, "content-type:") == 0)
    return low.find("text/plain") == std::string::npos ? Result::FilePart : Result::Ok;
  if (low.compare(0, 20, "content-disposition:") != 0) return Result::Ok;
  if (named) return Result::Malformed;   // one Content-Disposition per part
  named = true;
  if (paramValueAt(low, "filename") != std::string::npos || paramValueAt(low, "filename*") != std::string::npos)
    return Result::FilePart;
  const size_t v = paramValueAt(low, "name");
  return v != std::string::npos && paramValue(line, v, name) ? Result::Ok : Result::Malformed;
}

// One part's header block: its field name, or why the part is refused.
inline Result partName(const std::string& headers, std::string& name) {
  bool named = false;
  for (size_t at = 0; at < headers.size();) {
    size_t eol = headers.find("\r\n", at);
    if (eol == std::string::npos) eol = headers.size();
    const Result r = partHeader(headers.substr(at, eol - at), named, name);
    if (r != Result::Ok) return r;
    at = eol + 2;
  }
  if (name.empty()) return Result::Malformed;
  return name.size() <= kMaxNameBytes ? Result::Ok : Result::TooLarge;
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
    // A part with no header block at all (the delimiter line runs straight into the
    // blank line) has no field name.
    const size_t hdrEnd = body.find("\r\n\r\n", at);
    if (hdrEnd == std::string::npos || hdrEnd == at) return Result::Malformed;
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
