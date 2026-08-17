#pragma once
// Ported and adapted from Nuage-Solide src/agent/orch_schema.h + orchestrator.cpp
// (Head Orchestrator v2 - enqueueSpawn / runTurn parse path).
//
// The PORTABLE turn contract: parse a provider's JSON response string into a typed,
// already-validated `Turn` and hand it to the device turn-loop, so the device never
// pokes a raw JsonDocument. No Arduino - ArduinoJson only (host-safe). All caps and
// UTF-8 handling come from nimbus/orch/caps.h + nimbus/mem_cap.h.
//
// Contract shape (source: ORCH_SCHEMA_BODY, orch_schema.h):
//   { reply, memory, ask : string
//     device[] : object   (validated/executed device-side; carried raw here)
//     spawn[]  : { provider, model, skill, task, category, note, name } : string
//     await[]  : string }
//
// parseTurn is STRICT ON THE THREE STRING KEYS (reply/memory/ask required and typed)
// - a missing or mistyped one rejects the whole turn. EVERY array key is OPTIONAL:
// absent => empty, present-but-not-an-array => WrongType. This keeps old six-field
// turns valid AND lets a new turn omit spawn/await/device and drive the run via the
// live-integration arrays instead. Within a present array, legacy items (spawn/await/
// device) are still validated strictly per-item (a bad item rejects the turn), while
// the live-integration items (mem_write/mem_query/session_ops) are TOLERANT - a
// malformed item is dropped, never a whole-turn error. Defaults / lowercasing /
// byte-caps are applied on copy.

#include <string>
#include <vector>

namespace nimbus {
namespace orch {

// One sub-agent spawn intent. All fields are strings on the wire; `task` is the
// only semantically-required one (an item with an empty task is dropped, matching
// the device enqueueSpawn: `if (!task[0]) return;`).
struct Spawn {
  std::string provider;   // lowercased on copy; "" => device resolves by priority
  std::string model;      // may be empty/invalid; device coerces vs its choice list
  std::string skill;      // provider-native hint, e.g. "deep_research" | "web"
  std::string task;       // the NL instruction; non-empty or the item is dropped
  std::string category;   // "code" | "research" | "ops"; defaults to "research"
  std::string name;        // model-chosen short display name ("css-fixer"); owner-facing
                           // everywhere (Telegram/e-ink/prompt digest). "" = unnamed
                           // (older wire) -> callers fall back to model+category.
  std::string note;       // one-line owner ack; defaults to "On it."
  std::string project;    // v4.0.0: FileStore project tag (auto-persist target)
  std::vector<std::string> attach;  // v4.0.0: docs to splice into the instruction
};
// Spec-name alias (the SPEC calls this SpawnItem).
using SpawnItem = Spawn;

// A device[] action, carried as the RAW JSON text of the element. The portable
// device-action VALIDATOR (nimbus/orch/device_actions.*, sibling module) turns this
// into a typed+clamped action with an allowed/reason verdict; the executor lives on
// the device. Keeping the raw slice here decouples the turn parser from that module
// so each can be built and host-tested independently.
struct DeviceAction {
  std::string json;       // the element's serialized JSON object, e.g. {"led":{...}}
};

// A memory the model wants to store in the associative VDB this turn (Q2:
// explicit, LLM-directed - never auto-mirrored). content is byte-capped like the
// model-memory field; importance is clamped [0,1] on parse.
struct MemWrite {
  std::string content;
  double      importance = 0.5;
  bool        permanent  = false;
  std::string ttl;         // "" = default class; else session|days|weeks|months|permanent
};

// A conversational session op the model issues (the ONLY advertised way to
// spawn - the old spawn[] array is deprecated). The WIRE advertises just
// spawn|terminate (spawn: task/provider/model + the v4.0.0 fields below;
// terminate: id). tell/poll/list remain PARSEABLE as no-ops for old clients but
// are not in the schema. Unknown ops are dropped on parse.
struct SessionOp {
  std::string op;
  std::string id;        // target session tag (terminate)
  std::string message;   // tell (retired from the wire; parsed for compat)
  std::string task;      // spawn
  std::string provider;  // spawn (optional; device resolves by priority if "")
  std::string model;     // spawn (optional)
  // v4.0.0 (the wire finally carries what the JobEngine could always do):
  std::string skill;     // spawn: capsule id - injected into the brief (approved only)
  std::string name;      // spawn: model-chosen display name; also the persist doc name
  std::string project;   // spawn: FileStore project tag - the sub-result auto-persists
                         // to /mem/files/<project>/<name>-<tag>.md (fan-out runs)
  std::vector<std::string> attach;  // spawn: up to 4 "<project>/<name>" docs whose
                                    // CONTENT the device splices into the instruction
};

// A scratchpad update the model returns inline (a goals-update field) -
// a FREE write, no tool round. Each tier is applied ONLY when its `has*` flag is
// set (the wire sent a non-null value); an unset flag leaves that tier untouched.
// present=false => the whole `scratchpad` field was null/absent (no change).
struct ScratchUpdate {
  bool present = false;
  bool hasActive = false; std::string active;
  bool hasShort = false;  std::vector<std::string> shortItems;
  bool hasMid = false;    std::vector<std::string> midItems;
  bool hasLong = false;   std::vector<std::string> longItems;
};

struct Turn {
  std::string reply;                  // user-facing text ("" if none)
  std::string memory;                 // updated model memory ("" if unchanged)
  std::string ask;                    // question to the user ("" if none)
  ScratchUpdate scratchpad;           // inline scratchpad update (absent => no change)
  std::vector<DeviceAction> device;   // raw device[] elements (validated elsewhere)
  std::vector<Spawn>        spawn;     // DEPRECATED (Q3) - kept parseable; unadvertised
  std::vector<std::string>  await_;    // DEPRECATED - session.poll supersedes it
  // ---- live-integration additions (all optional; absent => empty) ----
  std::vector<MemWrite>    mem_write;    // store facts in the VDB (memory.write)
  std::vector<std::string> mem_query;    // searches; results echoed into the NEXT turn
  std::vector<SessionOp>   session_ops;  // spawn/terminate (tell/poll/list parsed for compat)
};

// Structured parse outcome. `detail` names the offending field/reason for logs+tests.
struct ParseError {
  enum class Code {
    Ok = 0,
    JsonError,          // did not deserialize (garbage / truncated / not JSON)
    NotObject,          // top-level value is not a JSON object
    MissingField,       // a required top-level key is absent
    WrongType,          // a top-level field or spawn field has the wrong JSON type
  };
  Code        code = Code::Ok;
  std::string detail;   // human-readable: which field, why
  bool ok() const { return code == Code::Ok; }
};

// Parse + validate a provider response string into `out`. Returns true iff the turn
// is contract-valid (err.code == Ok). On failure `out` is left cleared and `err`
// carries the reason. Deterministic and allocation-bounded; host-safe.
//
// Validation:
//   1. Must deserialize to a JSON OBJECT           -> JsonError / NotObject.
//   2. reply, memory, ask are REQUIRED strings (absent -> MissingField, mistyped
//      -> WrongType). EVERY array key (device/spawn/await/mem_write/mem_query/
//      session_ops) is OPTIONAL: absent => empty; present-but-not-an-array =>
//      WrongType. Old six-field turns still validate; a new turn may omit
//      spawn/await/device and use the live-integration arrays instead.
//   3. Each spawn[] element (STRICT) must be an object with the six string keys
//      present and typed (WrongType otherwise). Then, on the accepted item:
//        - empty task  -> the item is DROPPED silently (not an error).
//        - provider    -> lowercased.
//        - category "" -> "research"; note "" -> "On it.".
//        - every field byte-capped UTF-8-safe to its device buffer size (caps.h).
//   4. spawn vector is truncated to kAgentMaxJobs (6) accepted items.
//   5. await[] entries must be strings (WrongType otherwise); blank ones skipped.
//   6. device[] elements must be objects (WrongType otherwise); each carried raw.
//   7. mem_write/mem_query/session_ops (live-integration) are TOLERANT per-item: a
//      malformed item is dropped (never a whole-turn error); each capped to
//      kAgentMaxJobs. importance accepts int- or float-encoded numbers, clamped [0,1].
//
// Byte caps applied to string outputs (device buffer parity, all UTF-8-safe):
//   reply/ask   : uncapped (user-facing; the device budgets them elsewhere)
//   memory      : kMemModelMax (1200) - mirrors the memory module's cap so the
//                 turn is self-consistent even before OrchMemory::setModel runs
//   spawn.task  : kSpawnTaskMax (4096); provider/model/category/skill/name/
//                 project/note to their (buffer-1) usable sizes; attach refs to
//                 kSpawnAttachRefMax each. The task buffer is a PSRAM std::string.
bool parseTurn(const std::string& json, Turn& out, ParseError& err);

}  // namespace orch
}  // namespace nimbus
