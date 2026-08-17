#include "nimbus/orch/fetch_policy.h"

namespace nimbus {
namespace orch {

uint32_t FetchQueue::request(FetchPolicy pol, const std::string& url,
                             const std::string& project, const std::string& name,
                             const std::string& by, std::string& err) {
  if (pol == FetchPolicy::Off) {
    err = "URL downloads are turned off by the owner (web: Usage -> Downloads). "
          "Say so - do not retry.";
    return 0;
  }
  if (url.empty() || project.empty() || name.empty()) {
    err = "url, project and name are all required";
    return 0;
  }
  // One live request per URL: a retry storm must not fill the queue with
  // duplicates of the same ask (and a pending approval must not be re-promptable).
  size_t active = 0;
  for (const auto& r : q_) {
    const bool live = r.state == FetchState::PendingApproval ||
                      r.state == FetchState::Ready || r.state == FetchState::Scanning;
    if (!live) continue;
    active++;
    if (r.url == url) {
      err = "already requested (id " + std::to_string(r.id) + ", " +
            fetchStateName(r.state) + ")";
      return 0;
    }
  }
  if (active >= kMax) {
    err = "download queue is full - wait for a pending request to finish";
    return 0;
  }
  FetchReq r;
  r.id = nextId_++;
  r.url = url;
  r.project = project;
  r.name = name;
  r.requestedBy = by;
  // Scan mode ALSO enters Ready: the pump downloads to QUARANTINE first and only
  // promotes on a SAFE verdict - the state advances Ready -> Scanning at the
  // moment the quarantine download completes (device-side), keeping "Ready"
  // uniformly "the pump may act on this now".
  r.state = (pol == FetchPolicy::Approve) ? FetchState::PendingApproval
                                          : FetchState::Ready;
  q_.push_back(std::move(r));
  return q_.back().id;
}

bool FetchQueue::approve(uint32_t id) {
  FetchReq* r = findMut(id);
  if (!r || r->state != FetchState::PendingApproval) return false;
  r->state = FetchState::Ready;
  return true;
}

bool FetchQueue::deny(uint32_t id) {
  FetchReq* r = findMut(id);
  if (!r || r->state != FetchState::PendingApproval) return false;
  r->state = FetchState::Denied;
  r->err = "denied by the owner";
  return true;
}

FetchReq* FetchQueue::firstIn(FetchState s) {
  for (auto& r : q_)
    if (r.state == s) return &r;
  return nullptr;
}

const FetchReq* FetchQueue::find(uint32_t id) const {
  for (const auto& r : q_)
    if (r.id == id) return &r;
  return nullptr;
}
FetchReq* FetchQueue::findMut(uint32_t id) {
  for (auto& r : q_)
    if (r.id == id) return &r;
  return nullptr;
}

void FetchQueue::finish(uint32_t id, FetchState s, const std::string& err,
                        uint64_t bytes) {
  FetchReq* r = findMut(id);
  if (!r) return;
  r->state = s;
  r->err = err;
  r->bytes = bytes;
  // Trim finished rows beyond the keep window (oldest first) so the queue can
  // never grow unbounded across a long uptime.
  size_t finished = 0;
  for (const auto& e : q_) {
    if (e.state == FetchState::Done || e.state == FetchState::Denied ||
        e.state == FetchState::Failed)
      finished++;
  }
  for (auto it = q_.begin(); finished > kDoneKeep && it != q_.end();) {
    if (it->state == FetchState::Done || it->state == FetchState::Denied ||
        it->state == FetchState::Failed) {
      it = q_.erase(it);
      finished--;
    } else {
      ++it;
    }
  }
}

int FetchQueue::pendingCount() const {
  int n = 0;
  for (const auto& r : q_)
    if (r.state == FetchState::PendingApproval) n++;
  return n;
}

// ---- URL parsing -------------------------------------------------------------

ParsedUrl parseHttpsUrl(const std::string& url) {
  ParsedUrl p;
  const std::string scheme = "https://";
  if (url.compare(0, scheme.size(), scheme) != 0) return p;   // https ONLY
  // ⚠ prism: the path is printf'd into the HTTP request line - a control byte
  // (CR/LF/space) in the URL is a header-injection primitive ("GET /x HTTP/1.0
  // \r\nHost: other-vhost" makes a shared host serve a DIFFERENT origin than
  // the one the owner approved and the scanner was told about). Reject any
  // byte < 0x21 or DEL anywhere in the URL, before any parsing.
  for (unsigned char c : url)
    if (c < 0x21 || c == 0x7F) return p;
  size_t hostStart = scheme.size();
  size_t pathStart = url.find('/', hostStart);
  std::string hostPort = (pathStart == std::string::npos)
                             ? url.substr(hostStart)
                             : url.substr(hostStart, pathStart - hostStart);
  if (hostPort.empty()) return p;
  // Refuse credentials-in-URL ("user:pass@host") - a phishing/confusion shape
  // with no legitimate use here.
  if (hostPort.find('@') != std::string::npos) return p;
  size_t colon = hostPort.find(':');
  if (colon != std::string::npos) {
    const std::string ps = hostPort.substr(colon + 1);
    if (ps.empty()) return p;
    long v = 0;
    for (char c : ps) {
      if (c < '0' || c > '9') return p;
      v = v * 10 + (c - '0');
      if (v > 65535) return p;
    }
    p.port = (uint16_t)v;
    p.host = hostPort.substr(0, colon);
  } else {
    p.host = hostPort;
  }
  if (p.host.empty()) return p;
  p.path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
  p.ok = true;
  return p;
}

std::string resolveRedirect(const ParsedUrl& from, const std::string& location) {
  if (location.empty()) return "";
  if (location.compare(0, 8, "https://") == 0) return location;   // absolute
  if (location.compare(0, 7, "http://") == 0) return "";          // no downgrade
  if (location.compare(0, 2, "//") == 0) return "";               // proto-relative: ambiguous
  if (location[0] == '/') {                                       // same-host absolute path
    std::string out = "https://" + from.host;
    if (from.port != 443) out += ":" + std::to_string(from.port);
    return out + location;
  }
  return "";   // relative paths: not followed (rare for downloads; keep it simple)
}

}  // namespace orch
}  // namespace nimbus
