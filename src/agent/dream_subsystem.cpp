#include "dream_subsystem.h"

#include <time.h>

#include <string>

#include "../sys/agent_log.h"
#include "agent_config.h"       // ORCH_AUTO_TURN_MIN_HEAP (gate heap floor)
#include "memory_subsystem.h"   // engines + Lock + stats + nowHours
#include "orchestrator.h"
#include "store.h"       // injectScheduledTurn / activeJobCount

namespace agent {
namespace dream {

// The sessions that belong to the device's owner: the token/physically-
// authenticated local surfaces plus every Telegram chat with the owner role,
// plus "system" (device events - nobody's words).
static std::vector<std::string> ownerSessionIds() {
  std::vector<std::string> ids{"web", "serial", "voice", "system"};
  String owners = store::telegramOwners();
  int start = 0;
  while (start < (int)owners.length()) {
    int end = owners.indexOf(',', start);
    if (end < 0) end = owners.length();
    String one = owners.substring(start, end);
    one.trim();
    if (one.length()) ids.push_back(std::string(one.c_str()));
    start = end + 1;
  }
  // An empty owner list means the FIRST allow-list entry is the owner (the
  // single-account default the rest of the firmware uses).
  if (ids.size() == 4) {
    String first = orchestrator::firstAllowedChat();
    if (first.length()) ids.push_back(std::string(first.c_str()));
  }
  return ids;
}

namespace orch = nimbus::orch;

static agent::Hooks g_hooks;             // only onDreamStart/onDreamEnd used
static DreamGate    g_gateCfg;
static volatile uint32_t g_lastTurnEndMs = 0;   // 0 = no turn since boot

void begin(const agent::Hooks& hooks) {
  g_hooks = hooks;
  g_gateCfg.minHeap = ORCH_AUTO_TURN_MIN_HEAP;  // device-authoritative floor
}

void noteTurnEnd(uint32_t nowMs) { g_lastTurnEndMs = nowMs ? nowMs : 1; }

uint32_t gateDefer(const orch::LoopRecord& l) {
  if (!isReserved(l.id)) return 0;   // gate applies only to the dream loop
  GateInputs in;
  in.nowMs = millis();
  in.lastTurnEndMs = g_lastTurnEndMs;
  in.activeJobs = orchestrator::activeJobCount();
  in.freeHeap = ESP.getFreeHeap();
  GateResult r = evaluateGate(g_gateCfg, in);
  if (r.deferSec)
    alogf("dream: not idle (%s) - deferring %us", r.why ? r.why : "?",
          (unsigned)r.deferSec);
  return r.deferSec;
}

nimbus::orch::FireOutcome fire(const orch::LoopFireRequest& r, bool force) {
  const uint64_t epoch = (uint64_t)time(nullptr);
  if (g_hooks.onDreamStart) g_hooks.onDreamStart(agent::DreamStartEv{epoch});

  // --- phase 1: non-LLM maintenance (runs when the clock is synced; the turn may still defer) --
  // Under the memory Lock (recursive; shared with the web/MCP task). Never held
  // across the network turn below.
  int pruned = 0, deduped = 0, vecs = 0;
  if (!memory::clockSynced()) {
    // Release C3: with a boot-relative clock, decay ages every entry a fake
    // amount and pruneExpired compares real epochs against ~hour-0 - both
    // corrupt retention. The scheduled dream can't fire pre-sync (wall-clock
    // loops need SNTP), but the manual DREAM console path could. Skip, honestly.
    alog("dream: maintenance skipped (clock not synced)");
  } else {
    memory::Lock lk;
    orch::VectorMemory& v = memory::vectors();
    v.decayImportance(memory::config().decayFactor);   // was hardcoded 0.95 (dead knob)
    pruned  = v.pruneExpired(memory::nowHours());
    // Release C2: WINDOWED dedup - the full O(n²·dims) scan at the 5000-entry
    // cap is ~3.2e9 int8 ops against PSRAM UNDER THE GLOBAL MEMORY LOCK
    // (blocking turns + web). Older entries were deduped on previous nights;
    // scanning the newest 1500 (≈1.1M pairs) bounds the pass without losing
    // meaningful coverage.
    deduped = v.deduplicate(1500);
    vecs    = v.size();
    memory::persistVectors();
    alogf("dream: maintenance - decayed, pruned=%d deduped=%d (%d vectors)",
          pruned, deduped, vecs);
  }

  // --- phase 2: reflection turn --------------------------------------------
  // Yesterday ≈ the trailing 24 h of episodic history (wall hours). The digest
  // + inputs are per-fire transients (stack/heap strings freed on return; the
  // digest is byte-capped at kDigestCapBytes) - no steady-state SRAM.
  MemStats stats;
  std::string digest, scratch;
  bool emptyDay = false;
  uint64_t scratchHash = 0;
  {
    memory::Lock lk;
    const uint32_t nowH = memory::nowHours();
    orch::MsgQuery q;
    q.sinceHours = nowH > 24 ? nowH - 24 : 0;
    q.limit = 48;
    // v3.7.0 (prism R2, "namespace laundering"): the reflection turn runs as the
    // OWNER and distils what it reads into the owner's long-term memory. Left
    // unscoped it read EVERY session, so a guest's private conversation would be
    // absorbed into the owner's memories overnight - invisible to the guest and
    // never something they agreed to. The nightly digest is therefore limited to
    // the owner's OWN surfaces plus the device timeline; a member's history stays
    // in their conversation, where the owner can still read it deliberately.
    q.sessionAllow = ownerSessionIds();
    // CONVERSATION kinds only - the documented window contract. Without this the
    // query admits ALL kinds, including the ToolOutput/LlmResponse TRACE rows
    // that the previous night's OWN reflection turn captured into the owner
    // chat (otrace default-on): those seeded the next digest so `emptyDay` was
    // never true on a default SD device, permanently defeating the quiet-night
    // skip (prism, high). A photo/file the owner sent is a real conversation
    // item, so Image/File/Audio ride alongside Message (mirrors the live-turn
    // window at MsgQuery::alsoKinds).
    q.haveKind = true;
    q.kind = orch::MsgKind::Message;
    q.alsoKinds = {orch::MsgKind::Image, orch::MsgKind::File, orch::MsgKind::Audio};
    const auto msgs = memory::episodic().query(q);
    emptyDay = msgs.empty();
    digest = buildEpisodicDigest(msgs);
    memory::scratchpad().appendPromptBlock(scratch);
    scratchHash = orch::fnv64(memory::scratchpad().serialize());
    memory::Stats s = memory::stats();
    stats.vectors = s.vectorCount;
    stats.scratchItems = s.scratchItems;
    stats.episodicMsgs = s.episodicMsgs;
  }
  stats.pruned = pruned;
  stats.deduped = deduped;

  // Quiet-night skip: no owner conversation in 24 h AND the scratchpad is
  // byte-identical to where the LAST dream left it -> the paid reflection turn
  // has provably nothing to do. Stage-1 maintenance above already ran, which
  // is the part that must never be skipped. The console DREAM drill forces
  // through (force). o.detail varies by day so the semantic-repeat guard can't
  // mistake consecutive quiet nights for a stuck loop and pause dreaming.
  {
    const uint64_t lastHash = strtoull(store::dreamScratchHash().c_str(), nullptr, 16);
    if (skipReflection(emptyDay, scratchHash, lastHash, force)) {
      orch::FireOutcome o;
      o.ok = true;
      char fp[128];
      snprintf(fp, sizeof fp,
               "(dream d%lu: reflection skipped - quiet day, scratchpad "
               "unchanged; pruned=%d deduped=%d)",
               (unsigned long)(epoch / 86400), pruned, deduped);
      o.detail = fp;
      alogf("dream: %s", fp);
      if (g_hooks.onDreamEnd)
        g_hooks.onDreamEnd(agent::DreamEndEv{true, pruned, deduped, 0});
      return o;
    }
  }

  const std::string inputs = buildDreamInputs(digest, scratch, stats);

  // Ordinary scheduled turn: same rails (reboot/loop.create refused, LoopCaps
  // metered - attribution tag rides in as "loop:dream"). quietOk: an all-empty
  // reply delivers NOTHING (no 03:30 "Done." ping).
  orch::FireOutcome o = orchestrator::injectScheduledTurn(
      String(r.chatId.c_str()), String(inputs.c_str()), String(r.name.c_str()),
      String(r.id.c_str()), /*quietOk=*/true);

  // The dream's reply is USUALLY empty by design - feed the semantic-repeat
  // hash a per-night fingerprint instead, so kLoopMaxRepeats can't pause the
  // loop for doing its job quietly (the guard still catches a stuck NON-empty
  // reply, which varies... or doesn't, and then deserves the pause).
  if (o.detail.empty()) {
    char fp[64];
    snprintf(fp, sizeof fp, "(dream d%lu: pruned=%d deduped=%d)",
             (unsigned long)(epoch / 86400), pruned, deduped);
    o.detail = fp;
  }
  if (o.ok) {
    uint64_t post;
    { memory::Lock lk; post = orch::fnv64(memory::scratchpad().serialize()); }
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)post);
    store::setDreamScratchHash(hex);
  }
  if (g_hooks.onDreamEnd)
    g_hooks.onDreamEnd(agent::DreamEndEv{o.ok, pruned, deduped, o.tokens.total()});
  return o;
}

}  // namespace dream
}  // namespace agent
