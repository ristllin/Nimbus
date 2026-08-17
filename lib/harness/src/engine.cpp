#include "nimbus/harness/engine.h"

#include "nimbus/orch/budget.h"    // deriveBudget - per-turn caps from the head model's window
#include "nimbus/orch/compact.h"   // ORCH_COMPACT_PROMPT + buildCompactInputs + capSummary (runFold)

#include <ArduinoJson.h>

#include <algorithm>
#include <memory>

#include "nimbus/harness/log.h"
#include "nimbus/orch/turn.h"

// Lifted from src/agent/orchestrator.cpp (Stage G - the final lift: runTurn /
// handleMessage / injectScheduledTurn / maybeConsolidate / buildDynamicContext /
// the stuck-turn reaper). Every owner-visible string, log line, and gate moved
// BYTE-IDENTICAL - grep-diff the literals against the pre-lift file when in
// doubt; test_harness_turn pins the load-bearing ones (the retry/failover
// ladder, the budget refusal, the salvage path, the scheduled-turn rails).

namespace agent {

namespace orch = nimbus::orch;
namespace attn = nimbus::attn;
using solide::ring::Status;

static void trimInPlace(std::string& s) {
  const char* ws = " \t\r\n";
  size_t b = s.find_first_not_of(ws);
  size_t e = s.find_last_not_of(ws);
  s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

// ---- per-chat conversation map (Release B2) ---------------------------------
// One provider conversation PER CHAT - the old single "host|convId" slot made
// every channel (telegram chats, web, voice, serial) interleave into ONE
// provider thread: chat bleed, and each channel's history polluted by the rest.
static std::string convSanitize(std::string v) {
  for (char& c : v) if (c == '=' || c == ';' || c == '|') c = '_';
  return v;
}

std::string convMapGet(const std::string& raw, const std::string& chat,
                       const std::string& host) {
  if (raw.empty() || raw.find('=') == std::string::npos) return "";  // legacy/empty
  std::string key = convSanitize(chat) + "=";
  size_t start = 0;
  while (start < raw.size()) {
    size_t end = raw.find(';', start);
    if (end == std::string::npos) end = raw.size();
    if (raw.compare(start, key.size(), key) == 0) {
      std::string val = raw.substr(start + key.size(), end - start - key.size());
      size_t bar = val.find('|');
      if (bar == std::string::npos || bar == 0) return "";
      if (val.substr(0, bar) != host) return "";   // host switched -> fresh thread
      return val.substr(bar + 1);
    }
    start = end + 1;
  }
  return "";
}

std::string convMapSet(std::string raw, const std::string& chat,
                       const std::string& host, const std::string& convId) {
  if (raw.find('=') == std::string::npos) raw.clear();   // discard legacy format
  std::string key = convSanitize(chat) + "=";
  // Rebuild without this chat's entry (preserving order = age order).
  std::string out;
  int entries = 0;
  size_t start = 0;
  while (start < raw.size()) {
    size_t end = raw.find(';', start);
    if (end == std::string::npos) end = raw.size();
    std::string entry = raw.substr(start, end - start);
    start = end + 1;
    if (entry.empty() || entry.compare(0, key.size(), key) == 0) continue;
    out += entry + ";";
    entries++;
  }
  if (!convId.empty()) {
    // LRU bound: drop the OLDEST entries down to 7 so the append makes 8.
    while (entries >= 8) {
      size_t cut = out.find(';');
      out = (cut == std::string::npos) ? std::string() : out.substr(cut + 1);
      entries--;
    }
    out += key + convSanitize(host) + "|" + convSanitize(convId) + ";";
  }
  return out;
}

TurnEngine::TurnEngine(Deps d, Tuning t) : d_(std::move(d)), t_(t) {}

void TurnEngine::deliver(const std::string& chatId, const std::string& text) {
  if (d_.deliver) d_.deliver(chatId, text);
}

void TurnEngine::emitHead(uint8_t ringStatus) {
  if (!d_.event) return;
  attn::Event e;
  e.type = attn::Event::Type::JobState;
  e.key = keyFromTag("head");
  e.status = ringStatus;
  d_.event(e);
}

void TurnEngine::setHeadArc(bool lit) {
  emitHead((uint8_t)(lit ? Status::Running : Status::Offline));
}

// P6: the loop is the default turn path, but it needs headroom above the
// single-TLS + per-round floor.
// The loop gate evaluated against a SPECIFIC heap reading. runTurn passes the
// turn-ENTRY heap (the admission the turn floor granted) so the verdict is not
// corrupted by recall's transient dip; the out-of-band callers below pass live
// heap, which is fine - they are describing capability, not gating a turn.
bool TurnEngine::loopActiveAt(uint32_t heap) const {
  // +2000 (was +6000): a tool round's INTERNAL cost is a few hundred bytes (the
  // body/JSON are PSRAM-routed), so the old 34 KB gate silently dropped the loop to
  // single-shot on basically every turn in the 28-34 KB band. See docs/memory-model.md.
  return d_.cfg.loop.toolLoopOn && d_.cfg.loop.toolLoopOn() &&
         heap > (t_.loopMinHeap + 2000U);
}
bool TurnEngine::loopActiveNow() const { return loopActiveAt(freeHeap()); }

std::string TurnEngine::takePendingMemResults() {
  if (pendingMemResults_.empty()) return "";
  // BACKGROUND framing (owner field bug 2026-07-16): this block used to lead the
  // turn input with no do-not-latch instruction - the owner sent "try now?" (about
  // Telegram) and the model answered about the injected memory results instead.
  std::string s = std::string("\n[MEMORY RESULTS] (background: outcomes of YOUR memory ops "
                              "from last turn - reference material only. The owner has NOT "
                              "seen this and is NOT asking about it; answer the [USER] "
                              "message below on ITS topic.)\n") + pendingMemResults_;
  pendingMemResults_.clear();
  return s;
}

// The input channel a turn arrived on, keyed by the routing chatId (P6). The
// reply routes back to the SAME channel by default; the model can reach another
// via the reply.speak / reply.telegram tools when the loop is on.
const char* TurnEngine::channelOf(const std::string& chatId) {
  if (chatId == "serial") return "the developer serial console";
  if (chatId == "voice")  return "the device microphone (hold-to-talk); your reply shows on the e-ink panel";
  if (chatId == "web")    return "the web UI";
  return "Telegram; your reply is delivered as a Telegram message to that chat";
}

// [ACTIVE SESSIONS] body - byte-identical to the device jobsSummary() (same
// journal source, reached through JobEngine::sessionInfos).
std::string TurnEngine::jobsSummaryText() const {
  std::vector<orch::SessionInfo> infos =
      d_.jobs ? d_.jobs->sessionInfos() : std::vector<orch::SessionInfo>{};
  if (infos.empty()) return "No active jobs.";
  std::string s;
  for (const auto& r : infos)
    s += r.id + " (" + r.provider + "/" + r.model + ") " + r.title + " [" + r.state + "]\n";
  return s;
}

std::string TurnEngine::buildDynamicContext() {
  std::string ctx = "[ACTIVE SESSIONS]\n";
  ctx += jobsSummaryText();
  // The spawn cap (kAgentMaxJobs) was invisible to the model: it would promise
  // to start N sub-agents, the engine would silently refuse past the cap, and
  // the refusal never reached the turn. Surface the real headroom so it plans
  // within it and says so when it can't start them all (ORCH_D_SOPS).
  {
    // [SPAWN CAPACITY] - the HONEST throughput model (W5). The old "[SPAWN SLOTS]
    // N of 6 free ... over the cap is refused" read as a hard total ceiling, so
    // the head under-spawned a deep run ("only 6 slots"). The truth: the device
    // is single-threaded (one network call at a time) and drains a QUEUE one
    // dispatch per cycle into a small concurrency window - so a big fan-out is
    // fine, it just runs SEQUENTIALLY over successive waves. NOT concurrency.
    const int live   = d_.jobs ? (int)(d_.jobs->sessionInfos().size()) : 0;
    const int queued = d_.jobs ? d_.jobs->pendingCount() : 0;
    int queueFree = orch::kMaxPendingSpawns - queued;
    if (queueFree < 0) queueFree = 0;
    // Bound by the per-TURN parse cap (parseTurn keeps at most kAgentMaxJobs
    // session_ops) AND the queue room, so the number never promises more than a
    // turn can actually contain - no silent truncation.
    const int canStart = std::min(orch::kAgentMaxJobs, queueFree);
    ctx += "[SPAWN CAPACITY] " + std::to_string(live) + " running, " +
           std::to_string(queued) + " queued. You can start up to " +
           std::to_string(canStart) + " sub-agents this turn; they run "
           "SEQUENTIALLY (the device does one network call at a time, up to " +
           std::to_string(orch::kMaxActiveInflight) +
           " in flight), draining as each finishes. This is NOT a hard total "
           "limit - a big fan-out runs over successive waves: start a wave now, "
           "and when they finish you get an automatic turn to spawn the next, so "
           "you can work through dozens over a run. mistral sub-agents run strictly "
           "one-at-a-time; openai/anthropic ones run in parallel on the provider "
           "while the device collects them one by one.\n";
  }
  ctx += "[AVAILABLE PROVIDERS] sub-session priority: " +
         (d_.cfg.provider.subPriority ? d_.cfg.provider.subPriority() : std::string()) + "\n";
  // Name the CURRENT model so the agent knows its own identity (owner: "asking the
  // orchestrator which model it is, it has no idea"). Host = explicit orchHost else the
  // top of providerPriority; the adapter runs store::orchModel(host).
  {
    std::string host = d_.cfg.provider.orchHost ? d_.cfg.provider.orchHost() : std::string();
    if (host.empty()) {
      std::string pr =
          d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
      const size_t comma = pr.find(',');
      host = comma == std::string::npos ? pr : pr.substr(0, comma);
      trimInPlace(host);
    }
    if (!host.empty())
      ctx += "[YOUR MODEL] you are currently running on " + host + " / " +
             (d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(host) : std::string()) + "\n";
  }
  ctx += "[AVAILABLE MODELS] (yours + sub-agents')";
  bool any = false;
  static const char* kProviders[] = {"openai", "anthropic", "mistral"};
  for (const char* p : kProviders) {
    if (!d_.cfg.provider.hasKey || !d_.cfg.provider.hasKey(p)) continue;
    ctx += std::string(" ") + p + ": " + (d_.modelChoices ? d_.modelChoices(p) : std::string()) + ";";
    any = true;
  }
  // The custom LAN endpoint, when configured, is a REAL spawn/head target - its
  // absence here made the honesty rails (correctly) refuse "spawn on custom"
  // even though the registry had it (live-caught 2026-08-12 on the Ollama E2E).
  // No verify pass runs for custom, so the model name comes from config, not
  // modelChoices.
  if (d_.hosts.has("custom") && d_.cfg.provider.hasKey && d_.cfg.provider.hasKey("custom")) {
    ctx += " custom: " +
           (d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel("custom") : std::string()) +
           " (local network model);";
    any = true;
  }
  if (!any) ctx += " (none configured)";
  ctx += "\n";
  // W15: the skills index BEFORE the connector catalog - playbooks frame HOW to
  // use the capabilities the catalog then lists.
  if (d_.skillsIndex) ctx += d_.skillsIndex();
  // Per-provider capability + connector catalog (Phase C): what each provider's
  // turns/sub-agents can actually reach, so spawns are chosen knowingly.
  if (d_.connectorsCatalog) ctx += d_.connectorsCatalog();
  return ctx;
}

// ---- the turn core ----------------------------------------------------------

bool TurnEngine::runTurn(const std::string& inputs, const std::string& chatId,
                         const std::string& userText) {
  // Turn lifecycle guard: flags the in-flight state AND paints the head turn on
  // the ring as a live Running job ("head" key, blue comet in Full) so the user
  // SEES the device thinking - previously a text/voice turn gave no ring feedback
  // at all in Full posture. Offline on destruction frees the arc (also on error
  // paths, since the dtor always runs).
  curChat_ = chatId;   // reply.speak / reply.telegram default target for this turn
  toolReplied_ = false;
  // Lifecycle hook: turn start (observer-only; source set by the entry wrapper).
  if (d_.hooks.onTurnStart) {
    TurnStartEv ev;
    ev.source = turnSource_;
    ev.chatId = chatId;
    d_.hooks.onTurnStart(ev);
  }
  struct TurnGuard {
    TurnEngine* e;
    explicit TurnGuard(TurnEngine* eng) : e(eng) {
      e->turnInFlight_ = true;
      e->turnStartMs_ = e->nowMs();   // arm the stuck-turn reaper
      e->emitHead((uint8_t)Status::Running);   // styleFor(Running): blue comet
    }
    ~TurnGuard() {
      e->turnInFlight_ = false;
      // W6: if this turn spawned sub-agents, DON'T collapse the head arc now -
      // the children are still running (or pending dispatch), and the main-loop
      // head-arc reconciler keeps it lit until the last one finishes. Only a
      // childless turn frees the arc here. (An empty jobs dep => behave as before.)
      const int children = e->d_.jobs ? e->d_.jobs->activeCount() : 0;
      if (children == 0) e->emitHead((uint8_t)Status::Offline);
    }
  } turnGuard(this);

  // Associative recall (Phase 2): one embed of the user text, fail-open. Skipped
  // for synthesis turns (empty userText) and when heap lacks the headroom for a
  // second TLS handshake ahead of the LLM call (ORCH_RECALL_MIN_HEAP rationale).
  if (d_.fire) d_.fire("turnstart");   // heavy tier: "Roger that."
  // ⚠ Decide the tool loop from the TURN-ENTRY heap, ONCE, before recall spends it.
  //
  // The head-loop's own contract (orch_head_loop.cpp round-0 gate) says entry
  // gating is the CALLER's job - the turn floor already admitted this turn - and
  // that blocking round 0 makes the loop strictly LESS available than a
  // single-shot turn at the same heap. But the caller was re-sampling LIVE heap
  // (loopActiveNow()) a few lines below, AFTER recall's TLS embed and prompt
  // composition transiently depress it. Measured on Board 1, identical across
  // three turns: entry ~41 KB -> afterRecall ~30 KB -> afterCompose ~25 KB, so
  // the gate read ~25 KB and deferred the loop EVERY time recall ran - the device
  // could converse but never use a mid-turn tool. The ~10.4 KB recall dip is a
  // recovering transient (lwIP pbufs/PCB freed async on the tcpip thread a few ms
  // after tlsClose): entry heap is byte-identical turn to turn, so it is NOT a
  // leak. Gating on the admission heap fixes the inconsistency and frees no bytes.
  // Rounds AFTER the first still re-gate live (ORCH_LOOP_MIN_HEAP), and a round-0
  // OOM fails soft, so this cannot wedge the device.
  const uint32_t heapAtEntry = freeHeap();
  const bool loopOnThisTurn = loopActiveAt(heapAtEntry);
  // ONE principal for the whole turn: recall, the tool advertisement below, and
  // applyTurn's dispatch all read the same identity, so a chat cannot be
  // advertised one capability set and judged by another (W14).
  const orch::Principal turnWho = d_.apply.whoFor(chatId);
  std::vector<std::string> recalled;
  if (!userText.empty() && freeHeap() >= t_.recallMinHeap && d_.recall)
    recalled = d_.recall(userText, turnWho);
  const uint32_t heapAfterRecall = freeHeap();

  // Gather the LIVE inputs (device closure: HAL health, faults, sessions,
  // scratchpad); the engine fills tools/loopOn/recalled so the prompt's
  // advertisement and the loop wiring below share ONE source.
  // W14: scoped to THIS caller - admin-only tools are omitted for a member or
  // guest turn (they refuse at dispatch, so advertising them only produced a
  // failed call and a confusing walk-back, and described the admin surface).
  std::vector<orch::ToolRegistry::Spec> specs =
      d_.toolSpecs ? d_.toolSpecs(turnWho) : std::vector<orch::ToolRegistry::Spec>{};
  // Pick the host BEFORE composing: the context budget is derived from the head
  // model's window (owner ask 2026-08-05 - caps derive from the context length
  // allowed for the task), so the model must be known when the prompt is sized.
  std::string host = d_.cfg.provider.orchHost ? d_.cfg.provider.orchHost() : std::string();
  if (host.empty()) {
    std::string pr =
        d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
    size_t c = pr.find(',');
    host = (c != std::string::npos && c > 0) ? pr.substr(0, c) : pr;
    trimInPlace(host);
  }

  ComposeInputs ci = d_.composeInputs ? d_.composeInputs(curChat_) : ComposeInputs{};
  // W10 (prism): the speaker line is for a REAL human message only. The device
  // closure knows the chat, not the turn source - a synthesis turn ([FRESH
  // RESULTS] from untrusted sub-agent output) and a scheduled loop both carry a
  // chat id with nobody behind it, and must not be attributed to the owner.
  ci.speakerPresent = !userText.empty() && turnSource_ != TurnSource::Synthesis &&
                      turnSource_ != TurnSource::Loop;
  // Derive the per-turn context budget from the head model's window. Owner
  // overrides (non-zero MemConfig.maxContextBytes / set NVS loop caps) ride in
  // as BudgetOverrides and win verbatim; 0 = auto. At the 200K anchor the
  // derived values equal the old constants, so a fleet on defaults is
  // byte-identical (pinned by test_orch_budget).
  orch::BudgetOverrides bov;
  bov.maxContextBytes = ci.budgetBytes;  // device closure passes MemConfig's value (0 = auto)
  bov.toolResultCap = d_.cfg.loop.resultCap ? d_.cfg.loop.resultCap() : 0;
  bov.toolTotalCap = d_.cfg.loop.totalCap ? d_.cfg.loop.totalCap() : 0;
  const std::string headModel =
      d_.cfg.provider.orchModel ? d_.cfg.provider.orchModel(host) : std::string();
  const orch::ContextBudget budget =
      orch::deriveBudget(orch::modelCtxTokens(host, headModel), bov);
  ci.budgetBytes = budget.systemPromptBytes;
  hlog::logf("budget: ctx=%uK sys=%d res=%u tot=%u (%s)", (unsigned)(budget.ctxTokens / 1000),
             budget.systemPromptBytes, (unsigned)budget.toolResultBytes,
             (unsigned)budget.toolTotalBytes,
             (bov.maxContextBytes > 0 || bov.toolResultCap > 0 || bov.toolTotalCap > 0)
                 ? "owner"
                 : "auto");
  // Advertise only tools that are LIVE on this fabric, under the SANITIZED names
  // the wire accepts (loopToolHidden/loopToolName - shared with the dispatch
  // wiring below so "advertised == callable" can't drift). Verified live:
  // without this block Mistral/OpenAI skip mid-turn tools entirely.
  ci.loopOn = loopOnThisTurn;   // decided from entry heap (above), not live
  ci.tools.clear();
  for (const auto& s : specs) {
    if (loopToolHidden(s.name)) continue;
    ci.tools.push_back(orch::ToolInfo{loopToolName(s.name), s.description});
  }
  ci.recalled = recalled;
  std::string instructions = agent::composeInstructions(ci);
  // ONE line, only when the loop was actually withheld - the interesting case,
  // and it keeps a healthy device's log clean. entry -> recall -> compose shows
  // which phase spent the headroom that the gate then found missing.
  if (!ci.loopOn && d_.cfg.loop.toolLoopOn && d_.cfg.loop.toolLoopOn())
    hlog::logf("turn heap: entry=%u afterRecall=%u afterCompose=%u floor=%u prompt=%uB",
               (unsigned)heapAtEntry, (unsigned)heapAfterRecall, (unsigned)freeHeap(),
               (unsigned)t_.loopMinHeap, (unsigned)instructions.size());
  lastInstructions_ = instructions;   // captured for the PROMPT? console command / diagnostics

  // (host was picked above, before compose - the budget derivation needs it.)
  // Conversation state is per-CHAT + per-host (Release B2): the map
  // "chat=host|convId;..." replaces the single global slot that interleaved
  // every channel into one provider thread. Host-switch reset happens inside
  // convMapGet; legacy single-slot values are discarded (one fresh thread per
  // chat at upgrade - the same reset a failover does routinely).
  std::string stored = d_.cfg.provider.convId ? d_.cfg.provider.convId() : std::string();
  std::string convId = convMapGet(stored, chatId, host);

  // Head multi-turn tool-use loop context (opt-in via the tool-loop toggle). When
  // on, the head can call the memory.*/session.*/web.search registry tools mid-turn
  // and iterate, terminating on orch_turn. The dispatch closure routes through the
  // SAME handleMcp path external MCP clients use - so it takes the memory Lock and
  // persists/appliesConfig after a mutating tool, exactly once, in one place.
  // nullptr => adapters run their original single-shot structured turn.
  HeadTools headTools;
  const HeadTools* headToolsPtr = nullptr;
  // Counts tools the loop actually EXECUTED this turn. Guards the retry/failover
  // below: replaying a whole tool loop would re-run its side effects (mem writes,
  // spawns), so once any tool ran we fail soft instead of retrying.
  auto loopDispatched = std::make_shared<int>(0);
  // P6: the loop is the default turn path, but it needs headroom above the
  // single-TLS + per-round floor. Under memory pressure at turn start, fall back
  // to a single-shot turn (still fully functional - no tools mid-turn) and tell
  // the model why, rather than risk a mid-loop OOM. The ~2x round floor margin
  // keeps this rare on the SD-mounted firmware (~30 KB at turn time).
  if (d_.cfg.loop.toolLoopOn && d_.cfg.loop.toolLoopOn() && !loopOnThisTurn)
    hlog::logf("orchestrator: tool-loop DEFERRED - low entry heap %u (single-shot this turn)",
               (unsigned)heapAtEntry);
  if (loopOnThisTurn) {
    // Advertise only tools that are LIVE on this fabric, under the SANITIZED names
    // the wire accepts (loopToolHidden/loopToolName - shared with the prompt block
    // above so "advertised == callable" can't drift); map back to the registry
    // name on dispatch.
    auto nameMap = std::make_shared<std::map<std::string, std::string>>();
    for (const auto& s : specs) {
      if (loopToolHidden(s.name)) continue;
      std::string adv = loopToolName(s.name);
      (*nameMap)[adv] = s.name;
      headTools.specs.push_back(orch::ToolRegistry::Spec{adv, s.description, s.schemaJson});
    }
    auto mcp = d_.mcpDispatch;
    // v3.7.0: mid-turn tool calls run under the TURN's principal - same chat
    // namespace and owner status the turn's applyTurn writes use, so a member's
    // mid-loop memory.write cannot reach the shared namespace either.
    const nimbus::orch::Principal loopWho = d_.apply.whoFor(chatId);
    // Round prose ("thinking") observer (Glass Box A4): providers forward each
    // round's non-tool text here; the device persists it per-chat.
    headTools.onRoundText = [this](const std::string& text, int round) {
      if (!d_.hooks.onThinking) return;
      ThinkingEv ev;
      ev.chatId = curChat_;
      ev.text = text;
      ev.round = round;
      d_.hooks.onThinking(ev);
    };
    headTools.dispatch = [this, nameMap, loopDispatched, mcp, loopWho](const orch::HeadToolCall& call)
        -> orch::HeadToolResult {
      if (d_.hooks.onToolCall) d_.hooks.onToolCall(call);   // observer-only
      orch::HeadToolResult r;
      r.id = call.id;
      r.name = call.name;
      // Every exit routes through here so onToolResult fires exactly once per call.
      auto done = [this](orch::HeadToolResult& res) -> orch::HeadToolResult {
        if (d_.hooks.onToolResult) d_.hooks.onToolResult(res);
        return res;
      };
      ++(*loopDispatched);
      // Resolve the advertised (sanitized) name back to the registry name.
      auto it = nameMap->find(call.name);
      const std::string& regName = (it != nameMap->end()) ? it->second : call.name;
      JsonDocument rq;
      rq["jsonrpc"] = "2.0";
      rq["id"] = 1;
      rq["method"] = "tools/call";
      rq["params"]["name"] = regName;
      JsonDocument ad;
      if (deserializeJson(ad, call.argsJson) == ArduinoJson::DeserializationError::Ok &&
          ad.is<ArduinoJson::JsonObject>())
        rq["params"]["arguments"] = ad;
      else
        rq["params"]["arguments"].to<ArduinoJson::JsonObject>();
      std::string reqStr; serializeJson(rq, reqStr);
      std::string respStr = mcp ? mcp(reqStr, loopWho) : std::string();   // Lock + dispatch + persist
      JsonDocument rd;
      if (deserializeJson(rd, respStr) != ArduinoJson::DeserializationError::Ok) {
        r.isError = true; r.output = "tool response parse error"; return done(r);
      }
      if (!rd["error"].isNull()) {
        r.isError = true;
        r.output = std::string(rd["error"]["message"] | "dispatch error");
        return done(r);
      }
      ArduinoJson::JsonObjectConst res = rd["result"].as<ArduinoJson::JsonObjectConst>();
      r.isError = res["isError"] | false;
      ArduinoJson::JsonArrayConst content = res["content"].as<ArduinoJson::JsonArrayConst>();
      if (!content.isNull() && content.size() > 0)
        r.output = std::string(content[0]["text"] | "");
      return done(r);
    };
    headTools.cfg.maxRounds          = d_.cfg.loop.rounds ? d_.cfg.loop.rounds() : 12;  // user-tunable (P6)
    headTools.cfg.deadlineMs         =
        (uint32_t)(d_.cfg.loop.deadlineS ? d_.cfg.loop.deadlineS() : 600) * 1000U;
    headTools.cfg.roundMinHeap       = t_.loopMinHeap;                 // measured floor - not tunable
    // Derived (or owner-overridden) caps - deriveBudget already folded the NVS
    // overrides in. Null hooks (host tests without a loop config) keep the
    // legacy constants so their pins stay meaningful.
    headTools.cfg.maxToolResultBytes = d_.cfg.loop.resultCap ? budget.toolResultBytes : 4096;
    headTools.cfg.maxTotalToolBytes  = d_.cfg.loop.totalCap ? budget.toolTotalBytes : 24576;
    if (d_.spillResult)
      headTools.spill = [this, loopWho](const orch::HeadToolResult& full) {
        return d_.spillResult(full.name, full.output, loopWho.ns);
      };
    headToolsPtr = &headTools;
    hlog::logf("orchestrator: tool-loop ON (%d tools, %d rounds/%ds)",
               (int)headTools.specs.size(), headTools.cfg.maxRounds,
               d_.cfg.loop.deadlineS ? d_.cfg.loop.deadlineS() : 600);
  }

  std::string outJson, err;
  orch::TokenUsage turnUsage;   // real provider token usage, summed across
                                // tool-loop rounds AND failover attempts (Phase 0)
  // Lifecycle hook: turn end - fired exactly once on EVERY exit path below
  // (budget refusal, provider failure, parse fail, success), observer-only.
  // `rounds` carries the executed tool-call count (v1 proxy; see hooks.h).
  auto fireTurnEnd = [&](bool okFlag, size_t replyBytes) {
    if (!d_.hooks.onTurnEnd) return;
    TurnEndEv ev;
    ev.ok = okFlag;
    ev.host = host;
    ev.usage = turnUsage;
    ev.rounds = *loopDispatched;
    ev.replyBytes = replyBytes;
    ev.chatId = chatId;
    if (!okFlag) ev.error = err;   // last provider/parse error (reactive-fold seam)
    d_.hooks.onTurnEnd(ev);
  };
  // runTurnHost: OpenAI / Anthropic / Mistral through the ProviderHosts registry.
  // Every host receives the SAME canonical orch schema via its structured-output
  // mode; an unknown host string is treated as unavailable so it fails over.
  // With headToolsPtr set, every host runs the bounded multi-turn tool-use loop
  // instead of the single shot.
  auto runTurnHost = [&](const std::string& h, std::string& cv) -> bool {
    return d_.hosts.run(h, cv, instructions, inputs, outJson, err, headToolsPtr, &turnUsage);
  };
  // Keyed AND runnable: the device registered exactly the three orchTurn* hosts,
  // so registry membership mirrors the pre-lift openai/anthropic/mistral check.
  auto hostHasKey = [&](const std::string& h) -> bool {
    return d_.hosts.has(h) && d_.cfg.provider.hasKey && d_.cfg.provider.hasKey(h);
  };

  // Per-provider LLM token budget (owner: "limit budget per provider"). Opt-in - a
  // limit defaults to 0 (unlimited), so this only fires when the owner SET one and the
  // month's ceiling is reached. Prefer failing OVER to a keyed provider still in
  // budget; only refuse the turn when every keyed provider is exhausted.
  if (d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(host)) {
    std::string pr =
        d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
    std::string alt;
    for (size_t start = 0; start < pr.length();) {
      size_t c = pr.find(',', start);
      if (c == std::string::npos) c = pr.length();
      std::string cand = pr.substr(start, c - start);
      trimInPlace(cand);
      start = c + 1;
      if (cand.length() && hostHasKey(cand) && !d_.cfg.budget.overBudget(cand)) { alt = cand; break; }
    }
    if (alt.length()) {
      hlog::logf("orchestrator: %s over token budget -> failover %s", host.c_str(), alt.c_str());
      host = alt; convId = "";
    } else {
      deliver(chatId, std::string("\xE2\x9A\xA0\xEF\xB8\x8F ") + host +
              " has hit its monthly token budget. Raise the limit in Usage & Budget or wait for the reset.");
      fireTurnEnd(false, 0);
      return false;
    }
  }

  hlog::logf("orchestrator: turn host=%s (conv=%d heap=%u)", host.c_str(),
             (int)(convId.length() > 0), (unsigned)freeHeap());
  const bool convContinued = convId.length() > 0;   // introspection: history state at entry
  // Stage 2 phase 5: LOOP turns run the engine-owned FABRIC loop when the gate
  // is on - retry/failover happens INSIDE the loop, per round, against the
  // shared transcript (executed results are data; nothing re-dispatches). The
  // legacy between-turn retry/failover below then does not apply: it exists
  // precisely because the old loops could not carry a turn across providers.
  // A head the fabric cannot drive (custom) takes the legacy single-shot path
  // below - its step table only knows the cloud providers.
  auto fabricCan = [&](const std::string& h) {
    return !d_.hosts.fabricSupports || d_.hosts.fabricSupports(h);
  };
  const bool fabricOn = headToolsPtr && d_.hosts.fabric &&
                        d_.cfg.loop.midTurnFailover && d_.cfg.loop.midTurnFailover() &&
                        fabricCan(host);
  bool ok;
  if (fabricOn) {
    std::vector<std::string> hostList{host};
    std::string prio =
        d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
    for (size_t start = 0; start < prio.length() && hostList.size() < 3;) {
      size_t c = prio.find(',', start);
      if (c == std::string::npos) c = prio.length();
      std::string cand = prio.substr(start, c - start);
      trimInPlace(cand);
      start = c + 1;
      if (cand.empty() || cand == host || !hostHasKey(cand)) continue;
      if (!fabricCan(cand)) continue;   // custom in the priority list must not
                                        // truncate the ladder mid-switch
      if (d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(cand)) continue;
      hostList.push_back(cand);
    }
    ok = d_.hosts.fabric(
        hostList, instructions, inputs, outJson, err, *headToolsPtr, &turnUsage,
        [&](const std::string& from, const std::string& to) {
          deliver(chatId, std::string("\xE2\x9A\xA0\xEF\xB8\x8F ") + from +
                  " hit trouble mid-task - switching to " + to +
                  ". Your progress this turn carries over.");
          host = to;   // TurnEndEv / conv bookkeeping attribute to the final host
        });
    convId = "";   // stateless loops keep no provider-side conversation
  } else {
    ok = runTurnHost(host, convId);
  }
  // Retry/failover ONLY when no loop tool executed: a replay re-runs the whole tool
  // loop, duplicating side effects (memory writes, spawns). Once tools ran, a failed
  // turn fails soft to the error reply instead. (Fabric turns handled all of this
  // inside the loop - never re-enter here.)
  if (!ok && *loopDispatched > 0)
    hlog::logf("orchestrator: loop ran %d tool(s) before failing - skipping retry/failover",
               *loopDispatched);
  if (!ok && !fabricOn && *loopDispatched == 0) {   // one same-host retry on a fresh conversation
    hlog::logf("orchestrator: turn err (%s) -> fresh conv retry", err.c_str());
    if (d_.platform.delayMs) d_.platform.delayMs(400);
    convId = ""; ok = runTurnHost(host, convId);
  }
  if (!ok && !fabricOn && *loopDispatched == 0) {   // walk the priority list; up to 2 alternates that differ + have a key
    std::string prio =
        d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
    int tried = 0;
    size_t start = 0;
    while (!ok && tried < 2 && start < prio.length()) {
      size_t comma = prio.find(',', start);
      if (comma == std::string::npos) comma = prio.length();
      std::string fb = prio.substr(start, comma - start);
      trimInPlace(fb);
      start = comma + 1;
      if (fb.empty() || fb == host || !hostHasKey(fb)) continue;
      tried++;
      hlog::logf("orchestrator: %s down -> failover to %s", host.c_str(), fb.c_str());
      deliver(chatId, std::string("\xE2\x9A\xA0\xEF\xB8\x8F ") + host + " is unavailable - switching to " +
              fb + " (your recent messages carry over; the provider-side thread restarts).");
      convId = ""; host = fb; ok = runTurnHost(host, convId);
    }
  }
  // Introspection snapshot - success AND failure, so /api/lastturn always shows
  // what the model actually received on the most recent attempt.
  if (d_.hooks.onTurnDebug) {
    TurnDebugEv ev;
    ev.host = host;
    ev.convContinued = convContinued;
    ev.instructions = &instructions;
    ev.inputs = &inputs;
    ev.rawOut = &outJson;
    ev.ok = ok && !outJson.empty();
    d_.hooks.onTurnDebug(ev);
  }
  if (!ok || outJson.empty()) {
    hlog::logf("orchestrator: turn failed (%s)", err.c_str());
    // Honest failure reply (owner, 2026-08-05): the old fixed "trouble reaching
    // the orchestrator" line was itself a falsehood in most failures - the
    // provider often DID answer (4xx / schema refusal), tools may already have
    // run, and the QA judge correctly flagged the wording as dishonest when work
    // had happened. Name the real cause class and state that nothing is still
    // running, so a failed turn can't imply phantom background work.
    std::string cause;
    auto has = [&](const char* s) { return err.find(s) != std::string::npos; };
    if (err.empty())                              cause = "the provider gave no response";
    else if (has("key"))                          cause = "no working provider key (check Capabilities in the web app)";
    else if (has("network"))                      cause = "the provider could not be reached (check the connection)";
    else if (has("HTTP 401") || has("HTTP 403")) cause = "the provider rejected this device's key";
    else if (has("HTTP 429"))                     cause = "the provider rate-limited this device (wait a minute)";
    else if (has("HTTP 5"))                       cause = "the provider had a server error";
    else if (has("budget"))                       cause = "this turn hit its limits before a final answer";
    else {
      cause = err;  // short adapter tokens ("schema parse", "loop failed", …)
      for (char& c : cause) if (c == '\n' || c == '\r') c = ' ';
      if (cause.size() > 100) { cause.resize(100); cause += "…"; }
    }
    deliver(chatId, "That didn't finish - " + cause +
                    ". Nothing is still running; ask again to retry.");
    fireTurnEnd(false, 0);
    return false;
  }
  if (d_.cfg.provider.setConvId) {
    // Re-read at write time (a web convReset may have cleared the slot mid-turn)
    // and upsert only THIS chat's entry - other chats' threads stay intact.
    std::string raw = d_.cfg.provider.convId ? d_.cfg.provider.convId() : std::string();
    d_.cfg.provider.setConvId(convMapSet(raw, chatId, host, convId));
  }
  lastTurnUsage_ = turnUsage;   // real spend for this turn (Local Loops cost caps read this)
  // A real turn completed here (the early-return above already rejected failures), so
  // COUNT it unconditionally - a provider that returns no parseable `usage` object
  // must not leave the turn counter stuck at 0 (owner: 'used chat, count stayed 0').
  // Only the token SUM is gated on real usage data, so in/out totals stay honest.
  turnCount_++;
  if (!turnUsage.empty()) sessionUsage_ += turnUsage;
  // Per-provider monthly usage ledger (owner: budget per provider). Token spend for
  // LLM hosts, in/out split (output bills ~5x input - the $ estimates need it); a
  // Tavily/search call is recorded at its own call site. Rolls month + enforces
  // limits in the portable core; also feeds the daily graph buckets.
  if (d_.cfg.budget.recordTokens) {
    // Cache counters ride ONLY for hosts whose usage EXCLUDES them from the
    // prompt count (Anthropic). OpenAI includes cached tokens in input_tokens -
    // passing its cacheRead would double-meter the $ estimate.
    const bool excludesCache = host == "anthropic";
    d_.cfg.budget.recordTokens(host, turnUsage.promptTokens, turnUsage.completionTokens,
                               excludesCache ? turnUsage.cacheReadTokens : 0,
                               excludesCache ? turnUsage.cacheWriteTokens : 0,
                               attribution_);   // spend attribution: turn/synthesis/loop:<id>
  }
  // cacheR/cacheW (v4.1.1): prompt-cache hits/writes this turn - the proof the
  // caching breakpoints are actually landing, visible in /api/log.
  hlog::logf("orchestrator: turn -> %.80s [tokens in=%u out=%u cacheR=%u cacheW=%u]",
             outJson.c_str(), (unsigned)turnUsage.promptTokens,
             (unsigned)turnUsage.completionTokens,
             (unsigned)turnUsage.cacheReadTokens, (unsigned)turnUsage.cacheWriteTokens);

  // Parse + validate via the PORTABLE contract. On parse failure SALVAGE the reply
  // - never deliver raw wire JSON to a human (owner field bug 2026-07-16: a strict-
  // parse trip sent {"reply":...,"memory":"",...} verbatim to Telegram). The model's
  // JSON is usually syntactically valid (it came from a parsed tool_use input), so
  // pull reply/ask out of it loosely and deliver just that; only when nothing is
  // extractable fall back to a short human-readable error. Full JSON goes to the log.
  orch::Turn turn;
  orch::ParseError perr;
  if (!orch::parseTurn(outJson, turn, perr)) {
    hlog::logf("orchestrator: turn parse fail (%s) json=%.200s", perr.detail.c_str(),
               outJson.c_str());
    std::string salvaged;
    {
      JsonDocument sd;   // loose second parse just for the human-facing text
      if (deserializeJson(sd, outJson) == ArduinoJson::DeserializationError::Ok) {
        const char* rep = sd["reply"] | (const char*)nullptr;
        const char* ask = sd["ask"]   | (const char*)nullptr;
        if (rep && *rep) salvaged = rep;
        if (ask && *ask) { if (salvaged.length()) salvaged += "\n"; salvaged += ask; }
      }
    }
    deliver(chatId, salvaged.length()
                        ? salvaged
                        : std::string("I hit a formatting error composing that reply - please resend."));
    fireTurnEnd(false, salvaged.length());
    return false;
  }
  d_.apply.ttsEnabled = d_.cfg.ttsEnabled ? d_.cfg.ttsEnabled() : false;   // live per-turn (owner toggle)
  ApplyState st;
  st.scheduledTurn       = scheduledTurn_;
  st.quietFallback       = quietTurn_;
  st.toolRepliedThisTurn = toolReplied_;
  st.riskNote            = riskNote_;
  st.pendingMemResults   = &pendingMemResults_;
  bool spawned = false;
  agent::applyTurn(turn, chatId, d_.apply, st, spawned);
  riskNote_  = st.riskNote;   // consumed (cleared) by the portable apply
  lastReply_ = st.lastReply;  // Local Loops semantic-repeat hash
  fireTurnEnd(true, lastReply_.size());
  return true;
}

// ---- consolidation ----------------------------------------------------------

void TurnEngine::maybeConsolidate(const std::string& chatId) {
  if (!d_.jobs || !d_.jobs->hasFreshResults()) return;
  // ⚠ The synthesis turn is UNATTENDED input built from sub-agent results - the
  // classic injection vector (research agents ingest untrusted web content, and
  // their output is spliced into this prompt verbatim). It must carry the same
  // no-risk-switches guard as a scheduled turn: without this, a poisoned result
  // could flip sleepOvr/brightOvr/reboot with nobody at the keyboard (review).
  struct UnattendedGuard {
    TurnEngine* e;
    bool prev;
    std::string prevAttr;
    TurnSource prevSrc;
    explicit UnattendedGuard(TurnEngine* eng)
        : e(eng), prev(eng->scheduledTurn_), prevAttr(eng->attribution_),
          prevSrc(eng->turnSource_) {
      e->scheduledTurn_ = true;
      e->attribution_ = "synthesis";              // spend attribution + hook source
      e->turnSource_ = TurnSource::Synthesis;
    }
    ~UnattendedGuard() {
      e->scheduledTurn_ = prev;
      e->attribution_ = prevAttr;
      e->turnSource_ = prevSrc;
    }
  } unattendedGuard(this);
  if (freeHeap() < t_.autoTurnMinHeap) {
    hlog::logf("orchestrator: defer auto-synthesis (heap %u < %u)",
               (unsigned)freeHeap(), (unsigned)t_.autoTurnMinHeap);
    return;
  }
  // Snapshot the fresh block BEFORE consumption: if the synthesis turn fails
  // (provider outage / failover exhausted), the results must still reach the
  // owner RAW - takeFreshResults() frees the entries, so without this snapshot a
  // failed turn silently destroyed completed sub-agent work (review finding,
  // 2026-07-13: the "60 s fallback" only covers the PRE-consumption defer case).
  std::string fresh  = d_.jobs->takeFreshResults();
  std::string inputs = takePendingMemResults() + fresh + buildDynamicContext() +
    "\n[SYSTEM]\nOne or more of your sub-agents just finished (others may still be "
    "running - see [ACTIVE SESSIONS]). There is no new user message. "
    "Synthesize their results for the owner in reply, and fold anything worth keeping "
    "into memory.";
  hlog::log("orchestrator: auto-synthesis turn (batch complete)");
  if (!runTurn(inputs, chatId, "") && fresh.length()) {
    hlog::log("orchestrator: synthesis turn failed - delivering raw results (never lost)");
    deliver(chatId, fresh);
  }
  if (d_.journalGc) d_.journalGc();
}

// ---- owner-message turn core ------------------------------------------------

void TurnEngine::handleMessage(const std::string& text, const std::string& fromName,
                               const std::string& chatId) {
  if (freeHeap() < t_.turnHardFloor) {
    hlog::logf("orchestrator: user turn deferred, heap %u < floor %u",
               (unsigned)freeHeap(), (unsigned)t_.turnHardFloor);
    deliver(chatId, "One moment \xE2\x80\x94 I'm finishing some background work and low on "
                    "working memory. Please resend in a few seconds.");
    return;
  }
  // Episodic auto-capture (Q2): record the owner's message before the turn so the
  // history is durable + queryable even if the turn later fails.
  // Tag the row with the SENDER (owner ask: "who sent what" in the unified chat -
  // matters once several Telegram accounts talk to one device). Telegram first_name
  // is attacker-influenced: sanitize like approvePending (strip the tags delimiters
  // + control chars, cap 32). Pseudo-channels carry their channel name here.
  std::string fromTag;
  if (!fromName.empty()) {
    fromTag = "from:";
    for (size_t i = 0; i < fromName.length() && fromTag.length() < 37; i++) {
      char ch = fromName[i];
      if (ch >= 0x20 && ch != ',' && ch != ':') fromTag += ch;
    }
  }
  if (d_.episodicCaptureUser) d_.episodicCaptureUser(chatId, text, fromTag);

  attribution_ = "turn";                       // owner-driven spend (the default)
  turnSource_ = (chatId == "serial") ? TurnSource::Serial : TurnSource::Owner;
  std::string inputs = takePendingMemResults() +
    (d_.jobs ? d_.jobs->takeFreshResults() : std::string()) + buildDynamicContext() +
    "\n[CHANNEL] This message arrived via " + channelOf(chatId) +
    ". Your orch_turn `reply` field is ALREADY delivered to this same channel - that is"
    " the one and only answer; never send the same answer twice." +
    (loopActiveNow() ? " reply.speak additionally speaks a line on the device speaker;"
                       " reply.telegram is ONLY for messaging a DIFFERENT allowlisted"
                       " Telegram chat, never for answering this one." : "") +
    "\n\n[USER]\n" + text;
  runTurn(inputs, chatId, text);   // text drives associative recall
}

// ---- Local Loops: scheduled-turn executor -----------------------------------

nimbus::orch::FireOutcome TurnEngine::injectScheduledTurn(const std::string& chatId,
                                                          const std::string& prompt,
                                                          const std::string& name,
                                                          const std::string& loopId,
                                                          bool quietOk, bool once,
                                                          bool ownerReminder) {
  nimbus::orch::FireOutcome o;
  if (freeHeap() < t_.turnHardFloor) {
    o.detail = "deferred: low heap";
    hlog::logf("loops: scheduled turn deferred (heap %u)", (unsigned)freeHeap());
    return o;
  }
  lastReply_.clear();
  scheduledTurn_ = true;
  quietTurn_ = quietOk;
  attribution_ = loopId.empty() ? "loop" : ("loop:" + loopId);   // spend attribution
  turnSource_ = TurnSource::Loop;
  const std::string target =
      chatId.length() ? chatId : (d_.firstAllowedChat ? d_.firstAllowedChat() : std::string());
  // Mirror maybeConsolidate but with a [SCHEDULED LOOP] preamble; do NOT consume
  // fresh results (that clock belongs to sub-agent synthesis). prompt as userText
  // => associative recall runs for the scheduled task. A Once wakeup gets its own
  // honest framing: the model set it FOR ITSELF, it fires exactly once, and the
  // note is the context it asked to have restored.
  const std::string preamble =
    ownerReminder
      ? "\n[REMINDER]\nThe owner set a one-time reminder for this moment (with "
        "/remind) and it just fired. Deliver the reminder to them now - do what "
        "it asks if it's an action, otherwise just remind them plainly. There is "
        "no new owner message; this is the note they left:\n\n"
    : once
      ? "\n[WAKEUP]\nYour one-time wakeup \"" + name + "\" just fired - you scheduled it "
        "for yourself and it has now retired (it will not fire again; set another with "
        "wakeup.set if the work needs one). There is no new owner message. Your note "
        "when you set it:\n\n"
      : "\n[SCHEDULED LOOP]\nYour recurring task \"" + name + "\" is firing on its schedule. "
        "There is no new owner message - do the task now and reply to the owner concisely.\n\n";
  std::string inputs = takePendingMemResults() + buildDynamicContext() + preamble + prompt;
  o.ok = runTurn(inputs, target, prompt);
  scheduledTurn_ = false;
  quietTurn_ = false;
  attribution_ = "turn";                       // back to the owner default
  turnSource_ = TurnSource::Owner;
  o.tokens  = lastTurnUsage_;
  o.detail  = lastReply_;
  return o;
}

// ---- stuck-turn reaper ------------------------------------------------------

// (owner bug: a Telegram turn left the ring pulsing blue forever). The head
// "Running" arc + the in-flight flag are cleared ONLY by the TurnGuard dtor, so
// if the turn task dies (OOM / TLS stall mid-call) the dtor never runs and the blue
// processing ring strands with nothing to reset it (the attention watchdog skips
// Running by design). This runs on the always-alive main loop: if a turn has been in
// flight longer than the loop budget + 2 min margin (so it never trips a legitimately
// long agentic turn), it's presumed dead - free the head arc + clear the flag. Returns
// true when it fired (caller repaints).
// Fold host candidates: explicit orchHost (if usable) first, then the priority
// list in order - each must exist, have a key, and be in budget. Bounded at 3.
// Shared by canFoldNow (any candidate => a fold can run) and runFold (the
// failover ladder walks this exact list).
std::vector<std::string> TurnEngine::foldHostCandidates() const {
  std::vector<std::string> out;
  auto usable = [&](const std::string& h) {
    if (h.empty() || !d_.hosts.has(h)) return false;
    if (d_.cfg.provider.hasKey && !d_.cfg.provider.hasKey(h)) return false;
    if (d_.cfg.budget.overBudget && d_.cfg.budget.overBudget(h)) return false;
    return true;
  };
  auto add = [&](std::string h) {
    trimInPlace(h);
    if (!usable(h)) return;
    for (const auto& e : out)
      if (e == h) return;
    if (out.size() < 3) out.push_back(std::move(h));
  };
  if (d_.cfg.provider.orchHost) add(d_.cfg.provider.orchHost());
  std::string pr =
      d_.cfg.provider.providerPriority ? d_.cfg.provider.providerPriority() : std::string();
  size_t p = 0;
  while (p <= pr.size()) {
    size_t c = pr.find(',', p);
    add(pr.substr(p, (c == std::string::npos ? pr.size() : c) - p));
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return out;
}

bool TurnEngine::canFoldNow() const {
  if (turnInFlight_) return false;
  if (d_.platform.freeHeap && d_.platform.freeHeap() < t_.autoTurnMinHeap) return false;
  return !foldHostCandidates().empty();
}

TurnEngine::FoldResult TurnEngine::runFold(const std::string& chatId,
                                           const std::string& prevSummary,
                                           const std::string& digest,
                                           std::string& outSummary) {
  namespace orch = nimbus::orch;
  outSummary.clear();
  // Gate like an unattended turn: the fold is background work - defer, never
  // squeeze, under memory pressure. Defers are NOT failures (no breaker burn).
  if (d_.platform.freeHeap && d_.platform.freeHeap() < t_.autoTurnMinHeap) {
    hlog::logf("fold(%s): deferred - low heap", chatId.c_str());
    return FoldResult::Deferred;
  }
  if (turnInFlight_) return FoldResult::Deferred;   // never interleave with a live turn

  // Failover ladder (field 2026-08-11): the fold used to pin ONE host - while
  // the head turn's fabric failed over openai->anthropic and succeeded, the
  // fold burned its breaker retrying the same dead provider and spammed the
  // owner. A fold is replay-safe by construction (the reply IS the summary; no
  // applyTurn - no device actions, no mem writes, no spawns), so walking the
  // same <=3-host candidate list the fabric uses is strictly better.
  const std::vector<std::string> hostsToTry = foldHostCandidates();
  if (hostsToTry.empty()) return FoldResult::Deferred;

  // ALWAYS a fresh, throwaway provider thread: the fold must never ride or
  // extend the chat's chain (that is the thing being reset), and its convId is
  // never persisted.
  std::string outJson, err;
  std::string host;
  orch::TokenUsage usage;
  const std::string inputs = orch::buildCompactInputs(prevSummary, digest);
  bool ok = false;
  for (const auto& h : hostsToTry) {
    std::string convId;
    outJson.clear(); err.clear();
    orch::TokenUsage u;
    ok = d_.hosts.run(h, convId, orch::ORCH_COMPACT_PROMPT, inputs,
                      outJson, err, /*tools=*/nullptr, &u);
    if (d_.cfg.budget.recordTokens && !u.empty())
      d_.cfg.budget.recordTokens(h, u.promptTokens, u.completionTokens,
                                 h == "anthropic" ? u.cacheReadTokens : 0,
                                 h == "anthropic" ? u.cacheWriteTokens : 0,
                                 "compact");
    if (ok) { host = h; usage = u; break; }
    hlog::logf("fold(%s): %s failed: %s", chatId.c_str(), h.c_str(), err.c_str());
  }
  if (!ok) return FoldResult::Failed;
  orch::Turn t;
  orch::ParseError perr;
  if (!orch::parseTurn(outJson, t, perr) || t.reply.empty()) {
    hlog::logf("fold(%s): no summary in reply (%s)", chatId.c_str(), perr.detail.c_str());
    return FoldResult::Failed;
  }
  // The reply IS the summary; every other field of the turn is deliberately
  // ignored (no applyTurn - device actions, mem writes, spawns are all inert).
  outSummary = orch::capSummary(t.reply);
  hlog::logf("fold(%s): ok on %s (%u in / %u out)", chatId.c_str(), host.c_str(),
             (unsigned)usage.promptTokens, (unsigned)usage.completionTokens);
  return FoldResult::Ok;
}

void TurnEngine::clearChatConv(const std::string& chatId) {
  if (!d_.cfg.provider.setConvId) return;
  std::string raw = d_.cfg.provider.convId ? d_.cfg.provider.convId() : std::string();
  // convMapSet with an empty convId removes exactly this chat's record.
  d_.cfg.provider.setConvId(convMapSet(raw, chatId, "", ""));
}

bool TurnEngine::reapStuckTurn(uint32_t nowMs) {
  if (!turnInFlight_) return false;
  const uint32_t cap =
      (uint32_t)(d_.cfg.loop.deadlineS ? d_.cfg.loop.deadlineS() : 600) * 1000u + 120000u;
  if ((int32_t)(nowMs - turnStartMs_) < (int32_t)cap) return false;
  turnInFlight_ = false;
  emitHead((uint8_t)Status::Offline);   // collapse the blue head arc
  return true;
}

}  // namespace agent
