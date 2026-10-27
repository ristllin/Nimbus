#include "budget_command.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_timer.h>

#include <cstring>
#include <ctime>
#include <mutex>

#include "fs_atomic.h"
#include "nimbus/orch/budget_command.h"
#include "nimbus/orch/provider_park.h"
#include "spend_ceiling_store.h"
#include "store.h"
#include "../sys/agent_log.h"
#include "../sys/psram_static.h"   // the state lives in PSRAM

namespace agent {
namespace budget {

namespace {

using nimbus::orch::BudgetAccess;
using nimbus::orch::BudgetConfirm;
using nimbus::orch::BudgetPlan;
using nimbus::orch::BudgetRequest;
using nimbus::orch::BudgetVerb;
using nimbus::orch::BudgetVerdict;
using nimbus::orch::LimitsView;

// This build answers the spending commands, so the pause notices end with the one that
// lifts them (lib/core words them without it where nothing answers, the hosted
// instance). Set during static initialization, before any task runs.
const bool g_commandsInstalled = (nimbus::orch::installBudgetCommands(true), true);

// ---- the state: in PSRAM (docs/memory.md); only the locks stay in internal RAM -------

constexpr const char* kLogPath = "/data/budget_log.txt";
constexpr size_t kRows = 8;        // the change record keeps the newest 8 changes
constexpr size_t kLogMax = 2048;   // a longer file is not ours
constexpr size_t kPageRows = 3;    // the Usage page lists the newest 3

struct LastTurn {
  bool        known = false, ok = false;
  std::string host, model, requested;
  uint64_t    atMs = 0;
};
struct LogRow {
  uint32_t t = 0;
  char     src[10] = {};
  char     text[112] = {};
};
struct State {
  BudgetConfirm confirm;   // the change that waits (g_mx)
  LastTurn      last;      // /model's last reply (g_mx)
  LogRow        rows[kRows];   // the change record, oldest first (g_logMx)
  uint8_t       n = 0;
  bool          loaded = false;
};
std::mutex g_mx;      // the confirm window and the last reply (the poll task; cheap insurance)
std::mutex g_logMx;   // the change record (the poll task writes, the web task reads)
State&     g_st = ::nimbus::psramObject<State>();

uint64_t nowMs() { return (uint64_t)(esp_timer_get_time() / 1000); }   // 64-bit: never wraps

Reply reply(std::string text, std::string mirror = std::string()) { return {std::move(text), std::move(mirror)}; }

void pushRowLocked(uint32_t t, const char* src, const char* text) {
  if (g_st.n == kRows) {
    for (size_t i = 1; i < kRows; ++i) g_st.rows[i - 1] = g_st.rows[i];
    g_st.n = kRows - 1;
  }
  LogRow& r = g_st.rows[g_st.n++];
  r = LogRow();
  r.t = t;
  std::strncpy(r.src, src, sizeof(r.src) - 1);
  for (size_t i = 0, o = 0; text[i] && o < sizeof(r.text) - 1; ++i)
    r.text[o++] = (text[i] == '|' || text[i] == '\n' || text[i] == '\r') ? ' ' : text[i];
}

// Caller holds g_logMx. Reads the file once the file area is mounted.
void loadLogLocked() {
  if (g_st.loaded || !agent::flashMounted()) return;
  g_st.loaded = true;
  if (!LittleFS.exists(kLogPath)) return;
  File f = LittleFS.open(kLogPath, "r");
  if (!f) return;
  if (f.size() > kLogMax) { f.close(); return; }
  String body = f.readString();
  f.close();
  int start = 0;
  while (start < (int)body.length()) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();
    const String line = body.substring(start, end);
    start = end + 1;
    const int a = line.indexOf('|'), b = a < 0 ? -1 : line.indexOf('|', a + 1);
    if (a <= 0 || b <= a) continue;   // a torn line is skipped
    pushRowLocked((uint32_t)strtoul(line.substring(0, a).c_str(), nullptr, 10),
                  line.substring(a + 1, b).c_str(), line.substring(b + 1).c_str());
  }
}

// Caller holds g_logMx: the file's content.
std::string logBlobLocked() {
  std::string blob;
  for (size_t i = 0; i < g_st.n; ++i) {
    const LogRow& r = g_st.rows[i];
    blob += std::to_string(r.t) + "|" + r.src + "|" + r.text + "\n";
  }
  return blob;
}

const char* channelSlug(const String& chat) {
  if (chat == "web") return "web";
  if (chat == "serial") return "serial";
  return "telegram";
}
const char* channelLabel(const String& chat) {
  if (chat == "web") return "the web chat";
  if (chat == "serial") return "the serial console";
  return "Telegram";
}

// One applied change: the durable device log, the Usage page's record (written outside
// the record's lock, so the web task never waits on the flash write; only this task
// writes the file).
void recordChange(const BudgetPlan& p, const String& chat) {
  const std::string what = nimbus::orch::changeSummary(p);
  alogf("budget: %s, from %s (confirmed by the owner)", what.c_str(), channelSlug(chat));
  const time_t now = time(nullptr);
  std::string blob;
  {
    std::lock_guard<std::mutex> lk(g_logMx);
    loadLogLocked();
    pushRowLocked(now >= nimbus::orch::kCeilingClockValid ? (uint32_t)now : 0, channelSlug(chat), what.c_str());
    blob = logBlobLocked();
  }
  LittleFS.mkdir("/data");
  if (!agent::writeFileAtomic(kLogPath, blob, "budget_log"))
    alogf("budget: the change record did not store (the change itself did)");
}

// ---- the state ----------------------------------------------------------------------

// A provider's token or call budget used up (the ledger's own counters and limits, not
// its "over" flag, which also carries the dollar limit): turns and searches to it are
// refused too, so /usage and /model say so.
void addUsageLimits(LimitsView& v) {
  if (!v.loaded) return;
  JsonDocument d;
  if (deserializeJson(d, agent::store::providerBudgetJson()) != DeserializationError::Ok) return;
  for (JsonObjectConst e : d.as<JsonArrayConst>()) {
    const char* p = e["prov"] | "";
    const uint64_t tokens = (e["in"] | (uint64_t)0) + (e["out"] | (uint64_t)0);
    if (*p && nimbus::orch::usageBudgetReached(tokens, e["tokenLimit"] | (uint64_t)0, e["calls"] | (uint64_t)0,
                                               e["callLimit"] | (uint64_t)0))
      v.pauses.push_back({p, 0, 0, true});
  }
}

LimitsView view() {
  LimitsView v;
  nimbus::orch::limitsViewFromJson(agent::spend::stateJson(), v);
  nimbus::orch::ParkEntry e[nimbus::orch::ProviderPark::kMaxProviders];
  const size_t n = nimbus::orch::providerPark().snapshot(e, nimbus::orch::ProviderPark::kMaxProviders, millis());
  const uint8_t chatSeam = (uint8_t)(1u << (unsigned)nimbus::orch::PaidSeam::Chat);
  for (size_t i = 0; i < n; ++i) {
    // A refusal counts when it closes replies (a 403 on one other seam does not).
    const uint8_t kind = (e[i].seamMask & chatSeam) ? (uint8_t)e[i].kind : 0;
    if (e[i].paused || kind) v.pauses.push_back({e[i].provider, e[i].paused, kind, false});
  }
  addUsageLimits(v);
  return v;
}

// The provider's dollar budget, keeping its token and call limits and reset day (the
// same store the web page's Budgets row writes).
bool storeProviderCents(const std::string& provider, uint64_t cents) {
  uint64_t tokenLimit = 0;
  uint32_t callLimit = 0;
  uint8_t resetDay = 1;
  JsonDocument d;
  // Fail closed: a budget list that does not read (no memory for it) stores nothing,
  // rather than a dollar change that silently wipes the provider's token and call limits.
  if (deserializeJson(d, agent::store::providerBudgetJson()) != DeserializationError::Ok || !d.is<JsonArray>())
    return false;
  for (JsonObjectConst e : d.as<JsonArrayConst>()) {
    if (provider != (const char*)(e["prov"] | "")) continue;
    tokenLimit = e["tokenLimit"] | (uint64_t)0;
    callLimit = e["callLimit"] | (uint32_t)0;
    resetDay = e["resetDay"] | (uint8_t)1;
  }
  if (resetDay < 1 || resetDay > 28) resetDay = 1;
  return agent::store::setProviderBudget(String(provider.c_str()), tokenLimit, callLimit, resetDay, cents);
}

bool apply(const BudgetPlan& p) {
  if (p.overall) return agent::spend::setLimitMicro(p.toMicro);
  return storeProviderCents(p.req.target, p.toMicro / 10000ULL);
}

// Whether the state after the change is the one the reply is about to state: the limit
// read back, and the pauses as planned (a full cap table or ledger can drop a limit).
bool landedAsPlanned(const BudgetPlan& p) {
  const LimitsView after = view();
  if (after.frozen != p.overallFrozenAfter) return false;
  if (p.overall) return after.active && after.limitMicro == p.toMicro;
  const nimbus::orch::LimitRow* r = after.row(p.req.target);
  return (r ? r->capMicro : 0) == p.toMicro && (r ? r->atLimit() : false) == p.targetFrozenAfter;
}

Reply propose(const String& chat, const BudgetRequest& req, int64_t msgDate) {
  const LimitsView v = view();
  const BudgetPlan p = nimbus::orch::planBudgetChange(req, v);
  const bool ok = p.verdict == BudgetVerdict::Ok;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_st.confirm.offer(chat.c_str(), p, nowMs(), msgDate);   // a refused one cancels a waiting one
  }
  if (ok)
    alogf("budget: proposed from %s: %s (waits for /budget confirm)", channelSlug(chat),
          nimbus::orch::changeSummary(p).c_str());
  return reply(nimbus::orch::proposalText(p, v));
}

void markFailed(const String& chat) {
  std::lock_guard<std::mutex> lk(g_mx);
  g_st.confirm.failed(chat.c_str());
}

Reply confirm(const String& chat, int64_t msgDate) {
  BudgetPlan shown;
  BudgetConfirm::Ended why = BudgetConfirm::Ended::None;
  bool ok;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    ok = g_st.confirm.take(chat.c_str(), nowMs(), msgDate, shown, why);
  }
  if (!ok) return reply(nimbus::orch::confirmMissText(why, shown.req));
  // Re-planned on the state now: a change made meanwhile (the web page) is never
  // overwritten blind, the bounds hold against the limit as it is now, and it applies
  // only when it states exactly what the owner was shown.
  const LimitsView v = view();
  const BudgetPlan p = nimbus::orch::planBudgetChange(shown.req, v);
  if (!nimbus::orch::samePlannedEffect(shown, p)) {
    markFailed(chat);
    alogf("budget: a confirmed change no longer matched its proposal - nothing changed");
    return reply(nimbus::orch::replanRefusedText(p));
  }
  if (!apply(p)) {
    markFailed(chat);
    alogf("budget: a confirmed change did not store - nothing changed");
    return reply(nimbus::orch::storeFailedText(shown.req));
  }
  recordChange(p, chat);
  std::string text = nimbus::orch::appliedText(p, v);
  if (!landedAsPlanned(p)) text += "\nSend /usage to see what applies now.";
  return reply(text, std::string("Changed from ") + channelLabel(chat) + ": " + nimbus::orch::changeSummary(p) + ".");
}

Reply modelReply() {
  nimbus::orch::ModelFacts f;
  f.chosenHost = agent::store::orchHost().c_str();
  const String runs = agent::store::resolvedOrchHost();
  f.runsOnHost = runs.c_str();
  if (runs.length()) f.runsOnModel = agent::store::orchModel(runs).c_str();
  f.limits = view();
  std::lock_guard<std::mutex> lk(g_mx);
  f.last.known = g_st.last.known;
  f.last.ok = g_st.last.ok;
  f.last.host = g_st.last.host;
  f.last.model = g_st.last.model;
  f.last.requestedHost = g_st.last.requested;
  f.last.agoS = (uint32_t)((nowMs() - g_st.last.atMs) / 1000u);
  return reply(nimbus::orch::modelText(f));
}

// A change (proposal or confirm) that may not run now: no clock yet, or a message that
// arrived late. Its waiting change is cancelled too (fail closed).
std::string timingRefusal(const String& chat, int64_t msgDate) {
  const std::string why = nimbus::orch::budgetTimingRefusal((int64_t)time(nullptr), msgDate);
  if (why.empty()) return why;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_st.confirm.noteOther(chat.c_str());
  }
  alogf("budget: a change from %s refused (%s)", channelSlug(chat),
        msgDate > 0 && time(nullptr) >= nimbus::orch::kCeilingClockValid ? "the message arrived late" : "no clock yet");
  return why;
}

}  // namespace

Reply handle(const std::string& verb, const std::string& args, const String& chatId, bool owner,
             const nimbus::orch::BudgetMsg& msg) {
  const BudgetAccess access = nimbus::orch::budgetAccessFor(chatId.c_str(), owner, msg.voiceNote);
  if (access == BudgetAccess::Group || access == BudgetAccess::NotOwner) {
    alogf("budget: /%s refused from %s", verb.c_str(), access == BudgetAccess::Group ? "a group chat" : "a member chat");
    return reply(nimbus::orch::budgetAccessText(access));
  }
  if (verb == "model") return modelReply();
  BudgetRequest req;
  if (verb == "usage") req.verb = BudgetVerb::Show;
  else req = nimbus::orch::parseBudgetArgs(args);
  switch (req.verb) {
    case BudgetVerb::Show:  return reply(nimbus::orch::usageText(view()));
    case BudgetVerb::Help:  return reply(nimbus::orch::budgetHelpText());
    case BudgetVerb::Error: return reply(req.error);
    case BudgetVerb::Cancel: {
      std::lock_guard<std::mutex> lk(g_mx);
      return reply(nimbus::orch::cancelText(g_st.confirm.cancel(chatId.c_str())));
    }
    default: break;
  }
  if (access == BudgetAccess::ReadOnly) {
    alogf("budget: a change by voice refused (read-only channel)");
    return reply(nimbus::orch::budgetAccessText(access));
  }
  const std::string late = timingRefusal(chatId, msg.date);
  if (!late.empty()) return reply(late);
  return req.verb == BudgetVerb::Confirm ? confirm(chatId, msg.date) : propose(chatId, req, msg.date);
}

void noteMessage(const String& chatId, const nimbus::orch::Command& cmd) {
  if (nimbus::orch::budgetKeepsWaiting(cmd)) return;
  bool cancelled;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    cancelled = g_st.confirm.noteOther(chatId.c_str());
  }
  if (cancelled) alogf("budget: a waiting change was cancelled by the next message from %s", channelSlug(chatId));
}

void noteTurnEnd(const std::string& host, const std::string& model, const std::string& requestedHost,
                 const char* src, bool ok) {
  if (!src || !(std::strcmp(src, "owner") == 0 || std::strcmp(src, "serial") == 0 ||
                std::strcmp(src, "synthesis") == 0))
    return;   // a routine or the dream is not a reply the owner asked for
  if (host.empty()) return;
  std::lock_guard<std::mutex> lk(g_mx);
  g_st.last.known = true;
  g_st.last.ok = ok;
  g_st.last.host = host;
  g_st.last.model = model;
  g_st.last.requested = requestedHost;
  g_st.last.atMs = nowMs();
}

std::string changesJson() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  {
    std::lock_guard<std::mutex> lk(g_logMx);
    loadLogLocked();
    for (size_t i = g_st.n, k = 0; i-- > 0 && k < kPageRows; ++k) {
      JsonObject o = a.add<JsonObject>();
      o["t"] = g_st.rows[i].t;
      o["src"] = g_st.rows[i].src;
      o["text"] = g_st.rows[i].text;
    }
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace budget
}  // namespace agent
