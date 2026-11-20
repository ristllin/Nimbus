// The truth check bound into the plain engine (plain/truth.h; lane RB-G1). The check is lib/core's
// (nimbus::orch::truth); this file reads its inputs from the transcript and maps it onto the seams.
#include "nimbus/harness/plain/truth.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/exchange.h"   // providerSlugOf: the head's host as the tables name it
#include "nimbus/orch/effect_ring.h"
#include "nimbus/orch/tool_groups.h"         // kMoreTool
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;
namespace fx = nimbus::orch::fx;
namespace truth = nimbus::orch::truth;
using truth::Kind;
using truth::Truth;

// ---- the receipt kind of a tool -----------------------------------------------------------
namespace {
struct KindRow {
  const char* tool;
  Kind kind;
};
// The engine-2 effect tools by what they change (the receipt line's noun). An effect tool with no
// row is Other; a speech changes nothing stored (Read), as on the old engine.
constexpr KindRow kKinds[] = {
    {"write_file", Kind::File},   {"append_file", Kind::File},   {"edit_file", Kind::File},
    {"delete_file", Kind::File},  {"move_file", Kind::File},     {"fetch_url", Kind::File},
    {"pdf_write", Kind::File},    {"image_generate", Kind::File}, {"note", Kind::Memory},
    {"skill_save", Kind::Memory}, {"skill_delete", Kind::Memory}, {"schedule", Kind::Routine},
    {"send", Kind::Send},         {"device_control", Kind::Setting}, {"speak", Kind::Read},
    {"share_file", Kind::Setting}, {"agent_start", Kind::Other},  {"agent_stop", Kind::Other},
    // A `more` call that reached no tool (a listing, an unknown or refused name) ran nothing.
    {nimbus::orch::kMoreTool, Kind::Read},
};
}  // namespace

Kind receiptKind(const std::string& tool, bool effect) {
  if (!effect) return Kind::Read;
  for (const KindRow& r : kKinds)
    if (tool == r.tool) return r.kind;
  return Kind::Other;
}

// ---- the transcript's receipts --------------------------------------------------------------
namespace {
// The tool a call reaches: a `more` call names its target in its arguments.
std::string targetOf(const tx::Item& c) {
  if (c.n != nimbus::orch::kMoreTool) return c.n;
  ArduinoJson::JsonDocument d;
  if (deserializeJson(d, c.args, ArduinoJson::DeserializationOption::NestingLimit(16))) return c.n;
  const char* name = d["name"] | static_cast<const char*>(nullptr);
  return name && *name ? std::string(name) : c.n;
}

std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }
}  // namespace

// A result answers the oldest open call of its turn with its id, else the oldest open call of its
// turn (dialects reuse or omit call ids). A new turn closes what the last one left open.
void ReceiptScan::add(const tx::Item& it, std::vector<Receipt>& out) {
  if (!open_.empty() && it.turn != open_.front().turn) open_.clear();
  if (it.kind == tx::ItemKind::Call) {
    open_.push_back(Open{it.turn, it.id, targetOf(it), it.eff});
    return;
  }
  if (it.kind != tx::ItemKind::Result || open_.empty()) return;
  auto c = std::find_if(open_.begin(), open_.end(), [&](const Open& o) { return !it.id.empty() && o.id == it.id; });
  if (c == open_.end()) c = open_.begin();
  const Open call = *c;
  open_.erase(c);
  if (!call.effect) return;
  Receipt r;
  r.q = it.q;
  r.turn = it.turn;
  r.epoch = it.t;
  r.tool = call.tool;
  r.ok = it.ok;
  r.text = firstLine(it.x.empty() ? it.st : it.x);
  out.push_back(std::move(r));
}

// ---- receipts from the card -------------------------------------------------------------------
namespace {
bool segmentList(tx::TxFiles& files, const std::string& chatKey, std::vector<uint32_t>& segs) {
  std::string dir;
  std::vector<std::string> names;
  if (!tx::txDir(chatKey, dir).ok() || !files.list(dir, names)) return false;
  for (const std::string& n : names) {
    unsigned k = 0;
    const bool seg = n.size() == 16 && n.compare(0, 4, "seg-") == 0 && n.compare(10, 6, ".jsonl") == 0;
    if (seg && std::sscanf(n.c_str() + 4, "%6u", &k) == 1 && k) segs.push_back(k);
  }
  std::sort(segs.begin(), segs.end());
  return true;
}

// The receipts of one segment's lines below beforeQ, in file order. False when it cannot be read.
bool segmentReceipts(tx::TxFiles& files, const std::string& path, uint64_t beforeQ, std::vector<Receipt>& out) {
  std::string buf, chunk(tx::kFillSliceBytes, '\0');
  ReceiptScan scan;
  tx::Item it;
  size_t off = 0, got = 0;
  while (files.readAt(path, off, &chunk[0], chunk.size(), got) && got) {
    off += got;
    buf.append(chunk.data(), got);
    size_t at = 0;
    for (size_t nl; (nl = buf.find('\n', at)) != std::string::npos; at = nl + 1) {
      tx::LineHeader h;
      if (!tx::checkLine(buf.data() + at, nl + 1 - at, h).ok() || h.q >= beforeQ) continue;
      if (h.kind != tx::ItemKind::Call && h.kind != tx::ItemKind::Result) continue;
      if (tx::decodeLine(buf.data() + at, nl + 1 - at, it).ok()) scan.add(it, out);
    }
    buf.erase(0, at);
  }
  return true;
}
}  // namespace

RecentSource recentFromLog(tx::TxFiles& files, const std::string& chatKey, size_t maxSegments) {
  return [&files, chatKey, maxSegments](uint64_t beforeQ, size_t want, std::vector<Receipt>& out, bool& whole) {
    out.clear();
    whole = false;
    std::vector<uint32_t> segs;
    if (!segmentList(files, chatKey, segs)) return false;
    size_t read = 0;
    for (size_t i = segs.size(); i-- > 0 && read < maxSegments && out.size() < want; ++read) {
      std::string path;
      std::vector<Receipt> part;
      if (!tx::segmentPath(chatKey, segs[i], path).ok() || !segmentReceipts(files, path, beforeQ, part)) return false;
      out.insert(out.begin(), part.begin(), part.end());
      whole = i == 0;
    }
    whole = whole || segs.empty();
    if (out.size() > want) out.erase(out.begin(), out.end() - (long)want);
    return true;
  };
}

// ---- the binding ------------------------------------------------------------------------------
namespace {

fx::RingEntry ringEntry(const Receipt& r) {
  fx::RingEntry e;
  e.epoch = (int64_t)r.epoch;
  const std::string tool = fx::cleanText(r.tool, fx::kToolMax), text = fx::cleanText(r.text, fx::kReceiptMax);
  std::memcpy(e.tool, tool.data(), tool.size());
  std::memcpy(e.receipt, text.data(), text.size());
  e.stored = true;
  return e;
}

// The turn's own effects and the owner's message, read from the arena in one pass.
struct TurnRead {
  std::vector<Receipt> receipts;   // every effect receipt resident, in q order
  std::string owner;               // the turn's own message (its u items, or its device note)
  int16_t tzMinutes = 0;
  uint64_t oldestQ = 0;            // the arena's oldest resident q (0: empty)
};

void readArena(const tx::TxArena& arena, uint32_t turn, TurnRead& out) {
  ReceiptScan scan;
  tx::Item it;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok()) continue;
    if (!out.oldestQ) out.oldestQ = e.q;
    const bool own = e.turn == turn && (e.kind == tx::ItemKind::User || e.kind == tx::ItemKind::DeviceNote);
    if (e.kind != tx::ItemKind::Call && e.kind != tx::ItemKind::Result && !own) continue;
    if (!arena.get(i, it).ok()) continue;
    if (!own) {
      scan.add(it, out.receipts);
    } else if (e.kind == tx::ItemKind::User || out.owner.empty()) {
      if (e.kind == tx::ItemKind::User) out.tzMinutes = it.tzMinutes;
      out.owner += (out.owner.empty() ? "" : "\n") + it.x;
    }
  }
}

class Binding {
 public:
  explicit Binding(const TruthDeps& d) : d_(d), state_(d.state ? d.state : &own_) {}

  DeliverDecision beforeDeliver(const DeliverCtx& c) {
    DeliverDecision out;
    if (!c.turn || !c.text) return out;
    const uint32_t sent = probes_;   // every probe this binding sent: the difference is this reply's
    try {
      if (c.held && open_ && turn_ == c.turn->turn) heldRound(*c.turn, *c.text, out);
      else firstAnswer(*c.turn, *c.text, !c.cut && !c.last && !c.held, out);
    } catch (const std::bad_alloc&) {   // a refused allocation skips the check: the reply goes out as it is
      out = DeliverDecision{};
    }
    out.probes = (uint8_t)std::min<uint32_t>(255u, probes_ - sent);
    report_.probes = (uint8_t)std::min<uint32_t>(255u, (uint32_t)report_.probes + out.probes);
    return out;
  }

  void afterDeliver(const DeliveredCtx& c) {
    if (!open_ || !c.turn || c.turn->turn != turn_) return;
    if (!finished_) close(c.text ? *c.text : std::string(), false);   // a held round that failed
    end();
  }

  IdleOutcome idle(const IdleCtx& c) {
    IdleOutcome o;
    if (!shadowDue_ || c.ownerWaiting) return o;
    shadowDue_ = false;
    shadow_.host = shadowHost_.c_str();
    truth::HeadState& h = state_->head(shadowKey_, shadowStart_);
    truth::shadowProbe(shadow_, counted(), h);
    (void)truth::nextMode(h, d_.allowShadow);
    if (d_.save) d_.save(*state_);
    o.worked = true;
    return o;
  }

 private:
  // A probe that counts what it sends: the e item's column.
  truth::ProbeFn counted() {
    if (!d_.probe) return nullptr;
    return [this](const truth::ProbeRequest& r) {
      ++probes_;
      return d_.probe(r);
    };
  }

  void firstAnswer(const TurnCtx& turn, const std::string& reply, bool canHold, DeliverDecision& out) {
    open(turn);
    const std::string item = check_.atFinal(reply, canHold);
    if (item.empty()) return close(reply, true, &out);
    report_.held = true;
    heldReply_ = reply;
    out.verdict = DeliverVerdict::Hold;
    out.deviceNote = item;
    showWait(turn);
  }

  // The held round answered (or, from the engine's failure path, the held reply itself goes out:
  // then the round gave nothing).
  void heldRound(const TurnCtx& turn, const std::string& reply, DeliverDecision& out) {
    noteNew(turn);   // what the held round itself did
    check_.taint(turn.tainted);
    close(reply, !reply.empty() && reply != heldReply_, &out);
  }

  // Counters holds uint32_t fields only (orch_truth.cpp asserts it): added field by field.
  static void addCounters(truth::Counters& into, const truth::Counters& add) {
    constexpr size_t n = sizeof(truth::Counters) / sizeof(uint32_t);
    uint32_t a[n], b[n];
    std::memcpy(a, &into, sizeof a);
    std::memcpy(b, &add, sizeof b);
    for (size_t i = 0; i < n; ++i) a[i] += b[i];
    std::memcpy(&into, a, sizeof a);
  }

  // The turn's check, opened from the transcript.
  void open(const TurnCtx& turn) {
    turn_ = turn.turn;
    open_ = true;
    finished_ = false;
    report_ = TruthReport{};
    report_.turn = turn.turn;
    heldReply_.clear();
    HeadEndpoint e;
    e.host = turn.host;
    host_ = providerSlugOf(turn, e);
    start_ = startOf(turn);
    key_ = truth::stateKey(host_.c_str(), turn.model.c_str(), nimbus::orch::headqual::Line::E2);
    truth::HeadState& h = state_->head(key_, start_);
    TurnRead read;
    readArena(*d_.arena, turn.turn, read);
    truth::Turn t;
    t.host = host_.c_str();
    t.tainted = turn.tainted;
    t.mode = capped(truth::turnMode(start_, h, turn.tainted));
    t.owner = read.owner;
    t.nowEpoch = nowEpoch_ = d_.now ? d_.now() : (int64_t)turn.startedAt;
    t.utcOffsetSec = read.tzMinutes * 60;
    noted_ = 0;
    for (const Receipt& r : read.receipts) noted_ += r.turn == turn.turn;
    for (const Receipt& r : read.receipts)
      if (r.turn == turn.turn) t.now.push_back(effect(r));
    t.ringKnown = recent(turn.turn, read, t.recent);
    report_.ringKnown = t.ringKnown;
    check_.reset(nullptr, turn.chatId, read.owner);
    turnCounters_ = truth::Counters{};
    check_.start(std::move(t), counted(), &turnCounters_);
    report_.mode = check_.mode();
  }

  Truth startOf(const TurnCtx& turn) const {
    if (d_.mode == TruthMode::ReceiptOnly) return Truth::ReceiptOnly;
    return truth::startMode(host_.c_str(), turn.model.c_str(), nimbus::orch::headqual::Line::E2);
  }
  Truth capped(Truth m) const { return d_.mode == TruthMode::Shadow && m == Truth::Enforce ? Truth::Shadow : m; }

  static truth::TurnEffect effect(const Receipt& r) {
    truth::TurnEffect e;
    e.tool = r.tool;
    e.ok = r.ok;
    e.receipt = r.text;
    e.kind = receiptKind(r.tool, true);
    return e;
  }

  // The effects the held round added since the check opened.
  void noteNew(const TurnCtx& turn) {
    TurnRead read;
    readArena(*d_.arena, turn.turn, read);
    size_t seen = 0;
    for (const Receipt& r : read.receipts) {
      if (r.turn != turn.turn || seen++ < noted_) continue;
      check_.note(effect(r));
    }
    noted_ = std::max(noted_, seen);
  }

  // The chat's newest successful receipts before this turn, oldest first; true when they are its
  // whole recent record: the newest kRecentReceipts, or everything since the chat's first item.
  bool recent(uint32_t turn, const TurnRead& read, std::vector<fx::RingEntry>& out) {
    if (!firstFrom_) firstFrom_ = read.oldestQ ? read.oldestQ : 1;
    for (const Receipt& r : read.receipts)
      if (r.turn != turn) keep(r);
    if (!askedOlder_ && firstFrom_ > 1 && kept_.size() < kRecentReceipts) askOlder();
    trim();
    for (const Receipt& r : kept_) out.push_back(ringEntry(r));
    return kept_.size() >= kRecentReceipts || firstFrom_ <= 1 || olderWhole_;
  }
  void keep(const Receipt& r) {
    if (!r.ok) return;
    auto at = std::lower_bound(kept_.begin(), kept_.end(), r.q, [](const Receipt& k, uint64_t q) { return k.q < q; });
    if (at == kept_.end() || at->q != r.q) kept_.insert(at, r);
  }
  void askOlder() {
    askedOlder_ = true;
    std::vector<Receipt> older;
    bool whole = false;
    if (!d_.older || !d_.older(firstFrom_, kRecentReceipts * 2, older, whole)) return;
    for (const Receipt& r : older) keep(r);
    olderWhole_ = whole;
  }
  void trim() {
    if (kept_.size() > kRecentReceipts) kept_.erase(kept_.begin(), kept_.end() - (long)kRecentReceipts);
  }

  // The check's end with the reply that goes out: the receipt line, when one is due, rides out
  // with the reply as its status line.
  void close(const std::string& reply, bool roundReplied, DeliverDecision* out = nullptr) {
    const truth::Outcome o = check_.finish(reply, roundReplied);
    finished_ = true;
    report_.first = o.first;
    report_.retried = o.retried;
    report_.shadowDue = o.shadowDue;
    report_.receipt = !o.status.empty() && out;
    if (out && !o.status.empty()) {
      out->status = StatusLine::FailedWrite;
      out->statusText = o.status;
    }
    if (o.shadowDue) queueShadow(o, reply);
  }

  void queueShadow(const truth::Outcome&, const std::string& reply) {
    shadow_ = truth::Turn();
    TurnRead read;
    readArena(*d_.arena, turn_, read);
    shadow_.mode = Truth::Shadow;
    shadow_.owner = read.owner;
    shadow_.reply = reply;
    for (const Receipt& r : read.receipts)
      if (r.turn == turn_) shadow_.now.push_back(effect(r));
    for (const Receipt& r : kept_) shadow_.recent.push_back(ringEntry(r));
    shadow_.ringKnown = report_.ringKnown;
    shadow_.utcOffsetSec = read.tzMinutes * 60;
    shadow_.nowEpoch = nowEpoch_;
    shadowHost_ = host_;
    shadowKey_ = key_;
    shadowStart_ = start_;
    shadowDue_ = true;
  }

  // The turn is over: the head's mode moves on its counters, the state is saved, the rig is told.
  void end() {
    open_ = false;
    truth::HeadState& h = state_->head(key_, start_);
    addCounters(h.c, turnCounters_);
    (void)truth::nextMode(h, d_.allowShadow);
    if (d_.save) d_.save(*state_);
    if (opened_ && d_.progress && d_.progress->isOpen()) d_.progress->close(nowMs());
    opened_ = false;
    if (d_.done) d_.done(report_);
  }

  // A held turn: the device waits for one more answer, and a long turn says so (RB-E3's class).
  void showWait(const TurnCtx& turn) {
    if (!d_.progress) return;
    if (!d_.progress->isOpen() || d_.progress->turn() != turn.turn) {
      d_.progress->open(turn, turn.startedMs);
      opened_ = true;
    }
    ProgressEvent e;
    e.kind = ProgressKind::ProviderWait;
    e.nowMs = d_.nowMs ? d_.nowMs() : turn.startedMs;
    (void)d_.progress->tick(e);
  }
  uint32_t nowMs() const { return d_.nowMs ? d_.nowMs() : 0; }

  TruthDeps d_;
  truth::State own_;
  truth::State* state_;
  truth::TurnCheck check_;
  TruthReport report_;
  std::string host_, key_, heldReply_;
  truth::Counters turnCounters_;   // this turn's counts, added to the head's at the end (no pointer kept)
  Truth start_ = Truth::ReceiptOnly;
  uint32_t turn_ = 0, probes_ = 0;
  int64_t nowEpoch_ = 0;
  size_t noted_ = 0;
  bool open_ = false, finished_ = false, opened_ = false;
  // The recent receipts kept across turns (they stay after compaction takes them from the arena).
  std::vector<Receipt> kept_;
  uint64_t firstFrom_ = 0;
  bool askedOlder_ = false, olderWhole_ = false;
  // One shadow turn waiting for the idle step.
  truth::Turn shadow_;
  std::string shadowHost_, shadowKey_;
  Truth shadowStart_ = Truth::ReceiptOnly;
  bool shadowDue_ = false;
};

}  // namespace

Status installTruth(PlainHooks& hooks, const TruthDeps& deps) {
  if (deps.mode == TruthMode::Off) return Status::success();
  if (!deps.arena) return Status::fail(Err::InvalidArgument, "plain::installTruth");
  if (hooks.has(Seam::BeforeDeliver, HookSlot::Truth) || hooks.has(Seam::AfterDeliver, HookSlot::Truth) ||
      hooks.has(Seam::IdleStep, HookSlot::ShadowProbe))
    return Status::fail(Err::Busy, "plain::installTruth");
  std::shared_ptr<Binding> b = std::make_shared<Binding>(deps);
  Status st = hooks.beforeDeliver(HookSlot::Truth, [b](const DeliverCtx& c) { return b->beforeDeliver(c); });
  if (st.ok()) st = hooks.afterDeliver(HookSlot::Truth, [b](const DeliveredCtx& c) { b->afterDeliver(c); });
  if (st.ok()) st = hooks.idleStep(HookSlot::ShadowProbe, [b](const IdleCtx& c) { return b->idle(c); });
  return st;
}

}  // namespace plain
}  // namespace agent
