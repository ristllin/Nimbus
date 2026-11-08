#include "nimbus/orch/degraded.h"

#include <algorithm>

#include "nimbus/orch/memory_tools.h"   // EmbedFail, embedFailWords: the real cause of a parked embedding
#include "nimbus/orch/provider_slots.h"

namespace nimbus {
namespace orch {

namespace {

// ---- small helpers ---------------------------------------------------------------------

std::string joinNames(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) out += ", ";
    out += v[i];
  }
  return out;
}

std::vector<std::string> labels(const std::vector<std::string>& slugs) {
  std::vector<std::string> out;
  out.reserve(slugs.size());
  for (const std::string& s : slugs) out.push_back(degradedProviderLabel(s));
  return out;
}

std::string minutesWord(uint32_t ms) {
  const uint32_t m = ms / 60000u;
  return m <= 1 ? std::string("a minute") : std::to_string(m) + " minutes";
}

std::string ordinalChoice(size_t down) {
  if (down == 1) return "2nd choice";
  if (down == 2) return "3rd choice";
  return "later choice";
}

// "a -> b -> c" into its trimmed, non-empty parts.
std::vector<std::string> splitPath(const std::string& path) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= path.size()) {
    size_t next = path.find("->", pos);
    if (next == std::string::npos) next = path.size();
    size_t b = pos, e = next;
    while (b < e && path[b] == ' ') ++b;
    while (e > b && path[e - 1] == ' ') --e;
    if (e > b) out.push_back(path.substr(b, e - b));
    if (next >= path.size()) break;
    pos = next + 2;
  }
  return out;
}

// ---- 1. the main provider is unavailable -----------------------------------------------------

// A label's first word: the owner's short name for a two-word provider.
std::string firstWord(const std::string& label) { return label.substr(0, label.find(' ')); }

// "<down> down, <served> answers" in the longest form that fits kDegradedLineMax.
std::string fallbackLine(const std::string& allDown, const std::string& head, const std::string& served) {
  const std::string answers = " answers";
  const std::string forms[] = {
      allDown + " down, " + served + answers,
      head + " down, " + served + answers,
      firstWord(head) + " down, " + firstWord(served) + answers,
      "Main provider down, " + firstWord(served) + answers,
  };
  for (const std::string& f : forms)
    if (f.size() <= kDegradedLineMax) return f;
  return "Main provider down, backup answers";
}

DegradedCondition headFallback(const FallbackFact& f) {
  DegradedCondition c;
  c.id = DegradedId::HeadFallback;
  const std::string served = degradedProviderLabel(f.served);
  std::vector<std::string> down = labels(f.down);
  if (down.empty()) down.push_back(degradedProviderLabel(f.head));
  // Name the main provider and the one that answers; keep the providers between them when the
  // line has room, then shorter forms of the names ("Cumulo Nimbus" is "Cumulo", "Custom
  // endpoint" is "Custom"), and at the last "Main provider down" so the line never clips on the
  // strip. The reason below always has the full names.
  c.line = fallbackLine(joinNames(down), down.front(), served);

  const std::string head = down.front();
  const std::string phrase = f.headClosed && !f.headWhy.empty() ? f.headWhy : "did not answer the last turn";
  c.why = head + ", your main provider, " + phrase + ".";
  for (size_t i = 1; i < down.size(); i++) c.why += " " + down[i] + " did not answer either.";
  c.why += " " + served + ", your " + ordinalChoice(f.down.size() ? f.down.size() : 1) + ", answered instead.";
  if (f.headClosed && !f.headFix.empty()) c.why += " " + f.headFix + ".";
  c.why += " Turns return to " + head + " once it answers again.";
  return c;
}

// ---- 2. a parked or paused provider ---------------------------------------------------------

// The line's words after the name, for one provider.
std::string parkTail(const ParkedFact& p) {
  if (p.kind == ParkKind::Billing) {
    switch (p.cause) {
      case ParkCause::NeedsPlan: return "needs a plan";
      case ParkCause::KeyBudget: return "budget used up";
      case ParkCause::Suspended: return "account suspended";
      case ParkCause::WalletEmpty:
      case ParkCause::Unspecified: break;
    }
    return "is out of credit";
  }
  if (p.kind == ParkKind::Auth) return p.httpStatus == 403 ? "refuses some requests" : "key refused";
  const uint8_t own = p.paused & uint8_t(~kPauseSpendCeiling);
  if (own & kPauseProviderLimit) return "hit its monthly limit";
  if (own & kPauseOwner) return "is paused";
  return std::string();
}

bool holds(const ParkedFact& p) { return !parkTail(p).empty(); }

// Does the provider come back without the owner? A refusal park is probed again by itself and a
// provider's own monthly limit lifts when it is raised or the month turns. Only the owner's pause
// waits for the owner.
bool returnsByItself(const ParkedFact& p) {
  return p.kind != ParkKind::None || (p.paused & kPauseProviderLimit);
}
bool waitsForOwner(const ParkedFact& p) { return (p.paused & kPauseOwner) != 0; }

// The closing sentence: what happens next, true of every provider the line names.
std::string parkReturn(const std::vector<ParkedFact>& held) {
  bool byItself = false, byOwner = false;
  for (const ParkedFact& p : held) {
    byItself = byItself || returnsByItself(p);
    byOwner = byOwner || waitsForOwner(p);
  }
  const bool one = held.size() == 1;
  if (byItself && byOwner)
    return one ? "It is checked again by itself, and stays paused until you resume it."
               : "Those you paused stay paused until you resume them. The others are checked again by themselves.";
  if (byOwner) return one ? "It stays paused until you resume it." : "They stay paused until you resume them.";
  return one ? "It is checked again by itself." : "They are checked again by themselves.";
}

DegradedCondition providerParked(const std::vector<ParkedFact>& held) {
  DegradedCondition c;
  c.id = DegradedId::ProviderParked;
  std::vector<std::string> names;
  for (const ParkedFact& p : held) names.push_back(degradedProviderLabel(p.provider));
  if (held.size() == 1) {
    c.line = names[0] + " " + parkTail(held[0]);
  } else {
    c.line = joinNames(names) + " unavailable";
    if (c.line.size() > kDegradedLineMax) c.line = std::to_string(held.size()) + " providers unavailable";
  }
  for (const ParkedFact& p : held) {
    const ParkReason r = degradedParkReason(p);
    if (!c.why.empty()) c.why += " ";
    c.why += degradedProviderLabel(p.provider) + " " + r.phrase + ".";
    if (!r.fix.empty()) c.why += " " + r.fix + ".";
  }
  c.why += " Other providers answer meanwhile. " + parkReturn(held);
  return c;
}

// ---- 3. memory without embeddings ---------------------------------------------------------

// The one next step for an embedding outage, by what parked it. Credit advice for a width
// mismatch sends the owner to the wrong page, and a passing timeout needs no step at all. A table
// (one row per cause) keeps this flat; test_every_embed_cause_has_its_own_honest_reason fails for
// a cause with no row.
struct EmbedFixRow {
  EmbedFail kind;
  const char* fix;
};
constexpr const char* kTriedAgain = "It is tried again by itself.";
constexpr const char* kCreditPlan = "Check the provider's credit and plan under Assistant > Models.";
const EmbedFixRow kEmbedFix[] = {
    {EmbedFail::WidthMismatch, "Set Dimensions to the model's size under Memory > Embedding model."},
    {EmbedFail::NoModel, "Choose an embedding model under Memory > Embedding model."},
    {EmbedFail::RouteNotAllowed, "Choose another embedding provider under Memory > Embedding model."},
    {EmbedFail::NoKey, "Add the embedding provider's key under Assistant > Models."},
    {EmbedFail::KeyRejected, "Update the embedding provider's key under Assistant > Models."},
    {EmbedFail::OutOfCredit, kCreditPlan},
    {EmbedFail::CallTooLarge, kCreditPlan},
    {EmbedFail::AccountHeld, kCreditPlan},
    {EmbedFail::Busy, kTriedAgain},
    {EmbedFail::Unreachable, kTriedAgain},
    {EmbedFail::Timeout, kTriedAgain},
    {EmbedFail::RateLimited, kTriedAgain},
    {EmbedFail::RouterHeld, kTriedAgain},
    {EmbedFail::ProviderError, kTriedAgain},
    {EmbedFail::BadResponse, kTriedAgain},
    {EmbedFail::BadRequest, kTriedAgain},
    {EmbedFail::Unknown, kTriedAgain},
};

std::string embedFix(EmbedFail kind) {
  for (const EmbedFixRow& r : kEmbedFix)
    if (r.kind == kind) return r.fix;
  return "Check the embedding model under Memory > Embedding model and the key under Assistant > Models.";
}

DegradedCondition memory(bool parked, int pending, EmbedFail kind, const std::string& detail) {
  DegradedCondition c;
  c.id = DegradedId::Memory;
  if (parked && pending > 0) c.line = "Memory is keyword-only, saves queued";
  else if (parked) c.line = "Memory is keyword-only";
  else c.line = "Memory saves are queued";
  if (parked) {
    // The cause in the park's own words ("the model returns 1024-wide embeddings but memory is set
    // to 768"), when it has any; an unmapped cause says nothing rather than "unavailable" twice.
    std::string cause = kind == EmbedFail::None ? std::string() : embedFailWords(kind, detail);
    if (cause == "unavailable") cause.clear();
    c.why = "Embeddings are unavailable" + (cause.empty() ? std::string() : " (" + cause + ")") +
            ", so memory search matches words only";
    c.why += pending > 0 ? " and " + std::to_string(pending) + (pending == 1 ? " new memory waits" : " new memories wait") +
                               " in a queue."
                         : ".";
    c.why += " Nothing is lost: queued memories are stored once embeddings answer again. " + embedFix(kind);
  } else {
    c.why = std::to_string(pending) + (pending == 1 ? " new memory is" : " new memories are") +
            " waiting for an embedding. They are stored as soon as embeddings answer.";
  }
  return c;
}

// ---- 4. the store is full -----------------------------------------------------------------------

DegradedCondition storeFull(bool files) {
  DegradedCondition c;
  c.id = DegradedId::StoreFull;
  if (files) {
    c.line = "File storage is full";
    c.why = "The file store has no room, so new photos and files are not saved. Delete files you do not need in the"
            " Memory tab's Files section on the device web page.";
  } else {
    c.line = "Device storage is full";
    c.why = "The device's internal storage is full, so new memories are not saved. Delete files you do not need in the"
            " Memory tab's Files section on the device web page, or erase old data.";
  }
  return c;
}

// ---- 5. Telegram is silent ---------------------------------------------------------------------------

bool telegramSilent(const TelegramFact& t, DegradedCondition& c) {
  if (!t.configured || !t.wifiUp) return false;
  c.id = DegradedId::TelegramSilent;
  if (!t.taskAlive) {
    c.line = "Telegram has stopped";
    c.why = "The Telegram loop is not running, so messages are neither read nor answered. Restart the device to start it again.";
    return true;
  }
  if (t.authRejected) {
    c.line = "Telegram rejects the bot token";
    c.why = "Telegram refused the bot token, so no message reaches the device. Set a new bot token under"
            " Assistant > Connectors > Telegram.";
    return true;
  }
  const uint32_t allowed = t.turnBusy ? kTgIdleBusyMs : kTgIdleMs;
  if (t.idleMs > allowed) {
    c.line = "Telegram is not responding";
    c.why = "The Telegram loop has not checked in for " + minutesWord(t.idleMs) +
            ", so messages may not arrive. It usually recovers by itself. If it does not, restart the device.";
    return true;
  }
  if (t.fails >= kTgFailsUnreachable) {
    c.line = "Telegram is unreachable";
    c.why = "Telegram has not answered the last " + std::to_string(t.fails) +
            " checks. Messages wait on Telegram and arrive once it answers again.";
    return true;
  }
  return false;
}

// ---- 6. the clock -------------------------------------------------------------------------------------

DegradedCondition clockUnsynced(bool wifiUp) {
  DegradedCondition c;
  c.id = DegradedId::ClockUnsynced;
  c.line = "Clock not synced";
  c.why = wifiUp ? "The device has not reached a time server yet, so routines, the nightly dream and dated memories wait."
                   " It keeps trying."
                 : "The device is not on Wi-Fi, so its clock cannot sync. Routines, the nightly dream and dated memories wait."
                   " Reconnect it to Wi-Fi.";
  return c;
}

}  // namespace

const char* degradedSlug(DegradedId id) {
  switch (id) {
    case DegradedId::HeadFallback:   return "head_fallback";
    case DegradedId::ProviderParked: return "provider_parked";
    case DegradedId::Memory:         return "memory";
    case DegradedId::StoreFull:      return "store_full";
    case DegradedId::TelegramSilent: return "telegram_silent";
    case DegradedId::ClockUnsynced:  return "clock_unsynced";
  }
  return "";
}

std::string degradedProviderLabel(const std::string& slug) {
  if (const ProviderSlot* s = findProviderSlot(slug.c_str())) return s->label;
  if (slug == "custom") return "Custom endpoint";
  return slug;
}

ParkReason degradedParkReason(const ParkedFact& p) {
  ParkReason r;
  if (p.kind == ParkKind::Billing) {
    switch (p.cause) {
      case ParkCause::NeedsPlan:
        r.phrase = "needs credits or a plan for this account";
        r.fix = parkCauseFix(p.cause);
        break;
      case ParkCause::KeyBudget:
        r.phrase = "reached this device's monthly budget";
        r.fix = parkCauseFix(p.cause);
        break;
      case ParkCause::Suspended:
        r.phrase = "says this account is suspended";
        r.fix = parkCauseFix(p.cause);
        break;
      case ParkCause::WalletEmpty:
        r.phrase = "is out of credit";
        r.fix = parkCauseFix(p.cause);
        break;
      case ParkCause::Unspecified:
        r.phrase = "is out of credit";
        r.fix = "Add credit with the provider";
        break;
    }
    return r;
  }
  if (p.kind == ParkKind::Auth) {
    if (p.httpStatus == 403) {
      r.phrase = "refuses some requests";
      r.fix = "Check the key's access to the models in use with the provider";
    } else {
      r.phrase = "refused the key";
      r.fix = "Update the key under Assistant > Models";
    }
    return r;
  }
  const uint8_t own = p.paused & uint8_t(~kPauseSpendCeiling);
  if (own & kPauseProviderLimit) {
    r.phrase = "reached its monthly limit";
    r.fix = "Raise the limit under Assistant > Usage";
  } else if (own & kPauseOwner) {
    r.phrase = "is paused";
    r.fix = "Resume it under Assistant > Usage";
  }
  return r;
}

namespace {

// Every provider the park holds off, except the ones the fallback line already names as
// down: a park on one of those is that line's cause, not a second line.
std::vector<ParkedFact> heldProviders(const DegradedFacts& f, bool fallback) {
  std::vector<ParkedFact> held;
  const std::vector<std::string>& named = f.fallback.down;
  for (const ParkedFact& p : f.parks) {
    if (!holds(p)) continue;
    if (fallback && std::find(named.begin(), named.end(), p.provider) != named.end()) continue;
    held.push_back(p);
  }
  return held;
}

}  // namespace

DegradedSet computeDegraded(const DegradedFacts& f) {
  DegradedSet out;
  if (!f.orchestrator) return out;

  // 1. The head's fallback, 2. every other provider the park holds off.
  const FallbackFact& fb = f.fallback;
  const bool fallback = fb.active && (fb.headClosed || fb.ageMs < kFallbackQuietMs);
  if (fallback) out.push_back(headFallback(fb));
  const std::vector<ParkedFact> held = heldProviders(f, fallback);
  if (!held.empty()) out.push_back(providerParked(held));

  // 3. Memory, 4. storage, 5. Telegram.
  if (f.embedParked || f.pendingEmbeds > 0) out.push_back(memory(f.embedParked, f.pendingEmbeds, f.embedKind, f.embedDetail));
  if (f.storeFull || f.flashFull) out.push_back(storeFull(f.storeFull));
  DegradedCondition tg;
  if (telegramSilent(f.telegram, tg)) out.push_back(tg);

  // 6. The clock gets the boot to sync before it counts.
  if (!f.clockSynced && f.uptimeMs >= kClockGraceMs) out.push_back(clockUnsynced(f.wifiUp));
  return out;
}

// ---- the served-by memory ---------------------------------------------------------------------------------

void ServingTracker::noteTurn(const std::string& requested, const std::string& served, const std::string& path,
                              bool answered, uint32_t nowMs) {
  if (!answered || requested.empty() || served.empty()) return;
  std::lock_guard<std::mutex> lk(mx_);
  if (served == requested) {
    active_ = false;
    return;
  }
  std::vector<std::string> down;
  for (const std::string& h : splitPath(path))
    if (h != served && std::find(down.begin(), down.end(), h) == down.end()) down.push_back(h);
  if (down.empty() || down.front() != requested) down.insert(down.begin(), requested);
  active_ = true;
  head_ = requested;
  served_ = served;
  down_ = std::move(down);
  atMs_ = nowMs;
}

FallbackFact ServingTracker::fact(uint32_t nowMs) const {
  std::lock_guard<std::mutex> lk(mx_);
  FallbackFact f;
  f.active = active_;
  if (!active_) return f;
  f.head = head_;
  f.served = served_;
  f.down = down_;
  f.ageMs = nowMs - atMs_;
  return f;
}

void ServingTracker::prime() { std::lock_guard<std::mutex> lk(mx_); }

void ServingTracker::clear() {
  std::lock_guard<std::mutex> lk(mx_);
  active_ = false;
  head_.clear();
  served_.clear();
  down_.clear();
}

// ---- the calm -----------------------------------------------------------------------------------------------

bool DegradedDebounce::update(const DegradedSet& now, uint32_t nowMs) {
  bool changed = false;
  for (int i = 0; i < kDegradedCount; i++) {
    const DegradedId id = DegradedId(i);
    const DegradedCondition* c = nullptr;
    for (const auto& x : now)
      if (x.id == id) { c = &x; break; }
    auto shownIt = std::find_if(shown_.begin(), shown_.end(), [id](const DegradedCondition& s) { return s.id == id; });
    Slot& s = slot_[i];
    if (c) {
      if (shownIt != shown_.end()) {   // shown and still holding: a gap in it is forgotten
        s.pending = false;
        if (shownIt->line != c->line) {
          *shownIt = *c;
          changed = true;
        } else {
          shownIt->why = c->why;   // the reason moves silently
        }
        continue;
      }
      if (!s.pending) { s.pending = true; s.sinceMs = nowMs; }
      if (nowMs - s.sinceMs >= kRaiseMs) {
        auto at = std::find_if(shown_.begin(), shown_.end(), [id](const DegradedCondition& x) { return x.id > id; });
        shown_.insert(at, *c);
        s.pending = false;
        changed = true;
      }
      continue;
    }
    if (shownIt == shown_.end()) {   // neither shown nor holding: a raise that never held is dropped
      s.pending = false;
      continue;
    }
    if (!s.pending) { s.pending = true; s.sinceMs = nowMs; }
    if (nowMs - s.sinceMs >= kClearMs) {
      shown_.erase(shownIt);
      s.pending = false;
      changed = true;
    }
  }
  if (changed) ++gen_;
  return changed;
}

void DegradedDebounce::reset() {
  for (Slot& s : slot_) s = Slot();
  shown_.clear();
  gen_ = 0;
}

// ---- the surfaces --------------------------------------------------------------------------------------------

std::vector<std::string> degradedStripRows(const DegradedSet& set, size_t maxRows) {
  std::vector<std::string> rows;
  if (set.empty() || maxRows == 0) return rows;
  if (set.size() <= maxRows) {
    for (const auto& c : set) rows.push_back(c.line);
    return rows;
  }
  for (size_t i = 0; i + 1 < maxRows; i++) rows.push_back(set[i].line);
  rows.push_back("+" + std::to_string(set.size() - (maxRows - 1)) + " more");
  return rows;
}

std::string degradedDetailText(const DegradedSet& set) {
  std::string out;
  for (const auto& c : set) {
    if (!out.empty()) out += "\n\n";
    out += "- " + c.line + "\n" + c.why;
  }
  return out;
}

void degradedInto(const DegradedSet& set, uint32_t generation, JsonObject out) {
  out["count"] = (uint32_t)set.size();
  out["gen"] = generation;
  JsonArray items = out["items"].to<JsonArray>();
  for (const auto& c : set) {
    JsonObject o = items.add<JsonObject>();
    o["id"] = degradedSlug(c.id);
    o["line"] = c.line;
    o["why"] = c.why;
  }
}

}  // namespace orch
}  // namespace nimbus
