#include "nimbus/orch/episodic_log.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <map>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>

#include "nimbus/orch/blob_store.h"
#include "nimbus/orch/retention.h"

namespace nimbus {
namespace orch {

namespace {
// ArduinoJson allocator over the working-set hooks (PSRAM on the device, malloc
// on the host), so the document behind every decode of a stored row stays off
// the internal heap. ArduinoJson grows its pools with reallocate(), which the
// hooks do not offer, so each block carries its size in a small header.
struct WorkingJsonAllocator : ArduinoJson::Allocator {
  static constexpr size_t kHdr = 16;   // keeps the payload 16-byte aligned
  void* allocate(size_t n) override {
    unsigned char* p = static_cast<unsigned char*>(workingAlloc(n + kHdr));
    if (!p) return nullptr;
    std::memcpy(p, &n, sizeof(n));
    return p + kHdr;
  }
  void deallocate(void* q) override {
    if (q) workingFree(static_cast<unsigned char*>(q) - kHdr);
  }
  void* reallocate(void* q, size_t n) override {
    if (!q) return allocate(n);
    size_t old = 0;
    std::memcpy(&old, static_cast<unsigned char*>(q) - kHdr, sizeof(old));
    void* r = allocate(n);
    if (!r) return nullptr;   // realloc contract: the old block stays valid
    std::memcpy(r, q, old < n ? old : n);
    deallocate(q);
    return r;
  }
  static WorkingJsonAllocator& instance() {
    static WorkingJsonAllocator a;
    return a;
  }
};

// Decode one stored line into `d`. False on a torn/garbage line (a torn line
// lacks the first key).
bool decodeDoc(const char* data, size_t len, JsonDocument& d) {
  if (!data || len == 0) return false;
  if (deserializeJson(d, data, len) != DeserializationError::Ok) return false;
  return d["id"].is<const char*>();
}

// The hash a blob path or name carries (blobHashOf's rule) as a view: the
// basename up to its first '.', empty when there is none.
std::string_view blobHashView(const char* p, size_t n) {
  size_t start = 0;
  for (size_t i = n; i > 0; --i)
    if (p[i - 1] == '/') { start = i; break; }
  size_t end = start;
  while (end < n && p[end] != '.') ++end;
  return std::string_view(p + start, end - start);
}
}  // namespace


// ---- JSONL codec ------------------------------------------------------------
// Compact keys keep the day-streams small; ArduinoJson handles escaping of text
// that contains quotes/newlines/backslashes (the reason we don't hand-roll it).
std::string encodeEpisodicLine(const EpisodicMessage& m) {
  JsonDocument d;
  d["id"] = m.id;
  d["sid"] = m.sessionId;
  d["ts"] = m.tsHours;
  d["role"] = m.role;
  d["kind"] = kindName(m.kind);
  d["text"] = m.text;
  if (!m.blobPath.empty()) d["blob"] = m.blobPath;
  if (!m.tags.empty()) d["tags"] = m.tags;
  std::string out;
  serializeJson(d, out);
  return out;
}

bool decodeEpisodicLine(const std::string& line, EpisodicMessage& out) {
  return decodeEpisodicLine(line.data(), line.size(), out);
}

bool decodeEpisodicLine(const char* data, size_t len, EpisodicMessage& out) {
  JsonDocument d(&WorkingJsonAllocator::instance());
  if (!decodeDoc(data, len, d)) return false;
  out.id = d["id"] | "";
  out.sessionId = d["sid"] | "";
  out.tsHours = d["ts"] | (uint32_t)0;
  out.role = d["role"] | "";
  MsgKind k = MsgKind::Message;
  kindFromName(d["kind"] | "message", k);
  out.kind = k;
  out.text = d["text"] | "";
  out.blobPath = d["blob"] | "";
  out.tags = d["tags"] | "";
  return true;
}

// ---- civil date from an epoch-day number (Howard Hinnant's algorithm) -------
std::string AppendLogEpisodicStore::civilDate(uint32_t dayNum) {
  int64_t z = (int64_t)dayNum + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;                            // [0, 146096]
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0,399]
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);     // [0,365]
  int64_t mp = (5 * doy + 2) / 153;                          // [0,11]
  int64_t d = doy - (153 * mp + 2) / 5 + 1;                  // [1,31]
  int64_t m = mp < 10 ? mp + 3 : mp - 9;                     // [1,12]
  y += (m <= 2);
  char buf[16];
  snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld", (long long)y, (long long)m,
           (long long)d);
  return std::string(buf);
}

// Inverse of civilDate: "2026-07-16.jsonl" -> epoch-day. Deriving the day from
// the NAME costs no read; the previous probe (decode the file's first record)
// returned nothing for a file under the probe size and silently produced day 0.
uint32_t dayNumFromName(const std::string& name) {
  int y = 0, m = 0, d = 0;
  if (name.size() < 10) return 0;
  if (sscanf(name.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return 0;
  if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
  int64_t yy = y - (m <= 2);
  const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const unsigned yoe = (unsigned)(yy - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + (int64_t)doe - 719468;
  return days < 0 ? 0 : (uint32_t)days;
}

bool parseDayStreamName(const char* name, uint32_t& day) {
  if (!name) return false;
  const size_t n = std::strlen(name);
  if (n != 16 || std::strcmp(name + 10, ".jsonl") != 0) return false;
  for (size_t i = 0; i < 10; ++i) {
    const bool dash = (i == 4 || i == 7);
    if (dash ? name[i] != '-' : (name[i] < '0' || name[i] > '9')) return false;
  }
  const std::string date(name, 10);   // "YYYY-MM-DD" fits the small-string buffer: no allocation
  const uint32_t d = dayNumFromName(date);
  // Canonical round trip: the name must be exactly what dayFile() writes for that
  // day, which rejects impossible dates ("2026-02-31") and pre-epoch years.
  // Day 0 (1970-01-01) is valid: rows written before the clock is set land there.
  if (AppendLogEpisodicStore::civilDate(d).compare(0, 10, name, 10) != 0) return false;
  day = d;
  return true;
}

std::string AppendLogEpisodicStore::dayFile(uint32_t dayNum) const {
  return dir_ + "/" + civilDate(dayNum) + ".jsonl";
}

namespace {
const char* kSessionsFile = "sessions.jsonl";

// Parse the trailing hex run of an id ("m0000000a" -> 10) for the id high-water.
uint32_t parseIdSuffix(const std::string& id) {
  size_t i = id.size();
  while (i > 0) {
    char c = id[i - 1];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) break;
    --i;
  }
  if (i >= id.size()) return 0;
  uint32_t v = 0;
  for (size_t j = i; j < id.size(); ++j) {
    char c = id[j];
    uint32_t nib = (c <= '9') ? (c - '0') : ((c | 0x20) - 'a' + 10);
    v = v * 16 + nib;
  }
  return v;
}

// Split a buffer into lines, yielding each line's byte offset + content (newline
// stripped). A trailing partial line (no terminating '\n') is still yielded so a
// tolerant decoder can reject it.
void forEachLine(const std::string& buf,
                 const std::function<void(long, const std::string&)>& fn) {
  size_t start = 0;
  while (start < buf.size()) {
    size_t nl = buf.find('\n', start);
    size_t end = (nl == std::string::npos) ? buf.size() : nl;
    fn((long)start, buf.substr(start, end - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
}
}  // namespace

// ---- ring-boundary string conversions (CUM-185) -----------------------------
// The recent ring + index cache their strings in PSRAM (PsString); the public
// EpisodicMessage/EpisodicSession API stays std::string. These bridge the two.
AppendLogEpisodicStore::PsString AppendLogEpisodicStore::ps(const std::string& s) {
  PsString o;
  o.assign(s.data(), s.size());
  return o;
}
std::string AppendLogEpisodicStore::st(const PsString& s) {
  return std::string(s.data(), s.size());
}
AppendLogEpisodicStore::CachedMsg AppendLogEpisodicStore::cacheOf(const EpisodicMessage& m) {
  CachedMsg c;
  c.id = ps(m.id);
  c.sessionId = ps(m.sessionId);
  c.tsHours = m.tsHours;
  c.role = ps(m.role);
  c.kind = m.kind;
  c.text = ps(m.text);
  c.blobPath = ps(m.blobPath);
  c.tags = ps(m.tags);
  return c;
}
EpisodicMessage AppendLogEpisodicStore::msgOf(const CachedMsg& c) {
  EpisodicMessage m;
  m.id = st(c.id);
  m.sessionId = st(c.sessionId);
  m.tsHours = c.tsHours;
  m.role = st(c.role);
  m.kind = c.kind;
  m.text = st(c.text);
  m.blobPath = st(c.blobPath);
  m.tags = st(c.tags);
  return m;
}

// ---- sessions ---------------------------------------------------------------
void AppendLogEpisodicStore::addSession(const EpisodicSession& s) {
  for (auto& e : sessions_)
    if (e.id == s.id) { e = s; appendSessionRow(s); return; }  // upsert
  sessions_.push_back(s);
  appendSessionRow(s);
}

bool AppendLogEpisodicStore::setSessionStatus(const std::string& id, const std::string& status) {
  for (auto& e : sessions_)
    if (e.id == id) { e.status = status; appendSessionRow(e); return true; }
  return false;
}

void AppendLogEpisodicStore::appendSessionRow(const EpisodicSession& s) {
  JsonDocument d;
  d["id"] = s.id;
  d["started"] = s.startedHours;
  d["provider"] = s.provider;
  d["title"] = s.title;
  d["status"] = s.status;
  std::string line;
  serializeJson(d, line);
  line += '\n';
  fs_.append(dir_ + "/" + kSessionsFile, line);
}

std::vector<EpisodicSession> AppendLogEpisodicStore::sessions(const std::string& status) const {
  std::vector<EpisodicSession> out;
  for (const auto& s : sessions_)
    if (status.empty() || s.status == status) out.push_back(s);
  return out;
}

// ---- messages ---------------------------------------------------------------
void AppendLogEpisodicStore::addMessage(const EpisodicMessage& m) {
  std::string line = encodeEpisodicLine(m);
  uint32_t dayNum = m.tsHours / 24;
  long off = fs_.append(dayFile(dayNum), line + "\n");
  if (off < 0) {
    // FS append failed (mid-op SD loss): DON'T index a record we couldn't durably
    // write, but keep it in the RAM recent-window so the current conversation's
    // working set survives the vanished card (older on-disk history is what
    // degrades). unpersisted_ > 0 also signals the device to demote the SD tier.
    unpersisted_++;
    maxIdSuffix_ = std::max(maxIdSuffix_, parseIdSuffix(m.id));
    recent_.push_back(cacheOf(m));
    if ((int)recent_.size() > recentCap_)
      recent_.erase(recent_.begin(), recent_.begin() + (recent_.size() - recentCap_));
    return;
  }
  IdxRec r;
  r.tsHours = m.tsHours;
  r.dayNum = dayNum;
  r.offset = (uint32_t)off;
  r.len = (uint32_t)line.size();
  r.idSfx = epiIdSuffix(m.id);
  r.kind = m.kind;
  r.sessionId = ps(m.sessionId);
  r.blobHash = ps(blobHashOf(m.blobPath));
  index_.push_back(r);
  maxIdSuffix_ = std::max(maxIdSuffix_, parseIdSuffix(m.id));
  recent_.push_back(cacheOf(m));
  if ((int)recent_.size() > recentCap_)
    recent_.erase(recent_.begin(), recent_.begin() + (recent_.size() - recentCap_));
}

bool AppendLogEpisodicStore::readRec(const IdxRec& r, EpisodicMessage& out) const {
  std::string line = fs_.readRange(dayFile(r.dayNum), (long)r.offset, (long)r.len);
  return decodeEpisodicLine(line, out);
}

void AppendLogEpisodicStore::queryRing(const MsgQuery& q, EpiCollector& col) const {
  const uint32_t beforeSfx = epiIdSuffix(q.before);
  for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
    // Run the pre-filters first (only the id/session bridges below allocate a small
    // transient std::string; the kind/time checks are alloc-free), then score the
    // text IN PLACE, so only a row that enters the page pays for the full
    // CachedMsg -> EpisodicMessage conversion.
    if (beforeSfx && epiIdSuffix(st(it->id)) >= beforeSfx) continue;
    if (!q.sessionId.empty() && q.sessionId != it->sessionId.c_str()) continue;
    if (!q.sessionVisible(st(it->sessionId))) continue;   // v3.7.0 read boundary
    if (!q.kindVisible(it->kind)) continue;
    if (q.sinceHours && it->tsHours < q.sinceHours) continue;
    if (q.beforeHours && it->tsHours >= q.beforeHours) continue;
    const int sc = col.consider(it->id.data(), it->id.size(), it->text.data(), it->text.size());
    if (!col.wants(sc)) continue;
    col.add(msgOf(*it), sc);
    if (col.done()) break;
  }
}

uint32_t AppendLogEpisodicStore::indexFloorDay() const {
  return index_.empty() ? 0 : index_.front().dayNum;
}

std::vector<EpisodicMessage> AppendLogEpisodicStore::query(const MsgQuery& q) const {
  return query(q, nullptr);
}

std::vector<EpisodicMessage> AppendLogEpisodicStore::query(const MsgQuery& q,
                                                          EpiQueryInfo* info) const {
  EpiQueryInfo local;
  std::vector<EpisodicMessage> out;
  // One collector for every tier below, so the ring, the index and the cold
  // pass share one text rule, one page and one ranking (CUM-515).
  EpiCollector col(q, out);
  if (col.impossible()) {   // only common words: nothing can match, read nothing
    if (info) *info = local;
    return out;
  }
  // Whole history resident -> answer from the PSRAM ring with zero FS reads.
  // Serve from the RAM ring when the whole history fits it OR when SD writes have
  // been failing (unpersisted_ > 0): the index then points at an unreliable/absent
  // card, so the recent window is the best available answer (graceful degradation).
  // ⚠ "the ring holds every INDEXED row" is not "the ring holds every row": when
  // the boot scan truncated, the ring can be resident and still be missing months
  // of on-card history. A cold-scan request must therefore beat this fast path,
  // or the deep query silently answers from the newest 256 rows.
  // A byte cursor ("<day>:<off>") is a COLD-region continuation: the ring and
  // the index have already been paged past, so every ring path must yield to
  // the cold pass. queryRing/the index loop key off `epiIdSuffix(before)`, which
  // would parse a byte cursor's trailing digits as a bogus hex id - re-emitting
  // recent rows on every page, or (on a broad query) filling the page from the
  // ring and handing back an id cursor that never re-enters the cold scan, so
  // deep history below the first cold page becomes unreachable. Both are only
  // visible when the ring is resident AND the boot scan truncated (recentCap 512
  // on-device with one large active day-stream) - the shipped config, which the
  // small-ring host tests never hit.
  const bool byteCursor = q.before.find(':') != std::string::npos;
  const bool coldWanted = q.coldScan && hydrateTruncated_;
  if (unpersisted_ > 0 || (ringResident() && !coldWanted && !byteCursor)) {
    queryRing(q, col);
    if (!recent_.empty()) local.searchedToDay = recent_.front().tsHours / 24;
    local.olderExists = hydrateTruncated_;
    local.matched = col.matched();
    if (info) *info = local;
    return out;
  }

  // Index-first: cheap filters off the in-RAM index (out-of-window day files are
  // never opened); read + text-filter only the survivors, newest-first, until the
  // collector is done (unranked: `limit` rows) or the read budget runs out.
  const uint32_t beforeSfx = epiIdSuffix(q.before);
  bool indexCapped = false;
  if (ringResident() && !byteCursor) {
    // Every indexed row is ALSO in the RAM ring: reading them back off the card
    // would be ~100 ms per row for data already in memory. (Measured: a
    // 490-row index took ~60 s of SD opens and wedged the web task.) A byte
    // cursor has paged past the ring already, so skip it and fall to the cold
    // pass below.
    queryRing(q, col);
  } else if (!byteCursor) {
    int reads = 0;
    for (auto it = index_.rbegin(); it != index_.rend(); ++it) {
      const IdxRec& r = *it;
      if (beforeSfx && r.idSfx >= beforeSfx) continue;   // paging cursor
      if (!q.sessionId.empty() && q.sessionId != r.sessionId.c_str()) continue;
      if (!q.sessionVisible(st(r.sessionId))) continue;   // v3.7.0 read boundary
      if (!q.kindVisible(r.kind)) continue;
      if (q.sinceHours && r.tsHours < q.sinceHours) continue;
      if (q.beforeHours && r.tsHours >= q.beforeHours) continue;
      if (reads >= kIndexReadsPerCall) {                 // bounded, then paged
        // Resume in the INDEXED range, not the cold one: a byte cursor would
        // send the next call straight to the cold pass, which is bounded BELOW
        // each day's index floor - the rows in between would vanish. +1 so the
        // record we stopped at is included next time (the filter is exclusive).
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "m%08x", (unsigned)(r.idSfx + 1));
        indexCapped = true;
        local.nextBefore = idbuf;
        local.olderExists = true;
        break;
      }
      EpisodicMessage m;
      reads++;
      if (!readRec(r, m)) continue;  // tolerant: torn line drops out
      if (col.offer(std::move(m))) break;
    }
  }
  local.searchedToDay = indexFloorDay();

  // The index floor is not the history floor: everything the boot scan could not
  // afford is still on the card. Walk it only when the caller opted in AND the
  // hot pass came up short - a full window is what the caller asked for.
  if (indexCapped) {
    // Stopped on the read budget - the cursor is already set; do not also spend
    // this call's budget on the cold pass.
  } else if (col.full()) {
    // The page filled from the index: hand back the cursor that continues below
    // it. Without this a caller with a small window can never page past the
    // first screen of history. It is the oldest row EXAMINED: unranked that is
    // the last row returned; ranked, the bottom of the window it ranked.
    local.nextBefore = col.lastExamined();
    local.olderExists = true;
  } else if (q.coldScan) {
    queryCold(q, col, local);
  } else if (hydrateTruncated_) {
    local.olderExists = true;
  }

  local.matched = col.matched();
  if (info) *info = local;
  return out;
}

// ---- day-stream helpers -----------------------------------------------------
EpiPsVec<uint32_t> AppendLogEpisodicStore::dayStreams() const {
  EpiPsVec<uint32_t> days;
  fs_.forEachName(dir_, [&](const char* name) {
    uint32_t day = 0;
    if (parseDayStreamName(name, day)) days.push_back(day);
  });
  std::sort(days.begin(), days.end());
  days.erase(std::unique(days.begin(), days.end()), days.end());
  return days;
}

long AppendLogEpisodicStore::indexFloorOf(uint32_t day) const {
  // A linear pass, not a binary search: a clock that jumped back can append an
  // older day's row at the END of the index, and missing it here would make the
  // backfill index that row a second time.
  long floor = -1;
  for (const auto& r : index_)
    if (r.dayNum == day && (floor < 0 || (long)r.offset < floor)) floor = (long)r.offset;
  return floor;
}

namespace {
// Visit the '\n'-separated lines of `buf` newest first, as (offset in buf,
// length). Empty lines are skipped. Stops when `fn` returns true.
template <class Fn>
void forEachLineNewestFirst(const std::string& buf, Fn fn) {
  size_t e = buf.size();
  while (e > 0) {
    if (buf[e - 1] == '\n') { --e; continue; }
    size_t s = e;
    while (s > 0 && buf[s - 1] != '\n') --s;
    if (fn(s, e - s)) return;
    e = s;
  }
}
}  // namespace

AppendLogEpisodicStore::WinRead AppendLogEpisodicStore::readBackWindow(
    const std::string& path, long end, size_t window, size_t maxWindow, BackWindow& w) const {
  w.buf.clear();
  w.bytesRead = 0;
  w.first = end;
  if (window == 0) window = kColdWindow;
  if (maxWindow < window) maxWindow = window;
  long start = end > (long)window ? end - (long)window : 0;
  w.start = start;
  std::string buf = fs_.readRange(path, start, end - start);
  w.bytesRead = buf.size();
  if (buf.empty()) return WinRead::Empty;
  // A record boundary inside the window is a '\n' BEFORE its last byte: when
  // `end` is a record start, the last byte is the newest record's own
  // terminator, and treating it as a boundary would yield an empty window that
  // never moves `end` (a record longer than the window directly below the index
  // floor stalled the cold cursor that way).
  auto hasBoundary = [](const std::string& b) {
    return b.size() >= 2 && std::memchr(b.data(), '\n', b.size() - 1) != nullptr;
  };
  size_t win = (size_t)(end - start);
  while (start > 0 && !hasBoundary(buf)) {
    if (win >= maxWindow) { w.start = start; return WinRead::Stubborn; }
    // Grow backward: read only the earlier bytes (doubling, capped) and prepend.
    long add = (long)std::min(win, maxWindow - win);
    if (add > start) add = start;
    std::string head = fs_.readRange(path, start - add, add);
    if (head.empty()) { w.start = start; return WinRead::Empty; }
    w.bytesRead += head.size();
    head.append(buf);
    buf.swap(head);
    start -= add;
    win += (size_t)add;
  }
  w.start = start;
  long first = start;
  if (start > 0) {
    const size_t nl = buf.find('\n');   // before the last byte (hasBoundary)
    first = start + (long)nl + 1;
    buf.erase(0, nl + 1);
  }
  w.first = first;
  w.buf.swap(buf);
  return WinRead::Ok;
}

// ---- cold query -------------------------------------------------------------
// Reads day-streams the boot scan never indexed. Two rules make this safe to run
// inside a turn: a per-call budget (kColdMaxFiles / kColdMaxBytes) and a
// byte-resolution cursor, so a caller pages instead of ever asking for "all of
// it". Within a file the read is BACKWARD in kColdWindow chunks; each chunk
// drops its own partial head record and hands that boundary to the next (older)
// chunk, so a record straddling a boundary is read whole exactly once.
void AppendLogEpisodicStore::queryCold(const MsgQuery& q, EpiCollector& col,
                                       EpiQueryInfo& info) const {
  // Where to start: an explicit byte cursor, else just below the index floor.
  uint32_t curDay = 0;
  long     curEnd = -1;         // exclusive byte bound within curDay (-1 = whole file)
  const size_t colon = q.before.find(':');
  if (colon != std::string::npos) {
    curDay = (uint32_t)std::strtoul(q.before.substr(0, colon).c_str(), nullptr, 10);
    curEnd = std::strtol(q.before.substr(colon + 1).c_str(), nullptr, 10);
  }

  // Day-streams by name (the one day-stream rule), oldest first.
  const EpiPsVec<uint32_t> days = dayStreams();

  // ⚠ The unindexed region is PER FILE, not "every day below the index floor".
  // The boot scan reads each day-stream's TAIL, so the newest file can hold
  // thousands of rows the index never saw while its day is above the floor.
  // Anchoring on a single global floor skipped exactly the file that mattered
  // (a 1.7 MB day-stream, live on hardware). Per day: everything below the
  // oldest INDEXED offset in that same file is cold.
  std::map<uint32_t, uint32_t> floorOfDay;
  for (const auto& r : index_) {
    auto it2 = floorOfDay.find(r.dayNum);
    if (it2 == floorOfDay.end() || r.offset < it2->second) floorOfDay[r.dayNum] = r.offset;
  }

  int    files = 0;
  size_t bytes = 0;
  bool   more = false;
  // Where a budget-limited pass stopped, in FILE coordinates. Reporting the
  // day alone (with offset 0) reads as "this day is finished" and the next call
  // skips the file - losing everything below the point we actually reached.
  uint32_t stopDay = 0;
  long     stopOff = 0;

  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    const uint32_t dayNum = *it;
    const std::string path = dayFile(dayNum);
    const long fsize = fs_.size(path);
    if (fsize <= 0) continue;

    // This file's exclusive upper byte bound: the cursor if we are resuming
    // inside it, else this day's own index floor, else the whole file.
    long end = fsize;
    auto fit = floorOfDay.find(dayNum);
    if (fit != floorOfDay.end()) end = (long)fit->second;
    if (curEnd >= 0) {                       // resuming from a cursor
      if (dayNum > curDay) continue;         // newer than the cursor - already paged
      // Clamped to the file: the cursor comes from the caller (a model can echo
      // a stale or invented one), and an offset past the end would otherwise
      // walk down to the data one empty read at a time, outside the byte budget.
      if (dayNum == curDay) end = std::min(curEnd, fsize);
    }
    if (end <= 0) continue;

    if (files >= kColdMaxFiles || bytes >= kColdMaxBytes) {
      more = true; stopDay = dayNum; stopOff = end; break;
    }
    files++;
    info.searchedToDay = dayNum;

    while (end > 0) {
      if (bytes >= kColdMaxBytes) { more = true; stopDay = dayNum; stopOff = end; break; }
      // Read backward one window at a time, growing only when a single record
      // spans the whole window (rare - a big tool_output row).
      BackWindow w;
      const WinRead rd = readBackWindow(path, end, kColdWindow, kColdWindowMax, w);
      bytes += w.bytesRead;
      if (rd == WinRead::Empty) break;       // the file would not read: on to older ones
      if (rd == WinRead::Stubborn) {         // one record longer than the window cap
        if (w.start <= 0) break;
        end = w.start;                       // skip that span, keep going older
        continue;
      }
      bool pageDone = false;
      forEachLineNewestFirst(w.buf, [&](size_t off, size_t len) {
        EpisodicMessage m;
        if (!decodeEpisodicLine(w.buf.data() + off, len, m)) return false;   // tolerant
        info.searchedToDay = m.tsHours / 24;
        // The cold rows sit below the id range, so no cursor filter (beforeSfx 0);
        // the read boundary holds here too.
        if (!epiPassesFilters(q, m, 0)) return false;
        if (!col.offer(std::move(m))) return false;
        // Page done (see EpiCollector::done): resume EXACTLY below the row just emitted.
        info.nextBefore = std::to_string((unsigned long)dayNum) + ":" +
                          std::to_string((long long)(w.first + (long)off));
        pageDone = true;
        return true;
      });
      if (pageDone) {
        info.olderExists = true;
        info.coldFiles = files;
        info.coldBytes = bytes;
        return;
      }
      end = w.first;
    }
    if (more) break;
  }

  if (more) {
    info.olderExists = true;
    info.nextBefore = std::to_string((unsigned long)stopDay) + ":" +
                      std::to_string((long long)stopOff);
  }
  info.coldFiles = files;
  info.coldBytes = bytes;
}

// ---- hydrate ----------------------------------------------------------------
int AppendLogEpisodicStore::hydrate(int maxRows, size_t maxBytes,
                                    const std::function<void()>& yield) {
  index_.clear();
  recent_.clear();
  sessions_.clear();
  maxIdSuffix_ = 0;
  resetBackfill();

  // Replay sessions first (LWW by id). This file is small and bounded by the
  // number of sub-agent sessions, not by message volume.
  const std::string sessPath = dir_ + "/" + kSessionsFile;
  if (fs_.exists(sessPath)) {
    std::string buf = fs_.readAll(sessPath);
    forEachLine(buf, [&](long, const std::string& line) {
      JsonDocument d;
      if (line.empty() || deserializeJson(d, line) != DeserializationError::Ok) return;
      if (!d["id"].is<const char*>()) return;
      EpisodicSession s;
      s.id = d["id"] | "";
      s.startedHours = d["started"] | (uint32_t)0;
      s.provider = d["provider"] | "";
      s.title = d["title"] | "";
      s.status = d["status"] | "active";
      bool found = false;
      for (auto& e : sessions_)
        if (e.id == s.id) { e = s; found = true; break; }
      if (!found) sessions_.push_back(s);
    });
    if (yield) yield();
  }

  // Walk the day-streams NEWEST FIRST so the budget spends itself on the history
  // anyone actually asks for. (The unbounded version went oldest-first and read
  // everything, which is how a board with a month of chat stopped being able to
  // boot at all.)
  const EpiPsVec<uint32_t> days = dayStreams();

  std::vector<IdxRec> newestFirst;
  std::vector<CachedMsg> recentNewestFirst;   // CUM-185: cache mirror (PSRAM strings)
  size_t bytesRead = 0;
  bool truncated = false;

  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    if (maxRows > 0 && (int)newestFirst.size() >= maxRows) { truncated = true; break; }
    // How much of this file we may read. 0 = no byte budget: the whole file.
    size_t window = 0;
    if (maxBytes > 0) {
      const size_t room = maxBytes > bytesRead ? maxBytes - bytesRead : 0;
      // So little budget left that a tail read would hold a row or two: stop
      // here rather than open one more file for it (the walk that kept going
      // with a few hundred bytes of room opened every remaining file and
      // indexed one isolated row from each).
      if (room < kHydrateMinTail) { truncated = true; break; }
      window = room < kHydrateFileWindow ? room : kHydrateFileWindow;
    }
    const uint32_t day = *it;
    const std::string path = dayFile(day);
    const long fsize = fs_.size(path);
    if (fsize <= 0) continue;
    // Read the file's TAIL, not the whole thing. Checking the budget between
    // files is not enough: one day-stream can be megabytes on a busy device, and
    // readAll would pull all of it into a single string before the next budget
    // check ever ran - which is exactly how the boot scan blew past the watchdog
    // and left a board unable to start.
    std::string buf;
    long bufBase = 0;             // FILE offset of buf[0] (tail reads start mid-file)
    bool partial = false;
    if (window > 0 && (size_t)fsize > window) {
      // Take the NEWEST bytes, then drop the partial first record a tail read
      // almost certainly starts inside, so decodeEpisodicLine never sees a torn row.
      buf = fs_.readRange(path, fsize - (long)window, (long)window);
      bytesRead += window;
      const size_t nl = buf.find('\n');
      // ⚠ The index stores FILE offsets; forEachLine reports offsets within
      // `buf`. Without this base every row indexed from a tail-read file pointed
      // at the wrong bytes - readRec() then decoded garbage and dropped the row,
      // so on any device with a day-stream over the window (~128 KB, i.e. any
      // busy month) episodic search silently returned nothing for those rows and
      // the RAM ring was quietly carrying the feature.
      bufBase = (nl == std::string::npos) ? 0 : (fsize - (long)window + (long)nl + 1);
      buf = (nl == std::string::npos) ? std::string() : buf.substr(nl + 1);
      partial = true;
      truncated = true;
    } else {
      buf = fs_.readAll(path);
      bytesRead += buf.size();
    }
    // Within a day the file is append-ordered, so collect then reverse to keep
    // the newest-first accumulation consistent. The row's day is its FILE's day:
    // readRec() opens dayFile(dayNum), so the two must agree.
    std::vector<IdxRec> dayIdx;
    std::vector<EpisodicMessage> dayMsgs;
    forEachLine(buf, [&](long off, const std::string& line) {
      EpisodicMessage m;
      if (!decodeEpisodicLine(line, m)) return;  // torn/garbage line skipped
      IdxRec r;
      r.tsHours = m.tsHours;
      r.dayNum = day;
      r.offset = (uint32_t)(bufBase + off);
      r.len = (uint32_t)line.size();
      r.idSfx = epiIdSuffix(m.id);
      r.kind = m.kind;
      r.sessionId = ps(m.sessionId);
      r.blobHash = ps(blobHashOf(m.blobPath));
      dayIdx.push_back(r);
      dayMsgs.push_back(m);
      maxIdSuffix_ = std::max(maxIdSuffix_, parseIdSuffix(m.id));
    });
    for (auto ri = dayIdx.rbegin(); ri != dayIdx.rend(); ++ri) {
      if (maxRows > 0 && (int)newestFirst.size() >= maxRows) { truncated = true; break; }
      newestFirst.push_back(*ri);
    }
    // The ring never holds a row the index does not: when maxRows cut the index
    // short of recentCap_, an un-indexed ring row sat below the per-file index
    // floor too, so a cold scan returned it a second time (found by the CUM-515
    // read-boundary test; latent at the device defaults, maxRows 4000 > ring 512).
    for (auto mi = dayMsgs.rbegin();
         mi != dayMsgs.rend() && (int)recentNewestFirst.size() < recentCap_ &&
         recentNewestFirst.size() < newestFirst.size();
         ++mi)
      recentNewestFirst.push_back(cacheOf(*mi));
    if (yield) yield();   // the caller feeds the watchdog here
    // A file read only in part ends the walk: indexing an older file now would
    // leave this file's head as a hole inside the index, so the floor, the cold
    // cursor and "newest first" would all paper over missing rows. The backfill
    // continues from exactly here.
    if (partial) break;
  }

  // Flip both back to the oldest-first order the rest of the class assumes.
  index_.assign(newestFirst.rbegin(), newestFirst.rend());
  recent_.assign(recentNewestFirst.rbegin(), recentNewestFirst.rend());
  hydrateTruncated_ = truncated;
  bfStats_.complete = !truncated;
  bootIndexed_ = (int)index_.size();
  return bootIndexed_;
}

// ---- index backfill ---------------------------------------------------------
void AppendLogEpisodicStore::resetBackfill() {
  bfInit_ = false;
  bfDays_.clear();
  bfPos_ = 0;
  bfEnd_ = -1;
  bfReadErrors_ = 0;
  bfSkipped_ = false;
  bfStats_ = EpiBackfillStats{};
}

bool AppendLogEpisodicStore::backfillStep(size_t sliceBytes, int sliceRows) {
  if (!hydrateTruncated_) { bfStats_.complete = true; return false; }
  if (bfStats_.capped || bfStats_.stalled) return false;
  if (sliceBytes == 0) sliceBytes = kBackfillSliceBytes;
  if (sliceRows <= 0) sliceRows = kBackfillSliceRows;
  if (!bfInit_) {
    // Captured once: the walk only ever moves to OLDER days, and days older than
    // the cursor can only disappear (retention), never appear. A day that
    // vanishes reads back empty and is skipped.
    bfDays_ = dayStreams();
    std::reverse(bfDays_.begin(), bfDays_.end());   // newest first
    bfPos_ = 0;
    bfEnd_ = -1;
    bfInit_ = true;
  }
  while (bfPos_ < bfDays_.size()) {
    const uint32_t day = bfDays_[bfPos_];
    const std::string path = dayFile(day);
    if (bfEnd_ < 0) {
      // The unindexed region of a day is everything below its oldest indexed
      // row; a day with no indexed row is unindexed whole.
      const long floor = indexFloorOf(day);
      bfEnd_ = floor >= 0 ? floor : fs_.size(path);
    }
    if (bfEnd_ <= 0) { ++bfPos_; bfEnd_ = -1; continue; }
    // Rows remain unindexed: past the cap the index stays honestly incomplete
    // (cold queries still reach the rest, retention keeps deferring the sweep).
    if ((int)index_.size() >= indexCap_) { bfStats_.capped = true; return false; }
    return backfillWindow(day, path, sliceBytes, sliceRows);
  }
  if (bfSkipped_) {
    // A record too long to read whole was skipped: the card still holds a row
    // the index lacks, so the index must not claim to be complete.
    bfStats_.stalled = true;
    return false;
  }
  hydrateTruncated_ = false;
  bfStats_.complete = true;
  return false;
}

bool AppendLogEpisodicStore::backfillWindow(uint32_t day, const std::string& path,
                                            size_t sliceBytes, int sliceRows) {
  BackWindow w;
  const WinRead rd = readBackWindow(path, bfEnd_, sliceBytes, kBackfillWindowMax, w);
  bfStats_.bytes += w.bytesRead;
  bfStats_.slices++;
  if (rd == WinRead::Empty) {
    if (!fs_.exists(path)) { ++bfPos_; bfEnd_ = -1; return true; }   // pruned under us
    // The file is there but would not read: retry on later steps, then give up
    // for this boot with the index still marked incomplete (fail closed).
    if (++bfReadErrors_ >= kBackfillMaxReadErrors) { bfStats_.stalled = true; return false; }
    return true;
  }
  bfReadErrors_ = 0;
  if (rd == WinRead::Stubborn) {
    bfSkipped_ = true;
    bfEnd_ = w.start;   // skip the span, keep indexing older rows
    return true;
  }

  // Decode at most sliceRows lines (and never past the index cap), newest first.
  const int room = std::min(sliceRows, indexCap_ - (int)index_.size());
  EpiPsVec<IdxRec> rows;           // newest first
  EpiPsVec<CachedMsg> ringRows;    // newest first, only what the ring has room for
  const size_t ringRoom = recent_.size() < (size_t)recentCap_ ? (size_t)recentCap_ - recent_.size() : 0;
  int lines = 0;
  bool budgetHit = false;
  long oldestTaken = w.first;
  JsonDocument d(&WorkingJsonAllocator::instance());
  forEachLineNewestFirst(w.buf, [&](size_t off, size_t len) {
    if (lines >= room) { budgetHit = true; return true; }
    lines++;
    oldestTaken = w.first + (long)off;
    if (!decodeDoc(w.buf.data() + off, len, d)) return false;   // torn/garbage line skipped
    const char* id = d["id"] | "";
    const char* sid = d["sid"] | "";
    const char* blob = d["blob"] | "";
    MsgKind k = MsgKind::Message;
    kindFromName(d["kind"] | "message", k);
    IdxRec r;
    r.tsHours = d["ts"] | (uint32_t)0;
    r.dayNum = day;
    r.offset = (uint32_t)(w.first + (long)off);
    r.len = (uint32_t)len;
    r.idSfx = epiIdSuffix(id);
    r.kind = k;
    r.sessionId.assign(sid);
    const std::string_view h = blobHashView(blob, std::strlen(blob));
    r.blobHash.assign(h.data(), h.size());
    maxIdSuffix_ = std::max(maxIdSuffix_, parseIdSuffix(id));
    if (ringRows.size() < ringRoom) {
      CachedMsg c;
      c.id.assign(id);
      c.sessionId.assign(sid);
      c.tsHours = r.tsHours;
      c.role.assign(d["role"] | "");
      c.kind = k;
      c.text.assign(d["text"] | "");
      c.blobPath.assign(blob);
      c.tags.assign(d["tags"] | "");
      ringRows.push_back(std::move(c));
    }
    rows.push_back(std::move(r));
    return false;
  });
  // Stopped on the row budget: the next step resumes just above the oldest row
  // taken (the lines below it in this window are read again then). Otherwise
  // the whole window is done.
  bfEnd_ = budgetHit ? oldestTaken : w.first;

  if (!rows.empty()) {
    // Every row here sits below the day's indexed region, so it belongs just in
    // front of the first indexed row that is newer than it. A forward scan finds
    // that spot at once for a contiguous index (it is the front).
    const uint32_t newestOff = rows.front().offset;
    auto pos = std::find_if(index_.begin(), index_.end(), [&](const IdxRec& x) {
      return x.dayNum > day || (x.dayNum == day && x.offset > newestOff);
    });
    index_.insert(pos, std::make_move_iterator(rows.rbegin()), std::make_move_iterator(rows.rend()));
    // Keep "the ring is the newest rows of the index": rows older than the whole
    // ring go in front of it, oldest first.
    recent_.insert(recent_.begin(), std::make_move_iterator(ringRows.rbegin()),
                   std::make_move_iterator(ringRows.rend()));
    bfStats_.rows += (int)rows.size();
  }
  return true;
}

// ---- retention prune --------------------------------------------------------
void AppendLogEpisodicStore::sweepBlobs(const std::string& blobDir, EpiPruneReport& rep) {
  // Every hash any row references, as views into the PSRAM index and ring
  // strings: no per-row allocation, one PSRAM buffer. The ring is included so a
  // row the card refused (kept in RAM after a write failure) keeps its blob.
  EpiPsVec<std::string_view> refs;
  refs.reserve(index_.size() + recent_.size());
  for (const auto& r : index_)
    if (!r.blobHash.empty()) refs.emplace_back(r.blobHash.data(), r.blobHash.size());
  for (const auto& m : recent_) {
    const std::string_view h = blobHashView(m.blobPath.data(), m.blobPath.size());
    if (!h.empty()) refs.push_back(h);
  }
  std::sort(refs.begin(), refs.end());
  refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

  // Stream the directory (no materialized listing) and collect the orphans;
  // delete after the walk, never while the directory is open.
  EpiPsVec<EpiPsString> dead;
  int present = 0;
  fs_.forEachName(blobDir, [&](const char* name) {
    present++;
    const std::string_view h = blobHashView(name, std::strlen(name));
    if (!h.empty() && std::binary_search(refs.begin(), refs.end(), h)) return;
    dead.emplace_back(name);
  });
  rep.blobsPresent = present;
  for (const auto& name : dead) {
    std::string path = blobDir;
    path += '/';
    path.append(name.data(), name.size());
    if (isRetentionExempt(path)) continue;
    if (fs_.remove(path)) rep.removedBlobs.push_back(name);
  }
}

EpiPruneReport AppendLogEpisodicStore::prune(uint32_t cutoffDayNum, const std::string& blobDir) {
  EpiPruneReport rep;

  // Which whole day-files are strictly older than the cutoff? By FILE NAME, not
  // by the index: after a truncated boot the index does not know every day on
  // the card, and a day it never saw must still age out.
  EpiPsVec<uint32_t> oldDays = dayStreams();
  oldDays.erase(std::lower_bound(oldDays.begin(), oldDays.end(), cutoffDayNum), oldDays.end());

  // Remove each old day-file, but only treat a day as GONE (droppable from the
  // index) once its file is confirmed off disk. If remove() fails on an I/O error
  // the file survives; keeping its index records in sync with disk avoids hydrate()
  // replaying "pruned" rows on the next boot, and the day is retried next pass.
  EpiPsVec<uint32_t> gone;   // stays sorted (oldDays is)
  for (uint32_t day : oldDays) {
    const std::string path = dayFile(day);
    if (isRetentionExempt(path)) continue;
    fs_.remove(path);
    if (!fs_.exists(path)) {
      gone.push_back(day);
      rep.removedDayFiles.emplace_back(path.data(), path.size());
    }
  }
  auto isGone = [&](uint32_t day) { return std::binary_search(gone.begin(), gone.end(), day); };

  // Drop from the index + recent ring only the records whose day-file is gone.
  index_.erase(std::remove_if(index_.begin(), index_.end(),
                              [&](const IdxRec& r) { return isGone(r.dayNum); }),
               index_.end());
  recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                               [&](const CachedMsg& m) { return isGone(m.tsHours / 24); }),
                recent_.end());

  // Reference-count-scan blobs - but only over a COMPLETE index. With rows on
  // the card that the index has not seen yet, "referenced by no indexed row"
  // includes blobs those rows still point at, and the sweep would delete them
  // (it did, on a device whose boot scan indexed 400 of 1,346 rows). Defer it;
  // the backfill completes the index and a later pass sweeps.
  if (hydrateTruncated_) rep.blobGcSkipped = true;
  else sweepBlobs(blobDir, rep);

  // Compact sessions.jsonl (Release C4): the file was append-only-FOREVER - one
  // row per upsert, replayed whole at every hydrate. Rewrite it as exactly one
  // row per live session. Non-atomic (remove+append), but sessions_ stays in
  // RAM and rows re-append on the next upsert, so the crash window loses only
  // display metadata until then.
  {
    const std::string path = dir_ + "/" + kSessionsFile;
    if (!isRetentionExempt(path)) {
      fs_.remove(path);
      for (const auto& sess : sessions_) appendSessionRow(sess);
    }
  }

  rep.keptMessages = (int)index_.size();
  return rep;
}

}  // namespace orch
}  // namespace nimbus
