#pragma once
#include <cstdint>

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
  char     thinking[128]={};  // short rationale for e-paper
  char     reply[16384] = {}; // main text result. 1024 -> 4096 (owner R5f) ->
                              // 16384 (v4.0.0: a sub-agent can RETURN a whole
                              // document; auto-persist + the results ring carry
                              // it in full). ⚠ The envelope slot must live in
                              // PSRAM, never a static/internal buffer - the
                              // JobEngine allocates its slot via WorkingAllocator.
  char     error[128]  = {};  // set when state==Error
  // Sub-session token usage, filled ONLY by adapters whose terminal poll returns
  // a real provider `usage` object (OpenAI Responses does; the Anthropic events
  // poll does not - honest zeros there). Feeds the "spawn:<backend>" spend
  // attribution; additive POD, zeroed by the existing memsets.
  uint32_t promptTokens     = 0;
  uint32_t completionTokens = 0;
  Action   actions[kMaxActions] = {};
  int      actionCount = 0;
  Artifact artifacts[kMaxArtifacts] = {};
  int      artifactCount = 0;
};

}  // namespace orch
}  // namespace nimbus
