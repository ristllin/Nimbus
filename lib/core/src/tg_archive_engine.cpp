// tg_archive - the engine: writes inbound updates, outbound messages and media
// files to the card through the Fs seam, keeps the download queue, applies the
// stranger budgets, and runs the storage-pressure pass. Pure policy lives in
// tg_archive.cpp.
#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "nimbus/net/tg_dup_guard.h"   // dupHash: the same chat key as the duplicate guard
#include "nimbus/tg_archive.h"

namespace nimbus {
namespace tgarc {

using ArduinoJson::JsonDocument;

namespace {

constexpr int64_t kPlausibleClock = 1577836800LL;   // 2020-01-01: the device clock is set
constexpr size_t kCopySlice = 4096;
constexpr size_t kSeqCacheMax = 8;
constexpr size_t kSeenRing = 64;   // updates remembered per boot (over six full pages)
constexpr const char* kNoChat = "0";   // updates with no chat, and stranger overflow rows
// FAT gives every file whole clusters (16-32 KB on a 16-32 GB card); counting a file
// as 32 KB at least over-estimates what deleting it frees, so the pressure pass stops
// early and the next pass removes more, rather than deleting far more than needed.
constexpr uint64_t kClusterEstimate = 32 * 1024;

uint64_t onCard(uint64_t size) {
  const uint64_t clusters = size ? (size + kClusterEstimate - 1) / kClusterEstimate : 1;
  return clusters * kClusterEstimate;
}

// Every JSON document the engine builds rides this allocator when one is set (the
// device routes it to PSRAM, so even the small strings stay off internal SRAM).
ArduinoJson::Allocator* g_alloc = nullptr;
JsonDocument newDoc() { return g_alloc ? JsonDocument(g_alloc) : JsonDocument(); }

std::string toJson(const JsonDocument& doc) {
  std::string s;
  serializeJson(doc, s);
  return escapeControls(s);
}

std::string baseOf(const std::string& name) {
  const size_t dot = name.find('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string jobName(const Job& j) { return j.base + "." + j.ext; }

std::string inRow(const InMeta& m, const std::string& rawName, const std::string& fileName,
                  const std::string& st) {
  JsonDocument d = newDoc();
  d["v"] = 1;
  d["dir"] = "in";
  d["u"] = (long long)m.updateId;
  d["upd"] = m.updateType;
  d["chat"] = m.chat;
  if (!m.from.empty()) d["from"] = m.from;
  if (!m.fromId.empty()) d["fromId"] = m.fromId;
  if (m.messageId) d["mid"] = (long long)m.messageId;
  d["date"] = (long long)m.date;
  if (!m.mediaGroupId.empty()) d["mg"] = m.mediaGroupId;
  if (m.edited) d["edited"] = true;
  d["kind"] = contentInfo(m.content).name;
  if (!rawName.empty()) d["raw"] = rawName;
  if (!fileName.empty()) d["file"] = fileName;
  if (m.hasMedia) {
    d["fid"] = m.media.fileId;
    if (m.media.size) d["size"] = (unsigned long long)m.media.size;
  }
  d["st"] = st;
  return toJson(d);
}

std::string eventRow(const std::string& ref, const std::string& file, const char* st,
                     uint64_t bytes, const std::string& err) {
  JsonDocument d = newDoc();
  d["v"] = 1;
  d["ev"] = "media";
  d["ref"] = ref;
  d["file"] = file;
  d["st"] = st;
  d["bytes"] = (unsigned long long)bytes;
  if (!err.empty()) d["err"] = err;
  return toJson(d);
}

// The outbound record: the shape of a Telegram message, plus where it came from.
// The caller adds "text", or "caption" and the file fields.
void outItem(JsonDocument& d, const std::string& chat, uint32_t seq, int64_t epoch,
             const std::string& origin) {
  d["out"] = true;
  d["seq"] = seq;
  d["chat"]["id"] = (long long)std::strtoll(chat.c_str(), nullptr, 10);
  d["date"] = (long long)epoch;
  d["origin"] = origin;
}

struct OutRowArgs {
  std::string chat;
  uint32_t    seq = 0;
  int64_t     epoch = 0;
  std::string origin, kind, raw, file, st;
  uint64_t    bytes = 0;
};

std::string outRow(const OutRowArgs& a) {
  JsonDocument d = newDoc();
  d["v"] = 1;
  d["dir"] = "out";
  d["s"] = a.seq;
  d["chat"] = a.chat;
  d["date"] = (long long)a.epoch;
  d["origin"] = a.origin;
  d["kind"] = a.kind;
  d["raw"] = a.raw;
  if (!a.file.empty()) d["file"] = a.file;
  d["bytes"] = (unsigned long long)a.bytes;
  d["st"] = a.st;
  return toJson(d);
}

// The extension of an outbound file: its own when plain (a PNG stays a PNG), else
// by send kind.
std::string outExt(const std::string& kind, const std::string& srcPath) {
  Media m;
  m.type = Content::Document;
  m.fileName = srcPath.substr(srcPath.find_last_of('/') + 1);
  if (kind == "photo") m.mime = "image/jpeg";
  else if (kind == "voice") m.mime = "audio/ogg";
  else if (kind == "audio") m.mime = "audio/mpeg";
  return mediaExt(m);
}

}  // namespace

std::string escapeControls(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c >= 0x20) {
      out.push_back((char)c);
      continue;
    }
    char b[8];
    std::snprintf(b, sizeof b, "\\u%04x", c);
    out += b;
  }
  return out;
}

// ---- small-file helpers -------------------------------------------------------

void setJsonAllocator(ArduinoJson::Allocator* alloc) { g_alloc = alloc; }
ArduinoJson::Allocator* jsonAllocator() { return g_alloc; }

void Archive::setRedactions(std::vector<std::string> secrets) {
  redact_.clear();
  for (std::string& s : secrets)
    if (s.size() >= 8 && std::find(redact_.begin(), redact_.end(), s) == redact_.end())
      redact_.push_back(std::move(s));
}

std::string Archive::scrub(const std::string& in) const {
  std::string s = in;
  for (const std::string& secret : redact_)
    for (size_t at = s.find(secret); at != std::string::npos; at = s.find(secret, at))
      s.replace(at, secret.size(), "[redacted]");
  return s;
}

// Every record goes through one of the two writers below, so nothing reaches the
// card unscrubbed.
bool Archive::writeSmallAtomic(const std::string& path, const std::string& bytes) {
  const std::string tmp = path + ".tmp";
  const std::string clean = scrub(bytes);
  std::unique_ptr<Writer> w = fs_.openWrite(tmp, false);
  bool ok = w && w->write((const uint8_t*)clean.data(), clean.size());
  ok = w && w->close() && ok;
  if (ok) ok = fs_.rename(tmp, path);
  if (!ok) fs_.remove(tmp);
  return ok;
}

bool Archive::appendLine(const std::string& path, const std::string& line) {
  std::unique_ptr<Writer> w = fs_.openWrite(path, true);
  if (!w) return false;
  const std::string l = scrub(line) + "\n";
  const bool ok = w->write((const uint8_t*)l.data(), l.size());
  return w->close() && ok;
}

namespace {

// Lines of a file to `fn` until it returns true. 1: stopped there, 0: read to the end,
// -1: the file could not be opened. Lines over 16 KB are skipped.
int streamLines(Fs& fs, const std::string& path, const std::function<bool(const std::string&)>& fn) {
  std::unique_ptr<Reader> r = fs.openRead(path);
  if (!r) return -1;
  std::string line;
  bool skipping = false;
  uint8_t buf[512];
  for (int n = r->read(buf, sizeof buf); n > 0; n = r->read(buf, sizeof buf)) {
    for (int i = 0; i < n; ++i) {
      const char c = (char)buf[i];
      if (c == '\n') {
        if (!skipping && !line.empty() && fn(line)) return 1;
        line.clear();
        skipping = false;
      } else if (!skipping) {
        line.push_back(c);
        if (line.size() > 16384) { line.clear(); skipping = true; }
      }
    }
  }
  return (!skipping && !line.empty() && fn(line)) ? 1 : 0;
}

}  // namespace

bool forEachLineUntil(Fs& fs, const std::string& path, const std::function<bool(const std::string&)>& fn) {
  return streamLines(fs, path, fn) == 1;
}

bool forEachLine(Fs& fs, const std::string& path,
                 const std::function<void(const std::string&)>& fn) {
  return streamLines(fs, path, [&](const std::string& l) {
           fn(l);
           return false;
         }) >= 0;
}

bool Archive::indexHas(const std::string& indexPath, const std::string& needle) {
  return forEachLineUntil(fs_, indexPath,
                          [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

bool Archive::seenThisBoot(uint32_t chat, int64_t id) const {
  for (const SeenId& s : seen_)
    if (s.chat == chat && s.id == id) return true;
  return false;
}

void Archive::noteSeen(uint32_t chat, int64_t id) {
  if (seenThisBoot(chat, id)) return;
  if (seen_.size() < kSeenRing) {
    seen_.push_back({chat, id});
    return;
  }
  seen_[seenHead_] = {chat, id};
  seenHead_ = (seenHead_ + 1) % kSeenRing;
}

// ---- inbound ----------------------------------------------------------------

int Archive::captureInboundPage(const char* body, size_t len, const Now& now) {
  std::vector<Span> spans;
  splitResult(body, len, spans);
  int added = 0;
  for (const Span& s : spans)
    if (archiveOneUpdate(body + s.off, s.len, now)) ++added;
  return added;
}

bool Archive::archiveOneUpdate(const char* raw, size_t len, const Now& now) {
  InMeta m;
  if (!parseUpdateMeta(raw, len, m)) { ++stats_.writeErrors; return false; }
  if (m.date > lastServerEpoch_) {
    lastServerEpoch_ = m.date;
    lastServerMs_ = now.ms;
  }
  // An update with no chat still is archived, under "0" (no real chat has that id),
  // within the stranger budgets: anyone can cause one.
  const std::string chat = validChatDir(m.chat) ? m.chat : kNoChat;
  const uint32_t chatKey = nimbus::net::dupHash(chat);
  if (seenThisBoot(chatKey, m.updateId)) {   // written or verified this boot: no card read
    ++stats_.dupes;
    return false;
  }
  const bool stranger = chat == kNoChat || (trust_ && trust_(chat) == Trust::Stranger);
  curTrust_ = stranger ? Trust::Stranger : Trust::Allowed;
  const std::string day = dayFromEpoch(m.date);
  const std::string base = inBase(m.updateId);
  const std::string rawPath = itemPath(chat, day, base + ".json");
  if (fs_.exists(rawPath)) {
    replayed(m, chat, day, base);
    noteSeen(chatKey, m.updateId);
    return false;
  }
  if (stranger && !strangerAdmitted(m, chat, day, len)) return false;
  fs_.mkdirs(dayDirPath(chat, day));
  if (!writeSmallAtomic(rawPath, std::string(raw, len))) { ++stats_.writeErrors; return false; }
  const uint32_t errs = stats_.writeErrors;
  writeInRow(m, chat, day, base);
  if (stats_.writeErrors == errs) noteSeen(chatKey, m.updateId);   // a lost line is repaired on replay
  ++stats_.inItems;
  return true;
}

// A stranger's update within its budgets (true), or the reason it is not written:
// a replay already recorded as over the limit, a spent budget, the folder cap.
bool Archive::strangerAdmitted(const InMeta& m, const std::string& chat, const std::string& day, size_t len) {
  if (strangerOverflowSeen(chat, day, m.updateId)) {
    ++stats_.dupes;   // a replay of an update already recorded as over the limit
    return false;
  }
  const Admit a = admitStranger(chat, day, len);
  if (a == Admit::No) {
    ++stats_.strangerSkipped;
    return false;
  }
  if (a == Admit::Overflow) {
    writeOverflowRow(m, day);
    return false;
  }
  return true;
}

// A replay of an update already on the card: nothing new is written. A crash between
// the raw write and its index line leaves the line missing; the replay restores it.
void Archive::replayed(const InMeta& m, const std::string& chat, const std::string& day,
                       const std::string& base) {
  ++stats_.dupes;
  char needle[40];
  std::snprintf(needle, sizeof needle, "\"u\":%lld,", (long long)m.updateId);
  if (indexHas(indexPathOf(chat, day), needle)) return;
  writeInRow(m, chat, day, base);
  ++stats_.repaired;
}

// Charge one stranger update against the day's budgets. No: a budget is spent (the
// update is only counted). Overflow: within budget, but the card already holds
// kStrangerChatsMax stranger chats, so it gets one row in the "0" directory.
Archive::Admit Archive::admitStranger(const std::string& chat, const std::string& day,
                                      size_t bytes) {
  const std::string budgetDay = day == "undated" ? strangerDay_ : day;
  if (budgetDay > strangerDay_) loadStrangerDay(budgetDay);   // a new day (or a restart)
  if (strangerUpdates_ >= kStrangerDayUpdates || strangerBytes_ + bytes > kStrangerDayBytes)
    return Admit::No;
  auto it = std::find_if(strangerCount_.begin(), strangerCount_.end(),
                         [&](const std::pair<std::string, uint32_t>& e) { return e.first == chat; });
  if (it == strangerCount_.end()) {
    if (strangerCount_.size() >= kStrangerTableMax) return Admit::No;   // full: deny, never evict
    strangerCount_.emplace_back(chat, 0);
    it = strangerCount_.end() - 1;
  }
  if (it->second >= kStrangerDailyCap) return Admit::No;
  ++it->second;
  ++strangerUpdates_;
  strangerBytes_ += bytes;
  return knownStrangerDir(chat) ? Admit::Yes : Admit::Overflow;
}

// The day's stranger budgets as the card shows them: raw updates already archived for
// stranger chats on `day` (and stranger-limit rows in "0"), so a restart mid-day
// resumes the day's budget instead of starting a fresh one.
void Archive::loadStrangerDay(const std::string& day) {
  strangerDay_ = day;
  strangerCount_.clear();
  strangerUpdates_ = 0;
  strangerBytes_ = 0;
  for (const std::string& chat : listChats(fs_)) {
    if (!isStrangerChat(chat)) continue;
    const uint32_t n = strangerItemsOn(chat, day);
    strangerUpdates_ += n;
    if (n && chat != kNoChat && strangerCount_.size() < kStrangerTableMax) strangerCount_.emplace_back(chat, n);
  }
}

bool Archive::isStrangerChat(const std::string& chat) const {
  return chat == kNoChat || (isTelegramChat(chat) && trust_ && trust_(chat) == Trust::Stranger);
}

// Stranger items on the card for one chat and day: raw inbound updates ("<digits>.json",
// their bytes added to the day's total), plus "stranger limit" rows in "0".
uint32_t Archive::strangerItemsOn(const std::string& chat, const std::string& day) {
  uint32_t n = 0;
  for (const Entry& e : fs_.list(dayDirPath(chat, day))) {
    const bool rawIn = !e.dir && !e.name.empty() && std::isdigit((unsigned char)e.name[0]) &&
                       e.name.size() > 5 && e.name.compare(e.name.size() - 5, 5, ".json") == 0;
    if (!rawIn) continue;
    ++n;
    strangerBytes_ += e.size;
  }
  if (chat == kNoChat)
    forEachLine(fs_, indexPathOf(chat, day), [&](const std::string& l) {
      if (l.find("not archived: stranger limit") != std::string::npos) ++n;
    });
  return n;
}

// Whether `chat` may have (or already has) its own directory: the "0" bucket always,
// a stranger chat while fewer than kStrangerChatsMax are on the card.
bool Archive::knownStrangerDir(const std::string& chat) {
  if (!strangerDirsLoaded_) {
    strangerDirsLoaded_ = true;
    for (const std::string& c : listChats(fs_))
      if (c != kNoChat && isTelegramChat(c) && trust_ && trust_(c) == Trust::Stranger)
        strangerDirs_.push_back(c);
  }
  if (chat == kNoChat) return true;
  if (std::find(strangerDirs_.begin(), strangerDirs_.end(), chat) != strangerDirs_.end()) return true;
  if (strangerDirs_.size() >= kStrangerChatsMax) return false;
  strangerDirs_.push_back(chat);
  return true;
}

// True when `chat` has no folder of its own (the stranger folder cap is reached) and
// this update already has its "stranger limit" row: a replay must not be charged or
// recorded twice.
bool Archive::strangerOverflowSeen(const std::string& chat, const std::string& day, int64_t updateId) {
  if (chat == kNoChat) return false;
  knownStrangerDir(kNoChat);   // loads the folder list without claiming a slot
  if (std::find(strangerDirs_.begin(), strangerDirs_.end(), chat) != strangerDirs_.end()) return false;
  if (strangerDirs_.size() < kStrangerChatsMax) return false;
  char needle[40];
  std::snprintf(needle, sizeof needle, "\"u\":%lld,", (long long)updateId);
  return indexHas(indexPathOf(kNoChat, day), needle);
}

void Archive::writeOverflowRow(const InMeta& m, const std::string& day) {
  fs_.mkdirs(dayDirPath(kNoChat, day));
  if (!appendLine(indexPathOf(kNoChat, day), inRow(m, "", "", "not archived: stranger limit")))
    ++stats_.writeErrors;
  ++stats_.notArchived;
}

void Archive::writeInRow(const InMeta& m, const std::string& chat, const std::string& day,
                         const std::string& base) {
  const MediaRow r = recordMedia(m, chat, day, base);
  if (!appendLine(indexPathOf(chat, day), inRow(m, base + ".json", r.file, r.st)))
    ++stats_.writeErrors;
}

Archive::MediaRow Archive::recordMedia(const InMeta& m, const std::string& chat,
                                       const std::string& day, const std::string& base) {
  MediaRow r{"", "ok"};
  auto skip = [&](const std::string& why) {
    r.st = "not archived: " + why;
    ++stats_.notArchived;
    return r;
  };
  const ContentInfo ci = contentInfo(m.content);
  if (ci.policy == Policy::NotArchived) return skip(ci.name);
  if (ci.policy != Policy::Download || !m.hasMedia) return r;
  if (curTrust_ == Trust::Stranger) return skip("sender not on the allowlist");
  if (m.media.size > kMaxFileBytes) return skip("too large");
  const std::string ext = mediaExt(m.media);
  r.file = base + "." + ext;
  if (fs_.exists(itemPath(chat, day, r.file))) return r;
  r.st = "pending";
  Job j;
  j.chat = chat;
  j.day = day;
  j.base = base;
  j.ext = ext;
  j.fileId = m.media.fileId;
  enqueue(j);
  return r;
}

bool Archive::enqueue(const Job& j) {
  for (const Job& q : jobs_)
    if (q.chat == j.chat && q.day == j.day && q.base == j.base) return false;
  if (jobs_.size() >= kMaxJobs) {
    overflowed_ = true;
    return false;
  }
  jobs_.push_back(j);
  return true;
}

// ---- one download, shared with the turn path -------------------------------------

bool Archive::adoptMedia(const std::string& chat, int64_t updateId, int64_t date, const std::string& fileId,
                         Fs& src, const std::string& srcPath) {
  if (!validChatDir(chat) || fileId.empty()) return false;
  const std::string day = dayFromEpoch(date), base = inBase(updateId);
  auto it = std::find_if(jobs_.begin(), jobs_.end(),
                         [&](const Job& j) { return j.chat == chat && j.day == day && j.base == base; });
  if (it == jobs_.end() || it->fileId != fileId) return false;
  const Job j = *it;
  const std::string file = itemPath(chat, day, jobName(j));
  const std::string part = file + ".part";
  uint64_t bytes = 0;
  if (!fs_.exists(file)) {
    if (!copyIn(src, srcPath, part, bytes) || bytes == 0) {
      fs_.remove(part);
      return false;   // the job stays queued: the archive fetches it itself
    }
    if (!fs_.rename(part, file)) {
      fs_.remove(part);
      return false;
    }
  }
  jobs_.erase(it);   // nothing above touched the queue: the iterator is still valid
  finishJob(j, "ok", stats_.mediaOk, bytes, "");
  return true;
}

std::string Archive::mediaFor(const std::string& chat, int64_t updateId, int64_t date,
                              const std::string& fileId) {
  if (!validChatDir(chat) || fileId.empty()) return std::string();
  const std::string day = dayFromEpoch(date), base = inBase(updateId);
  std::unique_ptr<Reader> r = fs_.openRead(itemPath(chat, day, base + ".json"));
  if (!r) return std::string();
  std::string raw;
  uint8_t buf[512];
  for (int n = r->read(buf, sizeof buf); n > 0 && raw.size() < 64 * 1024; n = r->read(buf, sizeof buf))
    raw.append((const char*)buf, (size_t)n);
  InMeta m;
  if (!parseUpdateMeta(raw.data(), raw.size(), m) || !m.hasMedia || m.media.fileId != fileId) return std::string();
  const std::string file = itemPath(chat, day, base + "." + mediaExt(m.media));
  return fs_.exists(file) ? file : std::string();
}

// ---- outbound -----------------------------------------------------------------

int64_t Archive::bestEpoch(const Now& now) const {
  if (now.epoch >= kPlausibleClock) return now.epoch;
  if (lastServerEpoch_ > 0) return lastServerEpoch_ + (int64_t)((now.ms - lastServerMs_) / 1000);
  return 0;
}

uint32_t Archive::claimOutSeq(const std::string& chat, const std::string& day) {
  const std::string key = chat + "/" + day;
  auto it = std::find_if(seqCache_.begin(), seqCache_.end(),
                         [&](const std::pair<std::string, uint32_t>& e) { return e.first == key; });
  uint32_t seq;
  if (it != seqCache_.end()) {
    seq = it->second;
  } else {
    std::vector<std::string> names;
    for (const Entry& e : fs_.list(dayDirPath(chat, day))) names.push_back(e.name);
    seq = nextOutSeq(names);
  }
  // Belt and braces: never reuse a name that exists, whatever the cache says.
  for (int guard = 0; guard < 10000 && fs_.exists(itemPath(chat, day, outBase(seq) + ".json")); ++guard)
    ++seq;
  if (it != seqCache_.end()) {
    it->second = seq + 1;
  } else {
    if (seqCache_.size() >= kSeqCacheMax) seqCache_.erase(seqCache_.begin());
    seqCache_.emplace_back(key, seq + 1);
  }
  return seq;
}

// The day, directory and sequence number of the next outbound item for `chat`.
// Only real Telegram chats: the web, serial and voice pseudo-chats are not Telegram.
bool Archive::claimOut(const std::string& chat, const Now& now, OutSlot& slot) {
  if (!isTelegramChat(chat)) return false;
  slot.epoch = bestEpoch(now);
  slot.day = dayFromEpoch(slot.epoch);
  slot.dir = dayDirPath(chat, slot.day);
  fs_.mkdirs(slot.dir);
  slot.seq = claimOutSeq(chat, slot.day);
  return true;
}

bool Archive::captureOutText(const std::string& chat, const std::string& text,
                             const std::string& origin, const Now& now) {
  OutSlot s;
  if (!claimOut(chat, now, s)) return false;
  OutRowArgs a;
  a.chat = chat;
  a.seq = s.seq;
  a.epoch = s.epoch;
  a.raw = outBase(s.seq) + ".json";
  JsonDocument item = newDoc();
  outItem(item, chat, s.seq, s.epoch, origin);
  item["text"] = text;
  if (!writeSmallAtomic(s.dir + "/" + a.raw, toJson(item))) {
    ++stats_.writeErrors;
    return false;
  }
  a.origin = origin;
  a.kind = "text";
  a.bytes = text.size();
  a.st = "ok";
  if (!appendLine(indexPathOf(chat, s.day), outRow(a))) ++stats_.writeErrors;
  ++stats_.outItems;
  return true;
}

bool Archive::copyIn(Fs& src, const std::string& srcPath, const std::string& dstPath,
                     uint64_t& bytes) {
  bytes = 0;
  std::unique_ptr<Reader> r = src.openRead(srcPath);
  std::unique_ptr<Writer> w = fs_.openWrite(dstPath, false);
  if (!r || !w) return false;
  std::vector<uint8_t> buf(kCopySlice);
  bool ok = true;
  int n;
  while ((n = r->read(buf.data(), buf.size())) > 0) {
    if (bytes + (uint64_t)n > kMaxFileBytes || !w->write(buf.data(), (size_t)n)) { ok = false; break; }
    bytes += (uint64_t)n;
  }
  if (n < 0) ok = false;
  return w->close() && ok;
}

bool Archive::captureOutMedia(const OutMedia& om, Fs& src, const Now& now) {
  OutSlot s;
  if (!claimOut(om.chat, now, s)) return false;
  OutRowArgs a;
  a.chat = om.chat;
  a.seq = s.seq;
  a.epoch = s.epoch;
  const std::string base = outBase(s.seq);
  a.raw = base + ".json";
  a.file = base + "." + outExt(om.kind, om.srcPath);
  JsonDocument item = newDoc();
  outItem(item, om.chat, s.seq, s.epoch, om.origin);
  item["caption"] = om.caption;
  item["kind"] = om.kind;
  item["file"] = a.file;
  item["source"] = om.srcPath;
  if (!writeSmallAtomic(s.dir + "/" + a.raw, toJson(item))) { ++stats_.writeErrors; return false; }
  const std::string part = s.dir + "/" + a.file + ".part";
  const bool copied = copyIn(src, om.srcPath, part, a.bytes) && fs_.rename(part, s.dir + "/" + a.file);
  a.origin = om.origin;
  a.kind = om.kind;
  a.st = copied ? "ok" : "incomplete";
  if (!copied) ++stats_.mediaIncomplete;
  if (!appendLine(indexPathOf(om.chat, s.day), outRow(a))) ++stats_.writeErrors;
  ++stats_.outItems;
  return copied;
}

// ---- restored files ---------------------------------------------------------------

bool validRestoredName(const std::string& name) {
  if (name.size() < 20 || name.size() > 23 || name[0] != 'r' || name[17] != '.') return false;
  for (size_t i = 1; i < 17; ++i)
    if (!std::isxdigit((unsigned char)name[i]) || std::isupper((unsigned char)name[i])) return false;
  for (size_t i = 18; i < name.size(); ++i)
    if (!std::islower((unsigned char)name[i]) && !std::isdigit((unsigned char)name[i])) return false;
  return true;
}

bool Archive::recordRestored(const Restored& r) {
  if (!validChatDir(r.chat) || !validDay(r.day) || !validRestoredName(r.file)) return false;
  const std::string needle = "\"file\":\"" + r.file + "\"";
  if (indexHas(indexPathOf(r.chat, r.day), needle)) return true;   // already recorded
  JsonDocument d = newDoc();
  d["v"] = 1;
  d["dir"] = "restored";
  d["chat"] = r.chat;
  d["date"] = (long long)r.date;
  d["kind"] = r.kind;
  if (!r.role.empty()) d["role"] = r.role;
  d["file"] = r.file;
  d["bytes"] = (unsigned long long)r.bytes;
  if (!r.note.empty()) d["src"] = r.note;
  d["st"] = "ok";
  if (!appendLine(indexPathOf(r.chat, r.day), toJson(d))) { ++stats_.writeErrors; return false; }
  return true;
}

// ---- downloads ----------------------------------------------------------------

void Archive::finishJob(const Job& j, const char* st, uint32_t& counter, uint64_t bytes,
                        const std::string& err) {
  ++counter;
  if (!appendLine(indexPathOf(j.chat, j.day), eventRow(j.base, jobName(j), st, bytes, err)))
    ++stats_.writeErrors;
}

// A source error: the job goes to the back of the queue (others proceed) until it
// has used its tries; a permanent error ends it at once.
Step Archive::retryOrFail(const std::string& err, bool permanent) {
  Job j = jobs_.front();
  jobs_.pop_front();
  if (permanent || ++j.tries >= kMaxTries) {
    finishJob(j, "failed", stats_.mediaFailed, 0, err.empty() ? "download failed" : err);
    return Step::Failed;
  }
  jobs_.push_back(j);
  return Step::Retry;
}

struct Archive::Pump {
  uint8_t*                         buf;
  size_t                           cap;
  uint32_t                         budgetMs;
  const std::function<uint32_t()>* nowMs;
  const std::function<bool()>*     yield;
};

enum class Archive::PumpEnd : uint8_t { Eof, SrcError, WriteError, TooLarge, Budget };

namespace {

// Move bytes from the source to the card in slices until the file ends, the source
// fails, the card refuses a write, the size passes the bot limit, or (when this
// transfer may stop early) the budget or the yield check ends it. `got` counts
// the bytes on the card, including any resumed head.
Archive::PumpEnd pumpBytes(Source& src, Writer& w, const Archive::Pump& p, bool mayStop,
                           uint64_t& got) {
  const uint32_t start = (*p.nowMs)();
  for (;;) {
    const int n = src.read(p.buf, p.cap);
    if (n == 0) return Archive::PumpEnd::Eof;
    if (n < 0) return Archive::PumpEnd::SrcError;
    if (got + (uint64_t)n > kMaxFileBytes) return Archive::PumpEnd::TooLarge;
    if (!w.write(p.buf, (size_t)n)) return Archive::PumpEnd::WriteError;
    got += (uint64_t)n;
    if (!mayStop) continue;
    if ((*p.nowMs)() - start >= p.budgetMs || (*p.yield && (*p.yield)()))
      return Archive::PumpEnd::Budget;
  }
}

}  // namespace

Step Archive::downloadStep(Source& src, uint8_t* buf, size_t cap, uint32_t budgetMs,
                           const std::function<uint32_t()>& nowMs,
                           const std::function<bool()>& yield) {
  if (jobs_.empty() && overflowed_) {
    overflowed_ = false;
    resumePending();
  }
  if (jobs_.empty() || !buf || !cap) return Step::Idle;
  const Pump p{buf, cap, budgetMs, &nowMs, &yield};
  return runFront(src, p);
}

Step Archive::runFront(Source& src, const Pump& p) {
  Job& j = jobs_.front();
  const std::string file = itemPath(j.chat, j.day, jobName(j));
  const std::string part = file + ".part";
  if (fs_.exists(file)) { jobs_.pop_front(); return Step::Done; }
  const int64_t have = fs_.size(part);
  const uint64_t resumeAt = (have > 0 && !j.noResume) ? (uint64_t)have : 0;
  const OpenResult o = src.open(j.fileId, resumeAt);
  if (!o.ok) return retryOrFail(o.err, o.permanent);
  const bool append = resumeAt > 0 && o.resumed;
  if (resumeAt > 0 && !o.resumed) j.noResume = true;   // fetch whole from here on
  const int64_t expect = o.total >= 0 ? o.total + (int64_t)(append ? resumeAt : 0) : -1;
  uint64_t got = append ? resumeAt : 0;
  bool closed = true;
  const PumpEnd end = expect > (int64_t)kMaxFileBytes
                          ? PumpEnd::TooLarge
                          : transfer(src, p, j, append, got, closed);
  src.close();
  return endJob(end, closed, got, expect);
}

// One transfer into the job's .part file. A file that cannot resume is fetched
// whole; otherwise the transfer stops at the budget and resumes on a later step.
Archive::PumpEnd Archive::transfer(Source& src, const Pump& p, const Job& j, bool append,
                                   uint64_t& got, bool& closed) {
  fs_.mkdirs(dayDirPath(j.chat, j.day));
  std::unique_ptr<Writer> w = fs_.openWrite(itemPath(j.chat, j.day, jobName(j)) + ".part", append);
  if (!w) return PumpEnd::WriteError;
  const PumpEnd end = pumpBytes(src, *w, p, !j.noResume, got);
  closed = w->close();
  return end;
}

Step Archive::endJob(PumpEnd end, bool closed, uint64_t got, int64_t expect) {
  const Job done = jobs_.front();
  const std::string file = itemPath(done.chat, done.day, jobName(done));
  const std::string part = file + ".part";
  if (end == PumpEnd::Budget && closed) return Step::Partial;
  if (end == PumpEnd::TooLarge) {
    fs_.remove(part);
    jobs_.pop_front();
    finishJob(done, "not archived: too large", stats_.notArchived, 0, "");
    return Step::Failed;
  }
  if (end == PumpEnd::WriteError || !closed) {
    // The card refused a write: keep the .part and say so; retrying a full or failing
    // card only repeats the failure.
    jobs_.pop_front();
    finishJob(done, "incomplete", stats_.mediaIncomplete, got, "card write failed");
    return Step::Incomplete;
  }
  if (end == PumpEnd::SrcError || (expect >= 0 && got != (uint64_t)expect))
    return retryOrFail("download cut short", false);
  jobs_.pop_front();
  if (!fs_.rename(part, file)) {
    finishJob(done, "incomplete", stats_.mediaIncomplete, got, "card rename failed");
    return Step::Incomplete;
  }
  finishJob(done, "ok", stats_.mediaOk, got, "");
  return Step::Done;
}

// Items of one day still waiting for their media: a "pending" row with no later
// status event, and no finished file on the card.
std::vector<Job> Archive::pendingIn(const std::string& chat, const std::string& day) {
  std::map<std::string, Job> pend;
  forEachLine(fs_, indexPathOf(chat, day), [&](const std::string& line) {
    JsonDocument d = newDoc();
    if (deserializeJson(d, line)) return;
    if (d["ev"].is<const char*>()) {
      pend.erase(d["ref"] | "");
      return;
    }
    const std::string st = d["st"] | "";
    if (st != "pending" || !d["file"].is<const char*>() || !d["fid"].is<const char*>()) return;
    Job j;
    j.chat = chat;
    j.day = day;
    const std::string file = d["file"].as<const char*>();
    j.base = baseOf(file);
    if (j.base.size() + 1 >= file.size()) return;   // a hand-edited row: no extension
    j.ext = file.substr(j.base.size() + 1);
    j.fileId = d["fid"].as<const char*>();
    pend[j.base] = j;
  });
  std::vector<Job> out;
  for (const auto& kv : pend)
    if (!fs_.exists(itemPath(chat, day, jobName(kv.second)))) out.push_back(kv.second);
  return out;
}

int Archive::resumePending(int days) {
  int added = 0;
  for (const std::string& chat : listChats(fs_)) {
    std::vector<std::string> ds = listDays(fs_, chat);
    if ((int)ds.size() > days) ds.resize((size_t)days);
    for (const std::string& day : ds)
      for (const Job& j : pendingIn(chat, day)) added += enqueue(j) ? 1 : 0;
  }
  return added;
}

// ---- storage pressure ------------------------------------------------------------

std::vector<DayUsage> Archive::usage() {
  std::vector<DayUsage> days;
  for (const std::string& chat : listChats(fs_)) {
    const bool stranger = isStrangerChat(chat);
    for (const std::string& day : listDays(fs_, chat)) {
      DayUsage u;
      u.chat = chat;
      u.day = day;
      u.stranger = stranger;
      for (const Entry& e : fs_.list(dayDirPath(chat, day))) u.bytes += onCard(e.size);
      days.push_back(u);
    }
  }
  return days;
}

// Remove the planned day directories (and their queued downloads); report the
// oldest day left.
void Archive::applyPlan(const std::vector<DayUsage>& days, const PressurePlan& plan,
                        PressureOutcome& out) {
  std::vector<bool> removed(days.size(), false);
  for (size_t i : plan.remove) {
    // A day with a transfer in flight (a paused download, a restore upload) waits for
    // the next pass rather than losing a file that is being written.
    const std::vector<Entry> es = fs_.list(dayDirPath(days[i].chat, days[i].day));
    const bool busy = std::any_of(es.begin(), es.end(), [](const Entry& e) {
      return e.name.size() > 5 && e.name.compare(e.name.size() - 5, 5, ".part") == 0;
    });
    if (busy || !fs_.removeTree(dayDirPath(days[i].chat, days[i].day))) continue;
    removed[i] = true;
    ++out.removedDays;
    out.freed += days[i].bytes;
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& j) {
                  return j.chat == days[i].chat && j.day == days[i].day;
                }), jobs_.end());
  }
  for (size_t i = 0; i < days.size(); ++i) {
    if (removed[i] || days[i].day == "undated") continue;
    if (out.keptFrom.empty() || days[i].day < out.keptFrom) out.keptFrom = days[i].day;
  }
  // A chat with no day left loses its directory too (and stops counting against the
  // stranger directory cap, which is re-read on next need).
  for (size_t i = 0; i < days.size(); ++i)
    if (removed[i] && fs_.list(chatDirPath(days[i].chat)).empty()) fs_.removeTree(chatDirPath(days[i].chat));
  if (out.removedDays) {
    strangerDirs_.clear();
    strangerDirsLoaded_ = false;
  }
}

PressureOutcome Archive::pressurePass(uint64_t freeBytes, uint64_t totalBytes) {
  PressureOutcome out;
  const std::string marker = std::string(kRoot) + "/.pressure";
  if (freeBytes >= lowWater(totalBytes)) {
    if (freeBytes >= targetFree(totalBytes) && fs_.exists(marker)) fs_.remove(marker);
    return out;
  }
  const std::vector<DayUsage> days = usage();
  const PressurePlan plan = planPressure(days, freeBytes, totalBytes);
  out.triggered = plan.triggered;
  applyPlan(days, plan, out);
  stats_.pressureDays += out.removedDays;
  if (out.removedDays && !fs_.exists(marker)) {
    writeSmallAtomic(marker, "1");
    out.tellOwner = true;
  }
  return out;
}

// ---- reading -------------------------------------------------------------------

std::vector<std::string> listChats(Fs& fs) {
  std::vector<std::string> out;
  for (const Entry& e : fs.list(kRoot))
    if (e.dir && validChatDir(e.name)) out.push_back(e.name);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> listDays(Fs& fs, const std::string& chat) {
  std::vector<std::string> out;
  if (!validChatDir(chat)) return out;
  for (const Entry& e : fs.list(chatDirPath(chat)))
    if (e.dir && validDay(e.name)) out.push_back(e.name);
  // Newest first, "undated" last (as a string it would sort above every date).
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    if ((a == "undated") != (b == "undated")) return b == "undated";
    return a > b;
  });
  return out;
}

RowPage readRows(Fs& fs, const std::string& chat, const std::string& day, size_t from,
                 size_t limit) {
  RowPage p;
  if (!validChatDir(chat) || !validDay(day)) return p;
  size_t i = 0;
  forEachLine(fs, indexPathOf(chat, day), [&](const std::string& line) {
    if (i >= from && p.rows.size() < limit) p.rows.push_back(line);
    else if (i >= from) p.more = true;
    ++i;
  });
  p.next = from + p.rows.size();
  return p;
}

bool isMediaName(const std::string& name) {
  if (!validItemName(name) || name == "index.jsonl") return false;
  const size_t dot = name.find_last_of('.');
  const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
  return ext != "json" && ext != "part" && ext != "tmp";
}

std::vector<MediaRef> newestMedia(Fs& fs, const std::string& chat, size_t limit) {
  std::vector<std::pair<std::string, std::string>> dayDirs;   // (day, chat)
  for (const std::string& c : listChats(fs)) {
    if (!chat.empty() && c != chat) continue;
    for (const std::string& d : listDays(fs, c)) dayDirs.emplace_back(d, c);
  }
  std::sort(dayDirs.begin(), dayDirs.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second < b.second;
  });
  std::vector<MediaRef> out;
  for (const auto& dc : dayDirs) {
    std::vector<Entry> es = fs.list(dayDirPath(dc.second, dc.first));
    std::sort(es.begin(), es.end(), [](const Entry& a, const Entry& b) { return a.name > b.name; });
    for (const Entry& e : es) {
      if (e.dir || !isMediaName(e.name)) continue;
      if (out.size() >= limit) return out;
      out.push_back({dc.second, dc.first, e.name, e.size});
    }
  }
  return out;
}

}  // namespace tgarc
}  // namespace nimbus
