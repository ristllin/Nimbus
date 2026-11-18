// orch_effect_ring.cpp - each chat's stored ring of recent effects (lane RB-FX). The
// contract and the on-disk form are in nimbus/orch/effect_ring.h.
#include "nimbus/orch/effect_ring.h"

#include <cstdint>
#include <cstring>
#include <new>   // std::bad_alloc

#include "nimbus/orch/psram_alloc.h"

namespace nimbus {
namespace orch {
namespace fx {

namespace {

constexpr const char kTag[] = "f1\t";
constexpr size_t kTagLen = 3;

uint32_t crc32Of(const char* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= (uint8_t)p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// The sequence length a lead byte starts (2 to 4) and its payload bits; 0 when no valid
// sequence starts with it (a continuation, C0, C1, F5 and up).
size_t leadLen(uint8_t b, uint32_t& cp) {
  if (b >= 0xC2 && b <= 0xDF) {
    cp = b & 0x1F;
    return 2;
  }
  if (b >= 0xE0 && b <= 0xEF) {
    cp = b & 0x0F;
    return 3;
  }
  if (b >= 0xF0 && b <= 0xF4) {
    cp = b & 0x07;
    return 4;
  }
  return 0;
}

// A decoded scalar is valid: not overlong, not a surrogate, not past U+10FFFF.
bool scalarOk(size_t len, uint32_t cp) {
  const bool overlong = (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
  return !overlong && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// Length of the valid UTF-8 sequence at p (1 to 4), or 0 when the bytes there are not one.
size_t utf8Len(const uint8_t* p, size_t n, uint32_t& cp) {
  if (p[0] < 0x80) {
    cp = p[0];
    return 1;
  }
  const size_t len = leadLen(p[0], cp);
  if (len == 0 || len > n) return 0;
  for (size_t k = 1; k < len; ++k) {
    if ((p[k] & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (p[k] & 0x3F);
  }
  return scalarOk(len, cp) ? len : 0;
}

bool isControl(uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029; }

void copyField(char* dst, size_t dstSize, const std::string& s) {
  const size_t n = s.size() < dstSize - 1 ? s.size() : dstSize - 1;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

// Eight lower-case hex digits.
bool readHex8(const char* p, uint32_t& out) {
  out = 0;
  for (int i = 0; i < 8; ++i) {
    const char c = p[i];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (v < 0) return false;
    out = (out << 4) | (uint32_t)v;
  }
  return true;
}

// 1 to 19 decimal digits that fit an int64.
bool readEpoch(const char* p, size_t n, int64_t& out) {
  if (n == 0 || n > 19) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < '0' || p[i] > '9') return false;
    v = v * 10 + (uint64_t)(p[i] - '0');
  }
  if (v > (uint64_t)INT64_MAX) return false;
  out = (int64_t)v;
  return true;
}

// A field read from a line is taken only as cleanText would have written it.
bool cleanField(const char* p, size_t n, size_t cap, char* dst, size_t dstSize) {
  if (n > cap) return false;
  const std::string c = cleanText(p, n, cap);
  if (c.size() != n || std::memcmp(c.data(), p, n) != 0) return false;
  copyField(dst, dstSize, c);
  return true;
}

// Read a ring file in kReadSlice pieces into `out` (the newest kRingEntries good lines,
// oldest first). False when storage refused a read: nothing is to be trusted then.
bool readFile(tx::TxFiles& files, const std::string& path, WorkingVector<RingEntry>& out, uint32_t& lines,
              uint32_t& dropped, bool& torn) {
  size_t size = 0;
  if (!files.size(path, size)) return false;
  out.reserve(kRingEntries);   // one block, never grown by doubling (ruling 30)
  std::string line, buf(kReadSlice, '\0');
  line.reserve(kLineMax);
  bool overlong = false;
  for (size_t off = 0; off < size;) {
    size_t got = 0;
    if (!files.readAt(path, off, &buf[0], kReadSlice, got) || got == 0) return false;
    for (size_t i = 0; i < got; ++i) {
      if (buf[i] != '\n') {
        if (line.size() < kLineMax) line += buf[i];
        else overlong = true;
        continue;
      }
      ++lines;
      RingEntry e;
      if (!overlong && decodeLine(line.data(), line.size(), e)) {
        if (out.size() == kRingEntries) out.erase(out.begin());
        out.push_back(e);
      } else {
        ++dropped;
      }
      line.clear();
      overlong = false;
    }
    off += got;
  }
  torn = !line.empty() || overlong;
  dropped += torn ? 1 : 0;
  return true;
}

}  // namespace

std::string cleanText(const char* p, size_t n, size_t cap) {
  std::string out;
  const uint8_t* u = reinterpret_cast<const uint8_t*>(p);
  size_t i = 0;
  while (i < n) {
    uint32_t cp = 0;
    const size_t len = utf8Len(u + i, n - i, cp);
    const size_t take = len == 0 || isControl(cp) ? 1 : len;
    if (out.size() + take > cap) break;
    if (len == 0)
      out += '?';
    else if (isControl(cp))
      out += ' ';
    else
      out.append(p + i, len);
    i += len == 0 ? 1 : len;
  }
  return out;
}

bool ringKey(const std::string& chatId, std::string& key) {
  key.clear();
  if (chatId.empty()) return false;
  bool tg = chatId.size() <= kKeyMax;
  for (char c : chatId) tg = tg && ((c >= '0' && c <= '9') || c == '-');
  if (tg) {
    key = chatId;
    return true;
  }
  if (chatId.size() > kOtherIdMax) return false;
  static const char kHex[] = "0123456789abcdef";
  key = "k";
  for (char c : chatId) {
    key += kHex[(uint8_t)c >> 4];
    key += kHex[(uint8_t)c & 15];
  }
  return true;
}

bool ringPath(const std::string& chatId, std::string& path) {
  std::string key;
  path.clear();
  if (!ringKey(chatId, key)) return false;
  path = std::string(kFxRoot) + "/" + key + ".ring";
  return true;
}

std::string encodeLine(const RingEntry& e) {
  static const char kHex[] = "0123456789abcdef";
  std::string line(kTag);
  line += std::to_string((long long)(e.epoch > 0 ? e.epoch : 0));
  line += '\t';
  line += e.tool;
  line += '\t';
  line += e.receipt;
  const uint32_t crc = crc32Of(line.data(), line.size());
  line += '\t';
  for (int shift = 28; shift >= 0; shift -= 4) line += kHex[(crc >> shift) & 15];
  line += '\n';
  return line;
}

bool decodeLine(const char* p, size_t n, RingEntry& out) {
  if (n < kTagLen + 9 || n > kLineMax - 1 || std::memcmp(p, kTag, kTagLen) != 0) return false;
  const size_t body = n - 9;   // the last tab
  uint32_t crc = 0;
  if (p[body] != '\t' || !readHex8(p + body + 1, crc) || crc != crc32Of(p, body)) return false;
  // epoch \t tool \t receipt, between the tag and the last tab.
  const char* q = p + kTagLen;
  const char* end = p + body;
  const char* t1 = static_cast<const char*>(std::memchr(q, '\t', (size_t)(end - q)));
  if (!t1) return false;
  const char* t2 = static_cast<const char*>(std::memchr(t1 + 1, '\t', (size_t)(end - t1 - 1)));
  RingEntry e;
  if (!t2 || !readEpoch(q, (size_t)(t1 - q), e.epoch)) return false;
  if (!cleanField(t1 + 1, (size_t)(t2 - t1 - 1), kToolMax, e.tool, sizeof e.tool)) return false;
  if (!cleanField(t2 + 1, (size_t)(end - t2 - 1), kReceiptMax, e.receipt, sizeof e.receipt)) return false;
  e.stored = true;
  out = e;
  return true;
}

bool recordsAsEffect(FxKind k) {
  return k != FxKind::Read && k != FxKind::SubSourced && k != FxKind::SubUnsourced && k != FxKind::MemPinNoop &&
         k != FxKind::kCount;
}

std::string effectReceipt(const Effect& e) {
  TurnEffects one;
  one.add(e.kind, e.ok, e.ref, e.count, e.flags);
  return one.summary();
}

// ---- the ring ---------------------------------------------------------------------------------

struct EffectRing::Slot {
  char key[kKeyMax + 1];
  bool used;
  bool loaded;      // the file was read (or confirmed absent): RAM holds what it holds
  bool unsettled;   // a rewrite's rename was refused: the temp may be the only whole copy
  bool writeFailed; // the last write of this chat was refused (backing() says RamOnly)
  bool tornTail;    // the file may end in a part line: the next append starts with "~\n"
  uint32_t lines;   // lines in the file (dropped ones included), for the rewrite bound
  uint32_t lastUse;
  RingEntry* ring;  // kRingEntries, in the working allocator
  size_t head;      // index of the oldest entry
  size_t count;

  RingEntry& at(size_t i) { return ring[(head + i) % kRingEntries]; }
  void push(const RingEntry& e) {
    if (count < kRingEntries) {
      ring[(head + count++) % kRingEntries] = e;
    } else {
      ring[head] = e;
      head = (head + 1) % kRingEntries;
    }
  }
};

EffectRing::EffectRing(tx::TxFiles* files) : files_(files) {}

EffectRing::~EffectRing() {
  if (!slots_) return;
  for (size_t i = 0; i < kMaxChats; ++i) workingFree(slots_[i].ring);
  workingFree(slots_);
}

EffectRing::Slot* EffectRing::findSlot(const std::string& key) {
  for (size_t i = 0; i < kMaxChats; ++i)
    if (slots_[i].used && key == slots_[i].key) return &slots_[i];
  return nullptr;
}

// A free slot, or the least recently used one, cleared for `key` (its ring block is reused).
EffectRing::Slot* EffectRing::claimSlot(const std::string& key) {
  Slot* pick = nullptr;
  for (size_t i = 0; i < kMaxChats; ++i)   // a free slot has lastUse 0, older than any use
    if (!pick || slots_[i].lastUse < pick->lastUse) pick = &slots_[i];
  if (!pick->ring) pick->ring = static_cast<RingEntry*>(workingAlloc(kRingEntries * sizeof(RingEntry)));
  if (!pick->ring) {   // refused: no slot changed
    ++stats_.noMemory;
    return nullptr;
  }
  if (pick->used) ++stats_.evictions;
  RingEntry* ring = pick->ring;
  std::memset(pick, 0, sizeof(Slot));
  pick->ring = ring;
  pick->used = true;
  copyField(pick->key, sizeof pick->key, key);
  return pick;
}

EffectRing::Slot* EffectRing::slotFor(const std::string& key) {
  if (!slots_) {
    void* mem = workingAlloc(kMaxChats * sizeof(Slot));
    if (!mem) {
      ++stats_.noMemory;
      return nullptr;
    }
    slots_ = static_cast<Slot*>(mem);
    std::memset(mem, 0, kMaxChats * sizeof(Slot));
  }
  Slot* pick = findSlot(key);
  if (!pick) pick = claimSlot(key);
  if (!pick) return nullptr;
  pick->lastUse = ++tick_;
  if (files_ && (!pick->loaded || pick->unsettled)) loadInto(*pick, std::string(kFxRoot) + "/" + key + ".ring");
  return pick;
}

// Settle the chat's files (a temp left by a cut or refused rewrite), then, the first time,
// read the file into RAM. A loaded slot keeps RAM as the truth: settling never re-reads it.
void EffectRing::loadInto(Slot& s, const std::string& path) {
  // A refused allocation leaves the slot as it was (RAM untouched until the read is whole and
  // the merge needs no memory): the chat stays not loaded, RamOnly, and is loaded at its next use.
  try {
    loadFiles(s, path);
  } catch (const std::bad_alloc&) {
    ++stats_.noMemory;
  }
}

void EffectRing::loadFiles(Slot& s, const std::string& path) {
  std::vector<std::string> names;
  if (!files_->list(kFxRoot, names)) return;   // no card: stay in RAM
  const std::string base = path.substr(std::strlen(kFxRoot) + 1);
  const std::string tmp = path + ".tmp";
  bool hasMain = false, hasTmp = false;
  for (const std::string& n : names) {
    hasMain = hasMain || n == base;
    hasTmp = hasTmp || n == base + ".tmp";
  }
  if (hasTmp && hasMain) {
    files_->remove(tmp);   // a rewrite cut or refused before its rename: the file is whole
  } else if (hasTmp) {     // a rename that removed its target first: the temp is the only copy
    if (!files_->rename(tmp, path)) return;   // keep it; no write may make a second file
    hasMain = true;
  }
  s.unsettled = false;
  if (s.loaded) return;
  ++stats_.loads;
  // The file's entries, newest kRingEntries kept; RAM is touched only once the read is whole.
  WorkingVector<RingEntry> got;
  uint32_t lines = 0, dropped = 0;
  bool torn = false;
  if (hasMain && !readFile(*files_, path, got, lines, dropped, torn)) return;   // unreadable now: RAM as it was
  // Entries already in RAM never reached storage (no write runs before the load), so they are
  // newer than every line of the file: they stay, after it.
  WorkingVector<RingEntry> extra;
  extra.reserve(s.count);
  for (size_t i = 0; i < s.count; ++i) extra.push_back(s.at(i));
  s.head = s.count = 0;
  for (const RingEntry& e : got) s.push(e);
  for (const RingEntry& e : extra) s.push(e);
  s.lines = lines;
  s.tornTail = torn;
  s.loaded = true;
  stats_.loaded += (uint32_t)got.size();
  stats_.dropped += dropped;
}

bool EffectRing::rewrite(Slot& s, const std::string& path) {
  // A refused allocation skips the rewrite (nothing on storage changed); the next append
  // tries again.
  bool renaming = false;
  try {
    return rewriteFile(s, path, renaming);
  } catch (const std::bad_alloc&) {
    ++stats_.noMemory;
  }
  // Refused inside the whole write: a part temp may be left beside the whole file, and the
  // next use removes it. Refused inside the rename: the rename may have removed the file first,
  // so the temp may be the only copy; it is settled before the next write.
  if (renaming) s.unsettled = true;
  return false;
}

bool EffectRing::rewriteFile(Slot& s, const std::string& path, bool& renaming) {
  std::string all;
  all.reserve(s.count * kLineMax);
  for (size_t i = 0; i < s.count; ++i) all += encodeLine(s.at(i));
  const std::string tmp = path + ".tmp";
  if (!files_->writeWhole(tmp, all)) {
    files_->remove(tmp);   // the file is whole; a part temp would be taken for a cut rename
    return false;
  }
  renaming = true;
  if (!files_->rename(tmp, path)) {
    s.unsettled = true;   // the temp may be the only whole copy: settled before the next write
    return false;
  }
  for (size_t i = 0; i < s.count; ++i) s.at(i).stored = true;
  s.lines = (uint32_t)s.count;
  s.tornTail = false;
  ++stats_.rewrites;
  return true;
}

AppendResult EffectRing::append(const std::string& chatId, int64_t epoch, const std::string& tool,
                                const std::string& receipt) {
  std::lock_guard<std::mutex> lock(mu_);
  try {
    return appendLocked(chatId, epoch, tool, receipt);
  } catch (const std::bad_alloc&) {
    // Every allocation of an append comes before its write and its RAM entry: nothing changed.
    ++stats_.noMemory;
    return AppendResult::NoMemory;
  }
}

AppendResult EffectRing::appendLocked(const std::string& chatId, int64_t epoch, const std::string& tool,
                                      const std::string& receipt) {
  std::string key;
  if (!ringKey(chatId, key)) {
    ++stats_.refused;
    return AppendResult::Refused;
  }
  Slot* s = slotFor(key);
  if (!s) return AppendResult::NoMemory;   // the slot table or the chat's ring block was refused
  RingEntry e;
  e.epoch = epoch > 0 ? epoch : 0;
  copyField(e.tool, sizeof e.tool, cleanText(tool, kToolMax));
  copyField(e.receipt, sizeof e.receipt, cleanText(receipt, kReceiptMax));
  bool ok = false;
  const std::string path = std::string(kFxRoot) + "/" + key + ".ring";
  // Nothing is written before the file was read and settled: a line written beside an unread
  // file or a lone temp could be lost or put out of order at the next load.
  if (files_ && s->loaded && !s->unsettled) {
    const bool sep = s->tornTail;
    const std::string line = (sep ? std::string("~\n") : std::string()) + encodeLine(e);
    ok = files_->append(path, line.data(), line.size());
    s->lines += ok ? (sep ? 2 : 1) : 0;
    s->tornTail = !ok;      // a refusal may leave part of the line (or all of it) behind
    s->writeFailed = !ok;
  }
  e.stored = ok;
  s->push(e);
  if (!ok) {
    ++stats_.ramOnly;
    return AppendResult::RamOnly;
  }
  ++stats_.stored;
  if (s->loaded && s->lines >= kCompactLines) rewrite(*s, path);
  return AppendResult::Stored;
}

size_t EffectRing::visit(const std::string& chatId, size_t n, const std::function<void(const RingEntry&)>& fn) {
  std::lock_guard<std::mutex> lock(mu_);
  Slot* s = nullptr;
  try {
    std::string key;
    if (ringKey(chatId, key)) s = slotFor(key);
  } catch (const std::bad_alloc&) {   // the key or the path: nothing was read
    ++stats_.noMemory;
  }
  if (!s) return 0;
  const size_t m = n < s->count ? n : s->count;
  for (size_t i = s->count - m; i < s->count; ++i) fn(s->at(i));
  return m;
}

size_t EffectRing::recent(const std::string& chatId, size_t n, std::vector<RingEntry>& out) {
  out.clear();
  try {
    out.reserve(n < kRingEntries ? n : kRingEntries);
  } catch (const std::bad_alloc&) {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.noMemory;
    return 0;
  }
  return visit(chatId, n, [&out](const RingEntry& e) { out.push_back(e); });   // within the reserve
}

Backing EffectRing::backing(const std::string& chatId) {
  std::lock_guard<std::mutex> lock(mu_);
  Slot* s = nullptr;
  try {
    std::string key;
    if (files_ && ringKey(chatId, key)) s = slotFor(key);
  } catch (const std::bad_alloc&) {
    ++stats_.noMemory;
  }
  return s && s->loaded && !s->unsettled && !s->writeFailed ? Backing::Storage : Backing::RamOnly;
}

RingStats EffectRing::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

size_t EffectRing::residentChats() const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t n = 0;
  if (slots_)
    for (size_t i = 0; i < kMaxChats; ++i) n += slots_[i].used ? 1 : 0;
  return n;
}

}  // namespace fx
}  // namespace orch
}  // namespace nimbus
