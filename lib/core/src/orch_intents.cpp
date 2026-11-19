// Write-ahead intents and what a boot reads from them (nimbus/orch/intents.h). Lane RB-X1.
#include "nimbus/orch/intents.h"

#include <cstring>

namespace nimbus {
namespace orch {
namespace intents {

namespace {

// The t item slugs of a person's turn (plain::triggerSlug, frozen: owner, console, resume).
bool personSlug(const std::string& slug) { return slug == "owner" || slug == "console" || slug == "resume"; }

struct Open {
  OpenCall call;
  bool intent = false;
};

}  // namespace

tx::Status openCalls(const tx::TxArena& arena, std::vector<OpenCall>& out) {
  out.clear();
  std::vector<Open> open;
  tx::Item it;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok()) return tx::Status::fail(tx::Err::InvalidArgument, "intents::openCalls");
    if (e.kind != tx::ItemKind::Call && e.kind != tx::ItemKind::Intent && e.kind != tx::ItemKind::Result) continue;
    if (!arena.get(i, it).ok()) return tx::Status::fail(tx::Err::InvalidArgument, "intents::openCalls");
    if (it.kind == tx::ItemKind::Call) {
      Open o;
      o.call.q = it.q;
      o.call.turn = it.turn;
      o.call.id = it.id;
      o.call.n = it.n;
      open.push_back(o);
      continue;
    }
    // An intent marks, and a result closes, the oldest open call of its id.
    for (size_t k = 0; k < open.size(); ++k) {
      if (open[k].call.id != it.id) continue;
      if (it.kind == tx::ItemKind::Result) {
        open.erase(open.begin() + static_cast<long>(k));
        break;
      }
      if (open[k].intent) continue;
      open[k].intent = true;
      break;
    }
  }
  out.reserve(open.size());
  for (Open& o : open) {
    o.call.fate = o.intent ? CallFate::Unknown : CallFate::NotStarted;
    out.push_back(o.call);
  }
  return tx::Status::success();
}

const char* turnStateSlug(TurnState s) {
  switch (s) {
    case TurnState::Closed: return "closed";
    case TurnState::Unanswered: return "unanswered";
    case TurnState::AnswerStored: return "answer-stored";
    case TurnState::Delivered: return "delivered";
  }
  return "closed";
}

namespace {

// The newest turn number of any item but a digest (0: none).
tx::Status newestTurn(const tx::TxArena& arena, uint32_t& newest) {
  newest = 0;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok()) return tx::Status::fail(tx::Err::InvalidArgument, "intents::openTurn");
    if (e.kind != tx::ItemKind::Digest && e.turn > newest) newest = e.turn;
  }
  return tx::Status::success();
}

struct TurnFold {
  bool ended = false;
  bool delivered = false;
};

// One item of the newest turn into what openTurn reports; `it` is scratch.
tx::Status fold(const tx::TxArena& arena, size_t i, const tx::ArenaEntry& e, tx::Item& it, OpenTurn& out, TurnFold& f) {
  switch (e.kind) {
    case tx::ItemKind::TurnEnd: f.ended = true; return tx::Status::success();
    case tx::ItemKind::Delivery: f.delivered = true; return tx::Status::success();
    case tx::ItemKind::User:
      out.person = true;
      out.userQ = e.q;
      return tx::Status::success();
    case tx::ItemKind::TurnOpen:
    case tx::ItemKind::Assistant: break;
    default: return tx::Status::success();
  }
  if (!arena.get(i, it).ok()) return tx::Status::fail(tx::Err::InvalidArgument, "intents::openTurn");
  if (it.kind == tx::ItemKind::TurnOpen) {
    out.person = out.person || personSlug(it.trigger);
    if (it.attempt > out.attempts) out.attempts = it.attempt;
  } else if (it.fin) {
    out.answerQ = it.q;
  }
  return tx::Status::success();
}

TurnState stateOf(const TurnFold& f, const OpenTurn& t) {
  if (f.ended) return TurnState::Closed;
  if (f.delivered) return TurnState::Delivered;
  return t.answerQ ? TurnState::AnswerStored : TurnState::Unanswered;
}

}  // namespace

tx::Status openTurn(const tx::TxArena& arena, OpenTurn& out) {
  out = OpenTurn{};
  uint32_t newest = 0;
  tx::Status st = newestTurn(arena, newest);
  if (!st.ok() || !newest) return st;
  TurnFold f;
  tx::Item it;
  for (size_t i = 0; i < arena.count(); ++i) {
    tx::ArenaEntry e;
    if (!arena.header(i, e).ok()) return tx::Status::fail(tx::Err::InvalidArgument, "intents::openTurn");
    if (e.turn != newest || e.kind == tx::ItemKind::Digest) continue;
    if (!(st = fold(arena, i, e, it, out, f)).ok()) return st;
  }
  out.turn = newest;
  out.state = stateOf(f, out);
  return tx::Status::success();
}

const char* bootActionSlug(BootAction a) {
  switch (a) {
    case BootAction::None: return "none";
    case BootAction::Close: return "close";
    case BootAction::SendStored: return "send-stored";
    case BootAction::Resume: return "resume";
    case BootAction::Notice: return "notice";
  }
  return "none";
}

BootAction bootAction(const OpenTurn& turn, uint8_t attempts, uint8_t maxResumes, bool answerWhole) {
  if (turn.state == TurnState::Closed || !turn.turn) return BootAction::None;
  if (turn.state == TurnState::Delivered || !turn.person) return BootAction::Close;
  if (attempts >= maxResumes) return BootAction::Notice;
  if (turn.state == TurnState::AnswerStored) return answerWhole ? BootAction::SendStored : BootAction::Notice;
  return BootAction::Resume;
}

}  // namespace intents
}  // namespace orch
}  // namespace nimbus
