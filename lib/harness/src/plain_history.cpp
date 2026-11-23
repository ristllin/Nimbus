// plain/history.h - the engine-2 tools search and view (lane E2-SRCH).
#include "nimbus/harness/plain/history.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <new>

#include "nimbus/harness/log.h"
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

// "YYYY-MM-DD" (or "YYYY-MM-DD HH:MM") as local time: the first second of that day (or minute), or
// with `end` its last second. false: not that form.
bool parseLocal(const std::string& s, bool end, int16_t tzMin, uint64_t& out) {
  int y = 0, m = 0, d = 0, hh = -1, mm = -1;
  char tail = 0;
  const int n = std::sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d%c", &y, &m, &d, &hh, &mm, &tail);
  const bool dateOnly = n == 3 && s.size() == 10;
  const bool withTime = n == 5 && s.size() == 16;
  if (!(dateOnly || withTime) || y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  if (withTime && (hh < 0 || hh > 23 || mm < 0 || mm > 59)) return false;
  const int64_t day = int64_t(nimbus::orch::daysFromCivil(y, unsigned(m), unsigned(d))) * 86400;
  int64_t local = day + (withTime ? hh * 3600 + mm * 60 : 0);
  if (end) local += withTime ? 59 : 86399;
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
size_t charStart(const std::string& s, size_t i) {
  while (i > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
  return i;
}

// A bounded excerpt on one line around the first word's first occurrence.
std::string excerpt(const std::string& text, const std::vector<std::string>& words) {
  size_t at = 0;
  if (!words.empty()) {
    const std::string& w = words.front();
    for (size_t i = 0; i + w.size() <= text.size(); ++i) {
      size_t j = 0;
      while (j < w.size() && std::tolower(static_cast<unsigned char>(text[i + j])) == static_cast<unsigned char>(w[j])) ++j;
      if (j == w.size()) {
        at = i;
        break;
      }
    }
  }
  size_t from = at > kExcerptBytes / 4 ? charStart(text, at - kExcerptBytes / 4) : 0;
  size_t to = std::min(text.size(), from + kExcerptBytes);
  if (to < text.size()) to = charStart(text, to);
  std::string out;
  if (from > 0) out += R::kCutMark;
  for (size_t i = from; i < to; ++i) {
    const char c = text[i];
    out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  }
  if (to < text.size()) out += R::kCutMark;
  return out;
}

// "r12 Tue 17 Nov 2026 13:02 web_search result, outside content"
std::string label(const std::string& id, const std::string& when, tx::HitSource src, const std::string& tool,
                  bool outside) {
  std::string s = id;
  if (!when.empty()) s += " " + when;
  s += " ";
  if (src == tx::HitSource::Tool) s += tool + R::kSrcResult;
  else s += sourceWord(src);
  if (src == tx::HitSource::OldPerson || src == tx::HitSource::OldAssistant) s += R::kSrcEarlier;
  if (outside) s += R::kSrcOutside;
  return s;
}

// One device-log line per call: what it read off the card and what it answered. `what` is the id
// viewed or the scope searched, `hits` the hits (or 1 for a view that answered), `more` a remainder.
void logCall(const char* tool, const TurnCtx& t, const tx::ReadStats& c, size_t outBytes, const std::string& what,
             size_t hits, bool more, const char* st) {
  hlog::logf("e2: %s chat=%s %s hits=%u more=%d st=%s reads=%u bytes=%u cardMs=%u longestMs=%u out=%u", tool,
             t.chatKey.c_str(), what.c_str(), (unsigned)hits, int(more), st, (unsigned)c.reads, (unsigned)c.bytes,
             (unsigned)c.totalMs, (unsigned)c.longestMs, (unsigned)outBytes);
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

// The page of a text and its remainder line.
std::string page(const std::string& head, const std::string& body, size_t offset, size_t total) {
  std::string s = head + R::kBytes + "\n" + body;
  const size_t end = offset + body.size();
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

}  // namespace

std::string viewedPhoto(const std::string& toolName, const std::string& text) {
  const size_t pre = sizeof(R::kViewPhoto) - 1, suf = sizeof(R::kViewPhotoShown) - 1;
  if (toolName != kView || text.size() < pre + 2 + suf || text.compare(0, pre, R::kViewPhoto) != 0 ||
      text.compare(text.size() - suf, suf, R::kViewPhotoShown) != 0)
    return std::string();
  const std::string ref = text.substr(pre, text.find(' ', pre) - pre);   // "p<ref>", then its time
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
  // Only a chat's first conversation (its own key) holds what the import left behind.
  if (!d_.oldRows || turn.chatKey != turn.chatId) return nullptr;
  return d_.oldRows(turn.chatId);
}

ToolOutput HistoryTools::search(const ToolCall&, const TurnCtx& turn, JsonObjectConst a) {
  const std::string query = str(a, "query");
  if (tx::queryWords(query).empty()) return fail(R::kSearchNoWords);
  const std::string scope = str(a, "scope");
  if (scope == kNotes) return searchNotes(turn, a, query);
  if (!scope.empty() && scope != kHistory) return fail(R::kSearchBadArgs);
  tx::SearchAsk ask;
  ask.query = query;
  ask.before = str(a, "before");
  ask.openTurn = turn.turn;
  const uint64_t now = turn.startedAt;
  const std::string from = str(a, "from"), to = str(a, "to");
  if ((!from.empty() && !parseLocal(from, false, tz(now), ask.fromT)) ||
      (!to.empty() && !parseLocal(to, true, tz(now), ask.toT)) || !tx::cursorOk(ask.before) ||
      (ask.fromT && ask.toT && ask.fromT > ask.toT))
    return fail(R::kSearchBadArgs);
  tx::SearchDeps sd;
  sd.files = d_.files;
  sd.chatKey = turn.chatKey;
  sd.old = oldFor(turn);
  sd.nowMs = d_.nowMs;
  sd.limits = d_.limits;
  tx::SearchOutcome o;
  const tx::Status st = tx::search(sd, ask, o);
  card_ = o.card;
  const std::vector<std::string> words = tx::queryWords(query);
  std::string text;
  bool outside = false;
  for (const tx::SearchHit& h : o.hits) {
    const std::string when = whenOf(h.t, h.hasTz ? h.tzMinutes : tz(h.t));
    text += label(h.id, when, h.source, h.tool, h.outside) + ": " + excerpt(h.text, words) + "\n";
    outside = outside || h.outside;
  }
  if (o.hits.empty()) text += std::string(R::kSearchNone) + "\n";
  if (o.unreadBulk) text += std::to_string(o.unreadBulk) + R::kSearchUnread + "\n";
  if (!st.ok()) text += R::kCardStopped + o.next;
  else if (o.more) text += R::kSearchMore + whenOf(o.oldestT, tz(o.oldestT)) + R::kSearchBefore + o.next;
  else text += R::kSearchEnd;
  logCall(kSearch, turn, card_, text.size(), o.oldRead ? "history+old" : kHistory, o.hits.size(), o.more,
          tx::errSlug(st.err));
  ToolOutput out = ok(std::move(text));
  out.isError = !st.ok() && o.hits.empty();
  out.outside = outside;   // an excerpt of outside content is outside content
  return out;
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
  bool more = false;
  for (size_t j = i; j < notes.size() && !more; ++j)
    more = tx::matchesAll(nimbus::orch::notes::formatLine(notes[j]), words);
  text += more ? std::string(R::kNotesMore) + "n" + std::to_string(i) : std::string(R::kSearchEnd);
  logCall(kSearch, turn, card_, text.size(), kNotes, shown, more, "ok");
  return ok(std::move(text));
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
  return fail(R::kViewBadId);
}

ToolOutput HistoryTools::viewItem(const TurnCtx& turn, uint64_t q, size_t offset, size_t limit) {
  tx::ReadOnlyCounted files(*d_.files, d_.nowMs, card_);
  tx::Item it;
  tx::Status st = q ? tx::findItem(files, turn.chatKey, q, it) : tx::Status::fail(tx::Err::NotFound, "view");
  const bool text = it.kind == tx::ItemKind::User || it.kind == tx::ItemKind::Assistant ||
                    it.kind == tx::ItemKind::DeviceNote || it.kind == tx::ItemKind::Digest ||
                    it.kind == tx::ItemKind::Result || it.kind == tx::ItemKind::Call;
  if (st.err == tx::Err::NotFound || (st.ok() && !text)) return fail(R::kViewNoItem);
  if (!st.ok()) return fail(std::string(R::kCardStopped));
  std::string body;
  size_t total = 0;
  st = tx::itemText(files, turn.chatKey, it, offset, limit, body, total);
  if (st.err == tx::Err::NotFound) return fail(R::kViewGone);
  if (!st.ok()) return fail(std::string(R::kCardStopped));
  tx::HitSource src = tx::HitSource::Tool;
  if (it.kind == tx::ItemKind::User) src = tx::HitSource::Person;
  else if (it.kind == tx::ItemKind::Assistant) src = tx::HitSource::Assistant;
  else if (it.kind == tx::ItemKind::DeviceNote) src = tx::HitSource::Device;
  else if (it.kind == tx::ItemKind::Digest) src = tx::HitSource::Summary;
  const bool outside = it.kind == tx::ItemKind::Result && it.cls == tx::ResultClass::Outside;
  const std::string when = whenOf(it.t, it.kind == tx::ItemKind::User ? it.tzMinutes : tz(it.t));
  const std::string head = label("r" + std::to_string(q), when, src, it.n, outside) + " " + std::to_string(total);
  ToolOutput out = ok(page(head, body, offset, total));
  out.outside = outside;   // outside content keeps its class when it comes back
  logCall(kView, turn, card_, out.text.size(), "r" + std::to_string(q), 1, offset + body.size() < total, "ok");
  return out;
}

ToolOutput HistoryTools::viewOld(const TurnCtx& turn, uint64_t row, size_t offset, size_t limit) {
  tx::OldRows* old = oldFor(turn);
  if (!old) return fail(R::kViewNoItem);
  tx::DayRow r;
  const tx::Status st = old->row(row, r, card_);
  if (st.err == tx::Err::NotFound || (st.ok() && (r.ours || (r.role != tx::RowRole::Owner && r.role != tx::RowRole::Assistant))))
    return fail(R::kViewNoItem);
  if (!st.ok()) return fail(std::string(R::kCardStopped));
  const tx::HitSource src = r.role == tx::RowRole::Owner ? tx::HitSource::OldPerson : tx::HitSource::OldAssistant;
  const std::string body = offset < r.text.size() ? r.text.substr(offset, limit) : std::string();
  const std::string head = label(tx::rowIdString(row), whenOf(r.t, tz(r.t)), src, "", false) + " " +
                           std::to_string(r.text.size());
  ToolOutput out = ok(page(head, body, offset, r.text.size()));
  logCall(kView, turn, card_, out.text.size(), tx::rowIdString(row), 1, offset + body.size() < r.text.size(), "ok");
  return out;
}

ToolOutput HistoryTools::viewPhoto(const TurnCtx& turn, const std::string& ref) {
  if (ref.size() > 17 || ref.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz", 1) != std::string::npos)
    return fail(R::kViewBadId);
  tx::ReadOnlyCounted files(*d_.files, d_.nowMs, card_);
  tx::Item owner;
  bool found = false;
  const tx::Status st = tx::findPhoto(files, turn.chatKey, ref, owner, found);
  if (!st.ok()) return fail(std::string(R::kCardStopped));
  if (!found) return fail(R::kViewNoItem);
  const std::string when = whenOf(owner.t, owner.tzMinutes);
  const std::string head = R::kViewPhoto + ref + (when.empty() ? std::string() : " " + when);
  const bool shown = d_.photoReadable && d_.photoReadable(ref);
  logCall(kView, turn, card_, head.size(), ref, shown ? 1 : 0, false, shown ? "ok" : "unread");
  return shown ? ok(head + R::kViewPhotoShown) : fail(head + R::kViewPhotoUnread);
}

}  // namespace plain
}  // namespace agent
