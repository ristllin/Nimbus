// skills_device.cpp - the owner's skill files for engine 2 (lane E2-CHAT; skills_device.h).
#include "skills_device.h"

#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "memory_subsystem.h"   // dataFs, haveSd, Lock

namespace agent {
namespace skillsdev {

namespace {

constexpr const char* kSdRoot = "/mem/skills";      // skills.cpp's roots, on each tier
constexpr const char* kFlashRoot = "/data/skills";
constexpr size_t kFileCap = 8192;                   // the store's fileCap

const char* root() { return memory::haveSd() ? kSdRoot : kFlashRoot; }

// skills.cpp DeviceSkillFs::read: under the card lock, staged through PSRAM.
bool readFile(const std::string& path, std::string& out) {
  memory::Lock g;
  File f = memory::dataFs().open(path.c_str(), FILE_READ);
  if (!f) return false;
  size_t n = f.size();
  if (n > kFileCap) n = kFileCap;
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (!buf) {
    f.close();
    return false;
  }
  const size_t got = f.read((uint8_t*)buf, n);
  f.close();
  out.assign(buf, got);
  heap_caps_free(buf);
  return true;
}

// skills.cpp DeviceSkillFs::listDirs.
std::vector<std::string> listDirs(const std::string& dir) {
  std::vector<std::string> out;
  memory::Lock g;
  File d = memory::dataFs().open(dir.c_str());
  if (!d || !d.isDirectory()) return out;
  for (File e; (e = d.openNextFile());) {
    if (e.isDirectory()) {
      const char* full = e.name();   // may be the full path or the bare name
      const char* slash = strrchr(full, '/');
      out.push_back(slash ? slash + 1 : full);
    }
    e.close();
  }
  d.close();
  return out;
}

}  // namespace

std::vector<plain::SkillFile> files() {
  std::vector<plain::SkillFile> out;
  const std::string r = root();
  for (const std::string& id : listDirs(r)) {
    if (!plain::chatSkillIdOk(id)) continue;
    plain::SkillFile f;
    f.id = id;
    if (readFile(r + "/" + id + "/SKILL.md", f.md)) out.push_back(std::move(f));
  }
  return out;
}

plain::ChatSkills chatSkills() { return plain::chatSkillsOf(files()); }

bool skillBody(const std::string& id, std::string& out) {
  if (!plain::chatSkillIdOk(id)) return false;
  std::vector<plain::SkillFile> one(1);
  one[0].id = id;
  if (!readFile(std::string(root()) + "/" + id + "/SKILL.md", one[0].md)) return false;
  return plain::skillBody(one, id, out);
}

}  // namespace skillsdev
}  // namespace agent
