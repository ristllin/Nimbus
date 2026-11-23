// The frozen system part of a conversation (plain/prefix.h, lane E2-CACHE).
#include "nimbus/harness/plain/prefix.h"

#include <algorithm>
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
  const bool shaped = fieldsOf(in, at, f, manifest);
  const size_t body = at;
  if (!shaped || !getNum(in, at, "z", z) || at != in.size() || manifest > UINT32_MAX || z != fnv1a(in.data(), body))
    return Status::fail(Err::Torn, "plain::decodePrefix");
  f.manifestHash = (uint32_t)manifest;
  f.valid = true;
  out = std::move(f);
  return Status::success();
}

Status loadPrefix(tx::TxFiles& files, const std::string& chatKey, FrozenPrefix& out) {
  out = FrozenPrefix{};
  std::string dir;
  if (!tx::txDir(chatKey, dir).ok()) return Status::fail(Err::InvalidArgument, "plain::loadPrefix");
  std::vector<std::string> names;
  if (!files.list(dir, names)) return Status::fail(Err::Io, "plain::loadPrefix.list");
  if (std::find(names.begin(), names.end(), kName) == names.end()) return Status::fail(Err::NotFound, "plain::loadPrefix");
  const std::string path = dir + "/" + kName;
  size_t n = 0;
  if (!files.size(path, n)) return Status::fail(Err::Io, "plain::loadPrefix.size");
  if (n > kPrefixRecordMax) return Status::fail(Err::Torn, "plain::loadPrefix");
  std::string text(n, '\0');
  size_t got = 0;
  if (n && (!files.readAt(path, 0, &text[0], n, got) || got != n)) return Status::fail(Err::Io, "plain::loadPrefix.read");
  return decodePrefix(text, out);
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
