#include "nimbus/orch/episodic.h"

#include <algorithm>
#include <cstring>

namespace nimbus {
namespace orch {

namespace {
const char* kKindNames[] = {"message", "tool_output", "llm_response",
                            "file",    "image",       "audio",
                            "transcript", "log"};
}

bool epiTextMatch(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return true;
  auto lower = [](const std::string& s) {
    std::string o = s;
    for (char& c : o)
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
  };
  const std::string h = lower(hay);
  const std::string n = lower(needle);
  size_t i = 0;
  bool any = false;
  while (i < n.size()) {
    while (i < n.size() && (n[i] == ' ' || n[i] == '\t' || n[i] == '\n')) i++;
    size_t start = i;
    while (i < n.size() && n[i] != ' ' && n[i] != '\t' && n[i] != '\n') i++;
    if (i > start) {
      any = true;
      if (h.find(n.substr(start, i - start)) == std::string::npos) return false;
    }
  }
  return any ? true : true;   // whitespace-only needle filters nothing
}

int textMatchScore(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return 0;
  auto lower = [](const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
  };
  const std::string h = lower(hay), n = lower(needle);
  int total = 0;
  size_t i = 0;
  while (i < n.size()) {
    while (i < n.size() && (n[i] == ' ' || n[i] == '\t' || n[i] == '\n')) i++;
    size_t start = i;
    while (i < n.size() && n[i] != ' ' && n[i] != '\t' && n[i] != '\n') i++;
    if (i > start) {
      const std::string term = n.substr(start, i - start);
      int count = 0;
      for (size_t at = h.find(term); at != std::string::npos; at = h.find(term, at + term.size()))
        count++;
      if (count == 0) return 0;   // all-of-terms gate
      total += count;
    }
  }
  return total;
}

uint32_t epiIdSuffix(const std::string& id) {
  // "m0000a1f3" -> 0xa1f3. Any trailing hex run counts; 0 when there is none.
  size_t i = id.size();
  while (i > 0) {
    const char c = id[i - 1];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) break;
    i--;
  }
  if (i >= id.size()) return 0;
  uint32_t v = 0;
  for (size_t k = i; k < id.size(); k++) {
    const char c = id[k];
    const uint32_t d = (c <= '9') ? (uint32_t)(c - '0')
                                  : (uint32_t)((c | 0x20) - 'a' + 10);
    v = v * 16 + d;
  }
  return v;
}

const char* kindName(MsgKind k) {
  int i = (int)k;
  return (i >= 0 && i < 8) ? kKindNames[i] : "message";
}
bool kindFromName(const char* s, MsgKind& out) {
  if (!s) return false;
  for (int i = 0; i < 8; i++)
    if (std::strcmp(s, kKindNames[i]) == 0) { out = (MsgKind)i; return true; }
  return false;
}

void InMemoryEpisodicStore::addSession(const EpisodicSession& s) {
  for (auto& e : sessions_)
    if (e.id == s.id) { e = s; return; }  // upsert by id
  sessions_.push_back(s);
}

bool InMemoryEpisodicStore::setSessionStatus(const std::string& id, const std::string& status) {
  for (auto& e : sessions_)
    if (e.id == id) { e.status = status; return true; }
  return false;
}

void InMemoryEpisodicStore::addMessage(const EpisodicMessage& m) {
  msgs_.push_back(m);
  // ring cap: drop the oldest when over budget (device SQLite has no cap).
  if ((int)msgs_.size() > cap_)
    msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - cap_));
}

std::vector<EpisodicSession> InMemoryEpisodicStore::sessions(const std::string& status) const {
  std::vector<EpisodicSession> out;
  for (const auto& s : sessions_)
    if (status.empty() || s.status == status) out.push_back(s);
  return out;
}

// ---- binary persistence (device: LittleFS /data/episodic.bin) ----
namespace {
void putU32(std::string& b, uint32_t v) {
  b.push_back((char)(v & 0xFF)); b.push_back((char)((v >> 8) & 0xFF));
  b.push_back((char)((v >> 16) & 0xFF)); b.push_back((char)((v >> 24) & 0xFF));
}
void putStr(std::string& b, const std::string& s) {
  putU32(b, (uint32_t)s.size()); b.append(s);
}
bool getU32(const std::string& b, size_t& p, uint32_t& out) {
  if (p + 4 > b.size()) return false;
  out = (uint8_t)b[p] | ((uint8_t)b[p + 1] << 8) | ((uint8_t)b[p + 2] << 16) |
        ((uint32_t)(uint8_t)b[p + 3] << 24);
  p += 4; return true;
}
bool getStr(const std::string& b, size_t& p, std::string& out) {
  uint32_t n;
  if (!getU32(b, p, n)) return false;
  if (p + n > b.size() || n > (1u << 20)) return false;  // bound: no >1 MB field
  out.assign(b, p, n); p += n; return true;
}
}  // namespace

std::string InMemoryEpisodicStore::serialize() const {
  std::string b = "EP01";
  putU32(b, (uint32_t)sessions_.size());
  for (const auto& s : sessions_) {
    putStr(b, s.id); putU32(b, s.startedHours);
    putStr(b, s.provider); putStr(b, s.title); putStr(b, s.status);
  }
  putU32(b, (uint32_t)msgs_.size());
  for (const auto& m : msgs_) {
    putStr(b, m.id); putStr(b, m.sessionId); putU32(b, m.tsHours);
    putStr(b, m.role); b.push_back((char)(uint8_t)m.kind);
    putStr(b, m.text); putStr(b, m.blobPath); putStr(b, m.tags);
  }
  return b;
}

bool InMemoryEpisodicStore::deserialize(const std::string& blob) {
  sessions_.clear();
  msgs_.clear();
  if (blob.size() < 4 || blob.compare(0, 4, "EP01") != 0) return false;
  size_t p = 4;
  uint32_t sc;
  if (!getU32(blob, p, sc)) return false;
  for (uint32_t i = 0; i < sc; i++) {
    EpisodicSession s;
    if (!getStr(blob, p, s.id) || !getU32(blob, p, s.startedHours) ||
        !getStr(blob, p, s.provider) || !getStr(blob, p, s.title) ||
        !getStr(blob, p, s.status))
      return false;  // truncated - keep what parsed
    sessions_.push_back(std::move(s));
  }
  uint32_t mc;
  if (!getU32(blob, p, mc)) return false;
  for (uint32_t i = 0; i < mc; i++) {
    EpisodicMessage m;
    std::string kindByte;
    if (!getStr(blob, p, m.id) || !getStr(blob, p, m.sessionId) ||
        !getU32(blob, p, m.tsHours) || !getStr(blob, p, m.role))
      return false;
    if (p >= blob.size()) return false;
    m.kind = (MsgKind)(uint8_t)blob[p++];
    if (!getStr(blob, p, m.text) || !getStr(blob, p, m.blobPath) ||
        !getStr(blob, p, m.tags))
      return false;
    addMessage(m);  // honors the ring cap
  }
  return true;
}

std::vector<EpisodicMessage> InMemoryEpisodicStore::query(const MsgQuery& q) const {
  std::vector<EpisodicMessage> out;
  const uint32_t beforeSfx = epiIdSuffix(q.before);
  // Walk newest-first so `limit` keeps the most-recent matches (read_recent
  // reads reversed). msgs_ is oldest-first, so iterate in reverse.
  for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
    const EpisodicMessage& m = *it;
    if (beforeSfx && epiIdSuffix(m.id) >= beforeSfx) continue;   // paging cursor
    if (!q.sessionId.empty() && m.sessionId != q.sessionId) continue;
    if (!q.sessionVisible(m.sessionId)) continue;   // v3.7.0 read boundary
    if (!q.kindVisible(m.kind)) continue;
    if (q.sinceHours && m.tsHours < q.sinceHours) continue;
    if (q.beforeHours && m.tsHours >= q.beforeHours) continue;
    if (!epiTextMatch(m.text, q.textContains)) continue;
    out.push_back(m);
    if (q.limit > 0 && (int)out.size() >= q.limit) break;
  }
  return out;
}

}  // namespace orch
}  // namespace nimbus
