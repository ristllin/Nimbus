#include "nimbus/tg_archive_zip.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace nimbus {
namespace tgarc {

namespace {

// The CRC-32 table, built at compile time (flash, not RAM).
constexpr std::array<uint32_t, 256> makeCrcTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    t[i] = c;
  }
  return t;
}
constexpr std::array<uint32_t, 256> kCrc = makeCrcTable();

void le16(std::string& out, uint32_t v) {
  out += (char)(v & 0xFF);
  out += (char)((v >> 8) & 0xFF);
}
void le32(std::string& out, uint32_t v) {
  le16(out, v & 0xFFFF);
  le16(out, v >> 16);
}

bool skipped(const Entry& e) {
  if (e.dir || e.name.empty() || e.name[0] == '.') return true;
  const auto ends = [&](const char* s) {
    const size_t n = strlen(s);
    return e.name.size() >= n && e.name.compare(e.name.size() - n, n, s) == 0;
  };
  return ends(".part") || ends(".tmp");
}

// The DOS date of a "YYYY-MM-DD" day (1980-01-01 for "undated").
uint16_t dosDateOf(const std::string& day) {
  if (day.size() != 10 || day[4] != '-' || day[7] != '-') return (1 << 5) | 1;
  const int y = atoi(day.substr(0, 4).c_str()), m = atoi(day.substr(5, 2).c_str()),
            d = atoi(day.substr(8, 2).c_str());
  if (y < 1980 || m < 1 || m > 12 || d < 1 || d > 31) return (1 << 5) | 1;
  return (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
}

constexpr uint16_t kFlags = 0x0008 | 0x0800;   // data descriptor follows; UTF-8 names
constexpr uint16_t kVersion = 20;

}  // namespace

uint32_t zipCrc32(uint32_t crc, const uint8_t* data, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = kCrc[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

DayZip::DayZip(Fs& fs, const std::string& chat, const std::string& day)
    : fs_(fs), chat_(chat), day_(day), dir_(dayDirPath(chat, day)), dosDate_(dosDateOf(day)) {
  for (const Entry& e : fs_.list(dir_)) {
    if (skipped(e)) continue;
    File f;
    f.name = e.name;
    f.size = e.size;
    files_.push_back(f);
  }
  std::sort(files_.begin(), files_.end(),
            [](const File& a, const File& b) { return a.name < b.name; });
  phase_ = files_.empty() ? Phase::Central : Phase::Local;
  stage();
}

std::string DayZip::downloadName() const { return chat_ + "-" + day_ + ".zip"; }

bool DayZip::tooLarge() const {
  return files_.size() > 0xFFFFu || estimatedBytes() > 0xFFFFFFFFull;
}

uint64_t DayZip::estimatedBytes() const {
  uint64_t n = 22;   // end of central directory
  for (const File& f : files_) n += 30 + f.name.size() + f.size + 16 + 46 + f.name.size();
  return n;
}

void DayZip::stage() {
  pending_.clear();
  pendingAt_ = 0;
  if (phase_ == Phase::Local) {
    File& f = files_[index_];
    f.offset = pos_;
    le32(pending_, 0x04034b50);
    le16(pending_, kVersion);
    le16(pending_, kFlags);
    le16(pending_, 0);          // stored
    le16(pending_, 0);          // time
    le16(pending_, dosDate_);
    le32(pending_, 0);          // crc, sizes: in the data descriptor
    le32(pending_, 0);
    le32(pending_, 0);
    le16(pending_, (uint32_t)f.name.size());
    le16(pending_, 0);          // extra
    pending_ += f.name;
  } else if (phase_ == Phase::Descriptor) {
    const File& f = files_[index_];
    le32(pending_, 0x08074b50);
    le32(pending_, f.crc);
    le32(pending_, f.written);
    le32(pending_, f.written);
  } else if (phase_ == Phase::Central) {
    if (index_ == 0) centralAt_ = pos_;
    if (files_.empty()) { phase_ = Phase::End; stage(); return; }
    const File& f = files_[index_];
    le32(pending_, 0x02014b50);
    le16(pending_, kVersion);   // made by
    le16(pending_, kVersion);   // needed
    le16(pending_, kFlags);
    le16(pending_, 0);
    le16(pending_, 0);
    le16(pending_, dosDate_);
    le32(pending_, f.crc);
    le32(pending_, f.written);
    le32(pending_, f.written);
    le16(pending_, (uint32_t)f.name.size());
    le16(pending_, 0);          // extra
    le16(pending_, 0);          // comment
    le16(pending_, 0);          // disk
    le16(pending_, 0);          // internal attributes
    le32(pending_, 0);          // external attributes
    le32(pending_, f.offset);
    pending_ += f.name;
  } else if (phase_ == Phase::End) {
    le32(pending_, 0x06054b50);
    le16(pending_, 0);
    le16(pending_, 0);
    le16(pending_, (uint32_t)files_.size());
    le16(pending_, (uint32_t)files_.size());
    le32(pending_, pos_ - centralAt_);
    le32(pending_, centralAt_);
    le16(pending_, 0);
  }
}

size_t DayZip::drainPending(uint8_t* buf, size_t cap) {
  const size_t n = std::min(cap, pending_.size() - pendingAt_);
  memcpy(buf, pending_.data() + pendingAt_, n);
  pendingAt_ += n;
  pos_ += (uint32_t)n;
  return n;
}

// One file's bytes into `buf`; at its end, the data descriptor is staged next.
size_t DayZip::readData(uint8_t* buf, size_t cap) {
  const int n = reader_->read(buf, cap);
  if (n < 0) {
    failed_ = true;
    return 0;
  }
  if (n > 0) {
    File& f = files_[index_];
    f.crc = zipCrc32(f.crc, buf, (size_t)n);
    f.written += (uint32_t)n;
    pos_ += (uint32_t)n;
    return (size_t)n;
  }
  reader_.reset();
  phase_ = Phase::Descriptor;
  stage();
  return 0;
}

// Move to the next phase once the current one's bytes are all out.
void DayZip::advance() {
  switch (phase_) {
    case Phase::Local:
      reader_ = fs_.openRead(dir_ + "/" + files_[index_].name);
      if (!reader_) failed_ = true;
      else phase_ = Phase::Data;
      return;
    case Phase::Descriptor:
      if (++index_ < files_.size()) phase_ = Phase::Local;
      else { index_ = 0; phase_ = Phase::Central; }
      stage();
      return;
    case Phase::Central:
      if (++index_ >= files_.size()) phase_ = Phase::End;
      stage();
      return;
    default:
      phase_ = Phase::Done;
      return;
  }
}

size_t DayZip::fill(uint8_t* buf, size_t cap) {
  while (cap > 0 && !failed_ && phase_ != Phase::Done) {
    if (phase_ == Phase::Data) {
      const size_t n = readData(buf, cap);
      if (n > 0 || failed_) return n;
      continue;   // the file ended: its descriptor is staged
    }
    if (pendingAt_ < pending_.size()) return drainPending(buf, cap);
    advance();
  }
  return 0;
}

}  // namespace tgarc
}  // namespace nimbus
