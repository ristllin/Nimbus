// The device binding of engine 2's search and view (history_tools_device.h), lane E2-SRCH.
#include "history_tools_device.h"

#include <Arduino.h>

#include <memory>
#include <new>
#include <string>
#include <vector>

#include "../sys/agent_log.h"
#include "engine_api.h"                       // utcOffsetAt
#include "memory_subsystem.h"                 // memory::Lock, memory::episodic, haveSd, dataFs
#include "nimbus/harness/plain/history.h"
#include "nimbus/harness/turn_memory.h"       // PsramFirstScope
#include "nimbus/orch/blob_store.h"           // blobPath
#include "nimbus/orch/episodic.h"
#include "nimbus/orch/tx/old_engine.h"        // dayRowOf, rowIdString
#include "notes_device.h"                     // notesdev::notebook
#include "tx_store_device.h"                  // txdev::cardTxFiles

namespace agent::historydev {

namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

namespace {

// Record blocks one store query may read off the card (MsgQuery::maxReads): each query is one
// card-lock hold, and eight blocks of at most 8 KB keep it well under the 150 ms gate.
constexpr int kOldMaxReads = 8;

// The earlier engine's rows of one chat, newest first, through the store's own `before` cursor.
class EpisodicOld : public tx::OldRows {
 public:
  void bind(const std::string& chat) { sid_ = chat; }
  tx::Status page(const tx::OldPageAsk& ask, tx::OldPage& out, tx::ReadStats& stats) override {
    orch::MsgQuery q = base();
    q.before = ask.before;
    q.limit = (int)ask.max;
    if (ask.fromT) q.sinceHours = (uint32_t)(ask.fromT / 3600);
    if (ask.toT) q.beforeHours = (uint32_t)(ask.toT / 3600) + 1;
    orch::EpiQueryInfo info;
    std::vector<orch::EpisodicMessage> rows;
    if (!run(q, info, rows, stats)) return tx::Status::fail(tx::Err::Io, "historydev.page");
    out = tx::OldPage{};
    uint64_t lowest = 0;
    for (const orch::EpisodicMessage& m : rows) {
      out.rows.push_back(tx::dayRowOf(m));
      if (!lowest || out.rows.back().id < lowest) lowest = out.rows.back().id;
    }
    out.older = info.olderExists;
    out.next = !info.nextBefore.empty() ? info.nextBefore : (lowest ? tx::rowIdString(lowest) : std::string());
    return tx::Status::success();
  }
  tx::Status row(uint64_t id, tx::DayRow& out, tx::ReadStats& stats) override {
    orch::MsgQuery q = base();
    q.before = tx::rowIdString(id + 1);
    q.limit = 1;
    orch::EpiQueryInfo info;
    std::vector<orch::EpisodicMessage> rows;
    if (!run(q, info, rows, stats)) return tx::Status::fail(tx::Err::Io, "historydev.row");
    for (const orch::EpisodicMessage& m : rows)
      if (tx::rowIdNumber(m.id) == id) {
        out = tx::dayRowOf(m);
        return tx::Status::success();
      }
    return tx::Status::fail(tx::Err::NotFound, "historydev.row");
  }

 private:
  orch::MsgQuery base() const {
    orch::MsgQuery q;
    q.sessionId = sid_;
    q.sessionAllow = {sid_};   // the read boundary: this chat only
    q.coldScan = true;
    q.maxReads = kOldMaxReads;
    return q;
  }
  bool run(const orch::MsgQuery& q, orch::EpiQueryInfo& info, std::vector<orch::EpisodicMessage>& rows,
           tx::ReadStats& stats) {
    if (!memory::haveSd()) return false;
    const uint32_t t0 = millis();
    {
      memory::Lock lk;
      rows = memory::episodic().query(q, &info);
    }
    stats.add(millis() - t0, info.coldBytes);
    return true;
  }
  std::string sid_;
};

EpisodicOld s_old;   // tg_poll only

bool photoOnCard(const std::string& ref) {
  if (ref.size() < 2 || ref[0] != 'p' || !memory::haveSd()) return false;
  const std::string hash = ref.substr(1);
  memory::Lock lk;
  for (const char* ext : {"jpg", "bin"})
    if (memory::dataFs().exists(orch::blobPath("/mem/blobs", hash, ext).c_str())) return true;
  return false;
}

agent::plain::HistoryTools& tools() {
  static agent::plain::HistoryTools* t = [] {
    agent::plain::HistoryDeps d;
    d.files = &txdev::cardTxFiles();
    d.oldRows = [](const std::string& chat) -> tx::OldRows* {
      s_old.bind(chat);
      return &s_old;
    };
    d.notes = [](const orch::Principal& who, std::vector<orch::notes::Note>& out) {
      return notesdev::notebook().unpinned(who, out);
    };
    d.tzAt = [](uint64_t t) { return (int16_t)orchestrator::utcOffsetAt((uint32_t)t); };
    d.nowMs = [] { return (uint32_t)millis(); };
    d.photoReadable = photoOnCard;
    return new agent::plain::HistoryTools(d);
  }();
  return *t;
}

}  // namespace

void registerHistory(orch::ToolRegistry& reg) { tools().registerInto(reg); }

bool answer(const agent::plain::ToolCall& call, const agent::plain::TurnCtx& turn, agent::plain::ToolOutput& out) {
  if (call.name != "search" && call.name != "view") return false;
  const uint32_t t0 = millis();
  {
    const PsramFirstScope psram;
    (void)tools().answer(call, turn, out);
  }
  const tx::ReadStats& c = tools().lastCard();
  alogf("e2: %s done ms=%u card reads=%u bytes=%u ms=%u longest=%u out=%u stackFree=%u intLargest=%u",
        call.name.c_str(), (unsigned)(millis() - t0), (unsigned)c.reads, (unsigned)c.bytes, (unsigned)c.totalMs,
        (unsigned)c.longestMs, (unsigned)out.text.size(), (unsigned)uxTaskGetStackHighWaterMark(nullptr),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return true;
}

}  // namespace agent::historydev
