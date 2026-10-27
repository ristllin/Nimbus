#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/tg_archive.h"

// tg_archive_zip - one archived Telegram day as a ZIP, streamed (the archive view's
// "Export day"). Every file in the day folder (the index lines, the records and the
// media) goes in stored, uncompressed: photos and voice notes are already
// compressed, and storing needs no buffer. Sizes and CRCs follow each file in a data
// descriptor, so the archive is written in ONE pass, a chunk at a time, with no
// file held in RAM; the central directory goes last. Pure and host-tested over the
// archive's Fs seam (test/test_archive_web); the device fills a card stream from it
// (src/net/web_admission.h), each read only trying the card lock.
namespace nimbus {
namespace tgarc {

class DayZip {
 public:
  // The day folder's files (partial "*.part"/"*.tmp" writes are left out), in name
  // order. An empty or missing folder yields a valid, empty ZIP.
  DayZip(Fs& fs, const std::string& chat, const std::string& day);

  // The next bytes into `buf` (at most `cap`); 0 once the ZIP is complete or a read
  // failed (failed() then says so: the device cuts the download, so the client sees a
  // failed download rather than a short or silently wrong file).
  size_t fill(uint8_t* buf, size_t cap);

  // The ZIP's size from the listed file sizes (exact unless a file changes while it
  // streams), for the response's memory admission.
  uint64_t estimatedBytes() const;
  // A day this ZIP format cannot hold (more than 65,535 files or 4 GiB without the
  // Zip64 extension): the route refuses the export instead of writing a corrupt ZIP.
  bool tooLarge() const;
  size_t fileCount() const { return files_.size(); }
  bool failed() const { return failed_; }
  // "<chat>-<day>.zip", the download name.
  std::string downloadName() const;

 private:
  struct File {
    std::string name;
    uint64_t size = 0;      // listed
    uint32_t crc = 0;       // computed while streaming
    uint32_t written = 0;   // bytes actually streamed
    uint32_t offset = 0;    // local header offset in the ZIP
  };
  enum class Phase : uint8_t { Local, Data, Descriptor, Central, End, Done };
  void stage();   // queue the next header bytes for the current phase
  size_t drainPending(uint8_t* buf, size_t cap);
  size_t readData(uint8_t* buf, size_t cap);
  void advance();

  Fs& fs_;
  std::string chat_, day_, dir_;
  std::vector<File> files_;
  std::unique_ptr<Reader> reader_;
  std::string pending_;   // header bytes not yet handed out
  size_t pendingAt_ = 0;
  size_t index_ = 0;
  uint32_t pos_ = 0;      // bytes handed out so far
  uint32_t centralAt_ = 0;
  uint16_t dosDate_ = 0;
  Phase phase_ = Phase::Local;
  bool failed_ = false;
};

// CRC-32 (IEEE 802.3, the ZIP checksum), continued from `crc` over `n` bytes.
uint32_t zipCrc32(uint32_t crc, const uint8_t* data, size_t n);

}  // namespace tgarc
}  // namespace nimbus
