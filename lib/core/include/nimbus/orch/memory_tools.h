#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_archive.h"
#include "nimbus/orch/vector_memory.h"

// memory_tools - the `memory.*` MCP tool surface, wiring the portable memory
// engines (VectorMemory + Scratchpad + MemConfig) into ToolRegistry handlers:
// memory_write / memory_search + memory_config + a scratchpad tool for the
// goal tiers.
//
// This is the integration seam that makes the on-device MCP server real: an LLM
// (via the function-calling bridge) or an external MCP client calls
// memory.write / memory.search / memory.config / memory.scratchpad and it
// actually mutates the associative memory + working memory. The ONE thing that
// needs the device is the embedder (text -> vector via the provider /embeddings
// API); it is injected as a std::function, so host tests supply a deterministic
// fake and exercise the entire path (RPC -> tool -> engine) with no network.
//
// Everything here is Arduino-free and host-tested (pio test -e native).
namespace nimbus {
namespace orch {

// Text -> quantized embedding. The device binds this to the provider embeddings
// call (float result -> VectorMemory::quantize); tests bind a fake. An empty
// return signals "embedding unavailable"; the callee writes the REAL cause into
// `err` (the device seam's `embeddings::embedWith` err token) so the tool can
// report why instead of a fixed guess (CUM-435). `err` is left empty on success.
using Embedder =
    std::function<std::vector<int8_t>(const std::string& text, std::string& err)>;

// ---- honest embedding-failure reasons (CUM-435) -----------------------------
// The memory tools used to collapse every embedding failure to one fixed string
// ("embedding unavailable (provider offline?)"), hiding the real cause from the
// model and the owner. These map the device seam's raw err token to an honest,
// secret-safe class + words, the same honest-refusal shape as voice_route.
//
// A class per real failure mode; a new class with no words fails the host test
// that iterates this enum, so the mapping cannot silently rot.
enum class EmbedFail {
  None = 0,         // an embedding was produced (no failure)
  NoKey,            // no embeddings key for the configured provider
  KeyRejected,      // provider returned 401/403
  Busy,             // the single on-device TLS slot was in use (retryable)
  Unreachable,      // could not connect to the provider host
  Timeout,          // the provider did not answer in time
  OutOfCredit,      // a billing refusal (billing_refusal.h): out of credit or quota
  RateLimited,      // HTTP 429 without a billing token, or the router's rate_limited
  RouteNotAllowed,  // router refusal: endpoint_not_allowed
  ProviderError,    // any other non-200 HTTP status
  BadResponse,      // a 200 whose body did not parse as an embedding
  WidthMismatch,    // a vector whose width is not the store's (CUM-469: stored dims
                    // disagree with the model's native width, e.g. mistral-embed 1024)
  NoModel,          // no embeddings model configured
  BadRequest,       // nothing to embed (empty text)
  Unknown           // an unmapped token (generic line)
};

// Classify a raw failure token from `agent::embeddings::embedWith` err. Pure +
// host-tested. `detail` receives ONLY a bounded, secret-safe extra (a KNOWN
// provider slug for NoKey, the numeric HTTP status for ProviderError, or the two
// numeric widths "<got>/<expected>" for WidthMismatch) or is left empty - it
// NEVER echoes arbitrary raw text, so a key-shaped string in `raw` can never
// reach the tool result.
EmbedFail classifyEmbedFail(const std::string& raw, std::string& detail);

// Concise, honest cause phrase for a failure class (no "embeddings:" prefix, no
// trailing period), AGENTS section 6 style: US English, no em dash, states what
// happened. `detail` from classifyEmbedFail is folded in where the class carries
// one. Used inside the search fallback label. Pure + host-tested.
std::string embedFailWords(EmbedFail kind, const std::string& detail);

// Convenience: classify `raw` then format "embeddings: <words>[; next step]" for
// a tool that must refuse (write/update). Pure + host-tested.
std::string embedFailReason(const std::string& raw);
// The same line from an already classified failure (the park memo keeps the class,
// not the raw token).
std::string embedFailReasonFor(EmbedFail kind, const std::string& detail);

// ToolResult::code values of memory.write / memory.update. Every write ends in
// exactly one of them, and the words of a queued or refused one say so.
inline constexpr const char* kMemOutcomeStored    = "stored";
inline constexpr const char* kMemOutcomeDuplicate = "duplicate";
inline constexpr const char* kMemOutcomeQueued    = "queued";
inline constexpr const char* kMemOutcomeRefused   = "refused";

class PendingEmbeds;   // pending_embed.h
class EmbedPark;

// What the long-term memory writes actually did (lane M). Monotonic RAM counters
// bumped by the memory.write / memory.update handlers on EVERY path into them: a
// mid-turn tool call and the orch_turn mem_write[] field both dispatch through the
// registry, so one counter covers both. A background turn takes a snapshot before
// and after and reports the difference, so "ok" can mean "every write landed".
struct MemOpCounters {
  uint32_t added = 0;       // memory.write stored a new entry
  uint32_t dupSkipped = 0;  // memory.write matched an existing entry (importance bumped)
  uint32_t updated = 0;     // memory.update stored the new wording
  uint32_t queued = 0;      // a write/update could not embed and waits in the queue (lane P)
  uint32_t failed = 0;      // a write or update was refused (nothing stored)
  MemOpCounters operator-(const MemOpCounters& b) const {
    MemOpCounters d;
    d.added = added - b.added;
    d.dupSkipped = dupSkipped - b.dupSkipped;
    d.updated = updated - b.updated;
    d.queued = queued - b.queued;
    d.failed = failed - b.failed;
    return d;
  }
};

// Borrowed engines + seams the memory tools operate on. All pointers must
// outlive the registry the tools are added to.
struct MemoryContext {
  VectorMemory*  vec = nullptr;
  Scratchpad*    scratch = nullptr;
  MemConfig*     cfg = nullptr;
  EpisodicStore* episodic = nullptr;         // optional: enables memory.episodic
  // Cold store for TTL-expired memories (CUM-225). Bound ONLY when the archive
  // exists, which the device does only when an SD card is present - so memory.archive
  // is registered (exposed) only with a card. `archiveAvailable` is a live check the
  // handler consults so a card pulled mid-run refuses cleanly (the tool stays
  // registered, but every action reports the archive is gone).
  VectorArchive* archive = nullptr;          // optional: enables memory.archive
  std::function<bool()> archiveAvailable = [] { return true; };
  Embedder       embed;                      // required for write/search
  std::function<uint32_t()> nowHours = [] { return 0u; };  // clock for TTL stamping
  // Save the scratchpad after memory.scratchpad changed it; false = the save
  // failed and the notes live in RAM only until restart (the tool result says
  // so). Null => nothing to save (host rigs).
  std::function<bool()> persistScratch;
  MemOpCounters* ops = nullptr;               // optional: write outcome counters (lane M)
  // False while the memory engine cannot take writes (the device's FAULT memory):
  // memory.write and memory.update refuse as "memory engine unavailable", counted
  // like every other refusal, from a tool call or a turn's mem_write alike, and the
  // pending queue's drain stores nothing (its facts stay queued). Null = always
  // writable.
  std::function<bool()> writable;

  // ---- embedding outages (pending_embed.h). All optional: with `pending` null a
  // write the embedder cannot serve is refused exactly as before.
  // The durable queue a write lands in when it cannot be embedded now, and the
  // write that makes it durable (false = not durable: the write is refused).
  PendingEmbeds* pending = nullptr;
  std::function<bool()> persistPending;
  // The RAM memo that skips doomed embeds while the provider refuses.
  EmbedPark* park = nullptr;
  // Guards the park memo across tasks (device: the recursive memory Lock; the
  // drain embeds outside the lock, so the park is read and written under this).
  // Null = single-threaded host.
  std::function<void(const std::function<void()>&)> parkLock;
  std::function<uint32_t()> nowMs = [] { return 0u; };
  // Is nowHours() the real clock? A fact queued before the clock is set is
  // stamped when it is stored.
  std::function<bool()> clockSynced = [] { return true; };
  // The configured embeddings provider slug, named in the words only when known.
  std::function<std::string()> embedProvider;
  // A park started for a new reason (raw = the seam's token). The device tells
  // the owner once per billing episode.
  std::function<void(EmbedFail kind, const std::string& raw)> onParked;
  // Every memory.write / memory.update outcome code, for the turn and dream counts.
  std::function<void(const char* code)> onWriteOutcome;
};

// The honest effect reports the claim check reads (turn_effects.h): a pin on an
// already pinned memory, and a scratchpad change whose save failed.
extern const char kAlreadyPinnedPrefix[];   // "already pinned (" + id + kAlreadyPinnedTail
extern const char kAlreadyPinnedTail[];
extern const char kScratchRamOnlyNote[];    // appended to a scratchpad change result

// ---- automatic recall ----------------------------------------------------------
// One automatic recall for a turn: the hits, and - when embeddings could not serve
// it - why. Every recall seam (device, nimbusd, the lab) returns this, so a seam
// that hides an outage behind an empty list cannot be written by accident: an
// empty `hits` with an empty `degraded` means "nothing relevant", never "recall
// is down".
struct RecallOut {
  std::vector<std::string> hits;   // "[NN%] content" bullets (pending ones are tagged)
  std::string degraded;            // "" = semantic recall ran; else the cause words
};
// Degraded recall: a bounded keyword scan with the same read boundary and expiry
// rule as the semantic path, over the live store and the pending queue (tagged
// "pending, not stored yet"). Read-only: no importance boost, no TTL reset.
//
// It runs while embeddings are down, often on a device already short of memory, so
// it copies nothing: one non-copying pass picks the kLexScanMax most important
// entries as (importance, index) pairs (8 bytes each, through the working allocator,
// PSRAM on the device), each is scored in place, the queue is read by reference, and
// text is built only for the k hits returned. Its allocation is bounded by k and the
// scan cap, never by the store size (test_orch_recall_alloc counts it at 5,000
// entries).
// Entries a keyword scan examines at most, so a full store cannot turn a degraded
// search into a long stall under the memory lock.
inline constexpr size_t kLexScanMax = 2000;
struct RecallScope {
  std::vector<std::string> nsAllow;   // the caller's read set (empty = unscoped)
  uint32_t nowHours = 0;              // the expiry clock (0 = clockless: nothing expires)
  int k = 5;                          // at most this many hits
  // The device is below the recall memory floors (the same free-heap and largest-block
  // floors as the embed path): the scan is skipped and `degraded` says so.
  bool lowMemory = false;
};
// Appended to RecallOut::degraded when the scan was skipped for low memory.
extern const char kRecallLowMemoryTail[];
RecallOut lexicalRecall(const VectorMemory& vec, const PendingEmbeds* pending, const std::string& query,
                        const RecallScope& scope, const std::string& cause);

// ---- the pending-embed drain (one entry per call) ----------------------------
// Stores the oldest queued fact through the same rails as a direct write, under
// the CURRENT principal of its namespace. Crash-safe order: the vector store is
// persisted first, and the entry leaves the queue only after that write
// succeeded, so a crash replays it at most once more (its id is already stored,
// and that replay only pops it).
struct EmbedDrainDeps {
  // The principal that owns `ns` now (false: no such principal any more).
  std::function<bool(const std::string& ns, Principal& who)> principalForNs;
  // Run `fn` under the memory lock (null: run it directly). The embed call
  // itself always runs outside it.
  std::function<void(const std::function<void()>&)> withLock;
  // Persist the vector store; false = not durable (the entry stays queued).
  std::function<bool()> persistVectors;
};
enum class EmbedDrainStep {
  Idle,        // nothing queued
  Parked,      // embeddings are parked: no network
  Stored,      // stored, durable, and popped
  Duplicate,   // it was already stored (a replay, or a near-duplicate): popped
  Dropped,     // its namespace has no writer any more: popped, never stored
  Refused,     // the rails refused it now (quota): popped, the writer is told
  Failed,      // the embed failed: still queued, moved to the back
  NotDurable,  // the vector store could not be persisted: still queued
};
struct EmbedDrainResult {
  EmbedDrainStep step = EmbedDrainStep::Idle;
  std::string id;
};
EmbedDrainResult drainOne(const MemoryContext& ctx, const EmbedDrainDeps& d);
const char* embedDrainStepName(EmbedDrainStep s);

// Register memory.write / memory.search / memory.config / memory.scratchpad on
// `reg`, backed by `ctx`. `ctx` is captured by value (it holds pointers +
// std::functions), so the engines it points at must outlive `reg`.
void registerMemoryTools(ToolRegistry& reg, const MemoryContext& ctx);

}  // namespace orch
}  // namespace nimbus
