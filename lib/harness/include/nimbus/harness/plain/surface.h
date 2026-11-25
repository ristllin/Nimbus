#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "nimbus/harness/plain/engine.h"

// plain/surface.h - what the web pages say about an engine-2 chat (lane E2-P-SURF): the turn inspector
// (/api/lastturn), the turn log (/api/trace) and the summary card (/api/fold/status). Text for a person,
// never for a model. Each reads the chat the engine holds, so each must run where the engine runs its
// turns (tg_poll): src/net/web_engine2.cpp stages the ask and the turn task answers it between turns.
namespace agent {
namespace plain {
namespace surface {

inline constexpr size_t kTextMax = 1500000;   // one page of text: longer is cut, and says so
inline constexpr size_t kItemTextMax = 1000;  // one item's text in the turn log

// The last closed turn's request (exchange `round`, 1-based; 0 = the last one) rendered again from the
// stored items, with the turn's figures above it. When the render is refused, one true line says why.
// `found` false: no turn has closed since boot (the page says so).
std::string lastTurnText(const PlainEngine& e, size_t round, nimbus::orch::tx::ArenaAllocFn alloc,
                         nimbus::orch::tx::ArenaFreeFn release, bool& found);

// The turn log of one turn: every item the turn stored, in order, and its e item's figures. `row` is the
// day-stream row the turn's message mirrors (the web chat's turn id "m<hex>"); 0 = the last closed turn.
// `found` false: that turn is not in memory (summarized or older); the text says so.
std::string turnLogText(const PlainEngine& e, uint64_t row, bool& found);

// The summary card's JSON (the shape the Summary panel reads; engine 2's own fields: engine, summary,
// lines). `chat` is the conversation the card is for; `held` whether the engine holds it now.
std::string foldStatusJson(const PlainEngine* e, const std::string& chat, bool held, uint64_t nowEpoch);

// The one line a fold panel that has no meaning on engine 2 shows instead of nothing.
inline constexpr char kFoldNote[] =
    "Engine 2 writes this summary by itself when the conversation grows; it has no schedule to run by hand.";

// ---- the web page's ask, answered by the turn task ------------------------------------------------
// The engine's arena and log belong to the task that runs the turns. A web page stages one ask here; that
// task answers it between turns (orchestrator_plain.cpp pollJobs); the page's response waits for it up to
// kAskWaitMs and says so when a turn keeps the task busy. One ask at a time: a second says "busy".
enum class Ask : uint8_t { None = 0, LastTurn = 1, Trace = 2, Fold = 3 };
struct AskReq {
  Ask kind = Ask::None;
  uint64_t arg = 0;      // Trace: the turn's day-stream row (0 = the last turn)
  size_t round = 0;      // LastTurn: which request (0 = the last)
  std::string chat;      // Fold: the conversation the card is for ("" = the resident one)
  uint32_t id = 0;
};
struct AskAnswer {
  std::string body;
};
inline constexpr uint32_t kAskWaitMs = 8000;        // how long a page waits for the turn task
inline constexpr uint32_t kAskStaleMs = 12000;      // an ask nobody took or collected by then is dropped

class AskBox {
 public:
  // The page: stage an ask; its id, or 0 while another one is live.
  uint32_t stage(const AskReq& q, uint32_t nowMs) {
    std::lock_guard<std::mutex> lk(mx_);
    if (live_ && nowMs - at_ < kAskStaleMs) return 0;
    req_ = q;
    req_.id = ++seq_ ? seq_ : ++seq_;
    live_ = true;
    taken_ = answered_ = false;
    at_ = nowMs;
    ans_.body.clear();
    return req_.id;
  }
  // The turn task: the staged ask, once (false: none).
  bool pending(AskReq& out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!live_ || taken_) return false;
    taken_ = true;
    out = req_;
    return true;
  }
  // The turn task: its answer.
  void answer(uint32_t id, std::string body) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!live_ || id != req_.id) return;
    ans_.body = std::move(body);
    answered_ = true;
  }
  // The page: the answer when it is there (false: not yet). Takes it and frees the box.
  bool take(uint32_t id, AskAnswer& out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!live_ || id != req_.id || !answered_) return false;
    out = std::move(ans_);
    live_ = false;
    return true;
  }
  // The page gave up (a turn kept the task busy): the box is free again, a late answer is dropped.
  void abandon(uint32_t id) {
    std::lock_guard<std::mutex> lk(mx_);
    if (live_ && id == req_.id) {
      live_ = false;
      ans_.body = std::string();
    }
  }

 private:
  std::mutex mx_;
  AskReq req_;
  AskAnswer ans_;
  uint32_t seq_ = 0;
  uint32_t at_ = 0;
  bool live_ = false, taken_ = false, answered_ = false;
};
AskBox& askBox();

// The page's line when the turn task did not answer in time (a turn is running).
inline constexpr char kBusyText[] = "A turn is running, so this page cannot be read now. Try again when it ends.\n";
inline constexpr char kBusyFoldJson[] =
    "{\"engine\":2,\"state\":\"busy\",\"lastOk\":false,\"running\":false,\"queued\":false,\"cooldownS\":0,"
    "\"canRun\":false,\"why\":\"A turn is running. The summary shows when it ends.\","
    "\"lines\":[\"A turn is running. The summary shows when it ends.\"],\"chats\":[],\"summary\":\"\"}";

// What the turn task answers for `q`: the page's whole body. `e` null: no chat is held on the device now.
std::string answerAsk(const AskReq& q, const PlainEngine* e, const std::string& residentChat, uint64_t nowEpoch,
                      nimbus::orch::tx::ArenaAllocFn alloc, nimbus::orch::tx::ArenaFreeFn release);

}  // namespace surface
}  // namespace plain
}  // namespace agent
