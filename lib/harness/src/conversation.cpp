#include "nimbus/harness/conversation.h"

#include <algorithm>
#include <cstdio>

#include "nimbus/harness/dream.h"   // buildEpisodicDigest - the window's line renderer
#include "nimbus/harness/jobs.h"    // isJobDelivery - captured job rows are not conversation
#include "nimbus/mem_cap.h"         // utf8CapLen - every clip is UTF-8 safe
#include "nimbus/orch/blob_store.h" // blobHashOf - a media row's blob ref
#include "nimbus/orch/caps.h"       // kRecentTurnsMax
#include "nimbus/orch/loops.h"      // civilFromDays - day markers

namespace agent {

using nimbus::orch::EpisodicMessage;
using nimbus::orch::MsgKind;

const char kClearBoundaryTag[] = "ev:clear";
const char kClearBoundaryText[] = "Conversation cleared.";

bool isClearBoundary(const nimbus::orch::EpisodicMessage& m) {
  // Tags are a comma list: match the whole item, never a prefix of another tag.
  // (A sender cannot forge it - user rows carry only a sanitized "from:" tag.)
  return ("," + m.tags + ",").find(std::string(",") + kClearBoundaryTag + ",") !=
         std::string::npos;
}

std::vector<nimbus::orch::EpisodicMessage> liveConversation(
    const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
    const std::string& inFlightId, size_t maxRows) {
  std::vector<nimbus::orch::EpisodicMessage> out;
  for (const auto& m : newestFirst) {
    if (isClearBoundary(m)) break;   // everything older was cleared
    if (!inFlightId.empty() && m.id == inFlightId) continue;
    if (isJobDelivery(m.tags)) continue;   // job output renders in the chat, never here
    if (maxRows && out.size() >= maxRows) break;
    out.push_back(m);
  }
  return out;
}

std::string recentConversationBlock(const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
                                    const std::string& inFlightId, size_t capBytes) {
  const auto rows = liveConversation(newestFirst, inFlightId, (size_t)nimbus::orch::kRecentTurnsMax);
  if (rows.empty()) return std::string();
  // deriveBudget() already floors the caller's budget (budget.h); this guard only
  // keeps a zero or tiny cap from emptying the window (the pre-CUM-485 size).
  constexpr size_t kLegacyRecentConvBytes = 3000;
  if (capBytes < kLegacyRecentConvBytes) capBytes = kLegacyRecentConvBytes;
  std::string block =
      "\n## RECENT CONVERSATION (this chat, oldest first; the current message "
      "is NOT here - it is in [USER])\n";
  // CUM-485: newest rows ride whole (buildConversationWindow), not the digest's
  // per-row preview - on a stateless tool-loop turn this window is the only
  // place the model sees its previous reply.
  block += dream::buildConversationWindow(rows, capBytes);
  return block;
}

// ---- the turn-structured history (histV2) -----------------------------------

const char kAttTag[] = "att:";
const char kSrcTag[] = "src:";
const char kSentTag[] = "sent:";
const char kOutTag[] = "out:";
const char kSkillsTag[] = "skills:";
const char kTriggerTag[] = "ev:trigger";
const char kHistoryHeader[] =
    "\n## RECENT CONVERSATION (this chat, oldest first, one exchange per turn; the current "
    "message is NOT here - it is in [USER]. (xN) = the same message arrived N times in a row; "
    "[did: ...] = what that reply actually changed or looked up)\n";

namespace {

std::vector<std::string> splitTags(const std::string& tags) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= tags.size()) {
    const size_t c = tags.find(',', p);
    const size_t e = c == std::string::npos ? tags.size() : c;
    std::string item = tags.substr(p, e - p);
    while (!item.empty() && item.front() == ' ') item.erase(0, 1);
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!item.empty()) out.push_back(std::move(item));
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return out;
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool isMediaRow(const EpisodicMessage& m) {
  return m.kind == MsgKind::Image || m.kind == MsgKind::File || m.kind == MsgKind::Audio;
}

bool isOwnerRow(const EpisodicMessage& m) { return m.kind == MsgKind::Message && m.role == "user"; }

uint32_t toU32(const std::string& s) {
  uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') break;
    v = v * 10u + (uint32_t)(c - '0');
  }
  return v;
}

// ---- line breaks: a message must not be able to open a row or a header -------
size_t breakAt(const std::string& t, size_t i) {
  const auto at = [&](size_t k) -> unsigned char { return k < t.size() ? (unsigned char)t[k] : 0; };
  const unsigned char c = at(i);
  if (c == '\r') return at(i + 1) == '\n' ? 2 : 1;
  if (c == '\n' || c == '\v' || c == '\f' || (c >= 0x1C && c <= 0x1E)) return 1;
  if (c == 0xC2 && at(i + 1) == 0x85) return 2;                                      // NEL
  if (c == 0xE2 && at(i + 1) == 0x80 && (at(i + 2) == 0xA8 || at(i + 2) == 0xA9)) return 3;   // LS, PS
  return 0;
}

// Every break becomes LF plus a two-space indent, so a continuation line can
// never start a "- user:" row or a "## " header (the window's posture since
// the first release; the same break set as dream.cpp's window rows).
std::string indentBreaks(const std::string& t) {
  std::string out;
  out.reserve(t.size() + 16);
  for (size_t i = 0; i < t.size();) {
    const size_t br = breakAt(t, i);
    if (br) {
      i += br;
      out += '\n';
      if (i < t.size() && !breakAt(t, i)) out += "  ";   // a blank line stays blank
    } else {
      out += t[i++];
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
  return out;
}

std::string clipMessage(const std::string& s) {
  if (s.size() <= kHistoryMessageCap) return s;
  static const char kCut[] = " ...(cut)";
  const int keep = nimbus::utf8CapLen(s.c_str(), (int)s.size(), (int)(kHistoryMessageCap - sizeof(kCut)));
  return s.substr(0, (size_t)keep) + kCut;
}

// Critique B2 (G1): an image row's machine description only has to identify
// and summarize the photo - its ref names it and image.look reads it again -
// so it is capped to about 300 chars past the label. The clip lands on the
// DESCRIPTION only (at the insertion point, userHalf below), never on the
// owner's own text after it, and never splits a UTF-8 sequence.
std::string clipImageRow(const std::string& s) {
  if (s.rfind("[image", 0) != 0) return s;   // only a photo row's description
  const size_t close = s.find(']');
  const size_t label = close == std::string::npos ? 0 : close + 1;
  if (s.size() <= label + nimbus::orch::kImageRowMaxChars) return s;
  static const char kCut[] = " ...(cut)";
  const int keep = nimbus::utf8CapLen(s.c_str(), (int)s.size(),
                                      (int)(label + nimbus::orch::kImageRowMaxChars - sizeof(kCut)));
  return s.substr(0, (size_t)keep) + kCut;
}

// ---- time ---------------------------------------------------------------------
using nimbus::orch::kClockValidHours;   // under it a row's hour is boot-relative: no date

int64_t localDay(uint32_t tsHours, int32_t offMin) {
  const int64_t mins = (int64_t)tsHours * 60 + offMin;
  return mins >= 0 ? mins / 1440 : (mins - 1439) / 1440;
}

// The local day of a unix time in seconds (the send-time twin of localDay).
int64_t localDaySec(uint32_t unix, int32_t offMin) {
  return ((int64_t)unix + (int64_t)offMin * 60) / 86400;
}

// Two identical owner messages are one repeated message (xN) only when they were
// sent close together on the same day: with both send times known, at most
// kRepeatWindowSec apart and on the same local day; without them, in the same hour
// (a row without a send time keeps only its hour). A habitual "dinner 600" sent
// yesterday and again today stays two messages under their own days.
bool closeRepeat(uint32_t sentA, uint32_t sentB, uint32_t hourA, uint32_t hourB, int32_t offMin) {
  if (sentA && sentB) {
    const uint32_t gap = sentA > sentB ? sentA - sentB : sentB - sentA;
    return gap <= kRepeatWindowSec && localDaySec(sentA, offMin) == localDaySec(sentB, offMin);
  }
  return hourA == hourB;
}

std::string dayLabel(uint32_t tsHours, int32_t offMin) {
  const nimbus::orch::CivilTime c = nimbus::orch::civilFromDays((uint32_t)localDay(tsHours, offMin));
  char buf[24];
  snprintf(buf, sizeof(buf), "(%s %d %s)", nimbus::orch::wdayAbbr(c.wday), c.day, nimbus::orch::monthAbbr(c.month));
  return buf;
}

std::string clockLabel(uint32_t unixSec, int32_t offMin) {
  const int64_t local = (int64_t)unixSec + (int64_t)offMin * 60;
  const int64_t m = ((local / 60) % 1440 + 1440) % 1440;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", (int)(m / 60), (int)(m % 60));
  return buf;
}

// ---- fx: -> "[did: ...]" --------------------------------------------------------
// The fx tag lists its kinds in the same comma list as every other tag
// ("fx:mem,file,turn:m01"), so the kinds are the fx: item plus the items after
// it that carry no key.
void addDid(std::vector<std::string>& did, const std::string& tags) {
  static const struct { const char* tag; const char* word; } kWords[] = {
      {"mem", "memory"},  {"scratch", "scratchpad"}, {"file", "file"},    {"skill", "skill"},
      {"spawn", "sub-agents"}, {"send", "sent"},     {"speak", "spoke"},  {"sched", "schedule"},
      {"cfg", "settings"}, {"read", "lookup"},       {"none", "nothing"},
  };
  const std::vector<std::string> items = splitTags(tags);
  for (size_t i = 0; i < items.size(); i++) {
    if (!startsWith(items[i], "fx:")) continue;
    for (size_t k = i; k < items.size(); k++) {
      const std::string kind = k == i ? items[k].substr(3) : items[k];
      if (k > i && kind.find(':') != std::string::npos) break;
      std::string word = kind;
      for (const auto& w : kWords)
        if (kind == w.tag) word = w.word;
      bool seen = false;
      for (const auto& d : did) seen = seen || d == word;
      if (!seen && !word.empty()) did.push_back(word);
    }
    return;
  }
}

std::string didText(std::vector<std::string> did) {
  if (did.size() > 1)
    for (size_t i = 0; i < did.size(); i++)
      if (did[i] == "nothing") { did.erase(did.begin() + (long)i); break; }
  std::string out;
  for (const auto& d : did) out += (out.empty() ? "" : ", ") + d;
  return out;
}

// ---- grouping -------------------------------------------------------------------
struct Building {
  const EpisodicMessage* userRow = nullptr;   // user row or trigger row
  std::vector<const EpisodicMessage*> media;  // records to put back in place
  std::vector<const EpisodicMessage*> asst;
  std::string key;                            // what an assistant row's turn: tag names
  std::string firstId, lastId;
  size_t rows = 0;
  uint32_t repeats = 1;
  bool seedless = false;
  bool voice = false;

  void note(const EpisodicMessage& m) {
    if (firstId.empty() || rowIdAfter(firstId, m.id)) firstId = m.id;
    if (lastId.empty() || rowIdAfter(m.id, lastId)) lastId = m.id;
    rows++;
  }
};

// The leading "[...]" label of a media record ("[image 1/3 ref=...]"), "" if none.
std::string bracketLabel(const std::string& text) {
  if (text.empty() || text[0] != '[') return std::string();
  const size_t close = text.find(']');
  return close == std::string::npos || close > 120 ? std::string() : text.substr(0, close + 1);
}

// The user half: the user row's text with each attached record put back where
// its bare label sits (or after the text when no label marks a place).
std::string userHalf(const Building& b) {
  if (!b.userRow) {
    if (b.media.empty()) return "[no owner message - an automatic turn]";
    const EpisodicMessage& m = *b.media.front();
    if (!m.text.empty()) return clipImageRow(m.text);
    return m.kind == MsgKind::Audio ? "[voice note]" : std::string("[") + nimbus::orch::kindName(m.kind) + "]";
  }
  std::string text = b.userRow->text;
  size_t from = 0;
  for (const EpisodicMessage* m : b.media) {
    if (m->text.empty()) continue;
    const std::string media = clipImageRow(m->text);
    const size_t whole = text.find(m->text, from);
    if (whole != std::string::npos) {   // already in place (a file's name, say)
      from = whole + m->text.size();
      continue;
    }
    const std::string label = bracketLabel(media);
    const size_t at = label.empty() || label == media ? std::string::npos : text.find(label, from);
    if (at != std::string::npos) {
      text.replace(at, label.size(), media);
      from = at + media.size();
    } else {
      text += (text.empty() ? "" : "\n") + media;
      from = text.size();
    }
  }
  if (b.voice) text = "[voice note] " + text;
  return text;
}

// F1 (lane I): the skills this turn used or had attached, from its assistant
// rows' skills: tag - in order, deduped. The ids join with '.' inside the tag
// (a comma would read as the next tag item).
std::vector<std::string> turnSkills(const Building& b) {
  std::vector<std::string> ids;
  for (const EpisodicMessage* m : b.asst)
    for (const auto& id : splitRefs(tagValue(m->tags, kSkillsTag)))
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  return ids;
}

// The joined assistant text: the replies, the questions, a spoken line only
// when nothing was written, then the skills suffix (F1, lane I).
std::string joinAssistantText(const std::string& replies, const std::string& asks,
                              const std::string& spoken, const std::vector<std::string>& ids) {
  std::string text = replies;
  if (!asks.empty()) text += (text.empty() ? "" : "\n\n") + asks;
  if (text.empty()) text = spoken;   // a spoken line counts only when nothing was written
  if (ids.empty() || text.empty()) return text;
  std::string joined;
  for (const auto& id : ids) joined += (joined.empty() ? "" : ", ") + id;
  return text += " (skills: " + joined + ")";
}

std::string assistantHalf(const Building& b, std::string& did, uint32_t& replyHours) {
  std::string replies, asks, spoken;
  std::vector<std::string> kinds;
  bool tagged = false;
  for (const EpisodicMessage* m : b.asst) {
    const std::string out = tagValue(m->tags, kOutTag);
    std::string& into = out == "ask" ? asks : out == "tts" ? spoken : replies;
    if (!m->text.empty()) into += (into.empty() ? "" : "\n\n") + m->text;
    if (!tagValue(m->tags, "fx:").empty()) tagged = true;
    addDid(kinds, m->tags);
    if (m->tsHours > replyHours) replyHours = m->tsHours;
  }
  did = tagged ? didText(kinds) : std::string();
  return joinAssistantText(replies, asks, spoken, turnSkills(b));
}

HistTurn finish(const Building& b) {
  HistTurn t;
  t.firstId = b.firstId;
  t.lastId = b.lastId;
  t.seedless = b.seedless;
  t.repeats = b.repeats;
  t.rows = b.rows;
  t.user = clipMessage(userHalf(b));
  t.assistant = clipMessage(assistantHalf(b, t.did, t.replyHours));
  spillToPsram(t.user);
  spillToPsram(t.assistant);
  spillToPsram(t.did);
  const EpisodicMessage* head = b.userRow ? b.userRow : (b.media.empty() ? nullptr : b.media.front());
  if (!head && !b.asst.empty()) head = b.asst.front();
  if (head) t.tsHours = head->tsHours;
  if (b.userRow) t.sentUnix = toU32(tagValue(b.userRow->tags, kSentTag));
  return t;
}

// One live row in oldest-first order with the reader's repeat count.
struct Live {
  const EpisodicMessage* m;
  uint32_t rep;
};

// The in-flight turn's own media: the rows captured for it just before its user
// row, named by its att: refs (or, on older rows, repeated at the head of its text).
struct InFlight {
  std::vector<std::string> refs;
  std::string text;
  bool mediaRun = false;   // still inside the media rows right below the user row

  bool owns(const EpisodicMessage& m) const {
    if (!mediaRun || !isMediaRow(m)) return false;
    if (refs.empty()) return !m.text.empty() && startsWith(text, m.text);
    const std::string ref = nimbus::orch::blobHashOf(m.blobPath);
    for (const auto& r : refs)
      if (!ref.empty() && r == ref) return true;
    return false;
  }
};

// Newest-first rows -> the live rows, oldest first: stop at the newest /clear and
// at startAfter, drop job deliveries, and drop the in-flight turn (its user row
// and the media rows captured for it just before).
std::vector<Live> liveRows(const std::vector<EpisodicMessage>& newestFirst, const HistoryOptions& opt,
                           const std::vector<uint32_t>* repeats) {
  std::vector<Live> live;
  InFlight cur;
  for (size_t i = 0; i < newestFirst.size(); i++) {
    const EpisodicMessage& m = newestFirst[i];
    if (isClearBoundary(m)) break;
    if (!opt.startAfter.empty() && !rowIdAfter(m.id, opt.startAfter)) break;
    if (isJobDelivery(m.tags)) continue;
    if (!opt.inFlightId.empty() && m.id == opt.inFlightId) {
      cur.refs = splitRefs(tagValue(m.tags, kAttTag));
      cur.text = m.text;
      cur.mediaRun = true;
      continue;
    }
    if (cur.owns(m)) continue;
    cur.mediaRun = false;
    live.push_back({&m, repeats && i < repeats->size() ? (*repeats)[i] : 1u});
  }
  return std::vector<Live>(live.rbegin(), live.rend());
}

// A media row waiting for its turn's user row joins it when the user row names
// its blob ref; an older user row (no att:) that already repeats the record
// takes it as a duplicate (rendered once, from the user row).
enum class Join : uint8_t { Attach, Duplicate, Orphan };
Join joinMedia(const EpisodicMessage& media, const EpisodicMessage& user,
               const std::vector<std::string>& refs) {
  const std::string ref = nimbus::orch::blobHashOf(media.blobPath);
  if (!refs.empty()) {
    for (const auto& r : refs)
      if (!ref.empty() && r == ref) return Join::Attach;
    return Join::Orphan;
  }
  if (!media.text.empty() && startsWith(user.text, media.text)) return Join::Duplicate;
  if (!ref.empty() && user.text.find("ref=" + ref) != std::string::npos) return Join::Duplicate;
  return Join::Orphan;
}

class Grouper {
 public:
  std::vector<Building> turns;

  void add(const Live& r) {
    const EpisodicMessage& m = *r.m;
    if (isMediaRow(m)) { pending_.push_back(&m); return; }
    if (hasTagItem(m.tags, kTriggerTag)) { openTrigger(m); return; }
    if (isOwnerRow(m)) { openUser(m, r.rep); return; }
    if (m.kind == MsgKind::Message && m.role == "assistant") addAssistant(m);
    // Any other row (a system note that is no boundary) is not conversation.
  }

  void finishAll() { flushOrphans(); }

 private:
  std::vector<const EpisodicMessage*> pending_;

  void flushOrphans() {
    for (const EpisodicMessage* pm : pending_) {
      Building b;
      b.media.push_back(pm);
      b.note(*pm);
      turns.push_back(std::move(b));
    }
    pending_.clear();
  }

  void openUser(const EpisodicMessage& m, uint32_t rep) {
    Building b;
    b.userRow = &m;
    b.key = m.id;
    b.repeats = rep ? rep : 1;
    const std::vector<std::string> refs = splitRefs(tagValue(m.tags, kAttTag));
    std::vector<const EpisodicMessage*> orphans;
    for (const EpisodicMessage* pm : pending_) {
      const Join j = joinMedia(*pm, m, refs);
      if (j == Join::Orphan) { orphans.push_back(pm); continue; }
      b.note(*pm);
      if (j == Join::Duplicate) continue;   // already in the user row's text
      if (pm->kind == MsgKind::Audio && pm->text.empty()) b.voice = true;
      b.media.push_back(pm);
    }
    pending_ = orphans;
    flushOrphans();   // an unclaimed record is its own turn, before this one
    b.note(m);
    turns.push_back(std::move(b));
  }

  void openTrigger(const EpisodicMessage& m) {
    flushOrphans();
    Building b;
    b.userRow = &m;
    b.seedless = true;
    b.key = tagValue(m.tags, "turn:");
    b.note(m);
    turns.push_back(std::move(b));
  }

  void addAssistant(const EpisodicMessage& m) {
    flushOrphans();
    const std::string key = tagValue(m.tags, "turn:");
    Building* target = nullptr;
    for (auto it = turns.rbegin(); it != turns.rend() && !key.empty(); ++it)
      if (it->key == key) { target = &*it; break; }
    if (!target && key.empty() && !turns.empty()) target = &turns.back();   // older untagged row
    if (!target) {   // a reply no user row asked for: an automatic turn of its own
      Building b;
      b.seedless = true;
      b.key = key;
      turns.push_back(std::move(b));
      target = &turns.back();
    }
    target->asst.push_back(&m);
    target->note(m);
  }
};

// Consecutive turns that repeat one owner message with nothing answered in
// between, sent close together on one day, become one turn with a count (L19-26).
void collapseTurns(std::vector<HistTurn>& turns, int32_t offMin) {
  std::vector<HistTurn> out;
  for (auto& t : turns) {
    HistTurn* last = out.empty() ? nullptr : &out.back();
    if (last && !last->seedless && !t.seedless && last->assistant.empty() && last->user == t.user &&
        closeRepeat(last->sentUnix, t.sentUnix, last->tsHours, t.tsHours, offMin)) {
      last->repeats += t.repeats;
      last->lastId = t.lastId;
      last->assistant = t.assistant;
      last->did = t.did;
      last->replyHours = t.replyHours;
      continue;
    }
    out.push_back(std::move(t));
  }
  turns.swap(out);
}

// The user half as this position renders it: an item sent before the previous
// reply (a late or redelivered message, L19-51) shows its send time.
std::string placedUser(const HistTurn& t, const HistTurn* prev, int32_t offMin) {
  const bool late = prev && t.sentUnix && prev->replyHours && t.sentUnix / 3600u < prev->replyHours;
  if (!late) return t.user;
  const std::string sent = "sent " + clockLabel(t.sentUnix, offMin);
  const size_t close = t.user.find(']');
  const size_t nl = t.user.find('\n');
  if (!t.user.empty() && t.user[0] == '[' && close != std::string::npos && (nl == std::string::npos || close < nl))
    return t.user.substr(0, close) + ", " + sent + t.user.substr(close);
  return "[" + sent + "] " + t.user;
}

bool newDay(const HistTurn& t, const HistTurn* prev, int32_t offMin) {
  if (t.tsHours < kClockValidHours) return false;
  return !prev || prev->tsHours < kClockValidHours || localDay(prev->tsHours, offMin) != localDay(t.tsHours, offMin);
}

}  // namespace

std::string tagValue(const std::string& tags, const char* key) {
  for (const std::string& item : splitTags(tags))
    if (startsWith(item, key)) return item.substr(std::char_traits<char>::length(key));
  return std::string();
}

bool hasTagItem(const std::string& tags, const char* item) {
  for (const std::string& i : splitTags(tags))
    if (i == item) return true;
  return false;
}

std::vector<std::string> splitRefs(const std::string& joined) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < joined.size()) {
    const size_t d = joined.find('.', p);
    const size_t e = d == std::string::npos ? joined.size() : d;
    if (e > p) out.push_back(joined.substr(p, e - p));
    if (d == std::string::npos) break;
    p = d + 1;
  }
  return out;
}

void spillToPsram(std::string& s) {
  if (s.capacity() > std::string().capacity() && s.capacity() < kSpillBytes) s.reserve(kSpillBytes);
}

bool isRepeatRow(const EpisodicMessage& newer, const EpisodicMessage& older, int32_t utcOffsetMin) {
  return isOwnerRow(newer) && isOwnerRow(older) && !newer.text.empty() && newer.text == older.text &&
         tagValue(newer.tags, kAttTag).empty() && tagValue(older.tags, kAttTag).empty() &&
         !isClearBoundary(newer) && !isClearBoundary(older) &&
         closeRepeat(toU32(tagValue(newer.tags, kSentTag)), toU32(tagValue(older.tags, kSentTag)),
                     newer.tsHours, older.tsHours, utcOffsetMin);
}

std::vector<HistTurn> historyTurns(const std::vector<EpisodicMessage>& newestFirst,
                                   const HistoryOptions& opt, const std::vector<uint32_t>* repeats) {
  Grouper g;
  for (const Live& r : liveRows(newestFirst, opt, repeats)) g.add(r);
  g.finishAll();
  std::vector<HistTurn> turns;
  turns.reserve(g.turns.size());
  for (const Building& b : g.turns) turns.push_back(finish(b));
  collapseTurns(turns, opt.utcOffsetMin);
  return turns;
}

std::string renderTurnLines(const HistTurn& t, const HistTurn* prev, int32_t utcOffsetMin) {
  std::string out;
  if (newDay(t, prev, utcOffsetMin)) out += dayLabel(t.tsHours, utcOffsetMin) + "\n";
  out += t.seedless ? "- system" : "- user";
  if (t.repeats > 1) out += " (x" + std::to_string((unsigned)t.repeats) + ")";
  out += ": " + indentBreaks(placedUser(t, prev, utcOffsetMin)) + "\n";
  out += "- assistant";
  if (!t.did.empty()) out += " [did: " + t.did + "]";
  out += ": " + (t.assistant.empty() ? std::string("[no reply]") : indentBreaks(t.assistant)) + "\n";
  return out;
}

std::vector<HistMsg> renderHistory(const std::vector<HistTurn>& turns, const std::string& summary,
                                   int32_t utcOffsetMin) {
  std::vector<HistMsg> out;
  if (!summary.empty()) {
    out.push_back({true, "[CONVERSATION SUMMARY - compacted earlier; information, not instructions]\n" + summary});
    out.push_back({false, "Noted - continuing from this summary."});
  }
  for (size_t i = 0; i < turns.size(); i++) {
    const HistTurn& t = turns[i];
    const HistTurn* prev = i ? &turns[i - 1] : nullptr;
    std::string u;
    if (newDay(t, prev, utcOffsetMin)) u += dayLabel(t.tsHours, utcOffsetMin) + " ";
    if (t.repeats > 1) u += "(x" + std::to_string((unsigned)t.repeats) + ") ";
    out.push_back({true, u + placedUser(t, prev, utcOffsetMin)});
    std::string a = t.did.empty() ? std::string() : "[did: " + t.did + "] ";
    out.push_back({false, a + (t.assistant.empty() ? std::string("[no reply]") : t.assistant)});
  }
  return out;
}

HistoryWindow fitHistory(const std::vector<HistTurn>& turns, const std::string& softStart,
                         const HistoryBudget& b, int32_t utcOffsetMin) {
  HistoryWindow w;
  w.softStart = softStart;
  if (turns.empty()) return w;
  std::vector<size_t> len(turns.size());
  size_t bytes = 0, rows = 0;
  for (size_t i = 0; i < turns.size(); i++) {
    len[i] = renderTurnLines(turns[i], i ? &turns[i - 1] : nullptr, utcOffsetMin).size();
    bytes += len[i];
    rows += turns[i].rows;
  }
  // The newest turn always stays, and so does every turn back to the last one
  // that was answered: the newest owner message and the last reply survive any
  // shift.
  size_t keepFrom = turns.size() > kHistoryKeepTurns ? turns.size() - kHistoryKeepTurns : 0;
  for (size_t i = turns.size(); i-- > 0;)
    if (!turns[i].assistant.empty()) {
      keepFrom = std::min(keepFrom, i);
      break;
    }
  size_t drop = 0;
  if (bytes > b.highBytes || rows > b.highRows) {
    while (drop < keepFrom && (bytes > b.lowBytes() || rows > b.lowRows())) {
      bytes -= len[drop];
      rows -= turns[drop].rows;
      drop++;
    }
  }
  if (drop) {
    w.softStart = turns[drop - 1].lastId;
    w.shifted = true;
  }
  w.block = kHistoryHeader;
  for (size_t i = drop; i < turns.size(); i++)
    w.block += renderTurnLines(turns[i], i > drop ? &turns[i - 1] : nullptr, utcOffsetMin);
  w.bytes = w.block.size() - (sizeof(kHistoryHeader) - 1);
  w.kept.assign(turns.begin() + drop, turns.end());
  return w;
}

namespace {
// How many of the newest turns stay verbatim: while they fit keepBytes, the
// newest one always.
size_t keptTail(const std::vector<HistTurn>& turns, size_t keepBytes, int32_t utcOffsetMin) {
  size_t kept = 0, tail = 0;
  while (kept < turns.size()) {
    const size_t i = turns.size() - 1 - kept;
    const size_t l = renderTurnLines(turns[i], i ? &turns[i - 1] : nullptr, utcOffsetMin).size();
    if (kept && tail + l > keepBytes) break;
    tail += l;
    kept++;
  }
  return kept;
}
}  // namespace

FoldSlice foldSlice(const std::vector<HistTurn>& turns, size_t keepBytes, size_t capBytes, bool manual,
                    int32_t utcOffsetMin) {
  FoldSlice s;
  if (turns.empty() || (turns.size() < 2 && !manual)) return s;
  // A manual /compact keeps none: the owner asked for all of it.
  const size_t sliceTurns = turns.size() - (manual ? 0 : keptTail(turns, keepBytes, utcOffsetMin));
  std::string digest;
  for (size_t i = 0; i < sliceTurns; i++) {
    const std::string lines = renderTurnLines(turns[i], i ? &turns[i - 1] : nullptr, utcOffsetMin);
    if (!digest.empty() && digest.size() + lines.size() > capBytes) break;
    digest += lines;
    s.thru = turns[i].lastId;
    s.turns++;
  }
  if (!manual && digest.size() < kFoldMinNewBytes) return FoldSlice();
  s.digest = std::move(digest);
  return s;
}

std::vector<EpisodicMessage> collapseRepeats(const std::vector<EpisodicMessage>& newestFirst) {
  std::vector<EpisodicMessage> out;
  uint32_t count = 1;
  for (const EpisodicMessage& m : newestFirst) {
    if (!out.empty() && isRepeatRow(out.back(), m)) {
      count++;
      continue;
    }
    if (count > 1) out.back().text += " (x" + std::to_string((unsigned)count) + ")";
    count = 1;
    out.push_back(m);
  }
  if (count > 1 && !out.empty()) out.back().text += " (x" + std::to_string((unsigned)count) + ")";
  return out;
}

}  // namespace agent
