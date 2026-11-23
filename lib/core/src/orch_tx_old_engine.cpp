// What engine 2 reads from the earlier engine at a chat's first start (tx/old_engine.h), lane
// E2-MIG. Read only: the episodic store is queried, the fold record and the rules are copies.
#include "nimbus/orch/tx/old_engine.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "nimbus/orch/history_rows.h"   // kClockValidHours

namespace nimbus {
namespace orch {
namespace tx {

uint64_t rowIdNumber(const std::string& id) {
  if (id.size() < 2 || id[0] != 'm') return 0;
  char* end = nullptr;
  const unsigned long long n = std::strtoull(id.c_str() + 1, &end, 16);
  return (end && *end == 0) ? (uint64_t)n : 0;
}

std::string rowIdString(uint64_t n) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "m%08llx", (unsigned long long)n);
  return buf;
}

bool hasTag(const std::string& tags, const char* tag) {
  const std::string want(tag);
  size_t start = 0;
  while (start <= tags.size()) {
    size_t comma = tags.find(',', start);
    if (comma == std::string::npos) comma = tags.size();
    if (tags.compare(start, comma - start, want) == 0 && comma - start == want.size()) return true;
    start = comma + 1;
  }
  return false;
}

namespace {

RowRole roleOf(const EpisodicMessage& m) {
  if (m.role == "tool" || m.kind == MsgKind::ToolOutput) return RowRole::Tool;
  if (m.role == "user") {
    switch (m.kind) {
      case MsgKind::Message:
      case MsgKind::Image:
      case MsgKind::File:
      case MsgKind::Audio:
      case MsgKind::Transcript: return RowRole::Owner;
      default: return RowRole::Other;
    }
  }
  if (m.role == "assistant" && m.kind == MsgKind::Message) return RowRole::Assistant;
  return RowRole::Other;
}

}  // namespace

DayRow dayRowOf(const EpisodicMessage& m) {
  DayRow r;
  r.id = rowIdNumber(m.id);
  r.t = m.tsHours >= kClockValidHours ? (uint64_t)m.tsHours * 3600u : 0;   // boot-relative hours: unknown
  r.role = roleOf(m);
  r.photo = m.kind == MsgKind::Image;
  r.ours = hasTag(m.tags, "e2");
  r.text = m.text;
  return r;
}

OldFold oldFoldOf(const ChatFold& f) {
  OldFold o;
  o.summary = f.summary;
  o.thru = rowIdNumber(f.thru);
  o.shift = rowIdNumber(f.softStart);
  o.foldEpoch = f.lastFoldEpoch;
  return o;
}

Status dropMirroredPointer(RowSource& rows, OldFold& fold, bool& dropped) {
  dropped = false;
  if (!fold.thru) return Status::success();
  std::vector<DayRow> page;
  bool end = false;
  const Status s = rows.read(fold.thru - 1, fold.thru, 1, page, end);
  if (!s.ok()) return s;
  if (page.empty() || page.front().id != fold.thru || !page.front().ours) return Status::success();
  fold.thru = 0;
  fold.shift = 0;
  dropped = true;
  return Status::success();
}

namespace {

// The whole record of `chatId` in a fold blob (its fields, the 0x1E end included); npos when none.
size_t recordOf(const std::string& blob, const std::string& chatId, size_t& len) {
  for (size_t at = 0, end; (end = blob.find('\x1E', at)) != std::string::npos; at = end + 1) {
    if (blob.compare(at, chatId.size(), chatId) == 0 && blob.size() > at + chatId.size() && blob[at + chatId.size()] == '\x1F') {
      len = end - at + 1;
      return at;
    }
  }
  return std::string::npos;
}

// The mirror's shape (orch_tx_digest.cpp mirrorRecord): counters 0 and ";m" in the epoch field.
bool mirrorShape(const std::string& rec) {
  std::vector<std::string> f;
  for (size_t at = 0;;) {
    const size_t sep = rec.find('\x1F', at);
    if (sep == std::string::npos || f.size() == 7) break;
    f.push_back(rec.substr(at, sep - at));
    at = sep + 1;
  }
  if (f.size() < 7) return false;
  return f[1] == "0" && f[2] == "0" && f[3].find(";m") != std::string::npos && f[4] == "0" && f[5] == "0" &&
         f[6] == "0";
}

}  // namespace

bool restoreFoldRecord(const std::string& blob, const std::string& copy, const std::string& chatId, std::string& out) {
  out.clear();
  size_t len = 0, copyLen = 0;
  const size_t at = recordOf(blob, chatId, len);
  if (chatId.empty() || at == std::string::npos || !mirrorShape(blob.substr(at, len))) return false;
  const size_t from = recordOf(copy, chatId, copyLen);
  out = blob.substr(0, at);
  if (from != std::string::npos) out += copy.substr(from, copyLen);
  out += blob.substr(at + len);
  return true;
}

// ---- EpisodicRowSource ----------------------------------------------------------------------

namespace {

// One store page into `out`: oldest first, each row once, none past upTo. True when a row past
// upTo ended the read.
bool takeRows(const std::vector<EpisodicMessage>& rows, uint64_t after, uint64_t upTo, std::vector<DayRow>& out) {
  for (const EpisodicMessage& m : rows) {
    DayRow r = dayRowOf(m);
    if (r.id <= after || (!out.empty() && r.id <= out.back().id)) continue;
    if (upTo && r.id > upTo) return true;
    out.push_back(std::move(r));
  }
  return false;
}

// The forward query of one chat from `cursor`, cold scan on.
MsgQuery forwardQuery(const std::string& sid, const std::string& cursor, size_t limit) {
  MsgQuery q;
  q.sessionId = sid;
  q.coldScan = true;
  q.after = cursor;
  q.limit = (int)limit;
  return q;
}

// The newest row taken, else the read's start.
uint64_t newestOf(const std::vector<DayRow>& out, uint64_t after) { return out.empty() ? after : out.back().id; }

// The cursor after a page: the store's token, else the newest row id taken.
std::string nextCursor(const EpiQueryInfo& info, uint64_t newest) {
  return info.nextAfter.empty() ? rowIdString(newest) : info.nextAfter;
}

}  // namespace

Status EpisodicRowSource::read(uint64_t after, uint64_t upTo, size_t max, std::vector<DayRow>& out, bool& end) {
  out.clear();
  end = false;
  if (!max) return Status::success();
  // A read that goes on where the previous one stopped continues from the store's own token (a
  // cold byte cursor); any other start is the row id.
  const bool goesOn = after && after == lastServed_ && !resume_.empty();
  std::string cursor = goesOn ? resume_ : rowIdString(after);
  for (uint32_t n = 0; n < kMaxQueriesPerRead && !end && out.size() < max; ++n) cursor = page(cursor, after, upTo, max, out, end);
  if (!end && out.empty()) return Status::fail(Err::Io, "tx::EpisodicRowSource::read");
  lastServed_ = newestOf(out, after);
  resume_ = end ? std::string() : cursor;
  return Status::success();
}

std::string EpisodicRowSource::page(const std::string& cursor, uint64_t after, uint64_t upTo, size_t max,
                                    std::vector<DayRow>& out, bool& end) {
  EpiQueryInfo info;
  const std::vector<EpisodicMessage> rows = query_(forwardQuery(sid_, cursor, max - out.size()), info);
  ++queries_;
  if (!info.unreadable.empty()) unreadable_.push_back(info.unreadable);
  // A stale byte cursor names a region that is gone: the row id again.
  if (info.cursorStale) return rowIdString(newestOf(out, after));
  end = takeRows(rows, after, upTo, out) || !info.newerExists;
  return nextCursor(info, newestOf(out, after));
}

Status EpisodicRowSource::newest(uint64_t& id) {
  id = 0;
  // The store's newest-first page is a starting point only: after a restart its RAM ring is in
  // arrival order, not id order (rows stored before the clock was set come back out of place), so
  // the forward query then walks from the largest id seen to the real end.
  {
    MsgQuery q;
    q.sessionId = sid_;
    q.limit = (int)kRowPage;
    EpiQueryInfo info;
    for (const EpisodicMessage& m : query_(q, info)) id = std::max(id, rowIdNumber(m.id));
    ++queries_;
  }
  std::string cursor = rowIdString(id);
  for (uint32_t n = 0; n < kMaxQueriesPerRead; ++n) {
    EpiQueryInfo info;
    const std::vector<EpisodicMessage> rows = query_(forwardQuery(sid_, cursor, kRowPage), info);
    ++queries_;
    if (info.cursorStale) {
      cursor = rowIdString(id);
      continue;
    }
    for (const EpisodicMessage& m : rows) id = std::max(id, rowIdNumber(m.id));
    if (!info.newerExists) return Status::success();
    cursor = nextCursor(info, id);
  }
  return Status::fail(Err::Io, "tx::EpisodicRowSource::newest");
}

// ---- the standing rules ---------------------------------------------------------------------

namespace {

uint32_t fnv1a(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

std::map<uint32_t, uint32_t> parseMarker(const std::string& marker) {
  std::map<uint32_t, uint32_t> m;
  size_t start = 0;
  while (start < marker.size()) {
    size_t nl = marker.find('\n', start);
    if (nl == std::string::npos) nl = marker.size();
    const std::string line = marker.substr(start, nl - start);
    start = nl + 1;
    unsigned id = 0, h = 0;
    if (std::sscanf(line.c_str(), "p%u %x", &id, &h) == 2 && id) m[(uint32_t)id] = (uint32_t)h;
  }
  return m;
}

// The owner's notes by key (pinned and unpinned): the value each holds.
std::map<std::string, std::string> notesByKey(notes::Notebook& nb, const Principal& owner) {
  std::map<std::string, std::string> out;
  const std::string block = nb.pinnedBlock(owner);
  size_t start = 0;
  while (start < block.size()) {
    size_t nl = block.find('\n', start);
    if (nl == std::string::npos) nl = block.size();
    notes::Note n;
    if (notes::parseLine(block.substr(start, nl - start), n)) out[n.key] = n.value;
    start = nl + 1;
  }
  std::vector<notes::Note> more;
  if (nb.unpinned(owner, more))
    for (const notes::Note& n : more) out[n.key] = n.value;
  return out;
}

bool setRule(notes::Notebook& nb, const Principal& owner, const std::string& key, const std::string& text,
             RulesImportOutcome& out) {
  JsonDocument args;
  args["op"] = "set";
  args["key"] = key;
  args["value"] = text;
  args["kind"] = "rule";
  args["pin"] = true;
  const ToolResult r = nb.call(args.as<JsonObjectConst>(), owner, notes::Origin::Attended);
  if (r.success) return true;
  ++out.refused;
  if (out.firstError.empty()) out.firstError = key + ": " + r.error;
  return false;
}

}  // namespace

std::string ruleNoteKey(uint32_t id) { return "rule-p" + std::to_string(id); }

void importRules(const std::vector<OldRule>& rules, const std::string& marker, notes::Notebook& nb,
                 const Principal& owner, std::string& markerOut, RulesImportOutcome& out) {
  out = RulesImportOutcome{};
  std::map<uint32_t, uint32_t> copied = parseMarker(marker);
  const std::map<std::string, std::string> have = notesByKey(nb, owner);
  for (const OldRule& rule : rules) {
    if (!rule.id) continue;
    const uint32_t h = fnv1a(rule.text);
    const std::string key = ruleNoteKey(rule.id);
    const auto note = have.find(key);
    const auto was = copied.find(rule.id);
    if (was != copied.end() && was->second == h) {
      ++out.kept;
      continue;
    }
    if (was != copied.end()) {
      // The earlier engine changed the rule: follow it only where engine 2 left the copy alone.
      const bool untouched = note != have.end() && fnv1a(note->second) == was->second;
      if (untouched && !setRule(nb, owner, key, rule.text, out)) continue;
      ++(untouched ? out.updated : out.kept);
      copied[rule.id] = h;
      continue;
    }
    if (note != have.end()) {   // a note of this key is there already (a copy whose record was lost)
      ++out.kept;
      copied[rule.id] = h;
      continue;
    }
    if (!setRule(nb, owner, key, rule.text, out)) continue;
    ++out.added;
    copied[rule.id] = h;
  }
  markerOut.clear();
  for (const auto& kv : copied) {
    char line[32];
    std::snprintf(line, sizeof(line), "p%u %08x\n", (unsigned)kv.first, (unsigned)kv.second);
    markerOut += line;
  }
}

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
