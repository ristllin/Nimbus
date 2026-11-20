#pragma once
#include "nimbus/orch/tx/digest.h"
#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../scripted_provider/request_view.h"
#include "nimbus/orch/tx/item.h"

// tx_battery/props.h - the battery's properties as pure checkers (lane RB-B0b; PRD
// sections 2, 3, 7 and 11 L1). Each takes what was sent (a RequestView, the bytes) and
// what was stored (tx::Item) and answers true or false with the reason in `why`. The
// suites run them on real engine output; their counter-tests run them on hand-built good
// and bad inputs, so each property is shown to go red. A checker never reads a reply for
// words: it compares stored facts with sent bytes.
namespace txb {

namespace tx = nimbus::orch::tx;
namespace sp = harness_test::scripted;

// ---- prefix -----------------------------------------------------------------------
// The named change points (PRD section 2, "Prefix property"), logged as "change:<slug>"
// in the e item's events. Frozen: the battery counts them.
inline const std::vector<std::string>& changeSlugs() {
  static const std::vector<std::string> s = {"pointer",  "outside", "superseded", "photo",       "thinking",
                                             "compaction", "notes", "connector",  "recent-files", "ladder"};
  return s;
}
inline constexpr const char* kChangePrefix = "change:";

struct SentRequest {
  const sp::RequestView* view = nullptr;
  uint32_t turn = 0;   // the turn the request belongs to
};
using EventsByTurn = std::map<uint32_t, std::vector<std::string>>;

struct PrefixReport {
  size_t transitions = 0;
  size_t breaks = 0;
  size_t changePoints = 0;
};

namespace detail {
inline size_t changeCount(const EventsByTurn& ev, uint32_t turn, const char* only = nullptr) {
  auto it = ev.find(turn);
  if (it == ev.end()) return 0;
  size_t n = 0;
  for (const std::string& e : it->second)
    if (e.rfind(kChangePrefix, 0) == 0 && (!only || e == std::string(kChangePrefix) + only)) ++n;
  return n;
}
inline bool knownEvents(const EventsByTurn& ev, size_t& total, std::string& why) {
  total = 0;
  for (const auto& kv : ev)
    for (const std::string& e : kv.second) {
      if (e.rfind(kChangePrefix, 0) != 0) continue;
      const std::string slug = e.substr(std::char_traits<char>::length(kChangePrefix));
      const auto& known = changeSlugs();
      if (std::find(known.begin(), known.end(), slug) == known.end()) {
        why = "turn " + std::to_string(kv.first) + " logged an unknown change point '" + e + "'";
        return false;
      }
      ++total;
    }
  return true;
}
}  // namespace detail

namespace detail {
// Take one unspent change point of `turn` (only `only` when given); false when none is left.
inline bool spendPoint(const EventsByTurn& ev, std::map<uint32_t, size_t>& spent, uint32_t turn,
                       const char* only = nullptr) {
  if (changeCount(ev, turn, only) <= spent[turn]) return false;
  ++spent[turn];
  return true;
}
}  // namespace detail

// Request N+1 extends request N (system, tools, every earlier message) except at a named,
// counted change point: inside a turn only the ladder may break it; across a turn boundary
// the opening or the closing turn must have logged one. Each break spends one logged point,
// so breaks never outnumber the points and a point excuses one break only.
inline bool prefixHolds(const std::vector<SentRequest>& reqs, const EventsByTurn& events, PrefixReport& rep,
                        std::string& why) {
  rep = PrefixReport{};
  if (!detail::knownEvents(events, rep.changePoints, why)) return false;
  std::map<uint32_t, size_t> spent;
  for (size_t i = 1; i < reqs.size(); ++i) {
    ++rep.transitions;
    std::string diff;
    if (sp::isPrefix(*reqs[i - 1].view, *reqs[i].view, diff)) continue;
    ++rep.breaks;
    const uint32_t a = reqs[i - 1].turn, b = reqs[i].turn;
    const bool named = a == b ? detail::spendPoint(events, spent, b, "ladder")
                              : detail::spendPoint(events, spent, b) || detail::spendPoint(events, spent, a);
    if (!named) {
      why = "request " + std::to_string(i) + " (turn " + std::to_string(b) + ") does not extend request " +
            std::to_string(i - 1) + " (turn " + std::to_string(a) + "): " + diff +
            ", and no unspent change point was logged" + (a == b ? " (inside a turn only the ladder may)" : "");
      return false;
    }
  }
  return true;
}

// ---- pairing ------------------------------------------------------------------------
// In one request every call has exactly one result right after its round, and no result
// lacks its call (sp::callsPaired), and no id is called or answered twice.
inline bool viewPaired(const sp::RequestView& v, std::string& why) {
  if (!sp::callsPaired(v, why)) return false;
  std::map<std::string, int> calls, results;
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks) {
      if (b.kind == sp::BlockKind::ToolCall && ++calls[b.id] > 1) why = "call id " + b.id + " is sent twice";
      if (b.kind == sp::BlockKind::ToolResult && ++results[b.id] > 1) why = "call " + b.id + " has two results";
    }
  return why.empty();
}

namespace detail {
inline bool pairResult(const tx::Item& r, std::map<std::string, uint64_t>& callQ, std::map<std::string, int>& seen,
                       std::string& why) {
  auto c = callQ.find(r.id);
  if (c == callQ.end()) {
    why = "result r" + std::to_string(r.q) + " (" + r.n + ") answers no stored call " + r.id;
    return false;
  }
  if (++seen[r.id] > 1) {
    why = "call " + r.id + " has a second result r" + std::to_string(r.q);
    return false;
  }
  return true;
}
}  // namespace detail

// In the stored transcript (q order) every call id is unique and has exactly one result
// after it, every intent names a stored call, and no result answers nothing.
inline bool itemsPaired(const std::vector<tx::Item>& items, std::string& why) {
  std::map<std::string, uint64_t> callQ;
  std::map<std::string, int> seen;
  for (const tx::Item& it : items) {
    if (it.kind == tx::ItemKind::Call) {
      if (!callQ.emplace(it.id, it.q).second) {
        why = "call id " + it.id + " is stored twice";
        return false;
      }
    } else if (it.kind == tx::ItemKind::Intent && !callQ.count(it.id)) {
      why = "intent i" + std::to_string(it.q) + " names no stored call " + it.id;
      return false;
    } else if (it.kind == tx::ItemKind::Result && !detail::pairResult(it, callQ, seen, why)) {
      return false;
    }
  }
  for (const auto& kv : callQ)
    if (!seen.count(kv.first)) {
      why = "call " + kv.first + " (q" + std::to_string(kv.second) + ") has no stored result";
      return false;
    }
  return true;
}

// ---- coverage -----------------------------------------------------------------------
struct CoverInput {
  const std::vector<tx::Item>* items = nullptr;
  const std::map<std::string, std::string>* bulk = nullptr;   // handle -> payload
  // When `bulk` is null: read a payload back from storage by handle (the engine's TxLog).
  std::function<bool(const tx::Item& r, std::string& out)> readBulk;
  uint64_t pointer = 0;     // items at or below render bulk and outside results as stubs
  uint64_t thru = 0;        // the newest q a digest covers: older items are the digests' job
  uint32_t openTurn = 0;    // the turn the request belongs to (0 = none open)
  uint16_t dayStartMin = 240;   // the running day's start (TurnCtx::dayStartMin): the stamps' night clause
};

enum class Shown : uint8_t { Whole = 0, Stub = 1 };

// What a result must render as (PRD section 2, the stub rule): stub when superseded, when
// bulk or outside at or below the pointer, and when outside outside its own open turn;
// receipts stay verbatim (tier 1) until compacted.
inline Shown expectedShow(const tx::Item& r, const CoverInput& in, const std::set<uint64_t>& superseded) {
  if (superseded.count(r.q)) return Shown::Stub;
  if (r.cls == tx::ResultClass::Receipt) return Shown::Whole;
  if (r.q <= in.pointer) return Shown::Stub;
  if (r.cls == tx::ResultClass::Outside && r.turn != in.openTurn) return Shown::Stub;
  return Shown::Whole;
}

namespace detail {
inline std::set<uint64_t> supersededSet(const std::vector<tx::Item>& items) {
  std::set<uint64_t> s;
  for (const tx::Item& it : items)
    if (it.kind == tx::ItemKind::PointerMove) s.insert(it.sup.begin(), it.sup.end());
  return s;
}
inline std::vector<const sp::Block*> blocksOf(const sp::RequestView& v, sp::BlockKind k, const std::string& id) {
  std::vector<const sp::Block*> out;
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks)
      if (b.kind == k && b.id == id) out.push_back(&b);
  return out;
}
inline bool textIn(const sp::RequestView& v, const std::string& role, const std::string& text) {
  for (const sp::Message& m : v.messages)
    if (role.empty() || m.role == role)
      for (const sp::Block& b : m.blocks)
        if (b.kind == sp::BlockKind::Text && b.text.find(text) != std::string::npos) return true;
  return false;
}
inline size_t countImages(const sp::RequestView& v) {
  size_t n = 0;
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks) n += b.kind == sp::BlockKind::Image;
  return n;
}
inline std::string resultWhole(const tx::Item& r, const CoverInput& in) {
  if (r.handle.empty()) return r.x;
  std::string out;
  if (!in.bulk) return in.readBulk && in.readBulk(r, out) ? out : "<bulk " + r.handle + " not readable>";
  auto it = in.bulk->find(r.handle);
  return it == in.bulk->end() ? "<bulk " + r.handle + " not stored>" : it->second;
}
// "[N earlier turns, <dates>, not shown: search finds them]": N, or -1 when absent.
inline long omittedTurnsNamed(const sp::RequestView& v) {
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks) {
      const size_t at = b.text.find(" earlier turns, ");
      if (b.kind != sp::BlockKind::Text || at == std::string::npos || b.text.find("search finds them]") == std::string::npos)
        continue;
      size_t s = b.text.rfind('[', at);
      if (s != std::string::npos) return std::strtol(b.text.c_str() + s + 1, nullptr, 10);
    }
  return -1;
}
}  // namespace detail

namespace detail {
inline bool coverResult(const sp::RequestView& v, const tx::Item& r, const CoverInput& in, const std::set<uint64_t>& sup,
                        std::string& why) {
  auto blocks = blocksOf(v, sp::BlockKind::ToolResult, r.id);
  const std::string tag = "r" + std::to_string(r.q) + " (" + r.n + ")";
  if (blocks.size() != 1) {
    why = "result " + tag + " is rendered " + std::to_string(blocks.size()) + " times";
    return false;
  }
  const bool stub = expectedShow(r, in, sup) == Shown::Stub;
  const std::string want = stub ? r.st : resultWhole(r, in);
  if (blocks[0]->text != want) {
    why = "result " + tag + " should render " + (stub ? "as its stored stub" : "whole") + ", got " +
          std::to_string(blocks[0]->text.size()) + " B starting '" + blocks[0]->text.substr(0, 60) + "'";
    return false;
  }
  if (stub && want.find("view id=r" + std::to_string(r.q)) == std::string::npos) {
    why = "stub of " + tag + " has no view hint";
    return false;
  }
  return true;
}
inline bool coverItem(const sp::RequestView& v, const tx::Item& it, const CoverInput& in,
                      const std::set<uint64_t>& sup, std::string& why) {
  const std::string tag = std::string(1, tx::kindLetter(it.kind)) + std::to_string(it.q);
  switch (it.kind) {
    case tx::ItemKind::User:
      if (!it.x.empty() && !textIn(v, "", it.x)) why = "owner message " + tag + " is missing";
      break;
    case tx::ItemKind::Assistant:
      if (!it.x.empty() && !textIn(v, "assistant", it.x)) why = "assistant text " + tag + " is missing";
      break;
    case tx::ItemKind::DeviceNote:
      if (!textIn(v, "", it.x)) why = "device note " + tag + " is missing";
      break;
    case tx::ItemKind::Call: {
      auto calls = blocksOf(v, sp::BlockKind::ToolCall, it.id);
      if (calls.size() != 1 || calls[0]->name != it.n) why = "call " + tag + " (" + it.n + ") is not rendered once";
      break;
    }
    case tx::ItemKind::Result:
      return coverResult(v, it, in, sup, why);
    default:
      break;   // t, i, k, s, x, d, e are not message content
  }
  return why.empty();
}
}  // namespace detail

namespace detail {
// Turns after thru whose owner message is not rendered: the ones a ladder omitted.
inline std::vector<uint32_t> missingTurns(const sp::RequestView& v, const CoverInput& in, std::vector<uint32_t>& owners) {
  std::vector<uint32_t> missing;
  for (const tx::Item& it : *in.items)
    if (it.kind == tx::ItemKind::User && it.q > in.thru) {
      owners.push_back(it.turn);
      if (!it.x.empty() && !textIn(v, "", it.x)) missing.push_back(it.turn);
    }
  return missing;
}
inline bool omissionNamed(const sp::RequestView& v, const std::vector<uint32_t>& missing,
                          const std::vector<uint32_t>& owners, std::string& why) {
  if (missing.empty()) return true;
  const long named = omittedTurnsNamed(v);
  if (named != (long)missing.size()) {
    why = std::to_string(missing.size()) + " turns after thru are not rendered (first turn " +
          std::to_string(missing.front()) + ") and the omission line names " + std::to_string(named);
    return false;
  }
  for (size_t i = 0; i < missing.size(); ++i)
    if (missing[i] != owners[i]) {
      why = "omitted turns are not the oldest after thru (turn " + std::to_string(missing[i]) + ")";
      return false;
    }
  if (owners.size() - missing.size() < 6 && owners.size() >= 6) {
    why = "the omission reached into the newest 6 owner turns";
    return false;
  }
  return true;
}
}  // namespace detail

// Every stored item after thru is in the request: whole, as its stored stub (with its view
// hint), or inside a named omission of the oldest whole turns (never the newest 6 owner
// turns). Every photo is shown or named. Nothing is dropped silently.
inline bool covers(const sp::RequestView& v, const CoverInput& in, std::string& why) {
  if (!v.parsed) {
    why = "the request did not parse";
    return false;
  }
  std::vector<uint32_t> owners;
  const std::vector<uint32_t> missing = detail::missingTurns(v, in, owners);
  if (!detail::omissionNamed(v, missing, owners, why)) return false;
  const std::set<uint64_t> sup = detail::supersededSet(*in.items);
  size_t photos = 0, named = 0;
  for (const tx::Item& it : *in.items) {
    if (it.q <= in.thru || std::find(missing.begin(), missing.end(), it.turn) != missing.end()) continue;
    if (!detail::coverItem(v, it, in, sup, why)) return false;
    for (const tx::Part& p : it.parts)
      if (p.kind == tx::PartKind::Image) {
        ++photos;
        named += detail::textIn(v, "", "[photo " + p.ref);
      }
  }
  if (detail::countImages(v) + named < photos) {
    why = std::to_string(photos) + " photos stored after thru, " + std::to_string(detail::countImages(v)) +
          " shown and " + std::to_string(named) + " named";
    return false;
  }
  return true;
}

// After compaction: every digest still in the block is in the system text, and every item
// after the newest digest's thru is covered as above. thru comes from the s items, not
// from what the engine says.
// Since lane RB-C3 the newest cumulative summary covers everything before it: it alone must be in the
// system text (as the block shows it, without its last line end); older summaries and day digests are
// superseded and archived.
inline bool compactedCovers(const sp::RequestView& v, CoverInput in, std::string& why) {
  uint64_t thru = 0, summaryQ = 0;
  for (const tx::Item& it : *in.items)
    if (it.kind == tx::ItemKind::Digest && tx::isSummaryText(it.x)) summaryQ = it.q;
  for (const tx::Item& it : *in.items)
    if (it.kind == tx::ItemKind::Digest) {
      thru = std::max(thru, it.thru);
      if (it.q < summaryQ) continue;
      const std::string shown = tx::isSummaryText(it.x) ? it.x.substr(0, it.x.size() - 1) : it.x;
      if (!it.ext && v.system().find(shown) == std::string::npos) {
        why = "digest s" + std::to_string(it.q) + " (" + it.day + ") is not in the system text";
        return false;
      }
    }
  in.thru = thru;
  return covers(v, in, why);
}

// ---- overhead -----------------------------------------------------------------------
// The stamp an owner message renders with (RB-REC5's tx::messageStamp, exactly): the plain form
// '[2026-11-17 Tue 13:02]', the night form '[2026-11-18 Wed 00:30 (day 2026-11-17 until 04:00)]'
// before the day start, '[time unknown]' with no clock. "" when the stamp cannot be made.
inline std::string stampOf(const tx::Item& u, uint16_t dayStartMin) {
  char buf[tx::kStampMaxBytes];
  return tx::messageStamp(u.t, u.tzMinutes, dayStartMin, buf, sizeof buf).ok() ? std::string(buf) : std::string();
}

// What a request may carry beyond the stored text it shows (PRD section 2, test_tx_overhead:
// 96 B per owner message with the plain stamp, that is 74 B of framing plus the message's own
// stamp in whichever of RB-REC5's three forms it takes, and 64 B per photo label). The stored text is counted as the JSON
// string bytes of each rendered text (owner and assistant text, device notes, call ids,
// names and arguments, the result text shown, open-turn meta), so a line format's own keys
// never hide anything. Wire framing of a tool call, a result and an extra text block has its
// own fixed allowance, and the envelope (model, max_tokens, thinking, tool_choice, cache
// marks, the system and tools arrays) is 512 B once.
inline constexpr size_t kOverheadPerOwnerMessage = 74;   // beside the stamp's own bytes (stampBytes)
inline constexpr size_t kOverheadPerPhotoLabel = 64;
inline constexpr size_t kOverheadPerCall = 96;
inline constexpr size_t kOverheadPerResult = 80;
inline constexpr size_t kOverheadPerTextBlock = 48;   // an assistant text, a device note, open-turn meta
inline constexpr size_t kOverheadEnvelope = 512;

struct OverheadInput {
  size_t bodyBytes = 0;
  size_t itemBytes = 0;     // JSON string bytes of the rendered texts, + inline photo bytes
  size_t systemBytes = 0;
  size_t toolsBytes = 0;
  size_t ownerMessages = 0;
  size_t stampBytes = 0;    // the owner messages' stamps, each exactly as stampOf makes it
  size_t photoLabels = 0;
  size_t calls = 0;
  size_t results = 0;
  size_t textBlocks = 0;
};

inline long overheadBound(const OverheadInput& in) {
  return (long)(kOverheadPerOwnerMessage * in.ownerMessages + in.stampBytes + kOverheadPerPhotoLabel * in.photoLabels +
                kOverheadPerCall * in.calls + kOverheadPerResult * in.results + kOverheadPerTextBlock * in.textBlocks +
                kOverheadEnvelope);
}

// What the renderer adds around the stored items stays within the stated framing: text
// added around messages cannot pass unseen (PRD section 11, rule 3 against prompt-patching).
inline bool overheadWithin(const OverheadInput& in, long& overhead, std::string& why) {
  overhead = (long)in.bodyBytes - (long)in.itemBytes - (long)in.systemBytes - (long)in.toolsBytes;
  const long bound = overheadBound(in);
  if (overhead > bound) {
    why = "rendered " + std::to_string(overhead) + " B around " + std::to_string(in.ownerMessages) +
          " owner messages, " + std::to_string(in.calls) + " calls, " + std::to_string(in.results) + " results and " +
          std::to_string(in.photoLabels) + " photos; the bound is " + std::to_string(bound) + " B";
    return false;
  }
  return true;
}

// Every text block of every message is stored text: once the stored texts it contains are
// taken out (owner and assistant text, each owner text with its own stamp exactly as stampOf
// makes it, device notes, open-turn meta, photo stubs, the omission line), nothing visible is
// left. A sentence, a reminder or a memory block written around messages is caught however
// much framing slack the byte bound has; so is a stamp in any other form.

namespace detail {
inline size_t visibleBytes(const std::string& s) {
  size_t n = 0;
  for (char c : s) n += !std::isspace((unsigned char)c);
  return n;
}
inline bool takeOut(std::string& from, const std::string& text) {
  if (text.empty()) return false;
  const size_t at = from.find(text);
  if (at == std::string::npos) return false;
  from.erase(at, text.size());
  return true;
}
inline void takeOutPattern(std::string& from, const char* open, const char* close) {
  size_t at;
  while ((at = from.find(open)) != std::string::npos) {
    const size_t end = from.find(close, at);
    if (end == std::string::npos) return;
    from.erase(at, end - at + std::char_traits<char>::length(close));
  }
}
// '[N earlier turns, <dates>, not shown: search finds them]'
inline void takeOutOmission(std::string& from) {
  const size_t at = from.find(" earlier turns, ");
  if (at == std::string::npos) return;
  const size_t open = from.rfind('[', at);
  const char* close = "search finds them]";
  const size_t end = from.find(close, at);
  if (open != std::string::npos && end != std::string::npos)
    from.erase(open, end - open + std::char_traits<char>::length(close));
}
}  // namespace detail

inline bool noUnstoredText(const sp::RequestView& v, const std::vector<tx::Item>& items, std::string& why,
                           uint16_t dayStartMin = 240) {
  std::vector<std::pair<std::string, std::string>> owner;   // text, its stamp
  std::vector<std::string> other;
  for (const tx::Item& it : items) {
    if (it.kind == tx::ItemKind::User) owner.emplace_back(it.x, stampOf(it, dayStartMin));
    if (it.kind == tx::ItemKind::Assistant || it.kind == tx::ItemKind::DeviceNote || it.kind == tx::ItemKind::RoundMeta)
      other.push_back(it.x);
  }
  std::sort(owner.begin(), owner.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  std::sort(other.begin(), other.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks) {
      if (b.kind != sp::BlockKind::Text) continue;
      std::string rest = b.text;
      for (const auto& t : owner)
        if (detail::takeOut(rest, t.first)) detail::takeOut(rest, t.second);
      for (const std::string& t : other) detail::takeOut(rest, t);
      detail::takeOutPattern(rest, "[photo p", "]");
      detail::takeOutOmission(rest);
      if (detail::visibleBytes(rest) > 0) {
        why = "a " + m.role + " text block carries " + std::to_string(detail::visibleBytes(rest)) +
              " B that no stored item holds: '" + rest.substr(0, 80) + "'";
        return false;
      }
    }
  return true;
}

// The bytes a string takes inside a JSON body (quotes and escapes included).
inline size_t jsonStringBytes(const std::string& s) {
  JsonDocument d;
  d.set(s);
  return measureJson(d);
}

inline size_t systemJsonBytes(const sp::RequestView& v) {
  size_t n = 0;
  for (const std::string& s : v.systemBlocks) n += jsonStringBytes(s);
  return n;
}
inline size_t toolsJsonBytes(const sp::RequestView& v) {
  size_t n = 0;
  for (const std::string& t : v.toolsJson) n += t.size();
  return n;
}

// ---- delivery -----------------------------------------------------------------------
struct SentMessage {
  std::string chat;
  std::string text;
};

// The channel received exactly the deliver function's chunks of the model's text, in
// order, and then at most one harness-written message: allowed only when the turn ended
// with a status line from the closed list (`statusAllowed`), sent as its own message (equal
// to `statusText` when the caller knows it). Nothing appended, nothing rewritten.
inline bool deliveredExactly(const std::vector<std::string>& expected, bool statusAllowed, const std::string& statusText,
                             const std::vector<SentMessage>& sent, std::string& why) {
  for (size_t i = 0; i < expected.size(); ++i) {
    if (i >= sent.size()) {
      why = "chunk " + std::to_string(i) + " of the reply was never sent";
      return false;
    }
    if (sent[i].text != expected[i]) {
      why = "chunk " + std::to_string(i) + " differs from the model's text: sent '" + sent[i].text.substr(0, 80) + "'";
      return false;
    }
  }
  const size_t extra = sent.size() - std::min(sent.size(), expected.size());
  if (extra > 1) {
    why = std::to_string(extra) + " harness-written messages after one turn (at most one)";
    return false;
  }
  if (extra == 1 && (!statusAllowed || (!statusText.empty() && sent.back().text != statusText))) {
    why = "an extra message that is not the turn's status line: '" + sent.back().text.substr(0, 80) + "'";
    return false;
  }
  return true;
}

// ---- loop shape (conformance) -------------------------------------------------------
struct StepShape {
  size_t calls = 0;
  std::string text;
};

namespace detail {
inline size_t countKind(const sp::RequestView& v, sp::BlockKind k) {
  size_t n = 0;
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks) n += b.kind == k;
  return n;
}
}  // namespace detail

// The plain loop: a step with no call ends the turn, so requests = rounds with calls + 1;
// each request carries the previous request's calls and results plus the previous step's
// new ones; text that came with a call is kept and the loop continues; the delivered text
// is the last step's text.
inline bool loopShapeHolds(const std::vector<StepShape>& steps, const std::vector<const sp::RequestView*>& reqs,
                           const std::string& delivered, std::string& why) {
  if (steps.empty() || steps.back().calls != 0 || reqs.size() != steps.size()) {
    why = std::to_string(reqs.size()) + " requests for " + std::to_string(steps.size()) +
          " steps (a step without a call must end the turn, one request per step)";
    return false;
  }
  for (size_t i = 0; i + 1 < steps.size(); ++i)
    if (steps[i].calls == 0) {
      why = "step " + std::to_string(i) + " had no call but the loop went on";
      return false;
    }
  for (size_t i = 1; i < reqs.size(); ++i) {
    const size_t wantCalls = detail::countKind(*reqs[i - 1], sp::BlockKind::ToolCall) + steps[i - 1].calls;
    const size_t wantResults = detail::countKind(*reqs[i - 1], sp::BlockKind::ToolResult) + steps[i - 1].calls;
    const size_t gotCalls = detail::countKind(*reqs[i], sp::BlockKind::ToolCall);
    const size_t gotResults = detail::countKind(*reqs[i], sp::BlockKind::ToolResult);
    if (gotCalls != wantCalls || gotResults != wantResults) {
      why = "request " + std::to_string(i) + " carries " + std::to_string(gotCalls) + " calls and " +
            std::to_string(gotResults) + " results; the round before it leaves " + std::to_string(wantCalls) +
            " and " + std::to_string(wantResults);
      return false;
    }
    if (!steps[i - 1].text.empty() && !detail::textIn(*reqs[i], "assistant", steps[i - 1].text)) {
      why = "the text of step " + std::to_string(i - 1) + " (said with a call) is not kept in request " + std::to_string(i);
      return false;
    }
  }
  if (delivered != steps.back().text) {
    why = "delivered '" + delivered.substr(0, 60) + "' is not the last step's text";
    return false;
  }
  return true;
}

// ---- crash safety -------------------------------------------------------------------
struct CrashOutcome {
  size_t replies = 0;                     // messages carrying the reply to the owner message
  std::map<std::string, int> effectRuns;  // effect key (tool + args) -> times it ran
  bool killInsideSend = false;            // the cut fell inside the send: twice is named
  bool crashNotice = false;               // the plain crash notice was sent instead
};

// At-least-once reply, at-most-once effects (PRD section 7).
inline bool crashSafe(const CrashOutcome& o, std::string& why) {
  for (const auto& kv : o.effectRuns)
    if (kv.second > 1) {
      why = "effect " + kv.first + " ran " + std::to_string(kv.second) + " times";
      return false;
    }
  if (o.replies == 0 && !o.crashNotice) {
    why = "the owner message got no reply and no crash notice";
    return false;
  }
  const size_t most = o.killInsideSend ? 2 : 1;
  if (o.replies > most) {
    why = "the reply was sent " + std::to_string(o.replies) + " times" +
          (o.killInsideSend ? "" : " (only a cut inside the send may deliver twice)");
    return false;
  }
  return true;
}

}  // namespace txb
