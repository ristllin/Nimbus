// orch_effect_ring.cpp - each chat's stored ring of recent effects (lane RB-FX). The
// contract and the on-disk form are in nimbus/orch/effect_ring.h.
#include "nimbus/orch/effect_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

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

// Length of the valid UTF-8 sequence at p (1 to 4), or 0 when the bytes there are not one.
size_t utf8Len(const uint8_t* p, size_t n, uint32_t& cp) {
  const uint8_t b = p[0];
  size_t len;
  if (b < 0x80) {
    cp = b;
    return 1;
  }
  if (b >= 0xC2 && b <= 0xDF) {
    len = 2;
    cp = b & 0x1F;
  } else if (b >= 0xE0 && b <= 0xEF) {
    len = 3;
    cp = b & 0x0F;
  } else if (b >= 0xF0 && b <= 0xF4) {
    len = 4;
    cp = b & 0x07;
  } else {
    return 0;
  }
  if (len > n) return 0;
  for (size_t k = 1; k < len; ++k) {
    if ((p[k] & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (p[k] & 0x3F);
  }
  const bool overlong = (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
  if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return len;
}

bool isControl(uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029; }

void copyField(char* dst, size_t dstSize, const std::string& s) {
  const size_t n = s.size() < dstSize - 1 ? s.size() : dstSize - 1;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
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
  char head[32];
  std::snprintf(head, sizeof head, "%s%lld\t", kTag, (long long)(e.epoch > 0 ? e.epoch : 0));
  std::string line = head;
  line += e.tool;
  line += '\t';
  line += e.receipt;
  char tail[16];
  std::snprintf(tail, sizeof tail, "\t%08x\n", (unsigned)crc32Of(line.data(), line.size()));
  line += tail;
  return line;
}

bool decodeLine(const char* p, size_t n, RingEntry& out) {
  if (n < kTagLen + 9 || n > kLineMax - 1 || std::memcmp(p, kTag, kTagLen) != 0) return false;
  const size_t body = n - 9;   // the last tab
  if (p[body] != '\t') return false;
  uint32_t crc = 0;
  for (size_t i = body + 1; i < n; ++i) {
    const char c = p[i];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (v < 0) return false;
    crc = (crc << 4) | (uint32_t)v;
  }
  if (crc != crc32Of(p, body)) return false;
  // epoch \t tool \t receipt, between the tag and the last tab.
  const char* q = p + kTagLen;
  const char* end = p + body;
  const char* t1 = static_cast<const char*>(std::memchr(q, '\t', (size_t)(end - q)));
  if (!t1 || t1 == q || t1 - q > 19) return false;
  uint64_t epoch = 0;
  for (const char* c = q; c < t1; ++c) {
    if (*c < '0' || *c > '9') return false;
    epoch = epoch * 10 + (uint64_t)(*c - '0');
  }
  if (epoch > (uint64_t)INT64_MAX) return false;
  const char* t2 = static_cast<const char*>(std::memchr(t1 + 1, '\t', (size_t)(end - t1 - 1)));
  if (!t2) return false;
  RingEntry e;
  e.epoch = (int64_t)epoch;
  if (!cleanField(t1 + 1, (size_t)(t2 - t1 - 1), kToolMax, e.tool, sizeof e.tool)) return false;
  if (!cleanField(t2 + 1, (size_t)(end - t2 - 1), kReceiptMax, e.receipt, sizeof e.receipt)) return false;
  e.stored = true;
  out = e;
  return true;
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

EffectRing::Slot* EffectRing::slotFor(const std::string& chatId, const std::string& key) {
  (void)chatId;
  if (!slots_) {
    void* mem = workingAlloc(kMaxChats * sizeof(Slot));
    if (!mem) return nullptr;
    slots_ = static_cast<Slot*>(mem);
    std::memset(mem, 0, kMaxChats * sizeof(Slot));
  }
  Slot* pick = nullptr;
  for (size_t i = 0; i < kMaxChats; ++i) {
    Slot& s = slots_[i];
    if (s.used && key == s.key) {
      pick = &s;
      break;
    }
  }
  std::string path = std::string(kFxRoot) + "/" + key + ".ring";
  if (!pick) {
    for (size_t i = 0; i < kMaxChats; ++i) {
      Slot& s = slots_[i];
      if (!s.used) {
        pick = &s;
        break;
      }
      if (!pick || s.lastUse < pick->lastUse) pick = &s;
    }
    if (pick->used) ++stats_.evictions;
    if (!pick->ring) {
      pick->ring = static_cast<RingEntry*>(workingAlloc(kRingEntries * sizeof(RingEntry)));
      if (!pick->ring) return nullptr;
    }
    RingEntry* ring = pick->ring;
    std::memset(pick, 0, sizeof(Slot));
    pick->ring = ring;
    pick->used = true;
    copyField(pick->key, sizeof pick->key, key);
  }
  pick->lastUse = ++tick_;
  if (files_ && (!pick->loaded || pick->unsettled)) loadInto(*pick, path);
  return pick;
}

// Settle the chat's files (a temp left by a cut or refused rewrite), then, the first time,
// read the file into RAM. A loaded slot keeps RAM as the truth: settling never re-reads it.
void EffectRing::loadInto(Slot& s, const std::string& path) {
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
  std::string all;
  all.reserve(s.count * kLineMax);
  for (size_t i = 0; i < s.count; ++i) all += encodeLine(s.at(i));
  const std::string tmp = path + ".tmp";
  if (!files_->writeWhole(tmp, all)) {
    files_->remove(tmp);   // the file is whole; a part temp would be taken for a cut rename
    return false;
  }
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
  std::string key;
  if (!ringKey(chatId, key)) {
    ++stats_.refused;
    return AppendResult::Refused;
  }
  Slot* s = slotFor(chatId, key);
  if (!s) {
    ++stats_.refused;
    return AppendResult::Refused;
  }
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
  std::string key;
  if (!ringKey(chatId, key)) return 0;
  Slot* s = slotFor(chatId, key);
  if (!s) return 0;
  const size_t m = n < s->count ? n : s->count;
  for (size_t i = s->count - m; i < s->count; ++i) fn(s->at(i));
  return m;
}

size_t EffectRing::recent(const std::string& chatId, size_t n, std::vector<RingEntry>& out) {
  out.clear();
  return visit(chatId, n, [&out](const RingEntry& e) { out.push_back(e); });
}

Backing EffectRing::backing(const std::string& chatId) {
  std::lock_guard<std::mutex> lock(mu_);
  std::string key;
  if (!files_ || !ringKey(chatId, key)) return Backing::RamOnly;
  Slot* s = slotFor(chatId, key);
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
