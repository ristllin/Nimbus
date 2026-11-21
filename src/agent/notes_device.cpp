#include "notes_device.h"

#include <Arduino.h>   // xPortCanYield
#include <FS.h>
#include <LittleFS.h>

#include <new>

#include "fs_atomic.h"                       // agent::writeFileAtomic, flashMounted
#include "memory_subsystem.h"                // the card lock, dataFs, haveSd
#include "nimbus/fault.h"                    // FAULT sd_io
#include "nimbus/harness/plain/request.h"    // toolDecl, declSchema
#include "nimbus/harness/turn_memory.h"      // PsramFirstScope

namespace agent::notesdev {

namespace notes = nimbus::orch::notes;

namespace {

constexpr char kOwnerDir[] = "/data/notes";
constexpr char kCardDir[] = "/mem/notes";

const char* extOf(notes::Shelf s) {
  switch (s) {
    case notes::Shelf::Pinned: return ".md";
    case notes::Shelf::More: return ".more.md";
    case notes::Shelf::Pending: return ".pending.md";
  }
  return ".md";
}

bool isOwner(const std::string& person) { return person == "owner"; }

// Read a whole file from `fs`: a missing file is true and empty.
bool readWhole(fs::FS& fs, const std::string& path, std::string& out) {
  out.clear();
  if (!fs.exists(path.c_str())) return true;
  File f = fs.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) return false;
  const size_t sz = f.size();
  out.resize(sz);
  const size_t got = sz ? f.read(reinterpret_cast<uint8_t*>(&out[0]), sz) : 0;
  f.close();
  if (got != sz) {
    out.clear();
    return false;
  }
  return true;
}

// The owner's files on internal flash and a member's on the card. Files are named by the person
// key alone (notes::personKey: "owner" or a chat id of [0-9-]), so no path comes from the model.
class DeviceNotesIo : public notes::NotesIo {
 public:
  bool available(const std::string& person) override {
    if (person.empty()) return false;
    return isOwner(person) ? flashMounted() : memory::haveSd();
  }

  bool read(const std::string& person, notes::Shelf s, std::string& out) override {
    if (!available(person) || !xPortCanYield()) return false;
    const std::string p = path(person, s);
    if (isOwner(person)) return readWhole(LittleFS, p, out);
    memory::Lock g;
    fs::FS& fs = memory::dataFs();
    // A write cut between its two renames leaves the old copy as the .bak file only; a bare temp
    // file (a cut mid-write) is never read.
    if (!fs.exists(p.c_str()) && fs.exists((p + ".bak").c_str())) return readWhole(fs, p + ".bak", out);
    return readWhole(fs, p, out);
  }

  bool write(const std::string& person, notes::Shelf s, const std::string& bytes) override {
    if (!available(person) || !xPortCanYield()) return false;
    const std::string p = path(person, s);
    if (isOwner(person)) {
      LittleFS.mkdir("/data");
      LittleFS.mkdir(kOwnerDir);
      return writeFileAtomic(p.c_str(), bytes, "notes");
    }
    return writeCard(p, bytes);
  }

  notes::Caps caps(const std::string& person) override {
    return isOwner(person) ? notes::kOwnerCaps : notes::kMemberCaps;
  }

 private:
  static std::string path(const std::string& person, notes::Shelf s) {
    return std::string(isOwner(person) ? kOwnerDir : kCardDir) + "/" + person + extOf(s);
  }

  // FAT cannot rename over a file: the temp is written whole, the old file renamed to .bak, the
  // temp renamed into place, the .bak removed. Every cut leaves the old or the new copy readable.
  static bool writeCard(const std::string& p, const std::string& bytes) {
    if (nimbus::fault::active(nimbus::fault::SD_IO)) return false;
    memory::Lock g;
    fs::FS& fs = memory::dataFs();
    if (!fs.exists(kCardDir)) {
      fs.mkdir("/mem");
      if (!fs.mkdir(kCardDir) && !fs.exists(kCardDir)) return false;
    }
    const std::string tmp = p + ".tmp", bak = p + ".bak";
    if (!writeTemp(fs, tmp, bytes)) return false;
    const bool had = fs.exists(p.c_str());
    if (had) {
      if (fs.exists(bak.c_str())) fs.remove(bak.c_str());
      if (!fs.rename(p.c_str(), bak.c_str())) return fs.remove(tmp.c_str()), false;
    }
    if (!fs.rename(tmp.c_str(), p.c_str())) {
      if (had) fs.rename(bak.c_str(), p.c_str());   // the old copy back in place
      fs.remove(tmp.c_str());
      return false;
    }
    if (had) fs.remove(bak.c_str());
    return true;
  }
  static bool writeTemp(fs::FS& fs, const std::string& tmp, const std::string& bytes) {
    File f = fs.open(tmp.c_str(), FILE_WRITE);
    if (!f) return false;
    const size_t w = f.write(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    f.close();
    if (w == bytes.size()) return true;
    fs.remove(tmp.c_str());
    return false;
  }
};

DeviceNotesIo& deviceIo() {
  static DeviceNotesIo io;
  return io;
}

}  // namespace

notes::Notebook& notebook() {
  static notes::Notebook nb(deviceIo());
  return nb;
}

void registerNote(nimbus::orch::ToolRegistry& reg, notes::OriginFn origin) {
  const auto* decl = agent::plain::toolDecl("note");
  if (!decl) return;
  notes::OriginFn from = origin ? std::move(origin) : notes::OriginFn([] { return notes::Origin::Unattended; });
  reg.add(
      "note", decl->description,
      [from](ArduinoJson::JsonObjectConst args, const nimbus::orch::Principal& who) {
        const PsramFirstScope psram;
        return notebook().call(args, who, from());
      },
      agent::plain::declSchema(*decl, false));
}

std::string pinnedNotesFor(const nimbus::orch::Principal& who) {
  const PsramFirstScope psram;
  try {
    return notebook().pinnedBlock(who);
  } catch (const std::bad_alloc&) {
    return std::string();
  }
}

}  // namespace agent::notesdev
