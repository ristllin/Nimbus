#include "nimbus/orch/episodic_log.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <map>
#include <cstdio>
#include <functional>

#include "nimbus/orch/blob_store.h"

namespace nimbus {
namespace orch {


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
  if (line.empty()) return false;
  JsonDocument d;
  if (deserializeJson(d, line) != DeserializationError::Ok) return false;
  if (!d["id"].is<const char*>()) return false;  // a torn line lacks the first key
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

std::vector<EpisodicMessage> AppendLogEpisodicStore::queryRing(const MsgQuery& q) const {
  std::vector<EpisodicMessage> out;
  const uint32_t beforeSfx = epiIdSuffix(q.before);
  for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
    // Cheap PSRAM-side pre-filters first (no std::string allocation), so only a
    // surviving row pays for the CachedMsg -> EpisodicMessage conversion.
    if (beforeSfx && epiIdSuffix(st(it->id)) >= beforeSfx) continue;
    if (!q.sessionId.empty() && q.sessionId != it->sessionId.c_str()) continue;
    if (!q.sessionVisible(st(it->sessionId))) continue;   // v3.7.0 read boundary
    if (!q.kindVisible(it->kind)) continue;
    if (q.sinceHours && it->tsHours < q.sinceHours) continue;
    if (q.beforeHours && it->tsHours >= q.beforeHours) continue;
    EpisodicMessage m = msgOf(*it);
    if (!epiTextMatch(m.text, q.textContains)) continue;
    out.push_back(std::move(m));
    if (q.limit > 0 && (int)out.size() >= q.limit) break;
  }
  return out;
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
    std::vector<EpisodicMessage> out = queryRing(q);
    if (!recent_.empty()) local.searchedToDay = recent_.front().tsHours / 24;
    local.olderExists = hydrateTruncated_;
    if (info) *info = local;
    return out;
  }

  // Index-first: cheap filters off the in-RAM index (out-of-window day files are
  // never opened); read + text-filter only the survivors, newest-first, up to limit.
  const uint32_t beforeSfx = epiIdSuffix(q.before);
  std::vector<EpisodicMessage> out;
  bool indexCapped = false;
  if (ringResident() && !byteCursor) {
    // Every indexed row is ALSO in the RAM ring: reading them back off the card
    // would be ~100 ms per row for data already in memory. (Measured: a
    // 490-row index took ~60 s of SD opens and wedged the web task.) A byte
    // cursor has paged past the ring already, so skip it and fall to the cold
    // pass below.
    out = queryRing(q);
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
      if (!epiTextMatch(m.text, q.textContains)) continue;
      out.push_back(m);
      if (q.limit > 0 && (int)out.size() >= q.limit) break;
    }
  }
  local.searchedToDay = indexFloorDay();

  // The index floor is not the history floor: everything the boot scan could not
  // afford is still on the card. Walk it only when the caller opted in AND the
  // hot pass came up short - a full window is what the caller asked for.
  const bool needMore = q.limit <= 0 || (int)out.size() < q.limit;
  if (indexCapped) {
    // Stopped on the read budget - the cursor is already set; do not also spend
    // this call's budget on the cold pass.
  } else if (!needMore) {
    // The page filled from the index: hand back the cursor that continues below
    // it. Without this a caller with a small window can never page past the
    // first screen of history.
    local.nextBefore = out.back().id;
    local.olderExists = true;
  } else if (q.coldScan) {
    queryCold(q, out, local);
  } else if (hydrateTruncated_) {
    local.olderExists = true;
  }

  if (info) *info = local;
  return out;
}

// ---- cold query -------------------------------------------------------------
// Reads day-streams the boot scan never indexed. Two rules make this safe to run
// inside a turn: a per-call budget (kColdMaxFiles / kColdMaxBytes) and a
// byte-resolution cursor, so a caller pages instead of ever asking for "all of
// it". Within a file the read is BACKWARD in kColdWindow chunks; each chunk
// drops its own partial head record and hands that boundary to the next (older)
// chunk, so a record straddling a boundary is read whole exactly once.
void AppendLogEpisodicStore::queryCold(const MsgQuery& q,
                                       std::vector<EpisodicMessage>& out,
                                       EpiQueryInfo& info) const {
  // Where to start: an explicit byte cursor, else just below the index floor.
  uint32_t curDay = 0;
  long     curEnd = -1;         // exclusive byte bound within curDay (-1 = whole file)
  const size_t colon = q.before.find(':');
  if (colon != std::string::npos) {
    curDay = (uint32_t)std::strtoul(q.before.substr(0, colon).c_str(), nullptr, 10);
    curEnd = std::strtol(q.before.substr(colon + 1).c_str(), nullptr, 10);
  }

  // Day files by name - "YYYY-MM-DD.jsonl" sorts chronologically, which is all
  // the name is used for; each file's epoch-day comes from a decoded record, so
  // the calendar is never re-implemented here.
  std::vector<std::string> dayNames;
  for (const auto& name : fs_.list(dir_)) {
    if (name == kSessionsFile) continue;
    if (name.size() < 6 || name.compare(name.size() - 6, 6, ".jsonl") != 0) continue;
    dayNames.push_back(name);
  }
  std::sort(dayNames.begin(), dayNames.end());

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

  for (auto it = dayNames.rbegin(); it != dayNames.rend(); ++it) {
    const std::string name = *it;
    const std::string path = dir_ + "/" + name;
    const uint32_t dayNum = dayNumFromName(name);
    const long fsize = fs_.size(path);
    if (fsize <= 0) continue;

    // This file's exclusive upper byte bound: the cursor if we are resuming
    // inside it, else this day's own index floor, else the whole file.
    long end = fsize;
    auto fit = floorOfDay.find(dayNum);
    if (fit != floorOfDay.end()) end = (long)fit->second;
    if (curEnd >= 0) {                       // resuming from a cursor
      if (dayNum > curDay) continue;         // newer than the cursor - already paged
      if (dayNum == curDay) end = curEnd;
    }
    if (end <= 0) continue;

    if (files >= kColdMaxFiles || bytes >= kColdMaxBytes) {
      more = true; stopDay = dayNum; stopOff = end; break;
    }
    files++;
    info.searchedToDay = dayNum;

    while (end > 0) {
      if (bytes >= kColdMaxBytes) { more = true; stopDay = dayNum; stopOff = end; break; }
      // Read backward one window at a time, escalating only when a single record
      // spans the whole window (rare - a big tool_output row).
      long start = 0;
      std::string buf;
      size_t win = kColdWindow;
      bool haveWindow = false;
      for (;;) {
        start = end > (long)win ? end - (long)win : 0;
        buf = fs_.readRange(path, start, end - start);
        if (buf.empty()) break;              // read failed / nothing there
        if (start == 0 || buf.find('\n') != std::string::npos) { haveWindow = true; break; }
        if (win >= kColdWindowMax) break;    // give up on THIS window, not the file
        win *= 2;
      }
      bytes += buf.size();
      if (!haveWindow) {
        if (start <= 0) break;
        end = start;                         // skip the stubborn span, keep going older
        continue;
      }
      long first = start;                    // absolute offset of the first WHOLE record
      if (start > 0) {
        const size_t nl = buf.find('\n');
        first = start + (long)nl + 1;
        buf.erase(0, nl + 1);
      }
      // Split into (absolute offset, line) pairs, then walk them newest-first.
      std::vector<std::pair<long, std::string>> lines;
      size_t p = 0;
      while (p < buf.size()) {
        const size_t nl = buf.find('\n', p);
        const size_t e = (nl == std::string::npos) ? buf.size() : nl;
        if (e > p) lines.emplace_back(first + (long)p, buf.substr(p, e - p));
        if (nl == std::string::npos) break;
        p = nl + 1;
      }
      for (auto li = lines.rbegin(); li != lines.rend(); ++li) {
        EpisodicMessage m;
        if (!decodeEpisodicLine(li->second, m)) continue;   // tolerant
        info.searchedToDay = m.tsHours / 24;
        if (!q.sessionId.empty() && m.sessionId != q.sessionId) continue;
        if (!q.sessionVisible(m.sessionId)) continue;       // read boundary holds here too
        if (!q.kindVisible(m.kind)) continue;
        if (q.sinceHours && m.tsHours < q.sinceHours) continue;
        if (q.beforeHours && m.tsHours >= q.beforeHours) continue;
        if (!epiTextMatch(m.text, q.textContains)) continue;
        out.push_back(m);
        if (q.limit > 0 && (int)out.size() >= q.limit) {
          // Resume EXACTLY below the row just emitted.
          info.nextBefore = std::to_string((unsigned long)(m.tsHours / 24)) + ":" +
                            std::to_string((long long)li->first);
          info.olderExists = true;
          info.coldFiles = files;
          info.coldBytes = bytes;
          return;
        }
      }
      end = first;
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

  std::vector<std::string> files = fs_.list(dir_);
  std::sort(files.begin(), files.end());  // YYYY-MM-DD.jsonl sorts chronologically

  // Replay sessions first (LWW by id). This file is small and bounded by the
  // number of sub-agent sessions, not by message volume.
  for (const auto& name : files) {
    if (name != kSessionsFile) continue;
    std::string buf = fs_.readAll(dir_ + "/" + name);
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

  // Collect the day-streams, then walk them NEWEST FIRST so the budget spends
  // itself on the history anyone actually asks for. (The unbounded version went
  // oldest-first and read everything, which is how a board with a month of chat
  // stopped being able to boot at all.)
  std::vector<std::string> days;
  for (const auto& name : files) {
    if (name == kSessionsFile) continue;
    if (name.size() < 6 || name.compare(name.size() - 6, 6, ".jsonl") != 0) continue;
    days.push_back(name);
  }

  std::vector<IdxRec> newestFirst;
  std::vector<CachedMsg> recentNewestFirst;   // CUM-185: cache mirror (PSRAM strings)
  size_t bytesRead = 0;
  bool truncated = false;

  for (auto it = days.rbegin(); it != days.rend(); ++it) {
    if ((maxRows > 0 && (int)newestFirst.size() >= maxRows) ||
        (maxBytes > 0 && bytesRead >= maxBytes)) { truncated = true; break; }
    const std::string path = dir_ + "/" + *it;
    // Read the file's TAIL, not the whole thing. Checking the budget between
    // files is not enough: one day-stream can be megabytes on a busy device, and
    // readAll would pull all of it into a single string before the next budget
    // check ever ran - which is exactly how the boot scan blew past the watchdog
    // and left a board unable to start.
    const size_t room = (maxBytes > 0 && maxBytes > bytesRead)
                            ? (maxBytes - bytesRead) : kHydrateFileWindow;
    const size_t window = room < kHydrateFileWindow ? room : kHydrateFileWindow;
    const long fsize = fs_.size(path);
    std::string buf;
    long bufBase = 0;             // FILE offset of buf[0] (tail reads start mid-file)
    if (fsize > 0 && (size_t)fsize > window) {
      // Take the NEWEST bytes, then drop the partial first record a tail read
      // almost certainly starts inside, so decodeEpisodicLine never sees a torn row.
      buf = fs_.readRange(path, fsize - (long)window, (long)window);
      const size_t nl = buf.find('\n');
      // ⚠ The index stores FILE offsets; forEachLine reports offsets within
      // `buf`. Without this base every row indexed from a tail-read file pointed
      // at the wrong bytes - readRec() then decoded garbage and dropped the row,
      // so on any device with a day-stream over the window (~128 KB, i.e. any
      // busy month) episodic search silently returned nothing for those rows and
      // the RAM ring was quietly carrying the whole feature.
      bufBase = (nl == std::string::npos) ? 0 : (fsize - (long)window + (long)nl + 1);
      buf = (nl == std::string::npos) ? std::string() : buf.substr(nl + 1);
      truncated = true;
    } else {
      buf = fs_.readAll(path);
    }
    bytesRead += buf.size();
    // Within a day the file is append-ordered, so collect then reverse to keep
    // the newest-first accumulation consistent.
    std::vector<IdxRec> dayIdx;
    std::vector<EpisodicMessage> dayMsgs;
    forEachLine(buf, [&](long off, const std::string& line) {
      EpisodicMessage m;
      if (!decodeEpisodicLine(line, m)) return;  // torn/garbage line skipped
      IdxRec r;
      r.tsHours = m.tsHours;
      r.dayNum = m.tsHours / 24;
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
    for (auto mi = dayMsgs.rbegin();
         mi != dayMsgs.rend() && (int)recentNewestFirst.size() < recentCap_; ++mi)
      recentNewestFirst.push_back(cacheOf(*mi));
    if (yield) yield();   // the caller feeds the watchdog here
  }

  // Flip both back to the oldest-first order the rest of the class assumes.
  index_.assign(newestFirst.rbegin(), newestFirst.rend());
  recent_.assign(recentNewestFirst.rbegin(), recentNewestFirst.rend());
  hydrateTruncated_ = truncated;
  return (int)index_.size();
}

// ---- retention prune --------------------------------------------------------
EpiPruneReport AppendLogEpisodicStore::prune(uint32_t cutoffDayNum, const std::string& blobDir) {
  EpiPruneReport rep;

  // Which whole day-files are strictly older than the cutoff (and thus removable)?
  std::set<uint32_t> oldDays;
  for (const auto& r : index_)
    if (r.dayNum < cutoffDayNum) oldDays.insert(r.dayNum);

  // Remove each old day-file, but only treat a day as GONE (droppable from the
  // index) once its file is confirmed off disk. If remove() fails on an I/O error
  // the file survives; keeping its index records in sync with disk avoids hydrate()
  // replaying "pruned" rows on the next boot, and the day is retried next pass.
  std::set<uint32_t> gone;
  for (uint32_t day : oldDays) {
    std::string path = dayFile(day);
    fs_.remove(path);
    if (!fs_.exists(path)) {
      gone.insert(day);
      rep.removedDayFiles.push_back(path);
    }
  }

  // Drop from the index + recent ring only the records whose day-file is gone.
  index_.erase(std::remove_if(index_.begin(), index_.end(),
                              [&](const IdxRec& r) { return gone.count(r.dayNum) != 0; }),
               index_.end());
  recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                               [&](const CachedMsg& m) {
                                 return gone.count(m.tsHours / 24) != 0;
                               }),
                recent_.end());

  // Reference-count-scan blobs: delete any sidecar no surviving row references.
  std::set<std::string> referenced;
  for (const auto& r : index_)
    if (!r.blobHash.empty()) referenced.insert(st(r.blobHash));
  std::vector<std::string> present = fs_.list(blobDir);
  for (const auto& name : unreferencedBlobs(present, referenced)) {
    if (fs_.remove(blobDir + "/" + name)) rep.removedBlobs.push_back(name);
  }

  // Compact sessions.jsonl (Release C4): the file was append-only-FOREVER - one
  // row per upsert, replayed whole at every hydrate. Rewrite it as exactly one
  // row per live session. Non-atomic (remove+append), but sessions_ stays in
  // RAM and rows re-append on the next upsert, so the crash window loses only
  // display metadata until then.
  {
    const std::string path = dir_ + "/" + kSessionsFile;
    fs_.remove(path);
    for (const auto& sess : sessions_) appendSessionRow(sess);
  }

  rep.keptMessages = (int)index_.size();
  return rep;
}

}  // namespace orch
}  // namespace nimbus
