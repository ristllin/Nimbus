#include "nimbus/cloud/http_replay.h"

#include <cctype>
#include <cstring>

namespace nimbus {
namespace cloud {
namespace http_replay {

namespace {

std::string lower(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = (char)std::tolower((unsigned char)c);
  return o;
}

// Stripped from the passed-through request headers (protocol.ts HOP_BY_HOP + the ones
// we set ourselves). Compared lowercase.
bool isDroppedRequestHeader(const std::string& nameLower) {
  static const char* kDrop[] = {"connection", "keep-alive", "proxy-authenticate",
                                "proxy-authorization", "te", "trailer",
                                "transfer-encoding", "upgrade", "host",
                                "content-length", "x-nimbus-token"};
  for (const char* d : kDrop)
    if (nameLower == d) return true;
  return false;
}

// Allowlist of response headers forwarded in the `res` frame.
bool isKeptResponseHeader(const std::string& nameLower) {
  static const char* kKeep[] = {"content-type",     "cache-control",   "etag",
                                "content-encoding", "content-disposition", "location"};
  for (const char* k : kKeep)
    if (nameLower == k) return true;
  return false;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Any control char (incl. CR/LF/NUL) in a value we splice into the request head is a
// header/request-line injection attempt: a CRLF in a tunneled method/path/header would
// smuggle extra lines onto the token-authenticated loopback socket (CL.TE desync). The
// relay is untrusted at the wire, so reject rather than trust.
bool hasCtl(const std::string& s) {
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return true;
  return false;
}

}  // namespace

std::string buildRequestHead(const std::string& method, const std::string& path,
                             const Headers& headers, size_t bodyLen, const std::string& token) {
  // A control char in the request line is a smuggling attempt: fail the whole request
  // (the caller turns an empty head into an error response) rather than emit it.
  if (hasCtl(method) || hasCtl(path)) return "";
  std::string r;
  r.reserve(256 + headers.size() * 32);
  r += method;
  r += ' ';
  r += path;
  r += " HTTP/1.1\r\n";
  r += "Host: 127.0.0.1\r\n";
  r += "X-Nimbus-Token: ";
  r += token;
  r += "\r\n";
  for (const auto& h : headers) {
    if (isDroppedRequestHeader(lower(h.first))) continue;
    if (hasCtl(h.first) || hasCtl(h.second)) continue;  // drop header-injection attempts
    r += h.first;
    r += ": ";
    r += h.second;
    r += "\r\n";
  }
  r += "Content-Length: ";
  r += std::to_string(bodyLen);
  r += "\r\n";
  r += "Connection: close\r\n";
  r += "\r\n";
  return r;
}

void ResponseParser::feed(const uint8_t* data, size_t len) {
  if (state_ == State::Done || state_ == State::Error) return;
  buf_.insert(buf_.end(), data, data + len);
  for (;;) {
    if (state_ == State::Head) {
      parseHead_();
      if (state_ == State::Head || state_ == State::Error) break;  // need more, or failed
      continue;
    }
    if (state_ == State::BodyLength) {
      size_t avail = buf_.size();
      size_t need = contentLength_ - bodyRead_;
      size_t take = avail < need ? avail : need;
      if (body_.size() + take > maxBodyBytes_) {
        take = maxBodyBytes_ > body_.size() ? maxBodyBytes_ - body_.size() : 0;
        overflow_ = true;
      }
      body_.insert(body_.end(), buf_.begin(), buf_.begin() + take);
      buf_.erase(buf_.begin(), buf_.begin() + take);
      bodyRead_ += take;
      if (overflow_ || bodyRead_ >= contentLength_) state_ = State::Done;
      break;
    }
    if (state_ == State::BodyChunked) {
      parseChunked_();
      break;
    }
    if (state_ == State::BodyUntilClose) {
      if (body_.size() + buf_.size() > maxBodyBytes_) {
        size_t take = maxBodyBytes_ > body_.size() ? maxBodyBytes_ - body_.size() : 0;
        body_.insert(body_.end(), buf_.begin(), buf_.begin() + take);
        overflow_ = true;
        state_ = State::Done;
      } else {
        body_.insert(body_.end(), buf_.begin(), buf_.end());
      }
      buf_.clear();
      break;
    }
    break;
  }
}

void ResponseParser::parseHead_() {
  // Find the end of the header block.
  std::string s(buf_.begin(), buf_.end());
  size_t end = s.find("\r\n\r\n");
  if (end == std::string::npos) return;  // need more bytes
  std::string head = s.substr(0, end);
  buf_.erase(buf_.begin(), buf_.begin() + end + 4);

  size_t eol = head.find("\r\n");
  std::string statusLine = eol == std::string::npos ? head : head.substr(0, eol);
  // "HTTP/1.1 200 OK"
  size_t sp = statusLine.find(' ');
  if (sp == std::string::npos) { state_ = State::Error; return; }
  status_ = atoi(statusLine.c_str() + sp + 1);
  if (status_ < 100 || status_ > 599) { state_ = State::Error; return; }

  bool chunked = false;
  bool haveLen = false;
  size_t pos = (eol == std::string::npos) ? head.size() : eol + 2;
  while (pos < head.size()) {
    size_t next = head.find("\r\n", pos);
    if (next == std::string::npos) next = head.size();
    std::string line = head.substr(pos, next - pos);
    pos = next + 2;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = lower(trim(line.substr(0, colon)));
    std::string val = trim(line.substr(colon + 1));
    if (name == "transfer-encoding") {
      if (lower(val).find("chunked") != std::string::npos) chunked = true;
    } else if (name == "content-length") {
      contentLength_ = (size_t)strtoul(val.c_str(), nullptr, 10);
      haveLen = true;
    }
    if (isKeptResponseHeader(name)) outHeaders_.emplace_back(line.substr(0, colon), val);
  }

  if (chunked) {
    state_ = State::BodyChunked;
  } else if (haveLen) {
    state_ = contentLength_ == 0 ? State::Done : State::BodyLength;
  } else {
    state_ = State::BodyUntilClose;
  }
}

void ResponseParser::parseChunked_() {
  // Repeatedly: <hex-size>[;ext]\r\n <data> \r\n ; terminated by a 0-size chunk.
  for (;;) {
    std::string s(buf_.begin(), buf_.end());
    size_t eol = s.find("\r\n");
    if (eol == std::string::npos) return;  // need the size line
    size_t sz = (size_t)strtoul(s.substr(0, eol).c_str(), nullptr, 16);
    if (sz == 0) {
      // Last chunk. Consume optional trailers up to the final blank line.
      size_t term = s.find("\r\n\r\n");
      if (term == std::string::npos && s.substr(eol) == "\r\n") {
        // Only "0\r\n" so far; wait for the closing CRLF.
        return;
      }
      state_ = State::Done;
      buf_.clear();
      return;
    }
    // Reject an absurd chunk size BEFORE the size arithmetic below can overflow a
    // 32-bit size_t (a malicious 0xFFFFFFFF chunk would wrap eol+2+sz+2 and slip past
    // the "wait for more" guard, then over-read the heap). A chunk larger than the whole
    // body cap is never legitimate.
    if (sz > maxBodyBytes_) {
      state_ = State::Error;
      return;
    }
    if (buf_.size() < eol + 2 + sz + 2) return;  // wait for data + trailing CRLF
    const uint8_t* data = buf_.data() + eol + 2;
    if (body_.size() + sz > maxBodyBytes_) {
      size_t take = maxBodyBytes_ > body_.size() ? maxBodyBytes_ - body_.size() : 0;
      if (take > sz) take = sz;  // never read past the chunk's actual bytes (OOB guard)
      body_.insert(body_.end(), data, data + take);
      overflow_ = true;
      state_ = State::Done;
      buf_.clear();
      return;
    }
    body_.insert(body_.end(), data, data + sz);
    buf_.erase(buf_.begin(), buf_.begin() + eol + 2 + sz + 2);
  }
}

void ResponseParser::endOfStream() {
  if (state_ == State::BodyUntilClose) {
    state_ = State::Done;  // until-close bodies end when the socket closes
  } else if (state_ == State::BodyLength && bodyRead_ < contentLength_) {
    state_ = State::Error;  // truncated
  } else if (state_ == State::Head || state_ == State::BodyChunked) {
    state_ = State::Error;  // incomplete
  }
}

}  // namespace http_replay
}  // namespace cloud
}  // namespace nimbus
