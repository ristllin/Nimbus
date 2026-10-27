#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "nimbus/mem_cap.h"   // utf8CapLen - cut the reply on a character boundary

// Ported and adapted from Nuage-Solide src/agent/result_envelope.h
// (Head Orchestrator v2). Arduino-free: the fixed char[] fields are plain POD so
// this compiles + tests natively. Shared result type for both inline (fast) and
// heavy (async) agent paths; the device main loop only ever sees this struct,
// never provider-specific types.
namespace nimbus {
namespace orch {

enum class JobState : uint8_t {
  Queued     = 0,  // accepted, not yet dispatched
  Running    = 1,  // remote agent working
  NeedsInput = 2,  // waiting for human clarification (HITL; non-terminal)
  Done       = 3,  // terminal: result populated
  Error      = 4,  // terminal: error populated
  Unknown    = 5,  // poll failed; retry later (never persisted as terminal)
  Cancelled  = 6,  // terminal: cancelled
};

inline bool isTerminal(JobState s) {
  return s == JobState::Done || s == JobState::Error || s == JobState::Cancelled;
}

struct Artifact {
  char type[16];  // "file" | "pull_request" | "commit" | "link" | "text"
  char url[192];  // for type=="file": the provider file_id (download key)
  char label[64]; // for type=="file": the file name (carries the extension)
};

// artifacts[] carries provider-produced FILE references (v4.1): a Mistral
// code_interpreter sub-agent that writes a file fills these (mistralPoll), and
// the JobEngine fetches each to SD + registers it in the file store. 3 covers a
// chart+data+report run (~272 B each on the PSRAM-backed poll slot); past the
// cap the adapter appends an honest "[+N more ...]" note to the reply instead of
// dropping silently (prism v4.1 #7). actions[] stays unpopulated (deferred).
static constexpr int kMaxArtifacts = 3;
static constexpr int kMaxActions   = 1;

struct Action {
  char tool[32];
  bool ok;
  char summary[96];
};

struct ResultEnvelope {
  JobState state       = JobState::Unknown;
  char     jobId[96]   = {};  // "backend:remoteId"; empty for inline
  char     backend[16] = {};  // "anthropic" | "openai" | "mistral"
  char     category[16]= {};  // "code" | "research" | "ops"
  char     thinking[128]={};  // short rationale for the panel
  char     reply[16384] = {}; // main text result. 1024 -> 4096 (owner R5f) ->
                              // 16384 (v4.0.0: a sub-agent can RETURN a whole
                              // document; auto-persist + the results ring carry
                              // it in full). ⚠ The envelope slot must live in
                              // PSRAM, never a static/internal buffer - the
                              // JobEngine allocates its slot via WorkingAllocator.
  char     error[128]  = {};  // set when state==Error
  // Sub-session token usage, filled ONLY from a real provider `usage` object at the
  // terminal poll (the OpenAI Responses poll; the Anthropic session read once the
  // events poll shows it finished). Feeds the "spawn:<backend>" spend attribution;
  // additive POD, zeroed by the existing memsets.
  uint32_t promptTokens     = 0;
  uint32_t completionTokens = 0;
  // Prompt-cache traffic of the same usage, in the provider's native meaning
  // (TokenUsage): the Anthropic session read reports both outside promptTokens.
  // Lane DEF: the monthly spend ceiling prices them at their own rates.
  uint32_t cacheReadTokens  = 0;
  uint32_t cacheWriteTokens = 0;
  // Connector provenance (CUM-536), filled by adapters that can see what ran
  // (the Mistral Conversations response lists one `tool.execution` entry per tool
  // run). connectorAsked = display name of an attached connector the TASK asked
  // for ("" = none named; a custom connector's id may be a 36-char UUID, hence
  // 48); connectorCalls = runs that count for it (countsAsConnectorRun: built-ins
  // and other attached connectors' runs excluded). connectorAsked set with
  // connectorCalls == 0 means the result is the model's own guess, and the job
  // engine opens it with an honest marker. Zeroed by the existing memsets.
  uint16_t connectorCalls     = 0;
  char     connectorAsked[48] = {};
  // Provenance counted from the PROVIDER'S own record of the run (never from the
  // agent's text): toolRuns = every tool the sub-agent actually ran (Mistral
  // tool.execution entries, OpenAI output items ending in "_call", Anthropic
  // agent.tool_use / agent.mcp_tool_use / agent.custom_tool_use events); webRuns =
  // those that were web searches or fetches. A sub that claims "I searched the
  // web" with webRuns == 0 did not. Zeroed by the existing memsets (+4 B).
  uint16_t toolRuns = 0;
  uint16_t webRuns  = 0;
  // The same record by name (sub_provenance.h ToolTally::render): "web_search x2,
  // notion x0", a tool the run was offered and never ran counting 0 where the adapter
  // sees the offer (Mistral). The spawn ledger and [FRESH RESULTS] carry it, so a later
  // question about the sub is answered from what it ran. "" = the adapter recorded no
  // names. Zeroed by the existing memsets.
  char     toolNames[96] = {};
  Action   actions[kMaxActions] = {};
  int      actionCount = 0;
  Artifact artifacts[kMaxArtifacts] = {};
  int      artifactCount = 0;
};

// ---- the spill marker (L19-36) ------------------------------------------------------
// The marker a provider adapter's reply writer puts at the end of a reply the 16 KB
// envelope cut (providers/wire.h ReplyWriter). The writer keeps the whole reply aside,
// the job engine saves it to the card, and endWithSpillMarker turns this marker into
// one that names the saved file and the reply's real size.
inline constexpr const char* kEnvelopeCutMarker = "\n[cut at 16 KB]";

// The marker that ends a spilled reply: where the whole reply is (`path`, its real
// size `realBytes`) and how to read the rest; or, `path` empty, that the rest could
// not be saved and why (`why`).
inline std::string spillMarker(const std::string& path, size_t realBytes, const std::string& why) {
  if (path.empty())
    return "\n[cut at 16 KB: the rest of the reply could not be saved" + (why.empty() ? std::string() : " (" + why + ")") +
           "]";
  return "\n[cut at 16 KB: the whole reply, " + std::to_string(realBytes) + " bytes, is saved as " + path +
         "; read the rest with files.read from an offset]";
}

// Replace the reply's cut marker with spillMarker(...), keeping any note after it and
// cutting the reply text further, on a character boundary, to make room. False (the
// reply untouched) when the reply carries no cut marker.
inline bool endWithSpillMarker(ResultEnvelope& env, const std::string& path, size_t realBytes,
                               const std::string& why = std::string()) {
  const std::string reply(env.reply);
  const size_t at = reply.rfind(kEnvelopeCutMarker);
  if (at == std::string::npos) return false;
  const std::string tail = reply.substr(at + std::strlen(kEnvelopeCutMarker));
  const std::string mark = spillMarker(path, realBytes, why);
  const size_t cap = sizeof(env.reply) - 1;
  if (mark.size() + tail.size() >= cap) return false;
  const int keep = utf8CapLen(reply.c_str(), (int)at, (int)(cap - mark.size() - tail.size()));
  const std::string out = reply.substr(0, (size_t)keep) + mark + tail;
  std::memcpy(env.reply, out.c_str(), out.size() + 1);
  return true;
}

}  // namespace orch
}  // namespace nimbus
