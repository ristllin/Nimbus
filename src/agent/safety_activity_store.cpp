#include "safety_activity_store.h"

#include <Arduino.h>   // xTaskCreate / vTaskDelete / pdPASS (deferred report worker)
#include <FS.h>

#include <ArduinoJson.h>
#include <ctime>

#include "agent_config.h"                 // CUMULO_HOST_DEFAULT
#include "memory_subsystem.h"             // agent::memory::Lock (serializes card I/O)
#include "store.h"                        // agent::store:: cumulo/cloud accessors
#include "transport_tls.h"                // agent::deviceTransport() (TLS work-slot arbiter)
#include "version.h"                      // NIMBUS_FW_VERSION -> meta.fw
#include "../sys/agent_log.h"             // alogf(), agent::logring::g_secrets
#include "../sys/errlog_fs.h"            // errlog::activeFs()/onSdTier() - the durable tier
#include "../sys/errlog.h"               // errlog::kDir
#include "nimbus/logring.h"              // core::LogRing::redact (portable redaction)

using nimbus::orch::SafetyEntry;
using nimbus::orch::SafetyActivityLog;
using nimbus::orch::SafetyAllowlist;
using nimbus::orch::SafetyVerdict;
using nimbus::orch::SafetyStatus;
using nimbus::orch::AllowScope;
using nimbus::orch::AllowRule;
using nimbus::orch::ReportOutcome;

namespace agent {
namespace safety {

namespace {

// The two durable files, on the SAME card/flash tier the errlog uses. Distinct
// names from nimbus.log* so errlog's own enumeration never touches them.
constexpr char kActivityPath[] = "/log/safety.jsonl";
constexpr char kAllowPath[]    = "/log/safety_allow.jsonl";

SafetyActivityLog g_log;
SafetyAllowlist   g_allow;
bool              g_loaded  = false;
uint32_t          g_nextId  = 1;

// Deferred-report state (see the report section below). Declared here so listJson()
// can surface the snapshot for the tab to poll.
volatile bool g_reportPending = false;
std::string   g_reportPendingId;
std::string   g_reportLastId;
ReportOutcome g_reportLastOutcome = ReportOutcome::Failed;
std::string   g_reportLastMsg;
bool          g_reportHaveLast = false;

std::string redact(const std::string& raw) {
  return nimbus::orch::clampExcerpt(core::LogRing::redact(raw, agent::logring::g_secrets));
}

// Read a whole small file off the durable tier ("" if absent/unreadable). Caller
// holds memory::Lock.
std::string readWhole(const char* path) {
  ::fs::FS& fs = nimbus::errlog::activeFs();
  ::File f = fs.open(path, FILE_READ);
  if (!f) return std::string();
  std::string out;
  out.reserve((size_t)f.size());
  uint8_t buf[256];
  for (;;) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.append((const char*)buf, (size_t)n);
  }
  f.close();
  return out;
}

// Rewrite a whole small file (FILE_WRITE truncates on open). Caller holds the Lock.
// The ring is small and bounded, and the log is best-effort like the errlog: a crash
// mid-write leaves a torn last line, which loadAll() skips, and serialize() writes
// newest-first so any lost tail is the OLDEST entries. A direct truncate avoids the
// remove-before-rename window the codebase warns against (orch_persist.cpp).
void writeWhole(const char* path, const std::string& blob) {
  ::fs::FS& fs = nimbus::errlog::activeFs();
  fs.mkdir(nimbus::errlog::kDir);   // idempotent; neither FS auto-creates /log
  ::File f = fs.open(path, FILE_WRITE);
  if (!f) { alogf("safety: durable open failed (%s)", path); return; }
  size_t w = f.write((const uint8_t*)blob.data(), blob.size());
  f.close();
  if (w != blob.size()) alogf("safety: short write %u/%u", (unsigned)w, (unsigned)blob.size());
}

void seedNextId() {
  uint32_t hi = 0;
  for (const auto& e : g_log.entries()) {
    if (e.id.size() > 1 && e.id[0] == 's') {
      uint32_t v = 0; bool ok = true;
      for (size_t i = 1; i < e.id.size(); i++) {
        char c = e.id[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else { ok = false; break; }
        v = v * 16 + (uint32_t)d;
      }
      if (ok && v >= hi) hi = v + 1;
    }
  }
  g_nextId = hi ? hi : 1;
}

std::string nextId() {
  char b[16];
  snprintf(b, sizeof(b), "s%08x", (unsigned)g_nextId++);
  return b;
}

// Lazy one-time load. All public entry points call this first, and they all run
// well after the FS is mounted (web + turn paths), so the persisted history is
// loaded before the first persist could overwrite it.
void ensureLoaded() {
  if (g_loaded) return;
  g_log.loadAll(readWhole(kActivityPath));
  g_allow.loadAll(readWhole(kAllowPath));
  seedNextId();
  g_loaded = true;
}

std::string isoNow() {
  time_t t = time(nullptr);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return b;
}

}  // namespace

void begin() { memory::Lock g; ensureLoaded(); }

bool allowed(const std::string& rule, const std::string& sender, const std::string& rawExcerpt) {
  memory::Lock g;
  ensureLoaded();
  SafetyEntry probe;
  probe.rule = rule;
  probe.sender = sender;
  probe.excerpt = redact(rawExcerpt);
  return g_allow.allows(probe);
}

bool record(SafetyVerdict verdict, const std::string& rule, const std::string& channel,
            const std::string& sender, const std::string& source, const std::string& rawExcerpt) {
  memory::Lock g;
  ensureLoaded();
  SafetyEntry e;
  e.rule = rule;
  e.channel = channel;
  e.sender = sender;
  e.source = source;
  e.verdict = verdict;
  e.excerpt = redact(rawExcerpt);
  if (g_allow.allows(e)) return false;   // scoped unblock: nothing to record
  e.id = nextId();
  e.tsEpoch = (uint32_t)time(nullptr);
  e.status = SafetyStatus::Active;
  g_log.record(std::move(e));
  writeWhole(kActivityPath, g_log.serialize());
  return true;
}

std::string listJson(bool hasCumuloKey) {
  memory::Lock g;
  ensureLoaded();
  JsonDocument d;
  JsonArray entries = d["entries"].to<JsonArray>();
  for (const auto& e : g_log.entries()) {
    JsonObject o = entries.add<JsonObject>();
    o["id"]      = e.id;
    o["ts"]      = e.tsEpoch;
    o["verdict"] = nimbus::orch::safetyVerdictName(e.verdict);
    o["rule"]    = e.rule;
    o["channel"] = e.channel;
    o["sender"]  = e.sender;
    o["source"]  = e.source;
    o["excerpt"] = e.excerpt;
    o["status"]  = nimbus::orch::safetyStatusName(e.status);
  }
  JsonArray allow = d["allow"].to<JsonArray>();
  for (const auto& r : g_allow.rules()) {
    JsonObject o = allow.add<JsonObject>();
    o["id"]    = r.id;
    o["scope"] = nimbus::orch::allowScopeName(r.scope);
    o["value"] = r.value;
    o["ts"]    = r.tsEpoch;
  }
  JsonObject rep = d["report"].to<JsonObject>();
  rep["available"] = nimbus::orch::reportAvailable(hasCumuloKey);
  rep["copy"]      = nimbus::orch::reportUnavailableCopy();
  rep["pending"]   = (bool)g_reportPending;
  rep["pendingId"] = g_reportPending ? g_reportPendingId : std::string();
  if (g_reportHaveLast) {
    JsonObject last = rep["last"].to<JsonObject>();
    last["id"]      = g_reportLastId;
    last["outcome"] = nimbus::orch::reportOutcomeName(g_reportLastOutcome);
    last["message"] = g_reportLastMsg;
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

bool dismiss(const std::string& id) {
  memory::Lock g;
  ensureLoaded();
  if (!g_log.dismiss(id)) return false;
  writeWhole(kActivityPath, g_log.serialize());
  return true;
}

bool approve(const std::string& id, AllowScope scope, std::string& msgOut) {
  memory::Lock g;
  ensureLoaded();
  const SafetyEntry* e = g_log.find(id);
  if (!e) { msgOut = "Entry not found."; return false; }
  // ContentClass is a real scope ONLY for a genuine narrow category. A coarse rule
  // (the moderation-classifier block, which carries no sub-category) would make the
  // allow match every future block on that gate: a global off switch, which the whole
  // surface forbids. Refuse it and steer the owner to sender/pattern (CUM-215).
  if (scope == AllowScope::ContentClass && !nimbus::orch::contentClassApprovable(e->rule)) {
    msgOut = "That kind has no specific category. Approve this sender or this exact content instead.";
    return false;
  }
  std::string value;
  switch (scope) {
    case AllowScope::Sender:       value = e->sender;  break;
    case AllowScope::ContentClass: value = e->rule;    break;
    case AllowScope::Pattern:      value = e->excerpt; break;
  }
  if (!nimbus::orch::isConcreteAllowValue(value)) {
    msgOut = "Nothing to allow for that scope.";
    return false;
  }
  std::string outId;
  g_allow.add(scope, value, (uint32_t)time(nullptr), outId);   // dup is fine (already allowed)
  g_log.approve(id);
  writeWhole(kAllowPath, g_allow.serialize());
  writeWhole(kActivityPath, g_log.serialize());
  msgOut = "Approved. Similar items are now allowed.";
  return true;
}

bool revokeAllow(const std::string& id) {
  memory::Lock g;
  ensureLoaded();
  if (!g_allow.revoke(id)) return false;
  writeWhole(kAllowPath, g_allow.serialize());
  return true;
}

// ---- deferred report (web task queues; a worker task does the TLS POST) --------
//
// Single-slot handoff mirroring cloud_mint/provider_verify: the AsyncTCP web task
// must NEVER run a TLS acquire+handshake inline (it would stall every web request
// and can trip the loop watchdog). requestReport() validates + spawns a short-lived
// worker; the worker builds the frozen-contract payload and POSTs it; the tab polls
// listJson for the outcome snapshot (the g_report* state is declared at the top).
namespace {
void reportRunOne(const std::string& id) {
  SafetyEntry e;
  bool found = false;
  {
    memory::Lock g;
    ensureLoaded();
    const SafetyEntry* p = g_log.find(id);
    if (p) { e = *p; found = true; }
  }
  ReportOutcome oc = ReportOutcome::Failed;
  if (!found) {
    // The entry vanished (evicted/dismissed) between request and run.
    g_reportLastMsg = "Entry not found.";
  } else {
    nimbus::orch::SafetyReportInput in =
        nimbus::orch::reportInputFromEntry(e, std::string(store::cloudDeviceId().c_str()),
                                           isoNow(), NIMBUS_FW_VERSION);
    HttpRequest req;
    req.method = "POST";
    req.host   = nimbus::orch::cumuloHostFromBase(std::string(store::cumuloBase().c_str()),
                                                  CUMULO_HOST_DEFAULT);
    req.port   = 443;
    req.tls    = true;
    req.path   = nimbus::orch::kSafetyReportPath;
    req.headers.push_back({"Authorization", std::string("Bearer ") + store::cumuloKey().c_str()});
    req.headers.push_back({"Content-Type", "application/json"});
    req.body   = nimbus::orch::buildSafetyReportJson(in);
    HttpResponse res;
    std::string err;
    const bool ok = agent::deviceTransport().exec(req, res, err);
    oc = ok ? nimbus::orch::reportOutcomeFromHttp(res.status) : ReportOutcome::Failed;
    g_reportLastMsg = nimbus::orch::reportOutcomeCopy(oc);
    alogf("safety: report %s -> %s (http=%d)", id.c_str(), nimbus::orch::reportOutcomeName(oc),
          ok ? res.status : 0);
  }
  g_reportLastOutcome = oc;
  g_reportLastId = id;
  g_reportHaveLast = true;
}

void reportTask(void*) {
  reportRunOne(g_reportPendingId);
  g_reportPending = false;
  vTaskDelete(nullptr);
}
}  // namespace

ReportRequest requestReport(const std::string& id, std::string& msgOut) {
  {
    memory::Lock g;
    ensureLoaded();
    if (!g_log.find(id)) { msgOut = "Entry not found."; return ReportRequest::NotFound; }
  }
  if (!nimbus::orch::reportAvailable(store::hasCumuloKey())) {
    msgOut = nimbus::orch::reportUnavailableCopy();     // no key: no TLS, no task
    return ReportRequest::NoEntitlement;
  }
  if (g_reportPending) { msgOut = "A report is already in progress."; return ReportRequest::Busy; }
  g_reportPending   = true;
  g_reportPendingId = id;
  // 8 KB stack, matching cloud_mint's proven TLS worker (mbedTLS buffers ride PSRAM);
  // watchdog-free so the up-to-10 s slot acquire + handshake can't trip the loop watchdog.
  if (xTaskCreate(reportTask, "safetyrep", 8192, nullptr, 1, nullptr) != pdPASS) {
    g_reportPending = false;
    msgOut = "Device is busy. Try again.";
    return ReportRequest::Busy;
  }
  msgOut = "Reporting to Cumulo.";
  return ReportRequest::Started;
}

std::string consoleSummary() {
  memory::Lock g;
  ensureLoaded();
  int blocked = 0, suspected = 0, active = 0;
  for (const auto& e : g_log.entries()) {
    if (e.verdict == SafetyVerdict::Blocked) blocked++; else suspected++;
    if (e.status == SafetyStatus::Active) active++;
  }
  char b[160];
  snprintf(b, sizeof(b),
           "activity=%d/%d active=%d blocked=%d suspected=%d allow=%d tier=%s report=%s",
           g_log.size(), g_log.cap(), active, blocked, suspected, g_allow.size(),
           nimbus::errlog::onSdTier() ? "sd" : "flash",
           nimbus::orch::reportAvailable(store::hasCumuloKey()) ? "available" : "needs-subscription");
  return b;
}

}  // namespace safety
}  // namespace agent
