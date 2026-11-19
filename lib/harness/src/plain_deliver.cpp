// The delivery seam (plain/deliver.h), lane RB-E3: one delivery through the device's deliver
// function, the closed list of harness-written lines, the e item, and the dossier by reference.
#include "nimbus/harness/plain/deliver.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "nimbus/harness/channel.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/orch/deliver_text.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;

namespace {

constexpr uint32_t kFnvBasis = 2166136261u;
constexpr size_t kCauseMax = 64;   // the e item's cause is a slug

uint32_t fnv1a(uint32_t h, const void* data, size_t n) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
uint32_t fnv1a(uint32_t h, const std::string& s) { return fnv1a(h, s.data(), s.size()); }
uint32_t fnv1aU64(uint32_t h, uint64_t v) {
  unsigned char b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(v >> (8 * i));
  return fnv1a(h, b, sizeof b);
}

bool listed(StatusLine s) {
  for (StatusLine l : kAllStatusLines)
    if (l == s) return true;
  return false;
}

// A text the device's deliver function sends as itself, one part: nothing for the sanitiser
// (cleanReplyMarkup returns a text with no '<' unchanged) and within one part. telegramParts
// gives exactly [text] for it (or nothing when it is blank), so it goes out with no copy
// (ruling 59 item 1: a refused allocation must not cost the reply in hand).
bool onePart(const std::string& text) {
  return text.size() <= nimbus::orch::kTelegramPartBytes && text.find('<') == std::string::npos;
}
bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

// Part `i` of the reply, unless an earlier attempt sent it (`fromPart`). Progress is recorded
// the moment it went out; a refused part ends the reply (a later one would arrive out of order).
Status sendPart(const DeliverDeps& deps, const std::string& chat, const std::string& part, size_t i, uint16_t fromPart,
                DeliverOutcome& out) {
  if (i < fromPart) return Status::success();
  if (!deps.channel->send(chat, part)) return Status::fail(Err::Io, "plain::deliverTurn.part");
  out.sent = true;
  out.parts = static_cast<uint16_t>(std::max<size_t>(out.parts, i + 1));
  out.bytes += static_cast<uint32_t>(part.size());
  return Status::success();
}

// ---- the e item's bound ----------------------------------------------------------------
// Counts the bytes of an encoded line without keeping them (no allocation).
class CountingSink : public tx::LineSink {
 public:
  void put(const char*, size_t) override {}
};

// Measured with the widest q and time the store may give it (put() sets both after this).
bool fits(tx::Item& e) {
  const uint64_t q = e.q, t = e.t;
  e.q = e.t = UINT64_MAX;
  CountingSink sink;
  size_t bytes = 0;
  const tx::Status st = tx::encodeLineTo(e, sink, bytes);
  e.q = q;
  e.t = t;
  return st.ok() && bytes <= tx::kMaxLineBytes;
}

bool marker(const std::string& ev) { return ev == "events:cut" || ev == "exchanges:cut" || ev == "dossier:shed"; }

// The events the e item keeps: the first `keep` of `all`, within the list cap with room for the
// markers that say what was cut.
void setEvents(const std::vector<std::string>& all, size_t keep, bool exchangesCut, bool shed, std::vector<std::string>& out) {
  const size_t want = std::min(keep, all.size());
  const size_t others = (exchangesCut ? 1 : 0) + (shed ? 1 : 0);
  const bool eventsCut = want < all.size() || want + others > tx::kMaxListItems;
  const size_t n = std::min(want, tx::kMaxListItems - others - (eventsCut ? 1 : 0));
  out.assign(all.begin(), all.begin() + static_cast<long>(n));
  if (eventsCut) out.emplace_back("events:cut");
  if (exchangesCut) out.emplace_back("exchanges:cut");
  if (shed) out.emplace_back("dossier:shed");
}

// An omission line kept once moves on to the next exchange that read it from this one.
void handOnOmission(std::vector<tx::ExchangeFacts>& xs, size_t i) {
  if (i + 1 < xs.size() && xs[i + 1].omitThrough && xs[i + 1].omission.empty()) xs[i + 1].omission.swap(xs[i].omission);
}

bool shedOneReference(std::vector<tx::ExchangeFacts>& xs) {
  for (size_t i = 0; i < xs.size(); ++i)
    if (xs[i].bodyHash) {
      handOnOmission(xs, i);
      const tx::ExchangeFacts kept{xs[i].requestBytes, xs[i].ms, xs[i].httpStatus};
      xs[i] = kept;
      return true;
    }
  return false;
}

// The oldest exchange leaves (once every reference is shed). False when none is left.
bool dropOldestExchange(std::vector<tx::ExchangeFacts>& xs) {
  if (xs.empty()) return false;
  xs.erase(xs.begin());   // holds no reference by now
  return true;
}

}  // namespace

// ---- delivery --------------------------------------------------------------------------------
Status deliverTurn(const DeliverDeps& deps, const DeliverInput& in, DeliverOutcome& out) {
  if (!deps.channel || !deps.channel->send || !in.turn) return Status::fail(Err::InvalidArgument, "plain::deliverTurn");
  const std::string& chat = in.turn->chatId;
  Status st = Status::success();
  if (in.text && !out.replyDone) {
    if (onePart(*in.text)) {   // the reply as it is: nothing allocated on the way out
      if (!blank(*in.text)) st = sendPart(deps, chat, *in.text, 0, in.fromPart, out);
    } else {   // the device's own function: the sanitiser, then the split and the part bound
      const std::vector<std::string> parts = nimbus::orch::telegramParts(*in.text);
      for (size_t i = 0; st.ok() && i < parts.size(); ++i) st = sendPart(deps, chat, parts[i], i, in.fromPart, out);
    }
    if (st.ok()) out.replyDone = true;
  }
  if (!in.statusText || blank(*in.statusText) || out.statusSent) return st;   // no empty message
  if (!listed(in.status)) return Status::fail(Err::InvalidArgument, "plain::deliverTurn.unlisted");
  const std::string& line = *in.statusText;
  bool sent = false;
  if (line.size() <= nimbus::orch::kTelegramPartBytes) {
    sent = deps.channel->send(chat, line);
  } else {   // one message, never two: its first part
    const std::vector<std::string> head = nimbus::orch::splitDeliverable(line);
    sent = deps.channel->send(chat, head.front());
  }
  if (sent) out.statusSent = true;
  else st = Status::fail(Err::Io, "plain::deliverTurn.status");
  return st;
}

// ---- the e item --------------------------------------------------------------------------------
Status turnEndItem(const TurnCtx& turn, const tx::TurnEndFacts& facts, tx::Item& out) {
  if (out.kind != tx::ItemKind::TurnEnd || !turn.turn) return Status::fail(Err::InvalidArgument, "plain::turnEndItem");
  if (!out.turn) out.turn = turn.turn;
  tx::TurnEndFacts& e = out.end;
  e = facts;
  if (e.cause.size() > kCauseMax) e.cause.resize(kCauseMax);
  std::vector<std::string> events;
  events.swap(e.events);
  bool exchangesCut = false, shed = false;
  if (e.exchanges.size() > tx::kMaxListItems) {   // the newest are kept
    e.exchanges.erase(e.exchanges.begin(), e.exchanges.end() - static_cast<long>(tx::kMaxListItems));
    exchangesCut = true;
  }
  // An omission line the exchange before carried already is kept once: an empty line under a
  // set omitThrough reads as the one before it (rebuildRequest).
  for (size_t i = e.exchanges.size(); i-- > 1;)
    if (e.exchanges[i].omitThrough && e.exchanges[i].omission == e.exchanges[i - 1].omission) e.exchanges[i].omission.clear();
  const bool hadShed = std::find(events.begin(), events.end(), "dossier:shed") != events.end();
  size_t keep = events.size();
  setEvents(events, keep, exchangesCut, false, e.events);
  while (!fits(out)) {
    if (shedOneReference(e.exchanges)) {
      shed = true;
    } else if (dropOldestExchange(e.exchanges)) {
      exchangesCut = true;
    } else if (keep) {
      keep /= 2;
    } else if (!e.cause.empty()) {
      e.cause.clear();   // nothing else is left that could be long
    } else {
      return Status::fail(Err::InvalidArgument, "plain::turnEndItem.encode");   // nothing left to shed
    }
    setEvents(events, keep, exchangesCut, shed && !hadShed, e.events);
  }
  return Status::success();
}

bool shedReference(tx::Item& e) {
  if (e.kind != tx::ItemKind::TurnEnd || !shedOneReference(e.end.exchanges)) return false;
  std::vector<std::string>& ev = e.end.events;
  if (std::find(ev.begin(), ev.end(), "dossier:shed") == ev.end()) {
    if (ev.size() >= tx::kMaxListItems) {   // room from the newest plain event, never from a marker
      auto last = std::find_if(ev.rbegin(), ev.rend(), [](const std::string& x) { return !marker(x); });
      if (last == ev.rend()) return true;
      ev.erase(std::next(last).base());
      if (std::find(ev.begin(), ev.end(), "events:cut") == ev.end()) {
        ev.emplace_back("events:cut");
        return true;   // the cut marker took the room; the shed itself is named by the missing hash
      }
    }
    ev.emplace_back("dossier:shed");
  }
  return true;
}

// ---- the dossier by reference ------------------------------------------------------------------
uint32_t promptHashOf(const SystemBlocks& system) {
  return fnv1a(fnv1a(fnv1a(kFnvBasis, system.core), system.notes), system.digests);
}

uint32_t manifestHashOf(const std::vector<ToolSpec>& tools) {
  uint32_t h = kFnvBasis;
  for (const ToolSpec& s : tools) h = fnv1a(fnv1a(fnv1a(h, s.name), s.description), s.schemaJson);
  return h;
}

uint32_t requestHashOf(const HttpRequest& req) {
  uint32_t h = fnv1a(kFnvBasis, req.path);
  h = fnv1aU64(h, req.body.size());
  h = fnv1a(h, req.body);
  for (const BodySplice& s : req.splices) {
    h = fnv1aU64(fnv1aU64(fnv1aU64(h, s.at), s.len), s.base64 ? 1 : 0);
    if (s.bytes) h = fnv1a(h, s.bytes, s.len);
  }
  return h ? h : 1;   // 0 means "no reference"
}

void recordRequest(const RenderInput& in, const HttpRequest& sent, tx::ExchangeFacts& out) {
  out.fromQ = out.thruQ = 0;
  out.items = 0;
  tx::ArenaEntry first, last;
  if (in.items && in.items->count() && in.items->header(0, first).ok() &&
      in.items->header(in.items->count() - 1, last).ok()) {
    out.fromQ = first.q;   // every item the engine held: the request rendered them all
    out.thruQ = last.q;
    out.items = static_cast<uint32_t>(in.items->count());
  }
  out.stub = in.stubPointer;
  const LadderState none;
  const LadderState& l = in.ladder ? *in.ladder : none;
  out.closedThrough = l.closedThrough;
  out.stubThrough = l.stubThrough;
  out.omitThrough = l.omitThrough;
  out.omission = l.omissionLine;
  out.maxTokens = in.maxTokens;
  out.toolChoiceNone = in.toolChoiceNone;
  out.effort = in.head ? in.head->effort : std::string();
  out.bodyHash = requestHashOf(sent);
}

tx::ExchangeFacts exchangeFacts(const ExchangeResult& ex) {
  tx::ExchangeFacts f = ex.ref;
  f.requestBytes = ex.requestBytes;
  f.ms = ex.ms;
  f.httpStatus = static_cast<uint16_t>(ex.httpStatus);
  return f;
}

Status rebuildRequest(const RebuildInput& in, HttpRequest& out) {
  if (!in.end || !in.turn || !in.head || !in.wire || !in.system || !in.tools || !in.items ||
      in.exchange >= in.end->exchanges.size() || !in.end->exchanges[in.exchange].bodyHash)
    return Status::fail(Err::InvalidArgument, "plain::rebuildRequest");
  const tx::ExchangeFacts& ref = in.end->exchanges[in.exchange];
  if (promptHashOf(*in.system) != in.end->promptHash) return Status::fail(Err::NotFound, "plain::rebuildRequest.prompt");
  if (manifestHashOf(*in.tools) != in.end->manifestHash) return Status::fail(Err::NotFound, "plain::rebuildRequest.manifest");

  // The range, line for line as stored, into a block of its own: exactly the items the request
  // rendered, never fewer (owner ruling 81).
  size_t bytes = tx::kArenaMinBytes, n = 0;
  tx::ArenaEntry h;
  for (size_t i = 0; i < in.items->count(); ++i)
    if (in.items->header(i, h).ok() && h.q >= ref.fromQ && h.q <= ref.thruQ) {
      bytes += tx::recordBytes(h.lineBytes);
      ++n;
    }
  if (n != ref.items) return Status::fail(Err::NotFound, "plain::rebuildRequest.items");
  tx::TxArena range;
  Status st = in.alloc ? range.init(bytes, in.alloc, in.release)
                       : range.init(bytes, [](size_t k) { return std::malloc(k); }, [](void* p) { std::free(p); });
  if (!st.ok()) return st;
  for (size_t i = 0; st.ok() && i < in.items->count(); ++i) {
    const char* line = nullptr;
    size_t len = 0;
    if (!(st = in.items->header(i, h)).ok() || h.q < ref.fromQ || h.q > ref.thruQ) continue;
    if ((st = in.items->line(i, line, len)).ok()) st = range.appendLine(line, len);
  }
  if (!st.ok()) return st;
  tx::ArenaEntry first, last;
  if (n && (!range.header(0, first).ok() || !range.header(n - 1, last).ok() || first.q != ref.fromQ ||
            last.q != ref.thruQ))
    return Status::fail(Err::NotFound, "plain::rebuildRequest.range");

  TurnCtx turn = *in.turn;
  turn.roundNo = static_cast<uint16_t>(in.exchange);
  HeadEndpoint head = *in.head;
  head.effort = ref.effort;
  LadderState ladder;
  ladder.closedThrough = ref.closedThrough;
  ladder.stubThrough = ref.stubThrough;
  ladder.omitThrough = ref.omitThrough;
  ladder.omissionLine = ref.omission;
  for (size_t i = in.exchange; ladder.omitThrough && ladder.omissionLine.empty() && i-- > 0;)
    ladder.omissionLine = in.end->exchanges[i].omission;   // kept once, on the first that carried it
  RenderInput r;
  r.turn = &turn;
  r.head = &head;
  r.system = in.system;
  r.tools = in.tools;
  r.items = &range;
  r.stubPointer = ref.stub;
  r.toolChoiceNone = ref.toolChoiceNone;
  r.maxTokens = ref.maxTokens;
  r.bulk = in.bulk;
  r.photo = in.photo;
  r.ladder = &ladder;
  out = HttpRequest{};
  if (!(st = in.wire->render(r, out)).ok()) return st;
  if (requestHashOf(out) != ref.bodyHash) return Status::fail(Err::Torn, "plain::rebuildRequest.hash");
  return Status::success();
}

// ---- progress on a long turn -------------------------------------------------------------------
namespace {

std::string elapsedText(uint32_t ms) {
  const uint32_t s = ms / 1000;
  return s < 120 ? std::to_string(s) + " s" : std::to_string(s / 60) + " min";
}

// A tool's or guard's name as the owner may see it: a short machine name, else nothing.
std::string safeDetail(const std::string& d) {
  if (d.empty() || d.size() > 40) return std::string();
  for (char c : d)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return std::string();
  return d;
}

}  // namespace

std::string ProgressReporter::text(const ProgressEvent& e, uint32_t elapsedMs, uint16_t steps) {
  std::string line = "Still working (" + elapsedText(elapsedMs);
  if (steps) line += ", step " + std::to_string(steps);
  line += "): ";
  const std::string d = safeDetail(e.detail);
  switch (e.kind) {
    case ProgressKind::ToolRound: line += d.empty() ? "ran a tool." : "ran " + d + "."; break;
    case ProgressKind::ProviderWait: line += "waiting for the model's answer."; break;
    case ProgressKind::SoftGuard: line += "taking longer than usual, still making progress."; break;
  }
  return line;
}

std::string ProgressReporter::finalText(uint32_t elapsedMs, uint16_t steps) {
  std::string line = "Worked " + elapsedText(elapsedMs);
  if (steps) line += " (" + std::to_string(steps) + (steps == 1 ? " step)" : " steps)");
  return line + ".";
}

void ProgressReporter::open(const TurnCtx& turn, uint32_t startedMs) {
  chatId_ = turn.chatId;
  turn_ = turn.turn;
  startedMs_ = lastVisibleMs_ = startedMs;
  msgId_ = 0;
  haveMsg_ = false;
  lines_ = edits_ = steps_ = 0;
  last_.clear();
  open_ = true;
}

bool ProgressReporter::tick(const ProgressEvent& e) {
  if (!open_ || !deps_.send) return false;
  if (e.kind == ProgressKind::ToolRound) ++steps_;
  const uint32_t since = e.nowMs - lastVisibleMs_;
  bool due = false;
  if (!lines_) due = since >= kProgressFirstMs;
  else if (haveMsg_) due = since >= kProgressEditMs;
  else due = lines_ < kProgressMaxLines && since >= kProgressNewLineMs;
  if (!due) return false;
  try {
    return show(text(e, e.nowMs - startedMs_, steps_), e.nowMs);
  } catch (const std::bad_alloc&) {
    return false;
  }
}

bool ProgressReporter::show(const std::string& line, uint32_t nowMs) {
  if (line == last_) return false;
  if (haveMsg_) {
    if (!deps_.edit(chatId_, msgId_, line)) return false;
    ++edits_;
  } else {
    int64_t id = 0;
    if (!deps_.send(chatId_, line, id)) return false;
    ++lines_;
    msgId_ = id;
    haveMsg_ = id != 0 && static_cast<bool>(deps_.edit);
  }
  lastVisibleMs_ = nowMs;
  last_ = line;
  return true;
}

void ProgressReporter::close(uint32_t nowMs) {
  if (!open_) return;
  open_ = false;
  if (!haveMsg_) return;
  try {
    if (deps_.edit(chatId_, msgId_, finalText(nowMs - startedMs_, steps_))) ++edits_;
  } catch (const std::bad_alloc&) {
  }
}

Status installProgress(PlainHooks& hooks, ProgressReporter& r, std::function<uint32_t()> nowMs) {
  if (!nowMs) return Status::fail(Err::InvalidArgument, "plain::installProgress");
  Status st = hooks.afterResult(HookSlot::Observer, [&r, nowMs](const ResultCtx& c) {
    if (!c.turn) return;
    if (!r.isOpen() || r.turn() != c.turn->turn) r.open(*c.turn, c.turn->startedMs);
    ProgressEvent e;
    e.kind = ProgressKind::ToolRound;
    e.nowMs = nowMs();
    if (c.call) e.detail = c.call->name;
    (void)r.tick(e);
  });
  if (!st.ok()) return st;
  return hooks.afterDeliver(HookSlot::Observer, [&r, nowMs](const DeliveredCtx&) { r.close(nowMs()); });
}

}  // namespace plain
}  // namespace agent
