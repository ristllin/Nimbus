// plain/history.h - the engine-2 tools search and view (lane E2-SRCH).
#include "nimbus/harness/plain/history.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <new>

#include "nimbus/harness/log.h"
#include "nimbus/harness/plain/rails.h"     // itemOutside - the rails' item rule
#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/action_rules.h"     // actionRefusal - the tools' rows
#include "nimbus/orch/loops.h"              // daysFromCivil
#include "nimbus/orch/tx/old_engine.h"      // rowIdNumber, rowIdString
#include "nimbus/orch/tx/rules.h"           // photoWhen
#include "nimbus/utf8.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;
namespace R = texts::tool_results;
using ArduinoJson::JsonObjectConst;

namespace {

constexpr char kSearch[] = "search";
constexpr char kView[] = "view";
constexpr char kNotes[] = "notes";
constexpr char kHistory[] = "history";
constexpr char kFiles[] = "files";
constexpr char kMemories[] = "memories";

ToolOutput fail(const std::string& why) {
  ToolOutput o;
  o.isError = true;
  o.text = why;
  return o;
}
ToolOutput ok(std::string text) {
  ToolOutput o;
  o.text = std::move(text);
  return o;
}

std::string str(JsonObjectConst a, const char* k) {
  return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string();
}

// A local date "YYYY-MM-DD" or date and time "YYYY-MM-DD HH:MM" as its parts; false: not that form.
struct LocalTime {
  int y = 0, m = 0, d = 0, hh = 0, mm = 0;
  bool withTime = false;
};
bool parseParts(const std::string& s, LocalTime& t) {
  char tail = 0;
  const int n = std::sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d%c", &t.y, &t.m, &t.d, &t.hh, &t.mm, &tail);
  t.withTime = n == 5 && s.size() == 16;
  if (!(n == 3 && s.size() == 10) && !t.withTime) return false;
  if (!t.withTime) t.hh = t.mm = 0;
  const bool date = t.y >= 2000 && t.y <= 2100 && t.m >= 1 && t.m <= 12 && t.d >= 1 && t.d <= 31;
  return date && t.hh >= 0 && t.hh <= 23 && t.mm >= 0 && t.mm <= 59;
}

// That local time in epoch seconds: the first second of the day (or minute), or with `end` its last.
bool parseLocal(const std::string& s, bool end, int16_t tzMin, uint64_t& out) {
  LocalTime t;
  if (!parseParts(s, t)) return false;
  int64_t local = int64_t(nimbus::orch::daysFromCivil(t.y, unsigned(t.m), unsigned(t.d))) * 86400 + t.hh * 3600 +
                  t.mm * 60;
  if (end) local += t.withTime ? 59 : 86399;
  const int64_t utc = local - int64_t(tzMin) * 60;
  if (utc <= 0) return false;
  out = uint64_t(utc);
  return true;
}

std::string whenOf(uint64_t t, int16_t tz) {
  std::string w;
  if (!t || !tx::photoWhen(t, tz, w).ok()) w.clear();
  return w;
}
// An earlier-engine row's time, known to the hour only: "Tue 17 Nov 2026 13:xx".
std::string hourOf(uint64_t t, int16_t tz) {
  std::string w = whenOf(t, tz);
  if (w.size() > 3 && w[w.size() - 3] == ':') w.replace(w.size() - 2, 2, "xx");
  return w;
}

// What was outside content when it was stored: the rails' item rule, and a summary written inside a
// data frame (a summary of a turn that read outside content).
bool storedOutside(const tx::Item& it) {
  namespace S = texts::summarizer;
  return itemOutside(it) || (it.kind == tx::ItemKind::Digest && it.x.find(S::kFrameOpen) != std::string::npos);
}

// Text from outside content, shown inside the data frame; text inside it cannot open or close it ('<'
// before "data" or "/data" becomes '(', as plain_compact.cpp's summary lines).
std::string framed(const std::string& s) {
  namespace S = texts::summarizer;
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] != '<') continue;
    size_t j = i + 1;
    while (j < out.size() && (out[j] == '/' || out[j] == ' ')) ++j;
    if (j + 4 <= out.size() && (out.compare(j, 4, "data") == 0 || out.compare(j, 4, "DATA") == 0)) out[i] = '(';
  }
  return std::string(S::kFrameOpen) + out + S::kFrameClose;
}

const char* sourceWord(tx::HitSource s) {
  switch (s) {
    case tx::HitSource::Person:
    case tx::HitSource::OldPerson: return R::kSrcPerson;
    case tx::HitSource::Assistant:
    case tx::HitSource::OldAssistant: return R::kSrcAssistant;
    case tx::HitSource::Device: return R::kSrcDevice;
    case tx::HitSource::Summary: return R::kSrcSummary;
    case tx::HitSource::Tool: return "";
  }
  return "";
}

// The start of a UTF-8 character at or before `i`.
// "r12 Tue 17 Nov 2026 13:02 web_search result, outside content"
std::string label(const std::string& id, const std::string& when, tx::HitSource src, const std::string& tool,
                  bool outside, bool cleared = false) {
  std::string s = id;
  if (!when.empty()) s += " " + when;
  s += " ";
  if (src == tx::HitSource::Tool) s += tool + R::kSrcResult;
  else s += sourceWord(src);
  if (src == tx::HitSource::OldPerson || src == tx::HitSource::OldAssistant) s += R::kSrcEarlier;
  if (cleared) s += R::kSrcCleared;
  if (outside) s += R::kSrcOutside;
  return s;
}

// One device-log line per call: what it read off the card and what it answered. `what` is the id
// viewed or the scope searched, `hits` the hits (or 1 for a view that answered), `more` a remainder.
struct CallLog {
  const char* tool;
  std::string what;
  size_t hits;
  bool more;
  const char* st;
};
void logCall(const TurnCtx& t, const tx::ReadStats& c, size_t outBytes, const CallLog& l) {
  hlog::logf("e2: %s chat=%s %s hits=%u more=%d st=%s reads=%u bytes=%u cardMs=%u longestMs=%u out=%u", l.tool,
             t.chatKey.c_str(), l.what.c_str(), (unsigned)l.hits, int(l.more), l.st, (unsigned)c.reads,
             (unsigned)c.bytes, (unsigned)c.totalMs, (unsigned)c.longestMs, (unsigned)outBytes);
}

// What a stored item is, as a hit or a view names it; whether view opens it (it holds text).
tx::HitSource sourceOfItem(tx::ItemKind k) {
  switch (k) {
    case tx::ItemKind::User: return tx::HitSource::Person;
    case tx::ItemKind::Assistant: return tx::HitSource::Assistant;
    case tx::ItemKind::DeviceNote: return tx::HitSource::Device;
    case tx::ItemKind::Digest: return tx::HitSource::Summary;
    default: return tx::HitSource::Tool;
  }
}
bool opensAsText(tx::ItemKind k) {
  return k == tx::ItemKind::User || k == tx::ItemKind::Assistant || k == tx::ItemKind::DeviceNote ||
         k == tx::ItemKind::Digest || k == tx::ItemKind::Result || k == tx::ItemKind::Call;
}

// limit as read_file takes it: the floor and the cap (ruling 95).
size_t pageLimit(JsonObjectConst a) {
  if (!a["limit"].is<long long>()) return kViewPageMax;
  const long long v = a["limit"].as<long long>();
  return size_t(std::max<long long>((long long)kViewPageFloor, std::min<long long>((long long)kViewPageMax, v)));
}
bool pageOffset(JsonObjectConst a, size_t& off) {
  off = 0;
  if (a["offset"].isNull()) return true;
  if (!a["offset"].is<long long>() || a["offset"].as<long long>() < 0) return false;
  off = size_t(a["offset"].as<long long>());
  return true;
}

// The page of a text and its remainder line; `bodyLen` is the page's own length in the text (the shown
// body may be framed).
std::string page(const std::string& head, const std::string& shown, size_t offset, size_t total, size_t bodyLen) {
  std::string s = head + R::kBytes + "\n" + shown;
  const size_t end = offset + bodyLen;
  if (end < total) s += R::kViewNext + std::to_string(end) + " (" + std::to_string(total - end) + R::kViewMore + ")";
  return s;
}

// Who may read the turn's conversation: the action rules' row for the tool (an approved caller, never
// the LAN), and a person who is not the owner reads only the chat their namespace names. "" when the
// turn may read, else the one line that says why not.
std::string refusalFor(const std::string& tool, const TurnCtx& t) {
  if (!t.who || !tx::chatKeyOk(t.chatKey)) return R::kNotInTurn;
  const bool unattended = t.trigger != Trigger::Owner && t.trigger != Trigger::Console;
  const std::string why = nimbus::orch::actionRefusal(tool, nimbus::orch::actionCaller(*t.who, unattended, t.chatId));
  if (!why.empty()) return why;
  if (t.who->owner || t.who->ns == "chat:" + t.chatId) return std::string();
  return R::kNotInTurn;
}

// E2-CFIX2: the conversations of one chat. /clear moves the chat to "<chat>_c<g>" and keeps every
// earlier one ("<chat>" is 0); search reads them all, newest first, and an earlier one's ids and cursors
// carry its number ("c<g>r<q>", "c<g>:<cursor>"). The turn's own number; false: a key of another form.
bool generationOf(const TurnCtx& t, uint32_t& g) {
  g = 0;
  if (t.chatKey == t.chatId) return true;
  const std::string pre = t.chatId + "_c";
  if (t.chatId.empty() || t.chatKey.compare(0, pre.size(), pre) != 0) return false;
  const std::string n = t.chatKey.substr(pre.size());
  if (n.empty() || n.size() > 9 || n.find_first_not_of("0123456789") != std::string::npos) return false;
  g = uint32_t(std::stoul(n));
  return g > 0;
}
std::string keyOfGeneration(const TurnCtx& t, uint32_t g) {
  return g ? t.chatId + "_c" + std::to_string(g) : t.chatId;
}
// "c<g>" then `sep` at `at`: the number (below the turn's own) and the text after it; false: not that form.
bool earlierRef(const TurnCtx& t, const std::string& s, char sep, uint32_t& g, std::string& rest) {
  uint32_t cur = 0;
  const size_t end = s.find(sep);
  if (s.size() < 3 || s[0] != 'c' || end == std::string::npos || end < 2 || end > 10 || !generationOf(t, cur))
    return false;
  const std::string n = s.substr(1, end - 1);
  if (n.find_first_not_of("0123456789") != std::string::npos) return false;
  g = uint32_t(std::stoul(n));
  rest = s.substr(sep == ':' ? end + 1 : end);
  return g < cur;
}

// One hit's line: its label (a cleared conversation's id starts with 'c'; after a /clear the earlier
// engine's rows are of a cleared conversation too), then its excerpt, framed when outside content.
std::string hitLine(const tx::SearchHit& h, const std::string& when, bool afterClear) {
  const std::string ex = (h.cutBefore ? R::kCutMark : "") + h.excerpt + (h.cutAfter ? R::kCutMark : "");
  const bool old = h.source == tx::HitSource::OldPerson || h.source == tx::HitSource::OldAssistant;
  return label(h.id, when, h.source, h.tool, h.outside, h.id[0] == 'c' || (old && afterClear)) + ": " +
         (h.outside ? framed(ex) : ex) + "\n";
}

// After the hits: the further matches and their dates, how far the reading went, the cursor, or the end.
// `when` names an instant in its own zone.
template <typename When>
std::string restText(const tx::SearchOutcome& o, const When& when) {
  const std::string readTo = R::kSearchMore + when(o.readFromT) + R::kSearchBefore;
  if (!o.further) return o.complete ? std::string(R::kSearchEnd) : readTo + R::kBeforeEq + o.next;
  std::string text = std::to_string(o.further) + R::kSearchFurther;
  if (o.furtherFromT) text += ", " + when(o.furtherFromT) + R::kSearchTo + when(o.furtherToT);
  if (!o.complete) text += "\n" + readTo;   // the cursor below reaches the older part after these
  return o.next.empty() ? text : text + R::kBeforeEq + o.next;
}

// One conversation's search into the page: its hits (an earlier conversation's ids under `pre`, "c<g>"),
// the card it read, and its rest; its cursor under the same prefix.
void absorb(tx::SearchOutcome& all, tx::SearchOutcome& o, const std::string& pre) {
  for (tx::SearchHit& h : o.hits) {
    if (!pre.empty() && h.id[0] == 'r') h.id = pre + h.id;
    all.hits.push_back(std::move(h));
  }
  all.card.reads += o.card.reads;
  all.card.bytes += o.card.bytes;
  all.card.totalMs += o.card.totalMs;
  all.card.longestMs = std::max(all.card.longestMs, o.card.longestMs);
  all.oldRead = all.oldRead || o.oldRead;
  all.unreadBulk += o.unreadBulk;
  all.further = o.further;
  all.furtherFromT = o.furtherFromT;
  all.furtherToT = o.furtherToT;
  all.readFromT = o.readFromT;
  all.readToT = o.readToT;
  all.complete = o.complete;
  all.next = o.next.empty() ? std::string() : (pre.empty() ? o.next : pre + ":" + o.next);
}

// The page is full or the window's card budget is spent: the next call starts at conversation `g`.
bool pageEnds(tx::SearchOutcome& all, uint32_t g, size_t maxHits, const tx::SearchLimits& lim) {
  const bool spent = all.card.bytes >= lim.windowBytes || all.card.totalMs >= lim.cardMs;
  if (all.hits.size() < maxHits && !spent) return false;
  all.complete = false;
  all.next = "c" + std::to_string(g) + ":";
  return true;
}

}  // namespace

std::string viewedPhoto(const std::string& toolName, const std::string& text) {
  const size_t pre = sizeof(R::kViewPhoto) - 1, suf = sizeof(R::kViewPhotoShown) - 1;
  if (toolName != kView || text.size() < pre + 2 + suf || text.compare(0, pre, R::kViewPhoto) != 0 ||
      text.compare(text.size() - suf, suf, R::kViewPhotoShown) != 0)
    return std::string();
  const std::string ref = text.substr(pre, text.find_first_of(" :", pre) - pre);   // "p<ref>", then its time or ':'
  return ref.size() > 1 && ref[0] == 'p' ? ref : std::string();
}

void HistoryTools::registerInto(nimbus::orch::ToolRegistry& reg) const {
  // Reached only through answer(): any other route (the LAN, a direct dispatch) has no turn.
  const auto refuse = [](JsonObjectConst, const nimbus::orch::Principal&) {
    return nimbus::orch::ToolResult::fail(R::kNotInTurn);
  };
  if (const auto* d = toolDecl(kSearch)) reg.add("search", d->description, refuse, declSchema(*d, false));
  if (const auto* d = toolDecl(kView)) reg.add("view", d->description, refuse, declSchema(*d, false));
}

bool HistoryTools::answer(const ToolCall& call, const TurnCtx& turn, ToolOutput& out) {
  if (call.name != kSearch && call.name != kView) return false;
  card_ = tx::ReadStats{};
  ArduinoJson::JsonDocument args;
  (void)deserializeJson(args, call.argsJson, ArduinoJson::DeserializationOption::NestingLimit(8));
  const JsonObjectConst a = args.as<JsonObjectConst>();
  const std::string why = d_.files ? refusalFor(call.name, turn) : std::string(R::kNotInTurn);
  if (!why.empty()) out = fail(why);
  else out = call.name == kSearch ? search(call, turn, a) : view(call, turn, a);
  out.id = call.id;
  out.name = call.name;
  return true;
}

tx::OldRows* HistoryTools::oldFor(const TurnCtx& turn) const {
  // A chat's first conversation (its own key) holds what the import left behind; a later one reads
  // them as part of that cleared conversation (E2-CFIX2).
  uint32_t g = 0;
  if (!d_.oldRows || !generationOf(turn, g)) return nullptr;
  return d_.oldRows(turn.chatId);
}

// The history query of a search call; false: an argument is not valid.
bool HistoryTools::askOf(const TurnCtx& turn, JsonObjectConst a, tx::SearchAsk& ask) const {
  ask.query = str(a, "query");
  ask.before = str(a, "before");
  uint32_t g = 0;
  std::string inner;   // an earlier conversation's cursor is checked without its "c<g>:" (E2-CFIX2)
  const std::string own = earlierRef(turn, ask.before, ':', g, inner) ? inner : ask.before;
  ask.openTurn = turn.turn;
  const int16_t zone = tz(turn.startedAt);
  const std::string from = str(a, "from"), to = str(a, "to");
  if (!from.empty() && !parseLocal(from, false, zone, ask.fromT)) return false;
  if (!to.empty() && !parseLocal(to, true, zone, ask.toT)) return false;
  return tx::cursorOk(own) && !(ask.fromT && ask.toT && ask.fromT > ask.toT);
}

// The hits, best first, then the rest: how many more matches the window holds and their dates, or
// how far the reading went and that older parts were not read, with the cursor; or that it reached
// the start of the range.
// `afterClear`: the turn runs after a /clear, so the earlier engine's rows are of a cleared conversation.
std::string HistoryTools::answerText(const tx::SearchOutcome& o, const tx::Status& st, bool afterClear) const {
  std::string text;
  if (!st.ok()) return R::kCardStopped + o.next;   // nothing above it claims absence (ruling 116)
  for (const tx::SearchHit& h : o.hits) {
    const int16_t zone = h.hasTz ? h.tzMinutes : tz(h.t);
    text += hitLine(h, h.hourOnly ? hourOf(h.t, zone) : whenOf(h.t, zone), afterClear);
  }
  if (o.hits.empty()) text += std::string(R::kSearchNone) + "\n";
  if (o.unreadBulk) text += std::to_string(o.unreadBulk) + R::kSearchUnread + "\n";
  return text + restText(o, [this](uint64_t t) { return whenOf(t, tz(t)); });
}

ToolOutput HistoryTools::search(const ToolCall&, const TurnCtx& turn, JsonObjectConst a) {
  const std::string query = str(a, "query");
  if (tx::queryWords(query).empty()) return fail(R::kSearchNoWords);
  const std::string scope = str(a, "scope");
  if (scope == kNotes) return searchNotes(turn, a, query);
  if (scope == kFiles && d_.fileSearch) return searchFiles(turn, a, query);
  if (scope == kMemories && d_.memories) return searchMemories(turn, query);
  tx::SearchAsk ask;
  if ((!scope.empty() && scope != kHistory) || !askOf(turn, a, ask)) return fail(R::kSearchBadArgs);
  uint32_t g = 0;
  const bool chained = generationOf(turn, g);
  std::string inner;
  if (earlierRef(turn, ask.before, ':', g, inner)) ask.before = inner;
  tx::SearchOutcome o;
  const tx::Status st = searchFrom(turn, chained, g, ask, o);
  card_ = o.card;
  ToolOutput out = ok(answerText(o, st, turn.chatKey != turn.chatId));
  out.isError = !st.ok() && o.hits.empty();
  // An excerpt of outside content is outside content.
  out.outside = std::any_of(o.hits.begin(), o.hits.end(), [](const tx::SearchHit& h) { return h.outside; });
  logCall(turn, card_, out.text.size(),
          CallLog{kSearch, o.oldRead ? "history+old" : kHistory, o.hits.size(), !o.next.empty(), tx::errSlug(st.err)});
  return out;
}

// The history scope from conversation `g` (with `ask.before` its own cursor) to the chat's first one,
// newest first: each conversation's hits fill the page in turn; a page that stops inside a conversation
// or between two hands a cursor that names where (E2-CFIX2). `chained` false: the turn's key alone.
tx::Status HistoryTools::searchFrom(const TurnCtx& turn, bool chained, uint32_t g, tx::SearchAsk ask,
                                    tx::SearchOutcome& all) {
  uint32_t cur = 0;
  if (chained) (void)generationOf(turn, cur);
  const size_t maxHits = d_.limits.maxHits;
  all = tx::SearchOutcome{};
  for (;;) {
    const bool own = !chained || g == cur;
    tx::SearchDeps sd;
    sd.files = d_.files;
    sd.chatKey = own ? turn.chatKey : keyOfGeneration(turn, g);
    sd.old = sd.chatKey == turn.chatId ? oldFor(turn) : nullptr;   // the first conversation's
    sd.nowMs = d_.nowMs;
    sd.limits = d_.limits;
    sd.limits.maxHits = maxHits - all.hits.size();
    sd.itemOutside = storedOutside;
    if (!own) ask.openTurn = 0;   // the open turn is the new conversation's
    tx::SearchOutcome o;
    const tx::Status st = tx::search(sd, ask, o);
    absorb(all, o, own ? std::string() : "c" + std::to_string(g));
    if (!st.ok() || !o.next.empty() || !chained || g == 0) return st;
    --g;
    ask.before.clear();
    if (pageEnds(all, g, maxHits, d_.limits)) return st;
  }
}

ToolOutput HistoryTools::searchNotes(const TurnCtx& turn, JsonObjectConst a, const std::string& query) {
  const std::string before = str(a, "before");
  size_t start = 0;
  if (!before.empty()) {
    if (before.size() < 2 || before[0] != 'n' || before.find_first_not_of("0123456789", 1) != std::string::npos ||
        before.size() > 8)
      return fail(R::kSearchBadArgs);
    start = size_t(std::stoul(before.substr(1)));
  }
  std::vector<nimbus::orch::notes::Note> notes;
  if (d_.notes && !d_.notes(*turn.who, notes)) return fail(R::kCardStopped + before);
  const std::vector<std::string> words = tx::queryWords(query);
  std::string text;
  size_t shown = 0, i = start;
  for (; i < notes.size() && shown < kNotesPerPage; ++i) {
    const std::string line = nimbus::orch::notes::formatLine(notes[i]);
    if (!tx::matchesAll(line, words)) continue;
    text += R::kSrcNote + line + "\n";
    ++shown;
  }
  if (!shown) text += std::string(R::kSearchNone) + "\n";
  size_t rest = 0;
  for (size_t j = i; j < notes.size(); ++j) rest += tx::matchesAll(nimbus::orch::notes::formatLine(notes[j]), words);
  const bool more = rest > 0;
  text += more ? std::to_string(rest) + R::kSearchFurther + R::kBeforeEq + "n" + std::to_string(i)
               : std::string(R::kSearchEnd);
  logCall(turn, card_, text.size(), CallLog{kSearch, kNotes, shown, more, "ok"});
  return ok(std::move(text));
}

// Scope files (lane E2-P-MEM): engine 1's files.search scan and rank (FileSearch), eight files a page in
// the form of the other scopes, a file of outside content framed; then the cursor, or how far the scan
// read (the newest files first), or that it read them all.
ToolOutput HistoryTools::searchFiles(const TurnCtx& turn, JsonObjectConst a, const std::string& query) {
  const std::string before = str(a, "before");
  size_t start = 0;
  if (!before.empty()) {
    if (before.size() < 2 || before.size() > 8 || before[0] != 'f' ||
        before.find_first_not_of("0123456789", 1) != std::string::npos)
      return fail(R::kSearchBadArgs);
    start = size_t(std::stoul(before.substr(1)));
  }
  FileSearch found;
  if (!d_.fileSearch(*turn.who, query, found)) return fail(R::kCardStopped + before);
  std::string text;
  bool outside = false;
  size_t i = start;
  for (; i < found.hits.size() && i < start + kFilesPerPage; ++i) {
    const FileHit& h = found.hits[i];
    const std::string ex = R::kCutMark + h.excerpt + R::kCutMark;
    text += R::kSrcFile + h.path + ": " + (h.outside ? framed(ex) : ex) + "\n";
    outside = outside || h.outside;
  }
  if (i == start) text += std::string(R::kSearchNone) + "\n";
  const size_t rest = found.hits.size() > i ? found.hits.size() - i : 0;
  if (rest) text += std::to_string(rest) + R::kSearchFurther + R::kBeforeEq + "f" + std::to_string(i);
  else if (found.scanned < found.candidates)
    text += R::kSearchMore + whenOf(found.oldestT, tz(found.oldestT)) + R::kSearchBefore;
  else text += R::kSearchEnd;
  logCall(turn, card_, text.size(), CallLog{kSearch, kFiles, i - start, rest > 0, "ok"});
  ToolOutput out = ok(std::move(text));
  out.outside = outside;   // an excerpt of outside content is outside content
  return out;
}

// Scope memories (lane E2-P-MEM): engine 1's memory.search answer, whole and read only.
ToolOutput HistoryTools::searchMemories(const TurnCtx& turn, const std::string& query) {
  std::string text;
  const bool answered = d_.memories(*turn.who, query, text);
  logCall(turn, card_, text.size(), CallLog{kSearch, kMemories, answered ? size_t(1) : size_t(0), false, answered ? "ok" : "fail"});
  return answered ? ok(std::move(text)) : fail(text);
}

ToolOutput HistoryTools::view(const ToolCall&, const TurnCtx& turn, JsonObjectConst a) {
  const std::string id = str(a, "id");
  size_t offset = 0;
  if (id.size() < 2 || !pageOffset(a, offset)) return fail(R::kViewBadId);
  const size_t limit = pageLimit(a);
  if (id[0] == 'p') return viewPhoto(turn, id);
  const std::string digits = id.substr(1);
  const bool num = digits.size() <= 19 && digits.find_first_not_of("0123456789") == std::string::npos;
  if (id[0] == 'r' && num) return viewItem(turn, std::stoull(digits), offset, limit);
  if (id[0] == 'm' && tx::rowIdNumber(id)) return viewOld(turn, tx::rowIdNumber(id), offset, limit);
  uint32_t g = 0;
  std::string rest;
  if (earlierRef(turn, id, 'r', g, rest) && rest.size() > 1 && rest.size() <= 20 &&
      rest.find_first_not_of("0123456789", 1) == std::string::npos) {
    TurnCtx earlier = turn;
    earlier.chatKey = keyOfGeneration(turn, g);
    return viewItem(earlier, std::stoull(rest.substr(1)), offset, limit, "c" + std::to_string(g));
  }
  return fail(R::kViewBadId);
}

ToolOutput HistoryTools::viewItem(const TurnCtx& turn, uint64_t q, size_t offset, size_t limit,
                                  const std::string& cleared) {
  tx::ReadOnlyCounted files(*d_.files, d_.nowMs, card_);
  tx::Item it;
  tx::Status st = q ? tx::findItem(files, turn.chatKey, q, it) : tx::Status::fail(tx::Err::NotFound, "view");
  if (st.err == tx::Err::NotFound || (st.ok() && !opensAsText(it.kind))) return fail(R::kViewNoItem);
  std::string body;
  size_t total = 0;
  if (st.ok()) st = tx::itemText(files, turn.chatKey, it, tx::TextPage{offset, limit}, body, total);
  if (st.err == tx::Err::NotFound) return fail(R::kViewGone);
  // Outside when stored, whatever page is asked: the item's own class, or an assistant text of a turn
  // that holds outside content.
  bool outside = storedOutside(it);
  if (st.ok() && !outside && it.kind == tx::ItemKind::Assistant)
    st = tx::turnHoldsOutside(files, turn.chatKey, it.turn, storedOutside, outside);
  if (!st.ok()) return fail(std::string(R::kCardStopped));
  const std::string when = whenOf(it.t, it.kind == tx::ItemKind::User ? it.tzMinutes : tz(it.t));
  const std::string id = cleared + "r" + std::to_string(q);
  ToolOutput out = ok(page(label(id, when, sourceOfItem(it.kind), it.n, outside, !cleared.empty()) + " " +
                               std::to_string(total),
                           outside ? framed(body) : body, offset, total, body.size()));
  out.outside = outside;   // outside content keeps its class when it comes back
  logCall(turn, card_, out.text.size(), CallLog{kView, id, 1, offset + body.size() < total, "ok"});
  return out;
}

namespace {
// An earlier-engine row by id (the forward read, which reaches the cold region too); `found` false: none.
tx::Status rowById(tx::OldRows& old, uint64_t id, tx::DayRow& out, bool& found, tx::ReadStats& stats) {
  std::vector<tx::DayRow> rows;
  bool end = false;
  const tx::Status st = old.read(id - 1, 1, rows, end, stats);
  found = st.ok() && !rows.empty() && rows[0].id == id;
  if (found) out = rows[0];
  return st;
}
// A /clear row (ev:clear) after `id`: the person cleared that row.
tx::Status clearedAfter(tx::OldRows& old, uint64_t id, bool& cleared, tx::ReadStats& stats) {
  cleared = false;
  std::vector<tx::DayRow> rows;
  bool end = false;
  tx::Status st;
  const uint64_t bytes0 = stats.bytes;
  for (uint64_t after = id; st.ok() && !end && !cleared;) {
    rows.clear();
    st = old.read(after, tx::kOldPageRows, rows, end, stats);
    for (const tx::DayRow& n : rows) cleared = cleared || n.clearBoundary;
    if (rows.empty()) break;
    if (rows.back().id <= after || stats.bytes - bytes0 > tx::kSearchWindowBytes || stats.totalMs > tx::kSearchCardMs)
      return tx::Status::fail(tx::Err::Io, "plain::clearedAfter.bound");   // not read through: never "not cleared"
    after = rows.back().id;
  }
  return st;
}
}  // namespace

// An earlier-engine row of this chat by id, and none from before the chat's newest earlier-engine
// /clear: a row tagged ev:clear after it means the person cleared it.
ToolOutput HistoryTools::viewOld(const TurnCtx& turn, uint64_t row, size_t offset, size_t limit) {
  tx::OldRows* old = oldFor(turn);
  if (!old) return fail(R::kViewNoItem);
  tx::DayRow r;
  bool found = false, cleared = false;
  tx::Status st = rowById(*old, row, r, found, card_);
  if (st.ok() && found) st = clearedAfter(*old, row, cleared, card_);
  if (!st.ok()) return fail(std::string(R::kCardStopped));
  const bool conversation = r.role == tx::RowRole::Owner || r.role == tx::RowRole::Assistant;
  if (!found || cleared || r.ours || !conversation) return fail(R::kViewNoItem);
  const bool outside = r.role == tx::RowRole::Assistant || r.photo;   // its class is not known: fail closed
  const tx::HitSource src = r.role == tx::RowRole::Owner ? tx::HitSource::OldPerson : tx::HitSource::OldAssistant;
  const std::string body = offset < r.text.size() ? r.text.substr(offset, limit) : std::string();
  const bool afterClear = turn.chatKey != turn.chatId;   // the rows belong to the chat's first conversation
  const std::string head = label(tx::rowIdString(row), hourOf(r.t, tz(r.t)), src, "", outside, afterClear) + " " +
                           std::to_string(r.text.size());
  ToolOutput out = ok(page(head, outside ? framed(body) : body, offset, r.text.size(), body.size()));
  out.outside = outside;
  logCall(turn, card_, out.text.size(), CallLog{kView, tx::rowIdString(row), 1, offset + body.size() < r.text.size(), "ok"});
  return out;
}

ToolOutput HistoryTools::viewPhoto(const TurnCtx& turn, const std::string& ref) {
  if (ref.size() > 17 || ref.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz", 1) != std::string::npos)
    return fail(R::kViewBadId);
  tx::ReadOnlyCounted files(*d_.files, d_.nowMs, card_);
  tx::Item owner;
  bool found = false;
  tx::PhotoScan scan;
  scan.nowMs = d_.nowMs;
  const tx::Status st = tx::findPhoto(files, turn.chatKey, ref, owner, found, scan);
  if (st.err == tx::Err::TooLarge && std::string(st.where) == "tx::findPhoto.budget")   // the true reason
    return fail(R::kViewPhoto + ref + " " + R::kSearchMore + whenOf(scan.readFromT, tz(scan.readFromT)) +
                R::kSearchBefore);
  if (!st.ok()) return fail(std::string(R::kCardStopped));
  if (!found) return fail(R::kViewNoItem);
  const std::string when = whenOf(owner.t, owner.tzMinutes);
  const std::string head = R::kViewPhoto + ref + (when.empty() ? std::string() : " " + when);
  const bool shown = d_.photoReadable && d_.photoReadable(ref);
  logCall(turn, card_, head.size(), CallLog{kView, ref, shown ? size_t(1) : size_t(0), false, shown ? "ok" : "unread"});
  return shown ? ok(head + R::kViewPhotoShown) : fail(head + R::kViewPhotoUnread);
}

}  // namespace plain
}  // namespace agent
