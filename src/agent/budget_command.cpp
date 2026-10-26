#include "budget_command.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

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

struct LastTurn {
  bool        known = false, ok = false;
  std::string host, model, requested;
  uint32_t    atMs = 0;
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

bool persistLogLocked() {
  std::string blob;
  for (size_t i = 0; i < g_st.n; ++i) {
    const LogRow& r = g_st.rows[i];
    blob += std::to_string(r.t) + "|" + r.src + "|" + r.text + "\n";
  }
  LittleFS.mkdir("/data");
  return agent::writeFileAtomic(kLogPath, blob, "budget_log");
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

// One applied change: the durable device log, the Usage page's record.
void recordChange(const BudgetPlan& p, const String& chat) {
  const std::string what = nimbus::orch::changeSummary(p);
  alogf("budget: %s, from %s (confirmed by the owner)", what.c_str(), channelSlug(chat));
  const time_t now = time(nullptr);
  std::lock_guard<std::mutex> lk(g_logMx);
  loadLogLocked();
  pushRowLocked(now >= nimbus::orch::kCeilingClockValid ? (uint32_t)now : 0, channelSlug(chat), what.c_str());
  if (!persistLogLocked()) alogf("budget: the change record did not store (the change itself did)");
}

// ---- the state ----------------------------------------------------------------------

LimitsView view() {
  LimitsView v;
  nimbus::orch::limitsViewFromJson(agent::spend::stateJson(), v);
  nimbus::orch::ParkEntry e[nimbus::orch::ProviderPark::kMaxProviders];
  const size_t n = nimbus::orch::providerPark().snapshot(e, nimbus::orch::ProviderPark::kMaxProviders, millis());
  const uint8_t chatSeam = (uint8_t)(1u << (unsigned)nimbus::orch::PaidSeam::Chat);
  for (size_t i = 0; i < n; ++i) {
    // A refusal counts when it closes replies (a 403 on one other seam does not).
    const uint8_t kind = (e[i].seamMask & chatSeam) ? (uint8_t)e[i].kind : 0;
    if (e[i].paused || kind) v.pauses.push_back({e[i].provider, e[i].paused, kind});
  }
  return v;
}

// The provider's dollar budget, keeping its token and call limits and reset day (the
// same store the web page's Budgets row writes).
bool storeProviderCents(const std::string& provider, uint64_t cents) {
  uint64_t tokenLimit = 0;
  uint32_t callLimit = 0;
  uint8_t resetDay = 1;
  JsonDocument d;
  if (deserializeJson(d, agent::store::providerBudgetJson()) == DeserializationError::Ok) {
    for (JsonObjectConst e : d.as<JsonArrayConst>()) {
      if (provider != (const char*)(e["prov"] | "")) continue;
      tokenLimit = e["tokenLimit"] | (uint64_t)0;
      callLimit = e["callLimit"] | (uint32_t)0;
      resetDay = e["resetDay"] | (uint8_t)1;
    }
  }
  if (resetDay < 1 || resetDay > 28) resetDay = 1;
  return agent::store::setProviderBudget(String(provider.c_str()), tokenLimit, callLimit, resetDay, cents);
}

bool apply(const BudgetPlan& p) {
  if (p.overall) return agent::spend::setLimitMicro(p.toMicro);
  return storeProviderCents(p.req.target, p.toMicro / 10000ULL);
}

// Whether the state after the change matches what the reply is about to say.
bool landedAsPlanned(const BudgetPlan& p) {
  const LimitsView after = view();
  if (after.frozen != p.overallFrozenAfter) return false;
  if (p.overall) return true;
  const nimbus::orch::LimitRow* r = after.row(p.req.target);
  return (r ? r->frozen : false) == p.targetFrozenAfter;
}

Reply propose(const String& chat, const BudgetRequest& req) {
  const LimitsView v = view();
  const BudgetPlan p = nimbus::orch::planBudgetChange(req, v);
  if (p.verdict == BudgetVerdict::Ok) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_st.confirm.propose(chat.c_str(), req, millis());
    alogf("budget: proposed from %s: %s (waits for /budget confirm)", channelSlug(chat),
          nimbus::orch::changeSummary(p).c_str());
  }
  return {nimbus::orch::proposalText(p, v), std::string()};
}

Reply confirm(const String& chat) {
  BudgetRequest req;
  BudgetConfirm::Ended why = BudgetConfirm::Ended::None;
  bool ok;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    ok = g_st.confirm.take(chat.c_str(), millis(), req, why);
  }
  if (!ok) return {nimbus::orch::confirmMissText(why, req), std::string()};
  // Re-planned on the state now: a change made meanwhile (the web page) is never
  // overwritten blind, and the bounds hold against the limit as it is now.
  const LimitsView v = view();
  const BudgetPlan p = nimbus::orch::planBudgetChange(req, v);
  if (p.verdict != BudgetVerdict::Ok) return {"The limits changed since that was proposed. " + p.why, std::string()};
  if (!apply(p)) {
    alogf("budget: a confirmed change did not store - nothing changed");
    return {"The change could not be stored, so nothing changed. Try again in a minute.", std::string()};
  }
  recordChange(p, chat);
  std::string text = nimbus::orch::appliedText(p, v);
  if (!landedAsPlanned(p)) text += "\nSend /usage to see what is paused now.";
  return {text, std::string("Changed from ") + channelLabel(chat) + ": " + nimbus::orch::changeSummary(p) + "."};
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
  f.last.agoS = (uint32_t)(millis() - g_st.last.atMs) / 1000u;
  return {nimbus::orch::modelText(f), std::string()};
}

}  // namespace

Reply handle(const std::string& verb, const std::string& args, const String& chatId, bool owner) {
  const BudgetAccess access = nimbus::orch::budgetAccessFor(chatId.c_str(), owner);
  if (access == BudgetAccess::Group || access == BudgetAccess::NotOwner)
    return {nimbus::orch::budgetAccessText(access), std::string()};
  if (verb == "model") return modelReply();
  BudgetRequest req;
  if (verb == "usage") req.verb = BudgetVerb::Show;
  else req = nimbus::orch::parseBudgetArgs(args);
  switch (req.verb) {
    case BudgetVerb::Show:  return {nimbus::orch::usageText(view()), std::string()};
    case BudgetVerb::Help:  return {nimbus::orch::budgetHelpText(), std::string()};
    case BudgetVerb::Error: return {req.error, std::string()};
    case BudgetVerb::Cancel: {
      std::lock_guard<std::mutex> lk(g_mx);
      return {g_st.confirm.cancel(chatId.c_str()) ? "Cancelled. Nothing changed." : "No change is waiting.", std::string()};
    }
    default: break;
  }
  if (access == BudgetAccess::ReadOnly) return {nimbus::orch::budgetAccessText(access), std::string()};
  return req.verb == BudgetVerb::Confirm ? confirm(chatId) : propose(chatId, req);
}

void noteMessage(const String& chatId, const nimbus::orch::Command& cmd) {
  if (cmd.isCommand && cmd.verb == "budget") {
    const BudgetVerb v = nimbus::orch::parseBudgetArgs(cmd.args).verb;
    if (v == BudgetVerb::Confirm || v == BudgetVerb::Cancel || v == BudgetVerb::Set || v == BudgetVerb::Off) return;
  }
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
  g_st.last.atMs = millis();
}

std::string changesJson() {
  std::lock_guard<std::mutex> lk(g_logMx);
  loadLogLocked();
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (size_t i = g_st.n; i-- > 0;) {
    JsonObject o = a.add<JsonObject>();
    o["t"] = g_st.rows[i].t;
    o["src"] = g_st.rows[i].src;
    o["text"] = g_st.rows[i].text;
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace budget
}  // namespace agent
