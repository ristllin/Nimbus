// Sub-agents on engine 2 (plain/agents.h), lane E2-P-SUB: the model's agent tools and the report
// turn over the kept JobEngine.
#include "nimbus/harness/plain/agents.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <utility>

#include "nimbus/harness/plain/request.h"   // toolDecl, declSchema
#include "nimbus/harness/plain/texts.h"

namespace agent {
namespace plain {

namespace {

namespace orch = nimbus::orch;
namespace tr = texts::tool_results;
namespace dn = texts::device_notes;
using Err = orch::tx::Err;

constexpr size_t kNameMax = 24;

// A file-name segment of a-z0-9- from `in` (other characters become '-', runs of '-' fold).
std::string segment(const std::string& in, size_t max) {
  std::string out;
  for (char c : in) {
    if (out.size() >= max) break;
    const unsigned char u = (unsigned char)c;
    const char lc = (char)std::tolower(u);
    if (std::isalnum(u) && u < 0x80) out += lc;
    else if (!out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

std::string str(ArduinoJson::JsonObjectConst a, const char* key) {
  const char* v = a[key].is<const char*>() ? a[key].as<const char*>() : nullptr;
  return v ? std::string(v) : std::string();
}

// Every key of `a` is one of `allowed`.
bool onlyKeys(ArduinoJson::JsonObjectConst a, std::initializer_list<const char*> allowed) {
  for (ArduinoJson::JsonPairConst kv : a) {
    bool ok = false;
    for (const char* k : allowed) ok = ok || kv.key() == k;
    if (!ok) return false;
  }
  return true;
}

// The journal record of `idOrJob` (its tag or its provider job id), false when none.
bool findRecord(const orch::Journal* j, const std::string& idOrJob, orch::JobRecord& out) {
  if (!j || idOrJob.empty()) return false;
  for (int i = 0; i < j->count(); ++i)
    if (j->get(i, out) && (idOrJob == out.tag || idOrJob == out.jobId)) return true;
  return false;
}

std::set<std::string> openTags(const orch::Journal* j) {
  std::set<std::string> tags;
  orch::JobRecord r;
  for (int i = 0; j && i < j->count(); ++i)
    if (j->get(i, r)) tags.insert(r.tag);
  return tags;
}

struct Tools {
  AgentsDeps d;
  const orch::Journal* journal() const { return d.jobs ? d.jobs->journal() : nullptr; }
  std::string chat() const { return d.chat ? d.chat() : std::string(); }

  orch::ToolResult start(ArduinoJson::JsonObjectConst a) {
    const std::string chatId = chat();
    if (chatId.empty()) return orch::ToolResult::fail(tr::kAgentNoChat);
    if (!onlyKeys(a, {"name", "task", "files", "model"})) return orch::ToolResult::fail(tr::kAgentStartArgs);
    orch::Spawn s;
    s.name = str(a, "name");
    s.task = str(a, "task");
    s.model = str(a, "model");
    if (s.name.empty() || s.task.empty()) return orch::ToolResult::fail(tr::kAgentStartArgs);
    if (!a["files"].isNull()) {
      ArduinoJson::JsonArrayConst files = a["files"].as<ArduinoJson::JsonArrayConst>();
      if (files.isNull() || files.size() > kAgentMaxFiles) return orch::ToolResult::fail(tr::kAgentFiles);
      for (ArduinoJson::JsonVariantConst f : files) {
        const char* p = f.is<const char*>() ? f.as<const char*>() : nullptr;
        if (!p || !std::strchr(p, '/')) return orch::ToolResult::fail(tr::kAgentFiles);
        s.attach.push_back(p);
      }
    }
    s.provider = d.backend;
    s.category = "research";
    s.project = kAgentsProject;
    const std::set<std::string> before = openTags(journal());
    if (!d.jobs->enqueueSpawn(s, chatId, /*quiet=*/true)) return refused(chatId);
    if (d.dispatchNow) {
      d.jobs->noteSpawned();
      d.dispatchNow();
    }
    orch::JobRecord r;
    const orch::Journal* j = journal();
    for (int i = 0; j && i < j->count(); ++i)
      if (j->get(i, r) && !before.count(r.tag) && chatId == r.chatId)
        return orch::ToolResult::ok(std::string(tr::kAgentStarted) + r.tag + tr::kAgentJob + r.jobId);
    bool mayHaveRun = false;
    const std::string notes = d.jobs->takeSpawnNotes(chatId, &mayHaveRun);
    if (!notes.empty()) return orch::ToolResult::fail(tr::kAgentNotStarted + notes);
    return orch::ToolResult::ok(tr::kAgentQueued);
  }

  orch::ToolResult refused(const std::string& chatId) {
    const std::string notes = d.jobs->takeSpawnNotes(chatId);
    return orch::ToolResult::fail(tr::kAgentNotStarted + notes);
  }

  orch::ToolResult list() {
    const std::string chatId = chat();
    if (chatId.empty()) return orch::ToolResult::fail(tr::kAgentNoChat);
    std::string ledger = d.jobs->spawnLedgerText(chatId);
    const size_t nl = ledger.find('\n');   // the ledger's head line is the old engine's framing
    std::string out = nl == std::string::npos ? std::string() : ledger.substr(nl + 1);
    orch::JobRecord r;
    const orch::Journal* j = journal();
    for (int i = 0; j && i < j->count(); ++i) {   // a job a restart left open has no ledger line
      if (!j->get(i, r) || chatId != r.chatId || out.find(std::string("(") + r.tag + ",") != std::string::npos) continue;
      out += std::string("- ") + (r.name[0] ? r.name : r.tag) + " (" + r.tag + ", " + r.backend + "/" + r.model + ")" +
             tr::kAgentOpen;
    }
    return orch::ToolResult::ok(out.empty() ? std::string(tr::kAgentNone) : out);
  }

  orch::ToolResult stop(ArduinoJson::JsonObjectConst a, const orch::Principal& who) {
    const std::string chatId = chat();
    if (chatId.empty()) return orch::ToolResult::fail(tr::kAgentNoChat);
    const std::string id = str(a, "id");
    if (id.empty() || !onlyKeys(a, {"id"})) return orch::ToolResult::fail(tr::kAgentStopArgs);
    orch::JobRecord r;
    const bool open = findRecord(journal(), id, r);
    const bool mine = open && (chatId == r.chatId || who.owner);
    if (mine ? d.jobs->cancel(r.tag) : (!open && d.jobs->stopping(id.c_str())))
      return orch::ToolResult::ok(std::string(tr::kAgentStopping) + (open ? r.tag : id.c_str()));
    return orch::ToolResult::fail(std::string(tr::kAgentNoJob) + id);
  }
};

}  // namespace

Status agentReportPath(const std::string& id, const std::string& name, std::string& out) {
  const std::string sid = segment(id, kNameMax);
  if (sid.empty() || sid != id) return Status::fail(Err::InvalidArgument, "plain::agentReportPath");
  std::string n = segment(name, kNameMax);
  if (n.empty()) n = "report";
  out = std::string(kAgentsProject) + "/" + sid + "-" + n + ".md";
  return Status::success();
}

Status registerAgentTools(orch::ToolRegistry& reg, const AgentsDeps& d) {
  if (!d.jobs || !d.chat) return Status::fail(Err::InvalidArgument, "plain::registerAgentTools");
  auto tools = std::make_shared<Tools>();
  tools->d = d;
  const auto add = [&reg](const char* name, orch::ToolHandler h) {
    const auto* decl = toolDecl(name);
    if (decl) reg.add(decl->name, decl->description, std::move(h), declSchema(*decl, false));
    return decl != nullptr;
  };
  const bool ok =
      add("agent_start", [tools](ArduinoJson::JsonObjectConst a, const orch::Principal&) { return tools->start(a); }) &&
      add("agent_list", [tools](ArduinoJson::JsonObjectConst, const orch::Principal&) { return tools->list(); }) &&
      add("agent_stop", [tools](ArduinoJson::JsonObjectConst a, const orch::Principal& who) { return tools->stop(a, who); });
  return ok ? Status::success() : Status::fail(Err::NotFound, "plain::registerAgentTools.decl");
}

std::string agentReportNote(const JobEngine::FreshResult& f) {
  std::string head = std::string(dn::kAgentReport) + (f.model.empty() ? f.tag : f.model) + dn::kAgentId + f.tag;
  if (f.failed) head += dn::kAgentFailed;
  for (const std::string& p : f.saved) head += std::string(dn::kAgentSaved) + p;
  return head + "\n" + fenceOutsideContent(f.text + f.hint + f.provenance);
}

Status deliverReports(JobEngine& jobs, const std::string& chatId, const ReportTurnFn& turn, int& ran) {
  ran = 0;
  if (!turn) return Status::fail(Err::InvalidArgument, "plain::deliverReports");
  const JobEngine::FreshScope scope = jobs.freshScopeFor(chatId);
  std::vector<JobEngine::FreshResult> taken = jobs.peekFreshFor(scope);
  if (taken.empty()) return Status::success();
  (void)jobs.takeFreshResultsFor(scope, nullptr, nullptr, nullptr);   // consumed here, put back below if not run
  for (size_t i = 0; i < taken.size(); ++i) {
    const JobEngine::FreshResult& f = taken[i];
    const Status st = turn(f.chatId.empty() ? chatId : f.chatId, agentReportNote(f));
    if (st.err == Err::Busy) {
      jobs.restoreFresh(std::vector<JobEngine::FreshResult>(taken.begin() + (long)i, taken.end()));
      return st;
    }
    ++ran;
  }
  return Status::success();
}

}  // namespace plain
}  // namespace agent
