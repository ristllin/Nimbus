// The device binding of the engine-2 compaction driver (plain_fold_device.h, lane RB-C2).
#include "plain_fold_device.h"

#include <LittleFS.h>

#include "fs_atomic.h"

namespace agent {

namespace {

const char kCompactCtlPath[] = "/data/compact_ctl.txt";
const char kChatsumPath[] = "/data/chatsum.txt";

std::string readWhole(const char* path) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return std::string();
  String s = f.readString();
  f.close();
  return std::string(s.c_str(), s.length());
}

plain::TextIO textIO(const char* path, const char* tag) {
  plain::TextIO io;
  io.load = [path] { return readWhole(path); };
  io.save = [path, tag](const std::string& text) { return writeFileAtomic(path, text, tag); };
  return io;
}

}  // namespace

plain::TextIO deviceCompactCtlIO() { return textIO(kCompactCtlPath, "compact_ctl"); }
plain::TextIO deviceChatsumIO() { return textIO(kChatsumPath, "chatsum"); }

}  // namespace agent
