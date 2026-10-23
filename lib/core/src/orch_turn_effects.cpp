#include "nimbus/orch/turn_effects.h"

#include <cstring>

namespace nimbus {
namespace orch {

namespace {

struct FxInfo {
  const char* slug;     // machine slug (logs, tests)
  const char* tag;      // cross-turn tag group ("" = not tagged)
  const char* what;     // plain noun for the summary
};

// One row per FxKind, in enum order (static_assert below keeps them aligned).
constexpr FxInfo kFxInfo[] = {
    {"mem_write", "mem", "memory writes"},
    {"mem_update", "mem", "memory updates"},
    {"mem_pin", "mem", "memory pins"},
    {"mem_pin_noop", "", "pins on an already pinned memory (no change)"},
    {"mem_delete", "mem", "memory deletions"},
    {"scratch", "scratch", "scratchpad updates saved"},
    {"scratch_ram_only", "scratch", "scratchpad updates kept in RAM only (saving failed)"},
    {"file_write", "file", "files written"},
    {"skill_save", "skill", "skill changes"},
    {"spawn", "spawn", "sub-agents started"},
    {"spawn_refused", "", "sub-agents not started"},
    {"send", "send", "messages or files sent"},
    {"speak", "speak", "spoken replies"},
    {"schedule", "sched", "routines or reminders set"},
    {"config", "cfg", "settings changed"},
    {"read", "read", "lookups"},
    {"sub_sourced", "read", "sub-agent results with a lookup behind them"},
    {"sub_unsourced", "", "sub-agent results with NO lookup behind them"},
};
static_assert(sizeof(kFxInfo) / sizeof(kFxInfo[0]) == (size_t)FxKind::kCount,
              "every FxKind needs a row in kFxInfo");

bool startsWith(const std::string& s, const char* p) {
  return s.compare(0, std::strlen(p), p) == 0;
}

bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

void copyRef(char (&dst)[24], const std::string& ref) {
  // Keep the TAIL of a long ref (a file path's name is at its end), cut on a
  // UTF-8 boundary so the bytes stay valid.
  size_t from = ref.size() > sizeof(dst) - 1 ? ref.size() - (sizeof(dst) - 1) : 0;
  while (from < ref.size() && ((unsigned char)ref[from] & 0xC0) == 0x80) from++;
  const size_t n = ref.size() - from;
  std::memcpy(dst, ref.data() + from, n);
  dst[n] = '\0';
}

// The scratchpad and config tools also answer "view": that is a lookup, not a
// change. Their change answers start with these words.
bool scratchChanged(const std::string& out) {
  return startsWith(out, "active task set") || startsWith(out, "added to") ||
         startsWith(out, "replaced ") || contains(out, " cleared") ||
         startsWith(out, "scratchpad cleared");
}

FxKind refineKind(FxKind base, const std::string& out) {
  switch (base) {
    case FxKind::MemPin:
      if (startsWith(out, "already pinned")) return FxKind::MemPinNoop;
      if (startsWith(out, "unpin ok")) return FxKind::MemUpdate;
      if (startsWith(out, "delete ok")) return FxKind::MemDelete;
      return FxKind::MemPin;
    case FxKind::Scratch:
    case FxKind::ScratchRamOnly:
      if (!scratchChanged(out)) return FxKind::Read;
      return contains(out, "kept until restart only") ? FxKind::ScratchRamOnly : FxKind::Scratch;
    case FxKind::Config:
      return startsWith(out, "updated ") ? FxKind::Config : FxKind::Read;
    default:
      return base;
  }
}

std::string countText(int n) { return std::to_string(n); }

}  // namespace

const char* fxSlug(FxKind k) {
  const size_t i = (size_t)k;
  return i < (size_t)FxKind::kCount ? kFxInfo[i].slug : "unknown";
}

void TurnEffects::reset() {
  v_.clear();
  overflow_ = false;
}

bool TurnEffects::add(FxKind k, bool ok, const std::string& ref, uint8_t count, uint8_t flags) {
  if (k >= FxKind::kCount) return false;
  if (v_.size() >= kMax) {
    overflow_ = true;
    return false;
  }
  if (v_.capacity() == 0) v_.reserve(8);
  Effect e;
  e.kind = k;
  e.ok = ok;
  e.flags = flags;
  e.count = count ? count : 1;
  copyRef(e.ref, ref);
  v_.push_back(e);
  return true;
}

int TurnEffects::countOk(FxKind k) const {
  int n = 0;
  for (const Effect& e : v_)
    if (e.kind == k && e.ok) n += e.count;
  return n;
}

int TurnEffects::countFailed(FxKind k) const {
  int n = 0;
  for (const Effect& e : v_)
    if (e.kind == k && !e.ok) n += e.count;
  return n;
}

bool TurnEffects::anyOkWith(FxKind k, uint8_t flag) const {
  for (const Effect& e : v_)
    if (e.kind == k && e.ok && (e.flags & flag)) return true;
  return false;
}

void TurnEffects::dropPending() {
  size_t w = 0;
  for (size_t r = 0; r < v_.size(); r++)
    if (!(v_[r].flags & kFxPending)) v_[w++] = v_[r];
  v_.resize(w);
}

std::string TurnEffects::summary() const {
  if (v_.empty()) return "nothing (no tool changed or looked up anything)";
  std::string out;
  for (size_t k = 0; k < (size_t)FxKind::kCount; k++) {
    const FxKind kind = (FxKind)k;
    const int ok = countOk(kind), bad = countFailed(kind);
    if (ok + bad == 0) continue;
    if (!out.empty()) out += "; ";
    out += kFxInfo[k].what;
    out += ": " + countText(ok);
    if (bad) out += " (" + countText(bad) + " failed or refused)";
    if (kind == FxKind::Send && sendQueuedOnly(*this)) out += " (queued for Telegram, no delivery receipt)";
    std::string refs;
    for (const Effect& e : v_)
      if (e.kind == kind && e.ok && e.ref[0] && refs.size() < 48) refs += (refs.empty() ? "" : ", ") + std::string(e.ref);
    if (!refs.empty()) out += " (" + refs + ")";
  }
  if (overflow_) out += "; (more effects than the ledger holds)";
  return out;
}

std::string TurnEffects::fxTag() const {
  std::string out = "fx:";
  bool anyTag = false;
  for (size_t k = 0; k < (size_t)FxKind::kCount; k++) {
    const char* tag = kFxInfo[k].tag;
    if (!tag[0] || !anyOk((FxKind)k)) continue;
    // Several kinds share a group (mem, scratch, read): name it once.
    const std::string t(tag);
    if (out.find(":" + t) != std::string::npos || out.find("," + t) != std::string::npos) continue;
    const size_t need = t.size() + (anyTag ? 1 : 0);
    if (out.size() + need > 24) break;
    out += (anyTag ? "," : "") + t;
    anyTag = true;
  }
  return anyTag ? out : std::string("fx:none");
}

std::string fxRefFromOutput(const std::string& out) {
  // "stored memory <id>...": the id runs to the next space or punctuation.
  static const char* const kLeads[] = {"stored memory ", "saved ", "wrote "};
  for (const char* lead : kLeads) {
    if (!startsWith(out, lead)) continue;
    size_t b = std::strlen(lead), e = b;
    while (e < out.size() && out[e] != ' ' && out[e] != '\n' && out[e] != '(' && out[e] != ',') e++;
    return out.substr(b, e - b);
  }
  // "<action> ok (<id>)" and "already pinned (<id>)".
  const size_t open = out.find(" (");
  if (open != std::string::npos && (contains(out, " ok (") || startsWith(out, "already pinned"))) {
    const size_t close = out.find(')', open);
    if (close != std::string::npos) return out.substr(open + 2, close - open - 2);
  }
  return std::string();
}

std::string fxReceiptId(const std::string& out) {
  static const char kLead[] = "message_id ";
  const size_t at = out.find(kLead);
  if (at == std::string::npos) return std::string();
  size_t b = at + sizeof(kLead) - 1, e = b;
  while (e < out.size() && out[e] >= '0' && out[e] <= '9') e++;
  return e > b ? out.substr(b, e - b) : std::string();
}

bool voiceDelivered(const TurnEffects& fx) {
  for (size_t i = 0; i < fx.size(); i++) {
    const Effect& e = fx.at(i);
    if (e.kind == FxKind::Send && e.ok && (e.flags & kFxVoice) && (e.flags & kFxReceipt)) return true;
  }
  return false;
}

bool sendDelivered(const TurnEffects& fx) {
  for (size_t i = 0; i < fx.size(); i++) {
    const Effect& e = fx.at(i);
    if (e.kind == FxKind::Send && e.ok && (!(e.flags & kFxTelegram) || (e.flags & kFxReceipt))) return true;
  }
  return false;
}

bool sendQueuedOnly(const TurnEffects& fx) {
  bool queued = false;
  for (size_t i = 0; i < fx.size(); i++) {
    const Effect& e = fx.at(i);
    if (e.kind != FxKind::Send || !e.ok) continue;
    if ((e.flags & kFxTelegram) && !(e.flags & kFxReceipt)) queued = true;
    else return false;   // something did arrive
  }
  return queued;
}

namespace {
bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
}  // namespace

bool deviceSummaryEffect(const std::string& sum, FxKind& kind, bool& landed) {
  kind = FxKind::Config;
  if (sum.find("cfg(") != std::string::npos) { landed = false; return true; }   // no change, nothing
  if (endsWith(sum, "cfg ")) { landed = true; return true; }
  if (startsWith(sum, "tts")) {
    kind = FxKind::Speak;
    landed = sum == "tts ";
    return true;
  }
  if (startsWith(sum, "led") || startsWith(sum, "lights") || startsWith(sum, "reboot")) {
    landed = sum == "led " || sum == "lights " || sum == "reboot ";
    return true;
  }
  if (startsWith(sum, "orch_model(")) {
    landed = !contains(sum, "(invalid)") && !contains(sum, "(no-key)") && !contains(sum, "(no change)");
    return true;
  }
  return false;   // skip, protected-BLOCKED, unsupported: no action ran
}

// A send: its receipt, if the result names one. A voice send without a receipt did
// not arrive (it records as failed); one that went out as text is a plain send.
static void recordSend(TurnEffects& fx, const std::string& output, uint8_t flags) {
  if ((flags & kFxVoice) && contains(output, "as text")) flags &= (uint8_t)~kFxVoice;
  const std::string id = fxReceiptId(output);
  if (!id.empty()) flags |= kFxReceipt;
  if ((flags & kFxVoice) && id.empty()) {
    fx.add(FxKind::Send, false, std::string(), 1, flags);
    return;
  }
  fx.add(FxKind::Send, true, id.empty() ? fxRefFromOutput(output) : id, 1, flags);
}

// "queued memory <id>, not stored yet" / "queued update for <id>, not changed yet".
static bool queuedWrite(FxKind base, const std::string& out) {
  return (base == FxKind::MemWrite || base == FxKind::MemUpdate) &&
         (startsWith(out, "queued memory ") || startsWith(out, "queued update for "));
}

void recordToolEffect(TurnEffects& fx, FxKind base, bool isError, const std::string& output,
                      uint8_t flags) {
  if (queuedWrite(base, output)) {   // queued is not stored, success flag or not
    fx.add(base, false, std::string(), 1, (uint8_t)(flags | kFxQueued));
    return;
  }
  if (base == FxKind::Config && startsWith(output, "applied: ")) {   // device.control
    // The summary is the first line; a risk note may follow it.
    const size_t nl = output.find('\n');
    const std::string sum = output.substr(9, nl == std::string::npos ? std::string::npos : nl - 9);
    FxKind k;
    bool landed = false;
    if (deviceSummaryEffect(sum, k, landed)) fx.add(k, landed && !isError);
    else fx.add(FxKind::Read, !isError);
    return;
  }
  if (isError) {
    // A voice reply that stopped part way: the messages Telegram named did arrive
    // ("only 2 of the 3 voice messages reached Telegram (message_id 811, 812) ..."),
    // so they back a claim about them; the rest is recorded as failed.
    const std::string id = base == FxKind::Send ? fxReceiptId(output) : std::string();
    if (!id.empty()) fx.add(FxKind::Send, true, id, 1, (uint8_t)(flags | kFxReceipt));
    fx.add(base, false, std::string(), 1, flags);
    return;
  }
  if (base == FxKind::Send) {
    recordSend(fx, output, flags);
    return;
  }
  const FxKind k = refineKind(base, output);
  // A memory is pinned by a write only when the write stored it; a duplicate
  // says whether the existing one was already pinned (a pin that changed nothing).
  const bool stored = startsWith(output, "stored memory ");
  if (!stored) flags &= (uint8_t)~kFxPermanent;
  fx.add(k, true, fxRefFromOutput(output), 1, flags);
  if (k == FxKind::MemWrite && !stored && contains(output, "it is already pinned"))
    fx.add(FxKind::MemPinNoop, true);
}

}  // namespace orch
}  // namespace nimbus
