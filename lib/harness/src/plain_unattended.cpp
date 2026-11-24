// Routine runs and held delivery (plain/unattended.h). Lane RB-U1a.
#include "nimbus/harness/plain/unattended.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

#include "nimbus/harness/jobs.h"   // fenceOutsideContent: the one fence (RB-Q2a)
#include "nimbus/harness/log.h"
#include "nimbus/harness/platform.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/self_wakeup.h"
#include "nimbus/orch/tool_groups.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/rules.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

const char* const kDefaultRoutineTools[3] = {"web_search", "read_file", "send"};

namespace {

constexpr char kKeyPrefix[] = "routine-";
constexpr size_t kKeyPrefixLen = sizeof(kKeyPrefix) - 1;
// A routine note longer than this is kept as its head and a pointer to the whole (a stored line
// holds kMaxLineBytes with its framing).
constexpr size_t kNoteInlineMax = 48 * 1024;
constexpr size_t kNameMax = 64;

bool idChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

// One line of printable text: control characters become spaces, cut on a character boundary.
std::string oneLine(const std::string& s, size_t cap) {
  std::string out;
  for (char c : s) out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ? ' ' : c;
  if (out.size() <= cap) return out;
  size_t n = cap;
  while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
  out.resize(n);
  return out;
}

// Cut on a character boundary at most `cap` bytes.
std::string cutText(const std::string& s, size_t cap) {
  if (s.size() <= cap) return s;
  size_t n = cap;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

// The message stamp of `at` without its opening '[' ("Tue 17 Nov 2026 06:00]").
std::string stampTail(uint64_t at, int16_t tz, uint16_t dayStart) {
  char stamp[tx::kStampMaxBytes];
  if (!tx::messageStamp(at, tz, dayStart, stamp, sizeof stamp).ok()) {
    if (!tx::messageStamp(at, 0, dayStart, stamp, sizeof stamp).ok()) return "]";
  }
  return std::string(stamp + 1);
}

std::string stopLine(const std::string& slug) {
  return std::string(texts::device_notes::kLimitStopped) + slug + texts::device_notes::kLimitStoppedEnd;
}

uint64_t fnv64(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

bool listedName(const std::vector<std::string>& names, const std::string& n) {
  return std::find(names.begin(), names.end(), n) != names.end();
}

}  // namespace

// ---- keys, facts, tools ---------------------------------------------------------------
Status routineChatKey(const std::string& routineId, std::string& out) {
  out.clear();
  if (routineId.empty() || routineId.size() > kRoutineIdMax ||
      !std::all_of(routineId.begin(), routineId.end(), idChar))
    return Status::fail(Err::InvalidArgument, "plain::routineChatKey");
  out = std::string(kKeyPrefix) + routineId;
  return Status::success();
}

bool isRoutineChat(const std::string& chatKey) {
  return chatKey.size() > kKeyPrefixLen && chatKey.compare(0, kKeyPrefixLen, kKeyPrefix) == 0;
}

orch::UnattendedFacts routineFacts(const RoutineFire& f, bool outside) {
  orch::UnattendedFacts facts;
  facts.cls = f.def.wakeup ? orch::TurnClass::Wakeup : orch::TurnClass::Routine;
  facts.routineGrants = f.grants;
  facts.outside = outside;
  facts.listed = &f.def.paths;
  return facts;
}

std::vector<std::string> routineTools(const RoutineFire& f) {
  std::vector<std::string> named = f.def.tools;
  if (named.empty()) named.assign(std::begin(kDefaultRoutineTools), std::end(kDefaultRoutineTools));
  const orch::UnattendedFacts facts = routineFacts(f);
  std::vector<std::string> out;
  for (const std::string& n : named)
    if (!n.empty() && n != orch::kMoreTool && !listedName(out, n) && orch::unattendedMayList(facts, n)) out.push_back(n);
  return out;
}

Status routineRegistry(const orch::ToolRegistry& base, const RoutineFire& f, orch::ToolRegistry& out) {
  try {
    const std::vector<std::string> allowed = routineTools(f);
    out = base;
    // `more` never stays: a copy's `more` still reaches the base registry (tool_groups.h
    // registerMore), so a routine is offered only always-on tools it names.
    for (const orch::ToolInfo& t : base.manifest())
      if (!listedName(allowed, t.name) || base.isRouter(t.name) || t.name == orch::kMoreTool) out.remove(t.name);
    return Status::success();
  } catch (const std::bad_alloc&) {
    out = orch::ToolRegistry();
    return Status::fail(Err::TooLarge, "plain::routineRegistry");
  }
}

TurnCeiling routineCeiling() {
  TurnCeiling c;
  c.rounds = kRoutineRounds;
  c.softWallMs = kRoutineSoftMs;
  c.hardWallMs = kRoutineHardMs;
  c.spendMicroUsd = kRoutineMaxMicroUsd;
  return c;
}

// ---- the state ------------------------------------------------------------------------
std::string encodeRoutineState(const std::vector<RoutineOutput>& last) {
  std::string out;
  for (const RoutineOutput& o : last) {
    JsonDocument d;
    d["at"] = o.at;
    d["text"] = o.text;
    std::string line;
    serializeJson(d, line);
    out += line;
    out += '\n';
  }
  return out;
}

std::vector<RoutineOutput> decodeRoutineState(const std::string& text) {
  std::vector<RoutineOutput> out;
  size_t at = 0;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    JsonDocument d;
    if (!deserializeJson(d, text.data() + at, nl - at) && d["text"].is<const char*>() && d["at"].is<uint64_t>())
      out.push_back(RoutineOutput{d["at"].as<uint64_t>(), d["text"].as<const char*>()});
    at = nl + 1;
  }
  return out;
}

void nextRoutineState(std::vector<RoutineOutput>& last, const RoutineOutput& now) {
  if (!now.text.empty()) last.push_back(RoutineOutput{now.at, cutText(now.text, kRoutineStateMax)});
  while (last.size() > kRoutineStateOutputs) last.erase(last.begin());
  auto bytes = [&] {
    size_t n = 0;
    for (const RoutineOutput& o : last) n += o.text.size();
    return n;
  };
  while (last.size() > 1 && bytes() > kRoutineStateMax) last.erase(last.begin());
}

// ---- the texts --------------------------------------------------------------------------
std::string routineNoteText(const std::string& name, uint64_t at, int16_t tzMinutes, uint16_t dayStartMin,
                            const std::string& text) {
  std::string out = texts::device_notes::kRoutineNote;
  out += oneLine(name, kNameMax);
  out += ' ';
  out += stampTail(at, tzMinutes, dayStartMin);
  out += ' ';
  out += text;
  return out;
}

std::string routineInputText(const RoutineFire& f, int16_t tzMinutes, uint16_t dayStartMin) {
  std::string out;
  for (const RoutineOutput& o : f.last) {
    if (o.text.empty()) continue;
    out += routineNoteText(f.def.name, o.at, tzMinutes, dayStartMin, o.text);
    out += '\n';
  }
  out += '[';
  out += stampTail(f.fireAt, tzMinutes, dayStartMin);
  out += ' ';
  out += f.def.instruction;
  return out;
}

// A reply written after the fire read outside content enters the owner's transcript whole inside the
// one outside-content fence (jobs.h, RB-Q2a's), so his later turns read it as data (lane RB-RL).
std::string heldNoteText(const HeldNote& n, int16_t tzMinutes, uint16_t dayStartMin) {
  const std::string text = n.outside && !n.text.empty() ? fenceOutsideContent(n.text) : n.text;
  if (n.stop.empty()) return routineNoteText(n.name, n.at, tzMinutes, dayStartMin, text);
  if (text.empty()) return routineNoteText(n.name, n.at, tzMinutes, dayStartMin, stopLine(n.stop));
  return routineNoteText(n.name, n.at, tzMinutes, dayStartMin, text) + '\n' + stopLine(n.stop);
}

bool inQuiet(const QuietHours& q, uint64_t now, int16_t tzMinutes) {
  if (!q.respected) return false;
  if (now == 0) return true;
  const int64_t local = static_cast<int64_t>(now) + static_cast<int64_t>(tzMinutes) * 60;
  const uint16_t minute = static_cast<uint16_t>(((local % 86400) + 86400) % 86400 / 60);
  return orch::inQuietHours(minute, q.endMinute);
}

uint64_t quietEndAfter(const QuietHours& q, uint64_t now, int16_t tzMinutes) {
  if (!now || !inQuiet(q, now, tzMinutes)) return now;
  const int64_t local = static_cast<int64_t>(now) + static_cast<int64_t>(tzMinutes) * 60;
  const int64_t sec = ((local % 86400) + 86400) % 86400;
  const int64_t end = static_cast<int64_t>(q.endMinute % 1440) * 60;
  const int64_t wait = ((end - sec) % 86400 + 86400) % 86400;
  return now + static_cast<uint64_t>(wait ? wait : 86400);
}

// ---- the outbox -------------------------------------------------------------------------
uint64_t HeldNote::key() const { return fnv64(routine + '\n' + stop + '\n' + text); }

const HeldNote* HeldStore::find(const std::string& id) const {
  for (const HeldNote& n : notes_)
    if (n.id == id) return &n;
  return nullptr;
}

namespace {

void noteToJson(const HeldNote& n, JsonObject o) {
  o["id"] = n.id;
  o["r"] = n.routine;
  o["n"] = n.name;
  o["c"] = n.chatId;
  o["at"] = n.at;
  o["exp"] = n.expires;
  o["x"] = n.text;
  if (!n.stop.empty()) o["stop"] = n.stop;
  if (n.outside) o["o"] = true;
  o["s"] = static_cast<int>(n.state);
}

bool noteFromJson(JsonObjectConst o, HeldNote& n) {
  if (!o["id"].is<const char*>() || !o["x"].is<const char*>()) return false;
  n.id = o["id"].as<const char*>();
  n.routine = o["r"] | "";
  n.name = o["n"] | "";
  n.chatId = o["c"] | "";
  n.at = o["at"] | static_cast<uint64_t>(0);
  n.expires = o["exp"] | static_cast<uint64_t>(0);
  n.text = o["x"].as<const char*>();
  n.stop = o["stop"] | "";
  n.outside = o["o"] | false;
  const int s = o["s"] | 1;
  n.state = s == 2 ? HeldState::Sent : HeldState::Ready;   // a Pending note's fire is gone
  return !n.id.empty();
}

}  // namespace

Status HeldStore::load() {
  notes_.clear();
  keys_.clear();
  if (!file_.load) return Status::fail(Err::InvalidArgument, "plain::HeldStore::load");
  const std::string text = file_.load();
  if (text.empty()) return Status::success();
  JsonDocument d;
  if (deserializeJson(d, text)) {
    hlog::logf("routines: the held-note file does not read; starting empty");
    return Status::success();
  }
  for (JsonObjectConst o : d["notes"].as<JsonArrayConst>()) {
    HeldNote n;
    if (noteFromJson(o, n) && notes_.size() < kHeldNotesMax) notes_.push_back(n);
  }
  for (JsonObjectConst k : d["keys"].as<JsonArrayConst>())
    if (k["k"].is<const char*>()) keys_.push_back(Delivered{std::strtoull(k["k"].as<const char*>(), nullptr, 16), k["t"] | (uint64_t)0});
  return Status::success();
}

Status HeldStore::save() {
  if (!file_.save) return Status::fail(Err::InvalidArgument, "plain::HeldStore::save");
  JsonDocument d;
  d["v"] = 1;
  JsonArray notes = d["notes"].to<JsonArray>();
  for (const HeldNote& n : notes_) noteToJson(n, notes.add<JsonObject>());
  JsonArray keys = d["keys"].to<JsonArray>();
  for (const Delivered& k : keys_) {
    JsonObject o = keys.add<JsonObject>();
    o["k"] = hex64(k.key);
    o["t"] = k.at;
  }
  std::string out;
  serializeJson(d, out);
  return file_.save(out) ? Status::success() : Status::fail(Err::Io, "plain::HeldStore::save");
}

Status HeldStore::put(const HeldNote& n) {
  for (HeldNote& h : notes_)
    if (h.id == n.id) {
      h = n;
      return save();
    }
  if (notes_.size() >= kHeldNotesMax) return Status::fail(Err::TooLarge, "plain::HeldStore::put");
  notes_.push_back(n);
  return save();
}

Status HeldStore::remove(const std::string& id) {
  const size_t before = notes_.size();
  notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const HeldNote& n) { return n.id == id; }),
               notes_.end());
  return notes_.size() == before ? Status::fail(Err::NotFound, "plain::HeldStore::remove") : save();
}

Status HeldStore::done(const std::string& id, uint64_t key, uint64_t at) {
  notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const HeldNote& n) { return n.id == id; }),
               notes_.end());
  keys_.push_back(Delivered{key, at});
  while (keys_.size() > kDeliveredKeysMax) keys_.erase(keys_.begin());
  return save();
}

bool HeldStore::wasDelivered(const HeldNote& n) const {
  const uint64_t k = n.key();
  return std::any_of(keys_.begin(), keys_.end(), [&](const Delivered& d) { return d.key == k && d.at >= n.at; });
}

bool HeldStore::dropExpired(uint64_t now) {
  const size_t before = notes_.size();
  notes_.erase(std::remove_if(notes_.begin(), notes_.end(),
                              [&](const HeldNote& n) { return n.state != HeldState::Sent && n.expires <= now; }),
               notes_.end());
  if (notes_.size() == before) return false;
  (void)save();
  return true;
}

// ---- the owner's transcript -------------------------------------------------------------
namespace {

uint32_t newestTurn(const tx::TxArena& arena) {
  tx::ArenaEntry e;
  if (arena.count() && arena.header(arena.count() - 1, e).ok() && e.turn) return e.turn;
  return 1;
}

// The note's text in the item: whole, or past kNoteInlineMax its head and a pointer to the whole
// in the chat's bulk file.
Status noteBody(tx::TxLog& log, tx::Item& it, const std::string& note) {
  if (note.size() <= kNoteInlineMax) {
    it.x = note;
    return Status::success();
  }
  Status st = log.appendBulk(it.turn, note, it.handle);
  if (!st.ok()) return st;
  // E2-FIX5: no turn (and so no registry) here: the head names no view call until one is bound.
  return tx::longTextHead(note, tx::kInlineMessageCap, tx::viewPointer(it.q, tx::BoundTools()), it.x, true);
}

}  // namespace

Status appendOwnerNote(PlainEngine& owner, const std::string& note, uint64_t at) {
  tx::HeadState* head = owner.head();
  tx::TxLog* log = owner.deps().log;
  tx::TxArena* arena = owner.deps().arena;
  if (!head || !log || !arena || note.empty()) return Status::fail(Err::InvalidArgument, "plain::appendOwnerNote");
  tx::Item it;
  it.kind = tx::ItemKind::DeviceNote;
  it.note = tx::NoteClass::Routine;
  it.q = std::max({head->nextQ, arena->lastQ() + 1, log->lastQ() + 1});
  it.turn = newestTurn(*arena);
  it.t = at;
  Status st = noteBody(*log, it, note);
  if (!st.ok()) return st;
  tx::StoreState state = tx::StoreState::Stored;
  st = log->store(it, *arena, state);
  Status out = st;
  if (!st.ok() && st.err == Err::TooLarge) {   // the arena is full: the card keeps it, the next fill brings it
    tx::StoreState s2 = tx::StoreState::NotStored;
    const bool onCard = log->append(it, s2).ok() && s2 == tx::StoreState::Stored;
    out = onCard ? Status::fail(Err::TooLarge, "plain::appendOwnerNote.arena") : Status::fail(Err::Io, "plain::appendOwnerNote");
  }
  if (st.ok() || out.err == Err::TooLarge) head->nextQ = it.q + 1;
  if (log->isOpen()) (void)log->writeHead(*head);
  return out;
}

// ---- delivery of one note ---------------------------------------------------------------
Status deliverHeld(HeldStore& held, const OwnerSink& owner, const std::string& id, uint64_t now, int16_t tzMinutes,
                   uint16_t dayStartMin) {
  const HeldNote* found = held.find(id);
  if (!found) return Status::fail(Err::NotFound, "plain::deliverHeld");
  HeldNote n = *found;
  if (!owner.deliver || !owner.append) return Status::fail(Err::InvalidArgument, "plain::deliverHeld");
  if (n.state != HeldState::Sent) {
    TurnCtx ctx;
    ctx.chatKey = owner.chatKey;
    ctx.chatId = n.chatId.empty() ? owner.chatId : n.chatId;
    ctx.trigger = Trigger::Routine;
    const std::string line = n.stop.empty() ? std::string() : stopLine(n.stop);
    DeliverInput in;
    in.turn = &ctx;
    if (!n.text.empty()) in.text = &n.text;
    if (!line.empty()) {
      in.status = StatusLine::LimitReached;
      in.statusText = &line;
    }
    // P14 (lane E2-FIX2B): Sent is stored before the send, so a cut right after it never sends the
    // note twice; what is owed then is only the append (the note still reaches his transcript).
    HeldNote sending = n;
    sending.state = HeldState::Sent;
    (void)held.put(sending);
    DeliverOutcome o;
    (void)owner.deliver(in, o);
    // The text is the note: it must have gone (a stop line alone, when there is no text). Stays; tried again.
    if (n.text.empty() ? !o.statusSent : !o.sent) {
      (void)held.put(n);   // back to its state before: tried again
      return Status::fail(Err::Io, "plain::deliverHeld.send");
    }
    n.state = HeldState::Sent;
  }
  const Status st = owner.append(heldNoteText(n, tzMinutes, dayStartMin), n.at);
  if (!st.ok() && st.err != Err::TooLarge) return st;   // owed: the idle step appends it later
  (void)held.done(n.id, n.key(), now);
  return Status::success();
}

// ---- one fire ---------------------------------------------------------------------------
namespace {

// Each fire starts from its state alone: the routine's earlier fires stay on the card, below the
// fill's start. The next turn number is returned (above every stored item's turn: the next q).
Status freshStart(tx::TxLog& log, uint32_t& turn) {
  tx::HeadState h;
  Status st = log.open(h);
  if (st.err == Err::Io) return Status::success();   // no card: the engine runs from RAM, nothing older to load
  if (!st.ok()) return st;
  const uint64_t last = std::max(h.nextQ, log.lastQ() + 1) - 1;
  turn = static_cast<uint32_t>(std::min<uint64_t>(last + 1, UINT32_MAX));
  if (last == 0 || h.thru >= last) return Status::success();
  h.thru = last;
  h.nextQ = last + 1;
  h.openTurn = 0;
  return log.writeHead(h);
}

struct Fire {
  explicit Fire(const RoutineDeps& d, const RoutineFire& f) : deps(d), fire(f) {}
  const RoutineDeps& deps;
  RoutineFire fire;
  std::string key;
  std::vector<std::string> allowed;
  orch::ToolRegistry reg;
  tx::TxArena arena;
  std::unique_ptr<tx::TxLog> log;
  PlainHooks hooks;
  std::unique_ptr<PlainEngine> engine;
  HeldNote note;
  bool captured = false;
  TurnInput input;   // the fire's turn, here and not on the tg_poll stack (lane E2-FIX2B)

  std::string noteId() const { return fire.def.id + "-" + std::to_string(fire.fireAt); }

  DispatchDecision policy(const DispatchCtx& c) const {
    DispatchDecision d;
    if (!c.call || !c.turn) return d;
    const ToolCall& tc = c.target ? *c.target : *c.call;
    // A router call is refused whatever it names: its target is resolved against this fire's
    // registry, but the router itself would run against the device's whole one.
    if (!listedName(allowed, c.call->name) || !listedName(allowed, tc.name)) {
      d.verdict = DispatchVerdict::Refuse;
      d.text = reg.unknownToolText(tc.name, c.turn->who ? *c.turn->who : orch::Principal());
      return d;
    }
    JsonDocument a;
    if (deserializeJson(a, tc.argsJson) || !a.is<JsonObject>()) a.to<JsonObject>();
    // A write reaches a listed path only (orch::pathListed refuses any path holding "..", so no
    // separator, '/' or '\\', can climb out of a listed folder).
    const std::string r = orch::unattendedRefusal(routineFacts(fire, c.turn->tainted), tc.name, a.as<JsonObjectConst>());
    if (!r.empty()) {
      d.verdict = DispatchVerdict::Refuse;
      d.text = r;
    }
    return d;
  }

  // The fire's delivery: the reply goes into the outbox first (durable), never straight out.
  Status capture(const DeliverInput& in, DeliverOutcome& out) {
    if (in.text && !in.text->empty() && !captured) {
      captured = true;
      note.id = noteId();
      note.routine = fire.def.id;
      note.name = fire.def.name;
      note.chatId = deps.owner.chatId;
      note.at = fire.fireAt;
      note.expires = fire.fireAt + (fire.def.holdS ? fire.def.holdS : kHeldNoteExpiryS);
      note.text = *in.text;
      note.state = HeldState::Pending;
      (void)deps.held->put(note);   // durable before the turn closes; finish() puts it again, final
      out.sent = out.replyDone = true;
      out.parts = 1;
      out.bytes = static_cast<uint32_t>(in.text->size());
    }
    if (in.status != StatusLine::None) out.statusSent = true;   // the binding hears it (FireReport)
    return Status::success();
  }

  EngineDeps engineDeps() {
    EngineDeps e;
    e.platform = deps.platform;
    e.transport = deps.transport;
    e.hooks = &hooks;
    e.log = log.get();
    e.arena = &arena;
    e.files = deps.files;
    e.chatKey = key;
    e.dispatch = deps.dispatch;
    e.setup = [this](const TurnCtx& t, TurnSetup& out) {
      const Status st = deps.setup(t, out);
      if (!st.ok()) return st;
      out.facts.reg = &reg;
      out.facts.digestBlock.clear();
      out.facts.recentFiles.clear();
      out.ceiling = routineCeiling();
      return Status::success();
    };
    e.deliver = [this](const DeliverInput& in, DeliverOutcome& out) { return capture(in, out); };
    e.noCompaction = "routine-fires-start-from-their-state";   // nothing older is resident to compact
    return e;
  }

  Status build() {
    Status st = routineChatKey(fire.def.id, key);
    if (!st.ok()) return st;
    allowed = routineTools(fire);
    if (!(st = routineRegistry(*deps.reg, fire, reg)).ok()) return st;
    tx::ArenaAllocFn alloc = deps.platform->allocLarge ? deps.platform->allocLarge : [](size_t n) { return std::malloc(n); };
    tx::ArenaFreeFn release = deps.platform->freeLarge ? deps.platform->freeLarge : [](void* p) { std::free(p); };
    if (!(st = arena.init(deps.arenaBytes, alloc, release)).ok()) return st;
    log.reset(new tx::TxLog(*deps.files, key));
    if (deps.moreHooks) deps.moreHooks(hooks);
    return hooks.beforeDispatch(HookSlot::Policy, [this](const DispatchCtx& c) { return policy(c); });
  }
};

bool depsOk(const RoutineDeps& d) {
  return d.platform && d.platform->nowMs && d.transport && d.files && d.reg && d.dispatch && d.setup && d.held &&
         d.owner.deliver && d.owner.append;
}

// The fire's note, final: the limit it stopped at, or nothing at all (silence).
FireResult finish(Fire& f, const TurnOutcome& o, uint64_t now, FireReport& out) {
  const bool tripped = o.trip != GuardTrip::None;
  if (!f.captured && !tripped) return FireResult::Silent;
  if (!f.captured) {
    f.note.id = f.noteId();
    f.note.routine = f.fire.def.id;
    f.note.name = f.fire.def.name;
    f.note.chatId = f.deps.owner.chatId;
    f.note.at = f.fire.fireAt;
    f.note.expires = f.fire.fireAt + (f.fire.def.holdS ? f.fire.def.holdS : kHeldNoteExpiryS);
  }
  if (tripped) f.note.stop = guardTripSlug(o.trip);
  f.note.outside = std::find(o.end.events.begin(), o.end.events.end(), "change:outside") != o.end.events.end();
  f.note.state = HeldState::Ready;
  // P8: held for quiet hours, it lives its whole hold after they end.
  const uint64_t hold = f.fire.def.holdS ? f.fire.def.holdS : kHeldNoteExpiryS;
  f.note.expires = std::max(f.note.expires, quietEndAfter(f.deps.quiet, now, f.deps.tzMinutes) + hold);
  out.noteId = f.note.id;
  (void)f.deps.held->dropExpired(now);   // room for this one before the outbox counts as full
  const Status put = f.deps.held->put(f.note);
  if (put.ok() && inQuiet(f.deps.quiet, now, f.deps.tzMinutes)) return FireResult::Held;
  // The outbox refused it: never lost, it goes now, quiet hours or not. Storage refused the save:
  // the note is in the outbox's memory and goes through it. The outbox is full: on its own.
  if (!put.ok()) hlog::logf("routines: %s: the outbox refused the note; it goes now", f.key.c_str());
  if (put.err == Err::TooLarge) {
    HeldStore once(HeldFile{[] { return std::string(); }, [](const std::string&) { return true; }});
    (void)once.put(f.note);
    out.status = deliverHeld(once, f.deps.owner, f.note.id, now, f.deps.tzMinutes, f.deps.head.dayStartMin);
    return out.status.ok() ? FireResult::Sent : FireResult::Failed;
  }
  out.status = deliverHeld(*f.deps.held, f.deps.owner, f.note.id, now, f.deps.tzMinutes, f.deps.head.dayStartMin);
  return out.status.ok() ? FireResult::Sent : FireResult::Held;
}

}  // namespace

Status runRoutine(const RoutineDeps& deps, const RoutineFire& fire, FireReport& out) {
  out = FireReport{};
  if (!depsOk(deps) || !fire.fireAt) return out.status = Status::fail(Err::InvalidArgument, "plain::runRoutine");
  try {
    std::unique_ptr<Fire> f(new Fire(deps, fire));
    Status st = f->build();
    uint32_t turn = 1;
    if (st.ok()) st = freshStart(*f->log, turn);
    if (st.ok()) {
      f->engine.reset(new PlainEngine(f->engineDeps()));
      const uint64_t now = deps.platform->nowEpoch ? deps.platform->nowEpoch() : fire.fireAt;
      st = f->engine->boot(BootCtx{now, false});
    }
    if (!st.ok()) {
      out.result = FireResult::Failed;
      return out.status = st;
    }
    TurnInput& in = f->input;
    in.ctx = deps.head;
    in.ctx.chatKey = f->key;
    in.ctx.chatId = deps.owner.chatId;
    in.ctx.trigger = Trigger::Routine;
    in.ctx.turn = turn;
    in.tzMinutes = deps.tzMinutes;
    in.text = routineInputText(f->fire, deps.tzMinutes, deps.head.dayStartMin);
    st = f->engine->runTurn(in, out.outcome);
    out.output = RoutineOutput{fire.fireAt, f->captured ? f->note.text : std::string()};
    const uint64_t now = deps.platform->nowEpoch ? deps.platform->nowEpoch() : fire.fireAt;
    out.result = finish(*f, out.outcome, now, out);
    if (!st.ok() && out.result == FireResult::Silent) out.result = FireResult::Failed;
    if (!st.ok() && out.status.ok()) out.status = st;
    return out.status;
  } catch (const std::bad_alloc&) {
    out.result = FireResult::Failed;
    return out.status = Status::fail(Err::TooLarge, "plain::runRoutine.memory");
  }
}

// ---- the owner engine's seam -------------------------------------------------------------
namespace {

// One unit of the outbox's work, oldest first: drop, finish an append, or send.
bool outboxStep(const UnattendedDeps& d, const IdleCtx& c) {
  for (const HeldNote& n : d.held->notes()) {
    if (n.state != HeldState::Sent && (n.expires <= c.now || d.held->wasDelivered(n))) {
      hlog::logf("routines: held note %s dropped (%s)", n.id.c_str(), n.expires <= c.now ? "expired" : "delivered");
      (void)d.held->remove(n.id);
      return true;
    }
    const bool mine = n.chatId.empty() || n.chatId == d.owner.chatId;
    if (!mine) continue;
    // Worked only when it went: a channel that keeps refusing never starves the later idle slots.
    if (n.state == HeldState::Sent || !inQuiet(d.quiet, c.now, c.tzMinutes)) {
      const std::string id = n.id;
      if (deliverHeld(*d.held, d.owner, id, c.now, c.tzMinutes, d.dayStartMin).ok()) return true;
    }
  }
  return false;
}

}  // namespace

Status installUnattended(PlainHooks& hooks, const UnattendedDeps& deps) {
  if (!deps.held || !deps.owner.deliver || !deps.owner.append)
    return Status::fail(Err::InvalidArgument, "plain::installUnattended");
  Status st = hooks.bootResume(HookSlot::Routines, [deps](const BootCtx&) { return deps.held->load(); });
  if (!st.ok()) return st;
  return hooks.idleStep(HookSlot::Routines, [deps](const IdleCtx& c) {
    IdleOutcome o;
    if (!c.clockSet || !c.now) return o;
    if (outboxStep(deps, c)) {
      o.worked = true;
      return o;
    }
    RoutineFire fire;
    if (!deps.due || !deps.fireDeps || !deps.due(c.now, fire)) return o;
    FireReport rep;
    (void)runRoutine(deps.fireDeps(), fire, rep);
    if (deps.fired) deps.fired(rep);
    o.worked = true;
    return o;
  });
}

}  // namespace plain
}  // namespace agent
