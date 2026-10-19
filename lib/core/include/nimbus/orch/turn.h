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
//   { reply : string (required), memory, ask : string (optional)
//     device[] : object   (validated/executed device-side; carried raw here)
//     spawn[]  : { provider, model, skill, task, category, note, name } : string
//     await[]  : string }
//
// parseTurn is STRICT ON `reply` ONLY (required, must be a string) - without it
// there is no usable turn. `memory` and `ask` are OPTIONAL strings (CUM-483):
// absent, null, or mistyped read as "" ("memory unchanged" / "no question"), so a
// model that leaves one out (the Anthropic wire schema is advisory) keeps its
// side effects. EVERY array key is OPTIONAL and REPAIR-OR-REPORT: a field that
// arrives in another shape (a JSON string, a bare object) is repaired by the
// normalizer, and whatever cannot be repaired is named in Turn::parseNotes /
// Turn::spawnDrops - never zero items with no trace (the 16-18 Oct spawn loss:
// six turns promised 34 spawns and parseTurn read zero, silently). A malformed
// item is still never a whole-turn error. Defaults / lowercasing / byte-caps are
// applied on copy.

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
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
                           // everywhere (Telegram/panel/prompt digest). "" = unnamed
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

// The orch_turn fields the parse boundary knows, in wire-schema order
// (ORCH_SCHEMA_BODY), then the two retired arrays the parser still tolerates.
// One table serves the leak repair (which fields may hide in a leaked tail) and
// the container normalizer (what shape each field must have); a host test pins it
// to the schema's own property list, so a field added there fails until listed.
enum class TurnField : uint8_t {
  Reply = 0, Memory, Ask, Device, MemWrite, MemQuery, SessionOps, Scratchpad,
  Spawn, Await, Count
};
enum class FieldKind : uint8_t { String, ArrayOfObjects, ArrayOfStrings, Object };
const char* turnFieldName(TurnField f);
FieldKind   turnFieldKind(TurnField f);
// Exact, case-sensitive match on a field name of length n; false when unknown.
bool turnFieldFromName(const char* s, size_t n, TurnField& out);

// What the leak repair did to one turn (formatting F1). Masks are bit
// (1 << TurnField). RAM only, never persisted.
struct LeakRepair {
  bool     repaired = false;     // a leaked tool-markup tail was cut from a top-level string
  uint16_t recoveredMask = 0;    // fields recovered from that tail (absent or null before)
  uint16_t containerMask = 0;    // the recovered fields that carry actions (arrays, scratchpad)
  uint8_t  nestedCuts = 0;       // nested strings (a spawn task, a memory) cut at a leak
  uint8_t  droppedBlocks = 0;    // leaked container values that did not parse to their type
};

// One spawn intent the parse boundary saw but could not carry to the job
// engine, with the plain reason. Every entry is reported to the owner and to
// the head (apply), never dropped in silence.
struct SpawnDrop {
  std::string name;     // the item's display name ("" when it had none)
  std::string task;     // the item's task text, capped ("" when it had none)
  std::string reason;   // why it did not start, for the head's [SPAWN REFUSED] note
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
  // ---- parse-boundary report (RAM only; empty on a well-formed turn) ----
  // One line per field the normalizer repaired or dropped, for the log and the
  // in-turn correction ("session_ops arrived as a JSON string; repaired").
  std::vector<std::string> parseNotes;
  std::vector<SpawnDrop>   spawnDrops;   // spawn intents that cannot start (reported, never silent)
  LeakRepair               repair;       // leaked tool markup cut from reply/memory/ask
  int                      coercedFields = 0;   // container fields repaired from a JSON string
};

// The parse-boundary normalizer on its own (also run by parseTurn): repairs a
// leaked tool-markup tail (repairLeakedParams), then brings every container
// field to the shape the contract expects - a JSON string that parses to it, a
// bare object where a list of objects belongs, a spawn list hidden in an item's
// `op`, a task under another name, an op that is missing or a synonym of spawn.
// What cannot be repaired is reported in `report` (parseNotes / spawnDrops),
// never dropped in silence. Returns true when the document changed; a
// well-formed turn is left byte-identical and returns false with no notes.
bool normalizeTurnDoc(ArduinoJson::JsonObject root, Turn& report);

// Spawn items carried by a call to a turn field used as a tool (the head calls
// session_ops(...) as a function): the same normalizer over the call's
// arguments, whatever their shape ({"session_ops":[...]}, {"session_ops":"[...]"},
// a bare spawn object, {"op":"[...]"}, {}). Accepted items land in `out`
// (op "spawn" only); unusable ones in out.spawnDrops. Returns the accepted count.
int spawnsFromToolArgs(const std::string& argsJson, Turn& out);

// Length-aware salvage of a cut or unreadable head reply (parsing P3): a bounded
// single pass over the reply's leading JSON object (bare or fenced), which may be
// cut short. It reads only the TOP-LEVEL "reply" and "ask" strings - nested
// values (device, mem_write, session_ops, scratchpad) can never be salvaged -
// decoding JSON escapes (a lone surrogate becomes U+FFFD). A string cut before
// its closing quote keeps its decoded prefix, trimmed UTF-8-safe to the last
// sentence or word boundary, with *Complete = false.
struct Salvage {
  std::string reply, ask;
  bool replyComplete = false, askComplete = false;
  bool found() const { return !reply.empty() || !ask.empty(); }
};
bool salvageTurnText(const std::string& text, Salvage& out);

// The fixed line appended to a salvaged reply that was cut (by the provider's
// output cap) or that came from a turn the provider reported cut. Shared so the
// engine and the tests say the same thing.
extern const char kSalvageCutMarker[];

// True when a head-turn error names a cut answer: every line the typed step
// rule (classifyStepError, head_loop.h) reads as StepErr::Cut - the output limit
// after the one retry with more room, or an answer too large for the device -
// plus, for a line that rule cannot type, the legacy spellings (Anthropic
// max_tokens, an OpenAI incomplete response, a chat-completions finish_reason of
// length). A line it types as anything else (a 400 naming max_tokens) is not a
// cut. The engine's salvage path reads it.
bool isCutError(const std::string& err);

// The reason a spawn past the per-turn cap did not start (one wording for the
// parser and the pseudo-tool merge).
std::string spawnOverCapReason();

// Structured parse outcome. `detail` names the offending field/reason for logs+tests.
struct ParseError {
  enum class Code {
    Ok = 0,
    JsonError,          // did not deserialize (garbage / truncated / not JSON)
    NotObject,          // top-level value is not a JSON object
    MissingField,       // `reply` (the one required key) is absent
    WrongType,          // `reply` is present but not a JSON string
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
//   2. reply is the one REQUIRED string (absent -> MissingField, mistyped ->
//      WrongType). memory and ask are OPTIONAL strings: absent / null / mistyped
//      => "" (CUM-483). EVERY array key (device/spawn/await/mem_write/mem_query/
//      session_ops) is OPTIONAL: absent or present-but-not-an-array => empty.
//      Old six-field turns still validate; a new turn may omit
//      spawn/await/device and use the live-integration arrays instead.
//   3. Each spawn[] element is TOLERANT: a non-object, or one whose task is
//      missing / non-string / empty, is DROPPED silently (not an error). On an
//      accepted item:
//        - provider    -> lowercased.
//        - category "" -> "research"; note "" -> "On it."; other fields default "".
//        - every field byte-capped UTF-8-safe to its device buffer size (caps.h).
//   4. spawn vector is truncated to kAgentMaxJobs (6) accepted items.
//   5. await[] non-string entries are dropped; blank ones skipped.
//   6. device[] non-object elements are dropped; each kept element carried raw.
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

// How a head reply was read (CUM-495).
enum class ReplyShape {
  Turn,      // a contract-valid orch_turn (bare JSON, or the WHOLE reply fenced)
  Prose,     // plain text: delivered as the reply, nothing else rides it
  Unusable,  // opens like JSON but is not a turn - the caller's salvage path
};

// Lenient read of a head reply. A head whose wire cannot pin the schema (Z.ai
// ignored response_format json_schema and answered in Markdown) still produces a
// usable answer, and the device used to replace it with a canned "formatting
// error" apology. The rule:
//   - Turn: the WHOLE trimmed reply is exactly one JSON object, or exactly one
//     ``` fence whose whole content is one JSON object, and it passes parseTurn.
//     "Exactly" is checked by matching the object's closing brace, because the
//     JSON parser itself stops after the first value and ignores what follows -
//     a reply that only STARTS with an object is never executed as a turn.
//   - Unusable: the reply opens like a JSON object (a brace, or a fence whose
//     first content line starts with one) and is not a Turn - truncated, broken,
//     not a turn, or a turn-shaped object followed by more text - or the whole
//     reply is one JSON array. The caller's
//     salvage keeps reply/ask only (see leadingJsonObject); raw wire JSON never
//     reaches a person and nothing in it is executed.
//   - Prose: anything else, including a reply that opens with a JSON example that
//     is not turn-shaped and goes on in prose. `out` carries the trimmed text as
//     the reply and NOTHING else - quoted JSON never becomes device actions,
//     memory writes or spawns.
ReplyShape parseHeadReply(const std::string& text, Turn& out, ParseError& err);

// The leading JSON object of a reply - bare at the start, or the content of a
// leading ``` fence - as its exact text ("" when the reply does not open with a
// complete one). `wholeReply` (optional) reports whether that object IS the whole
// reply (nothing after it but whitespace and, when fenced, the closing fence).
// The salvage path reads reply/ask out of this span only; a provider's own
// lenient reader uses `wholeReply` to decide whether the reply is a turn.
std::string leadingJsonObject(const std::string& text, bool* wholeReply = nullptr);

}  // namespace orch
}  // namespace nimbus
