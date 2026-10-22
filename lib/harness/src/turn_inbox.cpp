#include "nimbus/harness/turn_inbox.h"

#include "nimbus/orch/history_rows.h"   // rowTextHash - a note binds to its routed text
#include "nimbus/orch/msg_batch.h"      // MessageBatcher - a Telegram turn's bucket

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

Inbound inboundForBucket(const nimbus::orch::MessageBatcher& b, size_t i,
                         const std::vector<std::string>& storedRefs) {
  Inbound in;
  in.chatId = b.chatIdAt(i);
  int32_t newest = 0;
  for (size_t m = 0; m < b.messageCountAt(i); ++m)
    if (b.updateIdAt(i, m) > newest) newest = b.updateIdAt(i, m);
  if (newest > 0) in.src = "tg:" + std::to_string(newest) + "x" + std::to_string(b.messageCountAt(i));
  const nimbus::orch::PsString rendered = b.render(i);
  std::string said(rendered.c_str(), rendered.size());
  size_t from = 0;
  bool cut = false;
  for (size_t k = 0; k < b.mediaCountAt(i) && k < storedRefs.size(); ++k) {
    if (storedRefs[k].empty()) continue;
    const std::string seg = b.mediaTextAt(i, k);
    const size_t close = seg.find(']');
    const size_t at = said.find(seg, from);
    in.refs.push_back(storedRefs[k]);
    if (close == std::string::npos || at == std::string::npos) continue;
    said.replace(at, seg.size(), seg.substr(0, close + 1));   // the label stays where the item sat
    from = at + close + 1;
    cut = true;
  }
  if (cut) in.said = said;
  return in;
}

bool v2Enabled() { return v2Flag(); }
void setV2Enabled(bool on) { v2Flag() = on; }

}  // namespace history
}  // namespace agent
