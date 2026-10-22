#include "nimbus/harness/turn_inbox.h"

#include "nimbus/orch/history_rows.h"   // rowTextHash - a note binds to its routed text

namespace agent {
namespace history {

namespace {
struct Slot {
  Inbound in;
  uint32_t textHash = 0;
  bool set = false;
};
Slot& slot() {
  static Slot s;
  return s;
}
bool& v2Flag() {
  static bool on = true;
  return on;
}
}  // namespace

void noteInbound(const Inbound& in, const std::string& routedText) {
  slot().in = in;
  slot().textHash = nimbus::orch::rowTextHash(routedText);
  slot().set = true;
}

bool takeInbound(const std::string& chatId, const std::string& routedText, Inbound& out) {
  Slot& s = slot();
  if (!s.set) return false;
  s.set = false;
  // Stale: noted for a turn that never reached the engine.
  const bool mine = s.in.chatId == chatId && s.textHash == nimbus::orch::rowTextHash(routedText);
  if (mine) out = std::move(s.in);
  s.in = Inbound();
  return mine;
}

bool v2Enabled() { return v2Flag(); }
void setV2Enabled(bool on) { v2Flag() = on; }

}  // namespace history
}  // namespace agent
