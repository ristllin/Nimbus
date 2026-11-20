// notes.h - the red stub (lane RB-N1): every rule answers "no" until the bodies land.
#include "nimbus/orch/notes.h"

namespace nimbus {
namespace orch {
namespace notes {

const char* shelfSlug(Shelf s) { return s == Shelf::Pinned ? "pinned" : s == Shelf::More ? "more" : "pending"; }
size_t fsBlocks(size_t, size_t) { return 0; }
size_t flashBound(const Caps&, size_t) { return 0; }
std::string personKey(const Principal&) { return std::string(); }
bool validKey(const std::string&) { return false; }
bool validValue(const std::string&) { return false; }
std::string formatLine(const Note&) { return std::string(); }
bool parseLine(const std::string&, Note&) { return false; }
std::string groupedBytes(size_t) { return std::string(); }
const std::vector<const char*>& noteArgs() {
  static const std::vector<const char*> a;
  return a;
}
ToolResult Notebook::call(ArduinoJson::JsonObjectConst, const Principal&, Origin) { return ToolResult::fail("stub"); }
ToolResult Notebook::keep(ArduinoJson::JsonObjectConst, const Principal&, Origin) { return ToolResult::fail("stub"); }
std::string Notebook::pinnedBlock(const Principal&) { return std::string(); }
bool Notebook::unpinned(const Principal&, std::vector<Note>&) { return false; }
bool Notebook::pending(const Principal&, std::vector<PendingNote>&) { return false; }
ToolResult Notebook::approve(const Principal&, const std::string&) { return ToolResult::fail("stub"); }
ToolResult Notebook::reject(const Principal&, const std::string&) { return ToolResult::fail("stub"); }
void Notebook::registerInto(ToolRegistry&, const std::string&, const std::string&, OriginFn) {}

}  // namespace notes
}  // namespace orch
}  // namespace nimbus
