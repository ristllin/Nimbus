#include "nimbus/audio_req.h"

#include <ArduinoJson.h>

namespace core {

static void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xff));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
}
static void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xff));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
  v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
}
static void put4(std::vector<uint8_t>& v, const char* tag) {  // always exactly 4 bytes
  for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>(tag[i]));
}

std::vector<uint8_t> wavHeader(uint32_t sampleRate, uint16_t channels,
                               uint16_t bits, uint32_t dataBytes) {
  // PCM must have whole bytes/sample and non-zero format; reject otherwise so a
  // bad config can't silently emit a malformed header.
  if (bits == 0 || (bits % 8) != 0 || channels == 0 || sampleRate == 0) return {};
  uint64_t br = static_cast<uint64_t>(sampleRate) * channels * (bits / 8);  // overflow-safe
  uint32_t byteRate = (br > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : static_cast<uint32_t>(br);
  uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8));
  std::vector<uint8_t> h;
  h.reserve(44);
  put4(h, "RIFF"); put32(h, 36 + dataBytes); put4(h, "WAVE");
  put4(h, "fmt "); put32(h, 16); put16(h, 1); put16(h, channels);
  put32(h, sampleRate); put32(h, byteRate); put16(h, blockAlign); put16(h, bits);
  put4(h, "data"); put32(h, dataBytes);
  return h;  // exactly 44 bytes
}

// Strip CR/LF/quotes so a value can't break out of its multipart header line
// (prevents header injection via filename / model / mime).
static std::string clean(const std::string& v) {
  std::string o;
  o.reserve(v.size());
  for (char c : v)
    if (c != '\r' && c != '\n' && c != '"') o.push_back(c);
  return o;
}

Multipart buildSttUpload(const std::string& boundaryIn, const std::string& modelIn,
                         const std::string& filenameIn, const std::string& mimeIn,
                         const std::string& languageIn) {
  std::string b = clean(boundaryIn), model = clean(modelIn), filename = clean(filenameIn),
              mime = clean(mimeIn), language = clean(languageIn);
  Multipart m;
  m.contentType = "multipart/form-data; boundary=" + b;
  std::string h;
  h += "--" + b + "\r\n";
  h += "Content-Disposition: form-data; name=\"model\"\r\n\r\n" + model + "\r\n";
  if (!language.empty()) {
    h += "--" + b + "\r\n";
    h += "Content-Disposition: form-data; name=\"language\"\r\n\r\n" + language + "\r\n";
  }
  h += "--" + b + "\r\n";
  h += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
  h += "Content-Type: " + mime + "\r\n\r\n";
  m.head = h;
  m.tail = "\r\n--" + b + "--\r\n";
  return m;
}

size_t readHttpBody(ByteReader& src, long contentLen, size_t cap, std::string& out) {
  out.clear();
  if (contentLen > 0 && static_cast<size_t>(contentLen) < cap)
    out.reserve(static_cast<size_t>(contentLen));
  uint8_t buf[256];
  while (out.size() < cap) {
    if (contentLen >= 0 && out.size() >= static_cast<size_t>(contentLen)) break;  // whole body read
    if (src.timedOut()) break;
    int avail = src.available();
    if (avail > 0) {
      size_t room = cap - out.size();
      if (contentLen >= 0) {  // never read past the declared body into trailing bytes
        size_t remain = static_cast<size_t>(contentLen) - out.size();
        if (remain < room) room = remain;
      }
      int want = avail;
      if (want > static_cast<int>(sizeof(buf))) want = static_cast<int>(sizeof(buf));
      if (static_cast<size_t>(want) > room) want = static_cast<int>(room);
      int n = src.read(buf, want);
      if (n > 0) out.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
      else if (n < 0) break;  // read error
      // n == 0: nothing this pass; loop re-checks connected()/timedOut()
    } else if (!src.connected()) {
      break;  // server closed -> body complete (the HTTP/1.0 Connection: close case)
    } else {
      src.idle();  // bytes not ready yet - yield and retry until timeout
    }
  }
  return out.size();
}

bool dechunkHttpBody(std::string& body) {
  std::string out;
  out.reserve(body.size());
  size_t pos = 0;
  bool decodedAny = false;
  for (;;) {
    // Chunk-size line: <hex>[;extensions]\r\n. Hex must start immediately.
    size_t lineEnd = body.find("\r\n", pos);
    if (lineEnd == std::string::npos) break;  // truncated size line -> keep what we have
    size_t sz = 0;
    size_t i = pos;
    bool anyHex = false;
    for (; i < lineEnd; i++) {
      char ch = body[i];
      int d;
      if (ch >= '0' && ch <= '9') d = ch - '0';
      else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
      else break;                                // ';' extension or junk ends the hex
      if (sz > (SIZE_MAX >> 4)) return false;    // absurd size -> not chunked framing
      sz = (sz << 4) | static_cast<size_t>(d);
      anyHex = true;
    }
    if (!anyHex) return false;  // no hex where a size must be -> not chunked at all
    if (sz == 0) { decodedAny = true; break; }   // terminal chunk (trailers ignored)
    size_t dataStart = lineEnd + 2;
    size_t take = body.size() - dataStart;
    if (take > sz) take = sz;                    // truncation keeps the partial chunk
    out.append(body, dataStart, take);
    decodedAny = true;
    if (take < sz) break;                        // connection-close truncation
    pos = dataStart + sz;
    // The CRLF after the chunk data; tolerate its absence - or just its lone
    // leading '\r' - at a connection-close-truncated tail (the decoded prefix
    // must survive truncation at ANY byte).
    if (pos + 2 <= body.size() && body[pos] == '\r' && body[pos + 1] == '\n') pos += 2;
    else if (pos >= body.size() ||
             (pos + 1 == body.size() && body[pos] == '\r')) break;
    else return false;                           // data not followed by CRLF -> not chunked
  }
  if (!decodedAny) return false;
  body.swap(out);
  return true;
}

std::string parseTranscription(const char* json, bool* ok) {
  if (ok) *ok = false;
  if (!json || !*json) return {};
  JsonDocument doc;
  if (deserializeJson(doc, json)) return {};  // truncated/garbage -> ok stays false
  if (ok) *ok = true;
  const char* text = doc["text"] | "";
  std::string s(text);
  const char* ws = " \t\r\n\f\v";
  size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return {};  // all whitespace / empty
  size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
}

}  // namespace core
