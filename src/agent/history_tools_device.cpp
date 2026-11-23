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

// The earlier engine's rows of one chat through the import's own forward reader (EpisodicRowSource:
// the store's `after` query, cold scan on, ring and cold regions alike, a stale byte cursor re-read
// from the row id), each store query one card-lock hold; a span it could not read is Io (lane
// E2-SRCH2: the newest-first page query stopped after one page on a ring-resident store).
struct OldBinding {
  std::string chat;
  std::unique_ptr<tx::EpisodicRowSource> src;
  std::unique_ptr<tx::RowSourceRows> rows;
};
OldBinding s_old;   // tg_poll only

tx::OldRows* oldRowsOf(const std::string& chat) {
  if (!memory::haveSd()) return nullptr;
  if (s_old.rows && s_old.chat == chat) return s_old.rows.get();
  s_old.rows.reset();
  s_old.src.reset(new (std::nothrow) tx::EpisodicRowSource(chat, [](const orch::MsgQuery& q, orch::EpiQueryInfo& info) {
    std::vector<orch::EpisodicMessage> rows;
    memory::Lock lk;
    rows = memory::episodic().query(q, &info);
    return rows;
  }));
  if (!s_old.src) return nullptr;
  tx::EpisodicRowSource* src = s_old.src.get();
  s_old.rows.reset(new (std::nothrow) tx::RowSourceRows(
      *src, [] { return (uint32_t)millis(); }, [src] { return src->unreadable().size(); }));
  s_old.chat = chat;
  return s_old.rows.get();
}

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
    d.oldRows = oldRowsOf;
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
