#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/jobs.h"
#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/tool_registry.h"

// plain/agents.h - sub-agents on engine 2 (PRD section 6) over the kept job fabric (jobs.cpp,
// fabric.cpp), lane E2-P-SUB. Nothing here is a second copy of the job machinery: the JobEngine
// queues, dispatches, polls, journals (a job survives a restart), meters and stops; this file is
// the model's three tools over it and the arrival of a report as its own turn.
//   - agent_start({name, task, files up to 4, model?}): the job is queued in the JobEngine on the
//     one backend the binding names, with the files as attachments and the agents project as its
//     result's home; `dispatchNow` lets a remote-job backend start inside the turn, so the result
//     is 'started id=<tag> job=<provider id>', the refusal's own reason, or 'queued' when the
//     dispatch waits for a free slot. Briefs carry the task and the files, never the transcript.
//   - agent_list: the chat's jobs from the ledger, then any open journal job a restart left.
//   - agent_stop({id}): JobEngine::cancel.
//   - arrival: the JobEngine's synthesis seam names a chat with parked results; each result of
//     that chat's scope becomes one report turn (Trigger::AgentResult: its text is stored as one
//     agent device note, outside content, and the turn's writes go through the rails). A turn
//     that could not start puts the rest back (JobEngine::restoreFresh): never lost; the
//     JobEngine's stuck fallback still delivers them raw after its 60 s.
// The whole report is saved by the JobEngine's persistResult seam before the job is marked seen;
// agentReportPath names that file: agents/<id>-<name>.md.
namespace agent {
namespace plain {

// The project every report is saved in.
inline constexpr char kAgentsProject[] = "agents";
inline constexpr size_t kAgentMaxFiles = 4;

// "agents/<id>-<name>.md" for job `id` named `name` (name made a valid segment: a-z0-9-, at most
// 24; an empty or wholly invalid name is "report"). InvalidArgument for an empty or invalid id.
Status agentReportPath(const std::string& id, const std::string& name, std::string& out);

struct AgentsDeps {
  JobEngine* jobs = nullptr;
  std::string backend = "anthropic";            // the provider every job runs on
  std::function<std::string()> chat;            // the running turn's chat ("" outside a turn)
  // One pump of the job engine, so a queued job is dispatched inside the turn (one request on
  // the same task, never concurrent). Null: jobs start at the next pump after the reply.
  std::function<void()> dispatchNow;
};

// agent_start (always on), agent_list and agent_stop (the agents group behind more) on `reg`,
// with their texts.h declarations. InvalidArgument without jobs or chat.
Status registerAgentTools(nimbus::orch::ToolRegistry& reg, const AgentsDeps& d);

// The device note a report turn stores: its header line (name, id, the file it was saved to,
// failed or not), then the report inside the outside-content fence.
std::string agentReportNote(const JobEngine::FreshResult& f);

// Runs one report turn in `chatId` with `note`. Busy: the turn did not start (nothing was stored)
// and the report is put back. Any other status: the turn ran.
using ReportTurnFn = std::function<Status(const std::string& chatId, const std::string& note)>;

// The arrival: every parked result of `chatId`'s scope, oldest first, one report turn each (in
// the chat that started the job). `ran` counts the turns that ran. A turn that did not start
// stops the arrival and puts that result and the rest back, in order; its status is returned.
Status deliverReports(JobEngine& jobs, const std::string& chatId, const ReportTurnFn& turn, int& ran);

}  // namespace plain
}  // namespace agent
