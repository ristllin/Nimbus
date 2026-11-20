// Crash resume (plain/resume.h): the bootResume seam. Lane RB-X1.
//
// At boot, after the engine filled the arena from the card: answer every call left without a
// result (never run again), then do the one thing the newest turn still needs (nimbus/orch/
// intents.h bootAction): nothing, close it, send its stored answer with no model call, resume
// it once through the engine's own runTurn, or send the plain crash notice. A resume or a send
// is counted in head.json before it starts.
#include "nimbus/harness/plain/resume.h"

#include <algorithm>

#include "nimbus/harness/plain/deliver.h"
#include "nimbus/harness/plain/rails.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/turn_journal.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/rules.h"

namespace agent {
namespace plain {

namespace tx = nimbus::orch::tx;
namespace intents = nimbus::orch::intents;
using intents::BootAction;

namespace {

// The e item causes and the event of a boot-closed turn (machine slugs, frozen once stored).
constexpr char kCauseClosed[] = "resume:closed";
constexpr char kCauseSent[] = "resume:sent-stored";
constexpr char kCauseNotice[] = "resume:notice";

class BootResume {
 public:
  BootResume(const ResumeDeps& d, tx::HeadState& head) : d_(d), head_(head) {}

  Status run(ResumeReport& rep) {
    intents::OpenTurn open;
    std::vector<intents::OpenCall> calls;
    Status st = intents::openTurn(*d_.arena, open);
    if (st.ok()) st = intents::openCalls(*d_.arena, calls);
    if (!st.ok()) return st;
    made_ = std::max<uint8_t>(open.attempts, head_.openTurn == open.turn ? head_.attempts : 0);
    rep.turn = open.turn;
    rep.action = intents::bootAction(open, made_, kMaxAutoResumes,
                                     open.state != intents::TurnState::AnswerStored || wholeAnswer(open));
    if (rep.action != BootAction::None && !(st = context(open)).ok()) return st;   // before anything is counted
    if (rep.action == BootAction::SendStored || rep.action == BootAction::Resume) {
      rep.counted = count(open.turn, static_cast<uint8_t>(made_ + 1));
      if (!rep.counted) rep.action = open.person ? BootAction::Notice : BootAction::Close;   // never an attempt it cannot count
    }
    // With little room the boot stores no result into the arena (a drop to make room is the engine's,
    // named by its omission line): the engine's own turn open answers the open calls, and the d
    // and e items go to the card only.
    roomy_ = d_.arena->freeBytes() >= kBootRoomBytes;
    if (roomy_ && !(st = answer(calls, rep.answered)).ok()) return st;
    return act(rep, open);
  }

 private:
  static constexpr size_t kBootRoomBytes = 4096;

  const ResumeDeps& d_;
  tx::HeadState& head_;
  TurnCtx ctx_;
  tx::Item item_;          // the item being stored, and one read back
  uint8_t made_ = 0;       // resumes and boot sends already made for the newest turn
  bool roomy_ = true;      // the arena has room for the boot's own items

  uint64_t now() const { return d_.now ? d_.now() : 0; }
  uint64_t nextQ() const { return std::max({head_.nextQ, d_.arena->lastQ() + 1, d_.log->lastQ() + 1}); }

  Status act(ResumeReport& rep, const intents::OpenTurn& open) {
    switch (rep.action) {
      case BootAction::Close: return close(kCauseClosed, false);
      case BootAction::SendStored: return sendStored(open);
      case BootAction::Notice: return notice(open);
      case BootAction::Resume: return resume(open, rep.outcome);
      case BootAction::None: break;
    }
    return Status::success();
  }

  // Read the item stored at q into item_.
  bool readAt(uint64_t q) {
    size_t i = 0;
    return q && d_.arena->find(q, i).ok() && d_.arena->get(i, item_).ok();
  }

  // The stored final answer can be sent whole: a long reply's a item names its whole text in the
  // bulk file (handle; lane RB-H81) and that payload reads back; any other a item is the reply
  // itself unless it is the stub an older engine left (the stub's opening as tx::stubText writes
  // it for that q). The head a long reply keeps inline is never sent as the reply.
  bool wholeAnswer(const intents::OpenTurn& open) {
    if (!readAt(open.answerQ)) return false;
    std::string whole;
    if (!item_.handle.empty()) return d_.log->readBulk(item_.handle, 0, tx::kBulkMaxBytes, whole).ok();
    tx::StubFacts f;
    f.tool = texts::device_notes::kAssistantSlug;
    f.q = open.answerQ;
    f.cls = tx::ResultClass::Bulk;
    std::string stub;
    if (!tx::stubText(f, stub).ok()) return true;
    const std::string mark = " r" + std::to_string(open.answerQ) + " ";
    const size_t at = stub.find(mark);
    return at == std::string::npos || item_.x.compare(0, at + mark.size(), stub, 0, at + mark.size()) != 0;
  }

  // The write-ahead counter: the attempts made once the one about to start has started, in
  // head.json, before it starts.
  bool count(uint32_t turn, uint8_t attempts) {
    if (!d_.log->isOpen()) return false;
    head_.openTurn = turn;
    head_.attempts = attempts;
    head_.nextQ = std::max(head_.nextQ, d_.arena->lastQ() + 1);
    return d_.log->writeHead(head_).ok();
  }

  Status put(tx::Item& it) {
    it.q = nextQ();
    it.t = now();
    tx::StoreState state = tx::StoreState::Stored;
    const Status st = roomy_ ? d_.log->store(it, *d_.arena, state) : d_.log->append(it, state);
    if (st.ok()) head_.nextQ = it.q + 1;
    return st;
  }

  // Step 1: every call left without a result gets one, by what its intent says.
  Status answer(const std::vector<intents::OpenCall>& calls, size_t& answered) {
    for (const intents::OpenCall& c : calls) {
      item_ = tx::Item{};
      item_.kind = tx::ItemKind::Result;
      item_.turn = c.turn;
      item_.id = c.id;
      item_.n = c.n;
      item_.ok = false;
      item_.x = c.fate == intents::CallFate::Unknown ? texts::tool_results::kOutcomeUnknown
                                                     : texts::tool_results::kRestartedBeforeRun;
      item_.bytes = static_cast<uint32_t>(item_.x.size());
      item_.lines = 1;
      stub(c);
      const Status st = put(item_);
      if (!st.ok()) return st;
      ++answered;
    }
    return Status::success();
  }

  void stub(const intents::OpenCall& c) {
    tx::StubFacts f;
    f.tool = c.n;
    f.q = nextQ();
    f.bytes = item_.bytes;
    f.lines = item_.lines;
    (void)tx::stubTime(now(), 0, f.hhmm);
    f.preview = item_.x.substr(0, tx::kStubPreviewBytes + 4);
    f.cls = tx::ResultClass::Receipt;
    if (!tx::stubText(f, item_.st).ok()) item_.st.clear();
  }

  // The chat's context for the turn; tainted when the turn already read outside content (the
  // resumed request carries it whole, as the first run's did).
  Status context(const intents::OpenTurn& open) {
    Status st = d_.context(ctx_);
    if (!st.ok()) return st;
    ctx_.turn = open.turn;
    ctx_.trigger = Trigger::Resume;
    ctx_.attempt = static_cast<uint8_t>(made_ + 1);
    ctx_.startedAt = now();
    ctx_.tainted = ctx_.tainted || readOutside(open.turn);
    return Status::success();
  }

  bool readOutside(uint32_t turn) {
    for (size_t i = 0; i < d_.arena->count(); ++i) {
      tx::ArenaEntry e;
      if (!d_.arena->header(i, e).ok() || e.turn != turn) continue;
      if (d_.arena->get(i, item_).ok() && itemOutside(item_)) return true;   // one item rule (RB-RL)
    }
    return false;
  }

  // The e item and head.json: the turn is closed, nothing of it runs again.
  Status close(const char* cause, bool namedSend) {
    item_ = tx::Item{};
    item_.kind = tx::ItemKind::TurnEnd;
    item_.turn = ctx_.turn;
    item_.q = nextQ();   // turnEndItem measures the line at its q
    tx::TurnEndFacts f;
    f.cause = cause;
    if (namedSend) f.events.push_back(kCauseSent);   // a reply that went out before the cut goes out twice
    Status st = turnEndItem(ctx_, f, item_);
    if (st.ok()) st = put(item_);
    if (!st.ok()) return st;
    head_.openTurn = 0;
    head_.attempts = 0;
    if (d_.log->isOpen()) (void)d_.log->writeHead(head_);   // a cache: the e item is the record
    return Status::success();
  }

  Status sendStored(const intents::OpenTurn& open) {
    if (!readAt(open.answerQ)) return Status::fail(Err::NotFound, "plain::resume.answer");
    std::string text = item_.x;   // a long reply: its whole text from the bulk file (RB-H81)
    if (!item_.handle.empty() && !d_.log->readBulk(item_.handle, 0, tx::kBulkMaxBytes, text).ok())
      return Status::fail(Err::NotFound, "plain::resume.answer");
    DeliverInput in;
    in.turn = &ctx_;
    in.text = &text;
    DeliverOutcome out;
    const Status sent = d_.deliver(in, out);
    if (!out.sent) {   // nothing reached the owner: the attempt is given back, a later boot sends it
      (void)count(open.turn, made_);
      return sent.ok() ? Status::fail(Err::Io, "plain::resume.send") : sent;
    }
    item_ = tx::Item{};
    item_.kind = tx::ItemKind::Delivery;
    item_.turn = ctx_.turn;
    item_.msgIds = out.msgIds;
    const Status st = put(item_);
    if (!st.ok()) return st;
    return close(kCauseSent, true);
  }

  // The plain crash notice (the released engine's own words), as the one status line. A notice
  // the channel refused leaves the turn open: a later boot sends it.
  Status notice(const intents::OpenTurn& open) {
    nimbus::orch::TurnJournal j;
    j.chat = ctx_.chatId;
    j.kind = "message";
    std::string hhmm;
    if (readAt(open.userQ)) {
      j.at = item_.t;
      for (const tx::Part& p : item_.parts)
        j.kind = p.kind == tx::PartKind::Image ? "photo" : p.kind == tx::PartKind::File ? "file" : "voice";
      (void)tx::stubTime(item_.t, item_.tzMinutes, hhmm);
    }
    const std::string line = nimbus::orch::crashNotice(d_.deviceName, &j, hhmm);
    DeliverInput in;
    in.turn = &ctx_;
    in.status = StatusLine::CrashNotice;
    in.statusText = &line;
    DeliverOutcome out;
    const Status sent = d_.deliver(in, out);
    if (!out.statusSent) return sent.ok() ? Status::fail(Err::Io, "plain::resume.notice") : sent;
    return close(kCauseNotice, false);
  }

  // The one automatic resume: the engine's own turn, from everything stored.
  Status resume(const intents::OpenTurn& open, TurnOutcome& outcome) {
    TurnInput in;
    in.ctx = ctx_;
    if (readAt(open.userQ)) {
      in.tzMinutes = item_.tzMinutes;
      in.ch = item_.ch;
    }
    return d_.runTurn(in, outcome);
  }
};

}  // namespace

Status installResume(PlainHooks& hooks, const ResumeDeps& deps) {
  if (!deps.log || !deps.arena || !deps.head || !deps.runTurn || !deps.deliver || !deps.context)
    return Status::fail(Err::InvalidArgument, "plain::installResume");
  return hooks.bootResume(HookSlot::Resume, [deps](const BootCtx&) {
    tx::HeadState* head = deps.head();
    if (!head) return Status::fail(Err::InvalidArgument, "plain::resume.head");
    ResumeReport rep;
    rep.status = BootResume(deps, *head).run(rep);
    if (deps.done) deps.done(rep);
    return rep.status;
  });
}

}  // namespace plain
}  // namespace agent
