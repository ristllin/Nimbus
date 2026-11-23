// The frozen system part of a conversation (plain/prefix.h, lane E2-CACHE).
#include "nimbus/harness/plain/prefix.h"

#include <algorithm>
#include <iterator>
#include <cstdlib>
#include <new>
#include <vector>

namespace agent {
namespace plain {

namespace {

namespace tx = nimbus::orch::tx;

constexpr char kMagic[] = "nbpx/1";   // the record format, version 1 (then a line end)
constexpr char kName[] = "prefix.txt";
constexpr char kTmpName[] = "prefix.txt.tmp";

uint32_t fnv1a(const char* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ (unsigned char)p[i]) * 16777619u;
  return h;
}

// "<key> <len>\n<bytes>\n"
void putField(std::string& out, const char* key, const std::string& v) {
  out += key;
  out += ' ';
  out += std::to_string(v.size());
  out += '\n';
  out += v;
  out += '\n';
}
void putNum(std::string& out, const char* key, uint64_t v) { putField(out, key, std::to_string(v)); }

bool getField(const std::string& in, size_t& at, const char* key, std::string& v) {
  const std::string head = std::string(key) + ' ';
  if (in.compare(at, head.size(), head) != 0) return false;
  size_t p = at + head.size();
  const size_t nl = in.find('\n', p);
  if (nl == std::string::npos || nl == p || nl - p > 8) return false;
  for (size_t i = p; i < nl; ++i)
    if (in[i] < '0' || in[i] > '9') return false;
  const size_t len = (size_t)std::strtoull(in.substr(p, nl - p).c_str(), nullptr, 10);
  p = nl + 1;
  if (len > in.size() || p + len + 1 > in.size() || in[p + len] != '\n') return false;
  v.assign(in, p, len);
  at = p + len + 1;
  return true;
}
bool getNum(const std::string& in, size_t& at, const char* key, uint64_t& v) {
  std::string s;
  if (!getField(in, at, key, s) || s.empty() || s.size() > 20) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  v = std::strtoull(s.c_str(), nullptr, 10);
  return true;
}

bool expired(const PrefixNow& n) {
  return n.now && n.lastReqAt && n.now > n.lastReqAt && n.now - n.lastReqAt > (uint64_t)n.lifetimeS + kCacheExpiryMarginS;
}

}  // namespace

const char* prefixRenderSlug(PrefixRender r) {
  switch (r) {
    case PrefixRender::None: return "";
    case PrefixRender::First: return "first";
    case PrefixRender::Connector: return "connector";
    case PrefixRender::Compaction: return "compaction";
    case PrefixRender::Notes: return "notes";
    case PrefixRender::Files: return "recent-files";
    case PrefixRender::Expired: return "expired";
  }
  return "";
}

PrefixRender decidePrefix(const FrozenPrefix& f, const PrefixNow& now) {
  if (!f.valid) return PrefixRender::First;
  if (f.blocks.core != now.blocks.core || f.manifestHash != now.manifestHash) return PrefixRender::Connector;
  if (f.digestText != now.digestText || f.stub != now.stub || f.dropped != now.dropped) return PrefixRender::Compaction;
  if (f.ownFrom && now.omitted >= f.ownFrom) return PrefixRender::Compaction;   // the turn relied on is left out
  if (f.seenNotes != now.blocks.notes) return PrefixRender::Notes;
  if (f.seenFiles != now.files) return PrefixRender::Files;
  if (expired(now) && (f.blocks.notes != now.blocks.notes || f.blocks.digests != now.blocks.digests))
    return PrefixRender::Expired;   // the provider holds nothing anyway: the blocks catch up for free
  return PrefixRender::None;
}

void renderPrefix(FrozenPrefix& f, const PrefixNow& now) {
  f.valid = true;
  f.blocks = now.blocks;
  f.manifestHash = now.manifestHash;
  f.digestText = now.digestText;
  f.seenNotes = now.blocks.notes;
  f.seenFiles = now.files;
  f.stub = now.stub;
  f.dropped = now.dropped;
  f.ownFrom = 0;
}

namespace {

// The lines of a block that hold something, in their order.
std::vector<std::string> linesInOrder(const std::string& block) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= block.size()) {
    size_t end = block.find('\n', at);
    if (end == std::string::npos) end = block.size();
    if (end > at) out.push_back(block.substr(at, end - at));
    at = end + 1;
  }
  return out;
}
// The same, sorted (compared as multisets).
std::vector<std::string> linesOf(const std::string& block) {
  std::vector<std::string> out = linesInOrder(block);
  std::sort(out.begin(), out.end());
  return out;
}
// `small` minus `big` as multisets (both sorted): what `small` has that `big` lacks.
std::vector<std::string> minus(const std::vector<std::string>& small, const std::vector<std::string>& big) {
  std::vector<std::string> out;
  std::set_difference(small.begin(), small.end(), big.begin(), big.end(), std::back_inserter(out));
  return out;
}

}  // namespace

namespace {
// `text` holds `line` as a whole: at its start or after a space, and at its end or before a space
// (the note tool's result: "saved, pinned (...): <line> (12 of 4096 B)").
bool holdsLine(const std::string& text, const std::string& line) {
  for (size_t at = text.find(line); at != std::string::npos; at = text.find(line, at + 1)) {
    const size_t end = at + line.size();
    if ((at == 0 || text[at - 1] == ' ') && (end == text.size() || text[end] == ' ')) return true;
  }
  return false;
}
}  // namespace

bool ownNotesAddition(const std::string& frozen, const std::string& seen, const std::string& now,
                      const std::vector<std::string>& ownTexts, const std::vector<std::string>& headers) {
  const std::vector<std::string> n = linesOf(now), s = linesOf(seen);
  if (!minus(linesOf(frozen), n).empty() || !minus(s, n).empty()) return false;   // a line removed or edited
  for (const std::string& line : minus(n, s)) {
    if (std::find(headers.begin(), headers.end(), line) != headers.end()) continue;
    const bool own = std::any_of(ownTexts.begin(), ownTexts.end(), [&](const std::string& t) { return holdsLine(t, line); });
    if (!own) return false;
  }
  return true;
}

namespace {
bool named(const std::vector<std::string>& paths, const std::string& path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}
// A list line is "<path>" or "<path> (<when>)", the time holding no parenthesis
// (tx::recentFilesList). Is `line` an entry of `path`?
bool lineOf(const std::string& line, const std::string& path) {
  if (line.compare(0, path.size(), path) != 0) return false;
  if (line.size() == path.size()) return true;
  const std::string tail = line.substr(path.size());
  return tail.size() > 3 && tail.compare(0, 2, " (") == 0 && tail.back() == ')' &&
         tail.find_first_of("()", 2) == tail.size() - 1;
}
}  // namespace

namespace {
const ListedFile* listed(const std::vector<ListedFile>& listing, const std::string& line) {
  const auto e = std::find_if(listing.begin(), listing.end(), [&](const ListedFile& f) { return f.line == line; });
  return e == listing.end() ? nullptr : &*e;
}
// The entries of `seen` and of `now` whose path the turn did not name, each in its order. A seen line
// is an own entry when a named path is its path: the line may be gone (a delete) or changed (a write
// stamps its time), so its path is read from the line itself. False: a line of `now` the store cannot
// account for.
bool otherEntries(const std::string& seen, const std::string& now, const std::vector<ListedFile>& listing,
                  const std::vector<std::string>& ownPaths, std::vector<std::string>& before,
                  std::vector<std::string>& after) {
  for (const std::string& line : linesInOrder(seen)) {
    const bool own = std::any_of(ownPaths.begin(), ownPaths.end(), [&](const std::string& p) { return lineOf(line, p); });
    if (!own) before.push_back(line);
  }
  for (const std::string& line : linesInOrder(now)) {
    const ListedFile* e = listed(listing, line);
    if (!e) return false;
    if (!named(ownPaths, e->path)) after.push_back(line);
  }
  return true;
}
}  // namespace

bool ownFilesChange(const std::string& seen, const std::string& now, const std::vector<ListedFile>& listing,
                    const std::vector<std::string>& ownPaths, uint64_t openedAt) {
  std::vector<std::string> before, after;
  if (!otherEntries(seen, now, listing, ownPaths, before, after)) return false;
  size_t k = 0;
  while (k < before.size() && k < after.size() && before[k] == after[k]) ++k;
  if (k < before.size() && k < after.size()) return false;   // another entry changed or moved
  for (size_t i = k; i < before.size(); ++i)   // left the bottom: the file is there as it was
    if (!listed(listing, before[i])) return false;
  for (size_t i = k; i < after.size(); ++i) {   // came into the bottom: written before the turn opened
    const uint64_t at = listed(listing, after[i])->lastWrite;
    if (!at || !openedAt || at >= openedAt) return false;
  }
  return true;
}

Status encodePrefix(const FrozenPrefix& f, std::string& out) {
  out.clear();
  if (!f.valid) return Status::fail(Err::InvalidArgument, "plain::encodePrefix");
  try {
    out = kMagic;
    out += '\n';
    putField(out, "core", f.blocks.core);
    putField(out, "notes", f.blocks.notes);
    putField(out, "digests", f.blocks.digests);
    putNum(out, "manifest", f.manifestHash);
    putField(out, "digest-text", f.digestText);
    putField(out, "seen-notes", f.seenNotes);
    putField(out, "seen-files", f.seenFiles);
    putNum(out, "stub", f.stub);
    putNum(out, "dropped", f.dropped);
    putNum(out, "own-from", f.ownFrom);
    putNum(out, "z", fnv1a(out.data(), out.size()));
  } catch (const std::bad_alloc&) {
    out.clear();
    return Status::fail(Err::TooLarge, "plain::encodePrefix");
  }
  if (out.size() > kPrefixRecordMax) {
    out.clear();
    return Status::fail(Err::TooLarge, "plain::encodePrefix");
  }
  return Status::success();
}

namespace {
bool headerOk(const std::string& in) {
  const size_t magic = sizeof kMagic;   // the format and its line end
  return in.size() >= magic && in.compare(0, magic - 1, kMagic) == 0 && in[magic - 1] == '\n';
}
bool fieldsOf(const std::string& in, size_t& at, FrozenPrefix& f, uint64_t& manifest) {
  return getField(in, at, "core", f.blocks.core) && getField(in, at, "notes", f.blocks.notes) &&
         getField(in, at, "digests", f.blocks.digests) && getNum(in, at, "manifest", manifest) &&
         getField(in, at, "digest-text", f.digestText) && getField(in, at, "seen-notes", f.seenNotes) &&
         getField(in, at, "seen-files", f.seenFiles) && getNum(in, at, "stub", f.stub) &&
         getNum(in, at, "dropped", f.dropped);
}
}  // namespace

Status decodePrefix(const std::string& in, FrozenPrefix& out) {
  out = FrozenPrefix{};
  if (!headerOk(in)) return Status::fail(Err::Torn, "plain::decodePrefix");
  FrozenPrefix f;
  size_t at = sizeof kMagic;
  uint64_t manifest = 0, z = 0;
  bool shaped = fieldsOf(in, at, f, manifest);
  if (shaped && in.compare(at, 8, "own-from") == 0) shaped = getNum(in, at, "own-from", f.ownFrom);   // E2-FIX6
  const size_t body = at;
  if (!shaped || !getNum(in, at, "z", z) || at != in.size() || manifest > UINT32_MAX || z != fnv1a(in.data(), body))
    return Status::fail(Err::Torn, "plain::decodePrefix");
  f.manifestHash = (uint32_t)manifest;
  f.valid = true;
  out = std::move(f);
  return Status::success();
}

namespace {
// One whole file read and decoded: Io when it could not be read, Torn when it does not decode.
Status readRecord(tx::TxFiles& files, const std::string& path, FrozenPrefix& out) {
  size_t n = 0;
  if (!files.size(path, n)) return Status::fail(Err::Io, "plain::loadPrefix.size");
  if (n > kPrefixRecordMax) return Status::fail(Err::Torn, "plain::loadPrefix");
  std::string text(n, '\0');
  size_t got = 0;
  if (n && (!files.readAt(path, 0, &text[0], n, got) || got != n)) return Status::fail(Err::Io, "plain::loadPrefix.read");
  return decodePrefix(text, out);
}
}  // namespace

Status loadPrefix(tx::TxFiles& files, const std::string& chatKey, FrozenPrefix& out) {
  out = FrozenPrefix{};
  std::string dir;
  if (!tx::txDir(chatKey, dir).ok()) return Status::fail(Err::InvalidArgument, "plain::loadPrefix");
  try {
    std::vector<std::string> names;
    if (!files.list(dir, names)) return Status::fail(Err::Io, "plain::loadPrefix.list");
    const bool main = std::find(names.begin(), names.end(), kName) != names.end();
    const bool tmp = std::find(names.begin(), names.end(), kTmpName) != names.end();
    if (main) return readRecord(files, dir + "/" + kName, out);
    if (!tmp) return Status::fail(Err::NotFound, "plain::loadPrefix");
    // The replace was cut after the old record left: the temporary copy is the newest whole one.
    const Status st = readRecord(files, dir + "/" + kTmpName, out);
    return st.err == Err::Torn ? Status::fail(Err::NotFound, "plain::loadPrefix.tmp") : st;
  } catch (const std::bad_alloc&) {
    out = FrozenPrefix{};
    return Status::fail(Err::Io, "plain::loadPrefix.memory");   // never "absent": the record stays for a later read
  }
}

Status savePrefix(tx::TxFiles& files, const std::string& chatKey, const FrozenPrefix& f) {
  std::string dir, text;
  if (!tx::txDir(chatKey, dir).ok()) return Status::fail(Err::InvalidArgument, "plain::savePrefix");
  Status st = encodePrefix(f, text);
  if (!st.ok()) return st;
  const std::string tmp = dir + "/" + kTmpName;
  if (!files.writeWhole(tmp, text) || !files.rename(tmp, dir + "/" + kName)) return Status::fail(Err::Io, "plain::savePrefix");
  return Status::success();
}

}  // namespace plain
}  // namespace agent
