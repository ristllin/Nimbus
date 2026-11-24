#include "orchestrator_common.h"

#include <atomic>

#include "store.h"
#include "telegram.h"                   // telegram::enabled - telegramDeliverable
#include "adapters/moderation.h"        // CUM-69 device classifier (behind the gate decision core)
#include "safety_activity_store.h"       // CUM-215 Safety activity log + scoped allowlist (record/allow)
#include "nimbus/orch/moderation.h"     // portable gate decision core (fail-open/closed, admin-exempt)
#include "nimbus/harness/config.h"      // telegramDeliverableFor (host-tested)
#include "nimbus/harness/head_arc.h"    // portable head-arc reconciler (W6)
#include "nimbus/orch/voice_notes.h"     // cutForSpeech (ttsAnnounce)
#include "nimbus/orch/loop_delivery.h"  // telegramOwnerOf: the owner rule from NVS lists (host-tested)
#include "orch_persist.h"               // LittleFsTenantStoreIO - the tenant table on flash
#include "../sys/agent_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>   // the tenant table's, session snapshot's and results ring's mutexes
#include <ArduinoJson.h>       // sessionsJson
#include "../sys/psram_static.h"   // g_journal in PSRAM
#include "nimbus/orch/result_store.h"   // recent-results ring (results.get/list)
#include "loops_subsystem.h"            // loops::firing - a routine's web delivery (deliver)
#include "prefs_subsystem.h"            // standing rules: the delivery filter
#include "memory_subsystem.h"           // memory::captureMessage - the reply sinks' history rows
#include "nimbus/orch/no_dash.h"         // applyOutputFilters (the delivery seam filter)
#include "adapters/audio_tts.h"         // spoken replies (TTS -> Telegram audio / device speaker)
#include "nimbus/tts_catalog.h"         // core::speakerTtsFormat - provider -> speaker format (host-tested)
#include "nimbus/speaker_reply.h"       // speakerFailText - the speaker's honest failure (host-tested)
#include "voice_reply.h"                // reply.telegram voice:true - Telegram with a receipt
#include "nimbus/fault.h"               // the speaker fault (reply.speak)
#include "../sfx/sound_fx.h"            // speakReply / awaitReplyStart (reply.speak); refreshConfig
#include "../sys/config_nvs.h"          // saveDeviceName (fillDeviceApplyDeps)
#include "model_refresh.h"              // R5: the orch_model miss answer (fillDeviceApplyDeps)
#include "nimbus/harness/device_apply.h"   // DeviceApplyDeps (lane E2-P-TOOLS)

// orchestrator_common - the engine-neutral half of the device seam (lanes RB-SEAM1a and
// RB-SEAM1b, no behavior change): job glue, ring arcs, voice, the tenant table, the session
// snapshot and staged terminates, the status getters, the reply sinks and the results ring,
// moved here from the old engine's seam so a second engine can be bound beside it. It
// defines every call orchestrator.h itself declares; an engine seam (orchestrator_e1.cpp)
// defines the calls of engine_api.h, and the two share orchestrator_common.h.
// test/test_engine_api_seam pins the split.

namespace agent {
namespace orchestrator {

namespace orch = nimbus::orch;
namespace attn = nimbus::attn;

// ---- module state shared with the engine seam ------------------------------------
HeavyFabric* g_fabric = nullptr;
Sinks        g_sinks;
// The portable sub-agent job machinery (spawn queue / dispatch gate / round-robin
// poll / fresh results / reap scheduler) - constructed in begin(), Stage F lift.
JobEngine*   g_jobs = nullptr;

// ---- job glue and its status getters -------------------------------------------

bool cancel(const char* tagOrJobId) {
  return g_jobs && g_jobs->cancel(tagOrJobId);
}

int  activeJobCount() { return g_journal.count(); }
int  pendingSpawnCount() { return g_jobs ? g_jobs->pendingCount() : 0; }   // queued, not yet dispatched

// ---- voice -----------------------------------------------------------------------
// Hold-to-talk message lifecycle (CUM-456) - see voiceMessages() in the header.
std::atomic<uint32_t> s_voiceHandled{0};
std::atomic<bool>     s_voiceOk{false};
bool                  s_inVoiceMessage = false;   // tg_poll only

// ---- attention-event helpers (the plan §3.6 integration seam) ---------------
// keyFromTag / ringStatusFor / emitJobState / emitJobCleared live in the portable
// JobEngine; the head-turn Running/Offline arc + the stuck-turn reaper live in
// the portable TurnEngine (reached through the engine seam below). emitAsk is
// turn-application, not job machinery, and rides the device ApplyDeps.

void emitAsk() {
  if (!g_sinks.event) return;
  attn::Event e; e.type = attn::Event::Type::IncomingAsk;
  g_sinks.event(e);
}

// Clear a pending "needs you" ask. Nothing used to fire this on the normal path, so
// an ask latched the Dark/Calm attention LED (and masked every other attention
// source) until the router's ~6-min backstop aged it out. The owner engaging - a
// message or a voice reply - IS the answer, so clear it then. (A scheduled loop
// turn is NOT the owner answering, so it deliberately doesn't call this.)
void clearAsk() {
  if (!g_sinks.event) return;
  attn::Event e; e.type = attn::Event::Type::AskCleared;
  g_sinks.event(e);
}

// Error arcs are calls-to-action: hold them for the tunable attention window
// (Param::AttnHoldMs, fed by main.cpp's applyConfig). Latched here because
// setAttnHoldMs can fire from applyConfig BEFORE begin() constructs the engine.
uint32_t g_attnHoldMs = 300000;  // 5 min default, mirrors the Param preset

// Speak a short confirmation of a job result aloud (opt-in via store::ttsEnabled).
// LIVE-GATED + BENCH-BROKEN: the audio path is unverified here (no key, broken
// bench audio). Compile-clean; the sink is nullable and no-ops when unset.
static void ttsAnnounce(const String& text) {
  if (!g_sinks.speak || !store::ttsEnabled() || text.length() == 0) return;
  int end = -1;
  for (int i = 0; i < (int)text.length() && i < 180; i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?' || c == '\n') { end = i + 1; break; }
  }
  // No sentence end in reach: the speech cut's rule, never in the middle of a character.
  String snippet = (end > 0) ? text.substring(0, end)
                             : String(nimbus::orch::cutForSpeech(std::string(text.c_str()), 180).text.c_str());
  g_sinks.speak(String(), snippet);   // no chat: the owner chat if the speaker cannot play it
}

// ---- reply sinks: who a reply may reach -----------------------------------------

// CUM-523: HarnessConfig::telegramDeliverable on this device - the portable
// agent::telegramDeliverableFor rule (config.h, host-tested) over the live bot,
// allowlist and public-mode facts.
bool telegramDeliverable(const std::string& chatId) {
  return agent::telegramDeliverableFor(agent::telegram::enabled(),
                                       store::telegramAllowlist().length() > 0,
                                       store::telegramPublic(), chatId);
}

// Is `cid` in the persisted Telegram allowlist? Read from NVS (thread-safe) - NOT
// telegram's poll-task-owned g_allowlist, since this runs on the turn task.
bool chatAllowed(const String& cid) {
  String al = store::telegramAllowlist();
  int s = 0;
  while (s < (int)al.length()) {
    int e = al.indexOf(',', s); if (e < 0) e = al.length();
    String t = al.substring(s, e); t.trim();
    if (t.length() && t == cid) return true;
    s = e + 1;
  }
  return false;
}

String firstAllowedChat() {
  String al = store::telegramAllowlist();
  int e = al.indexOf(','); if (e < 0) e = al.length();
  String t = al.substring(0, e); t.trim();
  return t;
}
bool isChatAllowed(const String& cid) { return chatAllowed(cid); }

// ---- ring arcs ---------------------------------------------------------------------

uint32_t headJobKey() { return keyFromTag("head"); }

// CUM-11 backstop metric: every time a belt-and-braces path (not the primary edge)
// has to clear a stuck ring arc, this counts it. The 24 h wake-up soak acceptance is
// this == 0 - the backstops are insurance, never the mechanism. Surfaced in
// /api/state as ring.backstopFires so the soak can assert it stayed flat.
static uint32_t s_ringBackstopFires = 0;
uint32_t ringBackstopFires() { return s_ringBackstopFires; }
void noteRingBackstopFired() { ++s_ringBackstopFires; }

// Stuck-turn reaper - the decision + the arc-freeing event live in the portable
// TurnEngine (loop budget + 2 min margin). Called from the always-alive loop().
bool reapStuckTurn(uint32_t nowMs) {
  const bool fired = engineReapStuckTurn(nowMs);
  if (fired) ++s_ringBackstopFires;   // a dead turn slipped past the primary clear
  return fired;
}

// W6: keep the orchestrator's own "head" ring arc lit while its sub-agents run.
// A fan-out turn returns (and the TurnGuard would clear the arc) the instant it
// enqueues the children - long before they finish - so the head arc collapsed
// before the child arcs appeared. This runs on the always-alive main loop (the
// same watchdog block as reapStuckTurn), asks the pure tracker whether to light,
// refresh, or clear, and drives the arc through the engine. Returns true when it
// changed the arc (so the caller can refreshRing()).
bool reconcileHeadArc(uint32_t nowMs) {
  if (!engineRunning()) return false;
  static nimbus::harness::HeadArcTracker s_tracker;
  static uint32_t s_lastTurnCount = 0;
  // Completion EDGE: turnCount() bumps once per finished turn. A short wake-up turn
  // can start AND finish between two watchdog ticks, so turnInFlight below never
  // samples true - the edge is what tells the tracker a (possibly arc-leaving) turn
  // happened, so the primary clear path fires instead of a backstop (CUM-11).
  const uint32_t tc = turnCount();
  const bool turnEnded = (tc != s_lastTurnCount);
  s_lastTurnCount = tc;
  const bool inTurn = turnInFlight();
  const int children = g_jobs ? g_jobs->activeCount() : 0;
  const auto action = s_tracker.reconcile(inTurn, children, nowMs, turnEnded);
  // Fold the tracker's own frozen-children backstop into the aggregate metric.
  static uint32_t s_lastTrackerBackstops = 0;
  const uint32_t tb = s_tracker.backstopFires();
  if (tb != s_lastTrackerBackstops) { s_ringBackstopFires += tb - s_lastTrackerBackstops; s_lastTrackerBackstops = tb; }
  switch (action) {
    case nimbus::harness::HeadArcTracker::Action::Light:
      engineSetHeadArc(true);
      return true;
    case nimbus::harness::HeadArcTracker::Action::Clear:
      engineSetHeadArc(false);
      return true;
    case nimbus::harness::HeadArcTracker::Action::None:
    default:
      return false;
  }
}

void setAttnHoldMs(uint32_t ms) {   // fed from Param::AttnHoldMs
  g_attnHoldMs = ms;                // latched: applyConfig can fire before begin()
  if (g_jobs) g_jobs->setAttnHoldMs(ms);
}

// Hold-to-talk (CUM-456): the count is read first (acquire), so a reader that sees
// a finished message also sees its ok.
void voiceMessages(uint32_t* handled, bool* lastOk) {
  const uint32_t n = s_voiceHandled.load(std::memory_order_acquire);
  if (handled) *handled = n;
  if (lastOk) *lastOk = s_voiceOk.load(std::memory_order_relaxed);
}

// ---- moderation gates (CUM-69) ---------------------------------------------
// The portable decision core (lib/core/orch_moderation) owns the fail-open/closed
// policy + admin exemption; the device adapter runs the classifier. Every gate is
// owner opt-in (default off) so with the switches off NO classifier call is made
// and the turn path is byte-identical to before.

static nimbus::orch::ModConfig modConfig() {
  nimbus::orch::ModConfig c;
  c.inbound   = store::modInbound();
  c.outbound  = store::modOutbound();
  c.injection = store::modInjection();
  return c;
}

// Run one gate: apply the role/switch gate; if it applies, classify + decide. The
// outbound gate can run while a turn holds the single TLS slot, so it uses a SHORT
// acquire timeout (skips = fail-open if busy) rather than stalling the reply; the
// inbound gate runs pre-turn with the slot free, so it waits the full window.
nimbus::orch::ModAction moderateGate(nimbus::orch::ModGate gate, const String& chatId,
                                     const String& text) {
  using namespace nimbus::orch;
  ModConfig cfg = modConfig();
  Role role = roleOfChat(chatId);
  if (!gateApplies(gate, role, cfg)) return ModAction::Allow;
  const uint32_t acquireMs = (gate == ModGate::OutboundReply) ? 250u : 15000u;
  ClassifierVerdict v = agent::moderation::classify(std::string(text.c_str()), gate, acquireMs);
  ModAction a = decide(gate, v);
  // Safety ACTIVITY surface (CUM-215): a text gate that blocks is a scanner verdict
  // the owner can review. Consult the owner's SCOPED allowlist first: an approved
  // sender/pattern turns the block into a real unblock (never a global off switch);
  // otherwise the verdict is recorded to the bounded activity log for the Safety tab.
  if (a == ModAction::Block) {
    const std::string s(chatId.c_str()), tx(text.c_str());
    const std::string src = modGateName(gate);   // inbound | outbound: the gate binding
    // The classifier returns no sub-category, so the rule is the coarse moderation
    // class (kCoarseModerationRule); the granular gate name goes to `source`. The
    // owner can unblock this sender or this exact content, but not "this type" (that
    // would be the whole gate - forbidden by contentClassApprovable). The allow-rule is
    // bound to this gate's source, so an inbound approval never quiets the outbound gate.
    if (agent::safety::allowed(kCoarseModerationRule, s, src, tx)) return ModAction::Allow;
    // Record ONLY a genuine classifier flag. A fail-closed Error (no key / outage) also
    // returns Block here, but it is the absence of a verdict, not flagged content: do not
    // persist unscreened guest text as "blocked" or make it reportable (CUM-215).
    if (blockIsScannerVerdict(v))
      agent::safety::record(SafetyVerdict::Blocked, kCoarseModerationRule, "telegram", s, src, tx);
  }
  return a;
}


// v3.7.0 RBAC: the tenant table (roles + quotas). Unlike the rest of the
// orchestrator state this genuinely has TWO writers - the web surface
// (/api/tenant, AsyncTCP) and the assistant's tenant.* tools (tg_poll, or
// AsyncTCP again via POST /mcp) - and both need a SYNCHRONOUS answer, because
// "that is the only admin" must come back as a 409 rather than land silently a
// second later. So it gets a real mutex instead of the staged-flag pattern: a
// push_back reallocating the vector under a concurrent find() is a
// use-after-free, not a skewed counter.
//
// Every access goes through the tenant* helpers below, which return values, not
// references - nothing may hold a pointer into the table across a task switch.
static nimbus::orch::TenantStore g_tenants;
// STATIC allocation, not lazy creation. "if (!mux) mux = create()" is itself a
// race: two tasks reaching it together both create one, each takes a different
// mutex, and the lock silently stops excluding anything - the exact bug this
// lock exists to prevent, now invisible. A static mutex needs no heap and
// exists before the first task runs.
static StaticSemaphore_t g_tenantMuxBuf;
static SemaphoreHandle_t g_tenantMux =
    xSemaphoreCreateRecursiveMutexStatic(&g_tenantMuxBuf);
struct TenantLock {
  TenantLock() { xSemaphoreTakeRecursive(g_tenantMux, portMAX_DELAY); }
  ~TenantLock() { xSemaphoreGiveRecursive(g_tenantMux); }
  TenantLock(const TenantLock&) = delete;
  TenantLock& operator=(const TenantLock&) = delete;
};

// ---- v3.7.0 tenant table ----------------------------------------------------
static std::vector<std::string> splitIds(const String& csv) {
  std::vector<std::string> out;
  int start = 0;
  while (start < (int)csv.length()) {
    int end = csv.indexOf(',', start);
    if (end < 0) end = csv.length();
    String one = csv.substring(start, end);
    one.trim();
    if (one.length()) out.push_back(std::string(one.c_str()));
    start = end + 1;
  }
  return out;
}

void loadTenants() {
  TenantLock lk;
  g_tenants.load(LittleFsTenantStoreIO::load());
  // First boot after the upgrade (or after a rollback wrote nothing): rebuild
  // from the legacy lists so nobody's access changes by surprise and the device
  // always has an admin.
  if (g_tenants.all().empty()) {
    g_tenants.adoptLegacy(splitIds(store::telegramOwners()),
                          splitIds(store::telegramAllowlist()));
    LittleFsTenantStoreIO::save(g_tenants.dump());
    alogf("rbac: adopted %u legacy tenants (%u admin)",
          (unsigned)g_tenants.all().size(), (unsigned)g_tenants.adminCount());
  }
}

void persistTenants() {
  TenantLock lk;
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump()))
    alog("rbac: tenant table did not persist - roles may revert on restart");
}

nimbus::orch::Role roleOfChat(const String& chatId) {
  using nimbus::orch::Role;
  // The token/physically-authenticated local surfaces are the device itself.
  if (chatId == "web" || chatId == "serial" || chatId == "voice") return Role::Admin;
  const std::string id(chatId.c_str());
  Role r;
  bool known;
  bool tableHasAdmin;
  {
    TenantLock lk;
    known = g_tenants.known(id);
    r = g_tenants.roleOf(id);
    tableHasAdmin = g_tenants.adminCount() > 0;
  }
  // An EXPLICIT row is the answer, including an explicit Unknown - that is what
  // revoking someone writes, and revoking must not be undone by the fallback
  // below. (A revoked chat deliberately stays on the Telegram allowlist so they
  // can still be told they no longer have access.)
  if (known) return r;
  // No row at all: while no Admin row exists, the chat the Telegram owner rule names
  // is the owner (a freshly onboarded owner may change the device before the table
  // holds a row); otherwise an allow-listed chat is a User; anyone else is Unknown
  // until approved (nimbus::orch::roleWithoutRow). The owner rule reads NVS
  // (thread-safe), never telegram's poll-task-owned g_owners/g_allowlist: roleOfChat
  // also runs on the web task (the /api/test seams), and the poll task reassigns those
  // Strings on every allowlist reload.
  return nimbus::orch::roleWithoutRow(
      isChatAllowed(chatId),
      nimbus::orch::telegramOwnerOf(std::string(store::telegramAllowlist().c_str()),
                                     std::string(store::telegramOwners().c_str()), id),
      tableHasAdmin);
}

// ---- tenant mutations (locked; every surface goes through these) -------------
// Each takes the lock, mutates, persists, and returns a plain value. Nothing
// hands out a reference into the table, so a caller on another task cannot hold
// a pointer across a reallocation.

std::vector<nimbus::orch::Tenant> tenantSnapshot(size_t* adminsOut) {
  TenantLock lk;
  if (adminsOut) *adminsOut = g_tenants.adminCount();
  return g_tenants.all();                      // by value, under the lock
}

bool tenantQuotaOf(const std::string& chatId, nimbus::orch::Quota& out) {
  TenantLock lk;
  const nimbus::orch::Tenant* t = g_tenants.find(chatId);
  if (!t) return false;
  out = t->quota;
  return true;
}

bool tenantSetRole(const std::string& chatId, nimbus::orch::Role r, std::string& err) {
  TenantLock lk;
  if (!g_tenants.setRole(chatId, r, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}

void tenantApproved(const std::string& chatId, bool telegramOwner) {
  TenantLock lk;
  std::string err;
  if (!nimbus::orch::noteApproved(g_tenants, chatId, telegramOwner, err)) {
    if (!err.empty()) alogf("rbac: approved %s without a row: %s", chatId.c_str(), err.c_str());
    return;
  }
  if (LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) return;
  g_tenants.load(LittleFsTenantStoreIO::load());   // RAM never disagrees with storage
  alogf("rbac: approved %s, its row did not persist", chatId.c_str());
}

bool tenantSetQuota(const std::string& chatId, const nimbus::orch::Quota& q,
                    std::string& err) {
  TenantLock lk;
  if (!g_tenants.setQuota(chatId, q, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}

bool tenantRemove(const std::string& chatId, std::string& err) {
  TenantLock lk;
  // Removing the ROW revokes access and frees a slot; it deliberately does NOT
  // erase what the person stored. An admin can still read those namespaces, and
  // silently destroying data as a side effect of a role change would be a nasty
  // surprise. Deleting the data is a separate, explicit act.
  if (!g_tenants.remove(chatId, err)) return false;
  // A change that did not reach the disk must not report success: the next boot
  // would load the OLD table, silently undoing a demotion the admin believes
  // landed. Roll the in-RAM table back so it never disagrees with storage.
  if (!LittleFsTenantStoreIO::saveChecked(g_tenants.dump())) {
    g_tenants.load(LittleFsTenantStoreIO::load());
    err = "couldn't save that change (storage full?) - nothing was changed";
    return false;
  }
  return true;
}


// PSRAM (psram_static.h): about 1.7 KB that internal RAM no longer holds. Task
// context only (single-writer on tg_poll), no lock member, so psramObject applies.
orch::Journal&               g_journal = ::nimbus::psramObject<orch::Journal>();   // shared (orchestrator_common.h)

// ---- sub-agent journal: cross-task snapshot + staged terminate (prism F24) ---
// The sub-agent journal (g_journal) is SINGLE-WRITER on the tg_poll task - pump()
// update/markSeen/gc it every cycle. External MCP clients reach session.list and
// session.terminate on the AsyncTCP web task, which must NEVER touch the journal
// concurrently (a poll firing mid-request could tear a record / corrupt job state).
// So, mirroring loops:: web-staging and the g_memEcho model:
//   * READS  (session.list, sessionsJson) go through a snapshot rebuilt on tg_poll
//     after each pump(), copied out under a short mutex - never the journal;
//   * WRITES (session.terminate) are STAGED from the web task and drained on
//     tg_poll (terminate becomes async - the remote cancel is best-effort anyway).
// The turn's OWN session_ops[] terminate stays synchronous: it runs on tg_poll via
// d.cancelSession -> cancel(), already the journal's writer task, so it's untouched.
SemaphoreHandle_t        g_sessMux = nullptr;   // guards g_sessSnapshot
// A FreeRTOS mutex held for one scope and given back on every path out, an exception
// included (a turn catches std::bad_alloc, L24). A null mutex holds nothing.
struct MuxHold {
  SemaphoreHandle_t m;
  explicit MuxHold(SemaphoreHandle_t mux) : m(mux) {
    if (m) xSemaphoreTake(m, portMAX_DELAY);
  }
  ~MuxHold() {
    if (m) xSemaphoreGive(m);
  }
  MuxHold(const MuxHold&) = delete;
  MuxHold& operator=(const MuxHold&) = delete;
};
static std::vector<nimbus::orch::SessionInfo> g_sessSnapshot;
static portMUX_TYPE             g_termSpin = portMUX_INITIALIZER_UNLOCKED;
static std::vector<std::string> g_termQueue;           // guarded by g_termSpin

// Rebuild the AsyncTCP-readable snapshot from the journal. Called ONLY on tg_poll
// (same task as pump), so the sessionInfos() journal read is race-free; the vector
// is built outside the lock and swapped in (O(1), no alloc while the mutex is held).
void refreshSessionSnapshot() {
  std::vector<nimbus::orch::SessionInfo> fresh =
      g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
  MuxHold hold(g_sessMux);
  g_sessSnapshot.swap(fresh);
}

// Apply web-staged terminates on tg_poll (the journal's single writer). Swap the
// queue out under the spinlock (O(1)), then cancel() each outside it. A bad/stale
// id is a harmless no-op (the job already finished between staging and draining).
void drainTerminates() {
  std::vector<std::string> work;
  portENTER_CRITICAL(&g_termSpin);
  work.swap(g_termQueue);
  portEXIT_CRITICAL(&g_termSpin);
  for (const std::string& id : work) {
    bool ok = g_jobs && g_jobs->cancel(id.c_str());
    alogf("orchestrator: staged terminate %s -> %s", id.c_str(), ok ? "cancelled" : "not found");
  }
}

#ifdef NIMBUS_TEST
static volatile bool g_testReboot = false;
void stageTestReboot() { g_testReboot = true; }
bool testRebootRequested() { return g_testReboot; }
#endif

static const char* stateStr(orch::JobState s) {
  switch (s) {
    case orch::JobState::Queued:     return "queued";
    case orch::JobState::Running:    return "running";
    case orch::JobState::NeedsInput: return "needs_input";
    case orch::JobState::Done:       return "done";
    case orch::JobState::Error:      return "error";
    case orch::JobState::Cancelled:  return "cancelled";
    default:                         return "unknown";
  }
}

String jobsSummary() {
  int n = g_journal.count();
  if (n == 0) return "No active jobs.";
  String s;
  for (int i = 0; i < n; i++) {
    orch::JobRecord rec;
    if (!g_journal.get(i, rec)) continue;
    s += String(rec.tag) + " (" + rec.backend + "/" + rec.model + ") " +
         rec.category + " [" + stateStr(rec.state) + "]\n";
  }
  return s;
}

String sessionsJson() {
  // AsyncTCP task: read the cross-task snapshot, NOT g_journal (single-writer on
  // tg_poll - a direct read here would race pump()). std::string fields are copied
  // into the doc, so the local `snap` staying alive through serializeJson is enough.
  auto snap = sessionInfosSnapshot();
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (const auto& s : snap) {
    JsonObject o = a.add<JsonObject>();
    o["tag"]      = s.id;
    o["backend"]  = s.provider;
    o["model"]    = s.model;
    o["category"] = s.title;
    o["state"]    = s.state;
  }
  String out; serializeJson(d, out); return out;
}

// tg_poll ONLY (the journal's writer task) - used by composeInputs on the turn.
// AsyncTCP/MCP callers must use sessionInfosSnapshot() instead.
std::vector<nimbus::orch::SessionInfo> sessionInfos() {
  return g_jobs ? g_jobs->sessionInfos() : std::vector<nimbus::orch::SessionInfo>{};
}

// Cross-task-safe read of the session snapshot (see refreshSessionSnapshot).
std::vector<nimbus::orch::SessionInfo> sessionInfosSnapshot() {
  std::vector<nimbus::orch::SessionInfo> out;
  MuxHold hold(g_sessMux);   // the copy allocates: given back on every path out (L24)
  out = g_sessSnapshot;
  return out;
}

bool sessionKnown(const std::string& id) {
  MuxHold hold(g_sessMux);
  for (const auto& s : g_sessSnapshot)
    if (s.id == id) return true;
  return false;
}

// ---- recent-results ring (results.get/list) ---------------------------------
// Own mutex (same pattern as g_sessMux): writers are tg_poll (clamp spill /
// fresh-result overflow), readers include the AsyncTCP /mcp task.
static nimbus::orch::ResultStore g_results;
static SemaphoreHandle_t g_resultsMux = xSemaphoreCreateMutex();

std::string resultsPut(const char* kind, const std::string& name, const std::string& fullText,
                       const std::string& jobTag, const std::string& ns) {
  MuxHold hold(g_resultsMux);   // the store allocates: given back on every path out (L24)
  return g_results.put(kind, name, fullText, millis(), jobTag, ns);
}

bool resultsGet(const std::string& tag, size_t& offset, size_t maxBytes, std::string& out,
                size_t& total, const nimbus::orch::Principal& who) {
  MuxHold hold(g_resultsMux);
  return g_results.getAt(tag, offset, maxBytes, out, total, who);
}

std::string resultsList(const nimbus::orch::Principal& who) {
  MuxHold hold(g_resultsMux);
  return g_results.list(who);
}

void stageTerminate(const std::string& id) {
  portENTER_CRITICAL(&g_termSpin);
  g_termQueue.push_back(id);   // bounded (kAgentMaxJobs); drained on tg_poll
  portEXIT_CRITICAL(&g_termSpin);
}


// ---- delivery helpers -------------------------------------------------------

// `system` marks GENUINE device-authored deterministic copy (command replies,
// confirmations, the inbound-block notice) - an out-of-band provenance signal set by
// the emitting call site, never inferred from the text. It is the ONLY thing that
// exempts a reply from the outbound screen. Everything guest-steerable - a model
// reply, a job result - is delivered with system=false (the default) and always
// screened. (CUM-275: the old exemption keyed on text.startsWith(deviceName), which
// a guest could prompt-inject the model into reproducing.)
// Returns false ONLY when the outbound screen suppressed the message (a full reply
// queue is logged, not "undelivered": the message still belongs in the history).
// CUM-826: whether the last deliver() on this task queued no content of its own, so the
// chat's history row is the owner's only copy: a routine's delivery to the web chat is
// async (the open page re-renders its history), and a full reply queue or a suppressed
// message queued nothing. The engine asks it right after a delivery returned
// (deliveryNeedsRow): the fire then spoke only once that row is written. Turn task.
bool s_lastDeliveryNeedsRow = false;
bool screenReply(const String& chatId, const String& text, bool system, String& body) {
  // Lane J (D-3): the owner's standing rules can switch on device-enforced
  // output filters. "Never use em dashes" is exactly the kind of rule a model
  // slips on in long formatted replies, so the rule ALSO strips them here, at
  // the one seam every outbound reply crosses (Telegram, web chat, voice) -
  // after the model, before the owner. Owner-facing turns only; device copy is
  // exempt (it never carries one anyway).
  body = text;
  const uint32_t filterMask = prefs::filtersForRole(roleOfChat(chatId) == nimbus::orch::Role::Admin);
  if (!system && filterMask)
    body = String(nimbus::orch::applyOutputFilters(std::string(text.c_str()), filterMask).c_str());
  // Gate 2 (outbound to guests, fail-open): when the owner turns it on, a reply to
  // a non-admin chat is screened; a flagged reply is suppressed (a classifier
  // outage fails open, so an outage never silences the assistant). Admin/web/serial
  // /voice resolve to Admin and are never screened (gateApplies returns Allow fast,
  // no classifier call). Runs on the delivering task, same TLS-slot discipline as
  // the fetch scan. Genuine device system copy is exempt so we never pay a classifier
  // call to screen our own known-safe strings - but only on provenance, not content.
  if (!nimbus::orch::outboundExempt(system) &&
      moderateGate(nimbus::orch::ModGate::OutboundReply, chatId, body) == nimbus::orch::ModAction::Block) {
    alogf("moderation: outbound reply to %s suppressed (flagged)", chatId.c_str());
    return false;
  }
  return true;
}

// `queuedOut` (optional): whether the reply sink took the message (candidate 5, G4: the daily
// fold alert is spent only then).
bool deliver(const String& chatId, const String& text, bool system, bool* queuedOut) {
  if (queuedOut) *queuedOut = false;
  s_lastDeliveryNeedsRow = true;   // until the sink took the content
  String body;
  if (!screenReply(chatId, text, system, body)) return false;
  // The send sink enqueues onto a bounded reply queue and returns false if it was
  // still full after its short block. The queue is now sized for a full turn's burst
  // (see REPLY_QUEUE_DEPTH), but if it ever overflows anyway, log it rather than drop
  // the owner's message silently - a dropped reply is otherwise invisible.
  // A routine's output to the web chat (CUM-505) goes through the sink like any
  // other: orchSendSink hands it to the web chat book as an ASYNC delivery (the open
  // page re-renders its history, where captureAssistant records it), never into the
  // legacy single slot or any turn's reply (agent::deliverToWebChat).
  const bool queued = g_sinks.send && g_sinks.send(chatId, body);
  if (queuedOut) *queuedOut = queued;
  if (g_sinks.send && !queued)
    alogf("orchestrator: reply queue full - dropped %d-byte message to %s",
          (int)body.length(), chatId.c_str());
  s_lastDeliveryNeedsRow = !queued || (chatId == "web" && loops::firing());
  return true;
}

// ---- output-channel helpers (P6: reply.speak / reply.telegram tools) --------
// How long reply.speak waits for the sound task to open the speaker: a short event
// clip or a sync step ahead of it in the queue, and a paused music track handing back
// the speaker, all fit.
static constexpr uint32_t kSpeakStartWaitMs = 10000;

bool speakOnDevice(const String& text, bool capture, String* why) {
  auto fail = [why](const String& w) {
    if (why) *why = w;
    return false;
  };
  if (text.length() == 0) return fail("nothing to say");
  if (nimbus::fault::active(nimbus::fault::SPEAKER)) return fail("the speaker is marked faulty");
  const size_t chars = nimbus::orch::textChars(std::string(text.c_str()));
  if (chars > nimbus::orch::kSpeakMaxChars) return fail(nimbus::orch::speakTooLongText(chars).c_str());
  // Synthesize in the format the CONFIGURED provider actually emits, then play it
  // on the speaker (I2S TX, independent of the mic). OpenAI -> WAV (played directly);
  // Mistral/Voxtral -> MP3 (decoded by the vendored minimp3). This is the field-bug
  // fix: the old path forced "wav", but a Mistral-only device can't make WAV, so
  // synth returned 0 and the device NEVER spoke. speakerTtsFormat() is host-tested.
  bool mp3 = false;
  // Use the EFFECTIVE provider (accounts for a key-driven fallback to the other
  // provider), so the format we synthesize matches the provider that will voice it.
  const char* fmt = core::speakerTtsFormat(std::string(agent::tts::activeProvider().c_str()), &mp3);
  const char* path = mp3 ? "/reply.mp3" : "/reply.wav";
  String ttsWhy;
  size_t n = agent::tts::synthesizeToFile(text, path, fmt, nullptr, &ttsWhy);
  if (!n) return fail(String("text-to-speech failed: ") + ttsWhy);
  // QUEUE the playback on the sfx task - never play inline. This runs on
  // tg_poll inside a tool round; the synth fetch above is bounded (~4 s) but
  // fetch + an 8 s clip held the tool dispatch locks past loopTask's 8 s
  // watchdog budget: task_wdt abort, observed in the field as "the harness
  // reset itself" on 'count to ten out loud'. Then wait (bounded, on this task,
  // which no watchdog watches) for the sfx task to open the speaker: a clip is
  // spoken only once it is playing, never when it was merely queued (22 Oct).
  const uint32_t ticket = ::sfx::speakReply(mp3);
  if (!ticket) {
    alogf("speak: could not queue %s (%u bytes) - sfx queue full/off?", path, (unsigned)n);
    return fail("the sound queue is full or off");
  }
  String playWhy;
  if (!::sfx::awaitReplyStart(ticket, kSpeakStartWaitMs, &playWhy)) {
    alogf("speak: %s", nimbus::sfx::speakerFailText(playWhy.c_str()).c_str());
    return fail(playWhy);
  }
  noteTurnSpoke();   // CUM-826: the owner hears it (before the history row, which can fail)
  // Capture spoken output so a voice-only reply is still in history ("what did you
  // just say?" works on stateless hosts once device history reads these rows). Skip
  // when the caller already records it (the tts device action captures in apply.cpp),
  // so the same utterance is not written to history twice.
  if (capture) memory::captureMessage(currentChat().c_str(), "assistant",
                                      orch::MsgKind::Message, text, "",
                                      withTurnTag("via:speaker"));
  return true;
}

bool sendToChat(const String& chatId, const String& text, bool asVoice, std::string* result,
                voicereply::Outcome* voiceOut) {
  String cur = currentChat();
  String cid = chatId.length() ? chatId : cur;
  // serial/voice/web aren't Telegram-deliverable channels.
  if (cid.length() == 0 || cid == "serial" || cid == "voice" || cid == "web") return false;
  if (text.length() == 0) return false;
  // SECURITY (prism): the model must not be able to exfiltrate to an arbitrary
  // chat. Only the CURRENT turn's chat (implicit) or an ALLOWLISTED chat may be
  // messaged - the allowlist is the Telegram auth boundary, inbound AND outbound.
  if (cid != cur && !chatAllowed(cid)) return false;
  if (asVoice) {
    // Telegram, never the speaker (22 Oct: the speaker went first and failed silently,
    // and the tool still said "sent"). The result is the receipt or why nothing arrived.
    const voicereply::Outcome o = voicereply::deliver(cid, text);
    if (result) *result = o.toolText;
    if (voiceOut) *voiceOut = o;
    if (o.notesSent > 0) {
      // Noted before the history row (which can fail): a voice note reached the owner
      // (the current chat or an allowlisted one), so the fire spoke (CUM-826).
      noteTurnSpoke();
      if (cid == cur && engineRunning()) engineNoteToolReplied(std::string(o.spokenText.c_str()));
      // Capture what reached the TARGET chat to its history.
      memory::captureMessage(cid.c_str(), "assistant", orch::MsgKind::Message, o.spokenText, "",
                             withTurnTag("via:telegram"));
    }
    return o.delivered;
  }
  const bool sent = g_sinks.send && g_sinks.send(cid, text);
  // The current turn's chat already got a real answer mid-loop - remember it so
  // applyTurn doesn't append a contentless "Done." as a second message.
  if (sent) {
    // Queued for the owner (the current chat or an allowlisted one): the fire spoke
    // (CUM-826), noted before the history row, which can fail.
    noteTurnSpoke();
    if (cid == cur && engineRunning()) engineNoteToolReplied(std::string(text.c_str()));
    // reply.telegram output was invisible to history - capture it (to the TARGET
    // chat, so a cross-chat send lands in the right conversation).
    memory::captureMessage(cid.c_str(), "assistant", orch::MsgKind::Message, text, "",
                           withTurnTag("via:telegram"));
  }
  return sent;
}

// Lane E2-P-TOOLS: engine 1's device-action executors (buildApplyDeps), shared with engine 2's
// device_control. Returns from execConfig whether a stored value CHANGED (23 Oct: a no-op
// action reports "no change" and never backs a "switched" reply).
namespace {

// The knobs stored as they are (engine 1's execConfig, split by lane E2-P-TOOLS for complexity).
bool execStoredKnobs(const nimbus::orch::ValidatedAction& va) {
  bool changed = false;
  if (va.hasLedBrightness) changed = store::setLedBright(va.ledBrightness) || changed;
  // `priority` tunes the SUB-SESSION provider preference only (never the
  // orchestrator-host list - see the protected-key rail).
  if (va.hasPriority)  changed = store::setSubPriority(String(va.priority.c_str())) || changed;
  if (va.hasTtsVoice)  changed = store::setTtsVoice(String(va.ttsVoice.c_str())) || changed;
  if (va.hasTtsProv)   changed = store::setTtsProvider(String(va.ttsProv.c_str())) || changed;
  if (va.hasTtsOn)     changed = store::setTtsEnabled(va.ttsOn) || changed;
  if (va.hasSleepOvr)  changed = store::setSleepOvr(va.sleepOvr) || changed;
  if (va.hasBrightOvr) changed = store::setBrightOvr(va.brightOvr) || changed;
  if (va.hasSttProv)   changed = store::setSttProvider(String(va.sttProv.c_str())) || changed;
  return changed;
}

// The sound knobs, applied live through the seam the web setters use.
bool execSoundKnobs(const nimbus::orch::ValidatedAction& va) {
  if (!(va.hasSfxLvlN || va.hasSfxLvlO || va.hasSfxVol || va.hasSfxTheme)) return false;
  bool changed = false;
  if (va.hasSfxLvlN)  changed = store::setSfxLevelNotif(va.sfxLvlN) || changed;
  if (va.hasSfxLvlO)  changed = store::setSfxLevelOrch(va.sfxLvlO) || changed;
  if (va.hasSfxVol)   changed = store::setSfxVolume(va.sfxVol) || changed;
  if (va.hasSfxTheme) changed = store::setSfxTheme(String(va.sfxTheme.c_str())) || changed;
  ::sfx::refreshConfig();
  return changed;
}

bool execDeviceName(const nimbus::orch::ValidatedAction& va) {
  if (!va.hasDevName) return false;
  const String before = nimbus::sys::deviceName();
  return nimbus::sys::saveDeviceName(String(va.devName.c_str())) != before;
}

}  // namespace

void fillDeviceApplyDeps(DeviceApplyDeps& d) {
  d.execConfig = [](const nimbus::orch::ValidatedAction& va) -> bool {
    const bool stored = execStoredKnobs(va);
    const bool sound = execSoundKnobs(va);
    const bool named = execDeviceName(va);
    return stored || sound || named;
  };
  if (g_sinks.device)
    d.stageDevice = [](const nimbus::orch::ValidatedAction& va) { g_sinks.device(va); };
  if (g_sinks.speak)
    d.speak = [](const std::string& chat, const std::string& s) {
      return g_sinks.speak(String(chat.c_str()), String(s.c_str()));
    };
  d.speakerVolume = [] { return (int)store::sfxVolume(); };   // live: a tts at volume 0 plays silence
  // Lane F: the check the web pick asks (the model store, the cold-start list, the default).
  d.modelIsValid = [](const std::string& p, const std::string& m) {
    return store::modelSelectable(String(p.c_str()), String(m.c_str()), nimbus::orch::PickRole::Any);
  };
  // R5 (lane H): a miss queues one list refresh and answers with the cache date.
  d.modelMiss = [](const std::string& p, const std::string& m) { return agent::model_refresh::missText(p, m); };
  d.providerHasKey = [](const std::string& p) { return store::providerHasKey(String(p.c_str())); };
  d.setOrchHostModel = [](const std::string& p, const std::string& m) {
    store::setOrchHost(String(p.c_str()));
    store::setOrchModel(String(p.c_str()), String(m.c_str()));
  };
}

}  // namespace orchestrator
}  // namespace agent
