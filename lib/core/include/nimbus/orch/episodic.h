#pragma once
#include <cstdint>
#include <string>
#include <vector>

// episodic - the "everything that ever happened" store: every session, message,
// and artifact, queryable by date / kind / session / text. It combines an
// append-only audit stream with filtered reads plus the session/message rows.
// Locked decision: on the device this is the append-log JSONL day-stream on SD
// + blob sidecars; here we define the PORTABLE record types + query model + an
// in-memory reference store, so the query semantics are host-tested and the
// device append-log impl is just another EpisodicStore behind the same interface.
//
// The kind column is an event-type discriminator widened with media kinds, so an
// image / audio note / transcript / raw file is a first-class, filterable row
// (referenced by blobPath -> /sd/mem/blobs/<hash>). Arduino-free -> host-tested.
namespace nimbus {
namespace orch {

// The event-type discriminator + the media kinds the design calls out.
enum class MsgKind : uint8_t {
  Message = 0,   // a chat message (user/assistant)
  ToolOutput,    // a tool result
  LlmResponse,   // a raw model response
  File,          // a stored file artifact (blobPath)
  Image,         // an image artifact
  Audio,         // a voice note / audio clip
  Transcript,    // an STT transcript
  Log,           // a system/healthcheck log line
};
const char* kindName(MsgKind k);
bool        kindFromName(const char* s, MsgKind& out);

struct EpisodicSession {
  std::string id;             // "s0007"
  uint32_t    startedHours = 0;
  std::string provider;       // "openai" | "anthropic" | "" (local)
  std::string title;          // one-line summary of the task
  std::string status = "active";  // "active" | "completed" | "timed_out"
};

struct EpisodicMessage {
  std::string id;
  std::string sessionId;
  uint32_t    tsHours = 0;
  std::string role;           // "user" | "assistant" | "system" | "tool"
  MsgKind     kind = MsgKind::Message;
  std::string text;           // inline text (empty for pure-blob rows)
  std::string blobPath;       // "/sd/mem/blobs/<hash>.ext" or ""
  std::string tags;           // comma-separated freeform tags
};

// Query filter for messages (any field left at its default is "don't care").
// The query model shared by every backend: by session, kind, time
// window, and a case-sensitive substring over `text` (full-text-ish).
struct MsgQuery {
  std::string sessionId;             // "" => any session
  bool        haveKind = false;
  MsgKind     kind = MsgKind::Message;
  // Additional kinds accepted alongside `kind`. The conversation window needs
  // this: a photo is stored as an Image row whose text IS its description, and
  // filtering to Message alone made every picture the owner sent invisible to
  // the model one turn later. A set, not a widened single kind, because the
  // trace rows (ToolOutput / LlmResponse) must stay OUT of the window - they
  // are for the debug view, and re-feeding them would double the context.
  std::vector<MsgKind> alsoKinds;
  uint32_t    sinceHours = 0;        // inclusive lower bound (0 => no bound)
  uint32_t    beforeHours = 0;       // exclusive upper bound (0 => no bound)
  std::string textContains;          // "" => no text filter
  int         limit = 50;            // max rows (most-recent first)
  // v3.7.0 READ BOUNDARY (distinct from sessionId, which is a caller's own
  // convenience filter). Empty = unscoped, for maintenance passes that
  // legitimately span every session (retention prune). A model-facing query
  // ALWAYS carries its principal's set, because memory.episodic is otherwise a
  // full-text read over every conversation the device has ever had - the
  // widest cross-principal leak in the system.
  std::vector<std::string> sessionAllow;

  // v4.0.0 DEEP HISTORY. The boot scan is budget-bounded (kHydrateMaxRows /
  // kHydrateMaxBytes), so on a device with months of chat the older rows are on
  // the card but not in the index - invisible to a plain query. `coldScan` opts
  // a query into walking those day-streams from disk, under its own per-call
  // budget. OFF by default: the hot per-turn windows must stay zero-read.
  bool coldScan = false;
  // Paging cursor: return only rows STRICTLY OLDER than this position. Accepts
  // a row id ("m0000a1f3") for the indexed range, or a byte cursor
  // "<dayNum>:<offset>" for the cold range. Empty = start at the newest row.
  // The store emits the exact token to pass next (EpiQueryInfo::nextBefore) -
  // no caller, and certainly no model, computes a cursor itself.
  std::string before;

  bool kindVisible(MsgKind k) const {
    if (!haveKind) return true;
    if (k == kind) return true;
    for (MsgKind a : alsoKinds) if (a == k) return true;
    return false;
  }

  bool sessionVisible(const std::string& sid) const {
    if (sessionAllow.empty()) return true;
    for (const auto& a : sessionAllow) if (a == sid) return true;
    return false;
  }
};

// What a query actually reached - so an answer about history can be HONEST
// about its own floor instead of implying "not found" means "never happened".
struct EpiQueryInfo {
  uint32_t searchedToDay = 0;   // oldest epoch-day examined (0 = nothing read)
  bool     olderExists = false; // history remains below that floor
  std::string nextBefore;       // token to pass as MsgQuery::before to continue
  int      coldFiles = 0;       // day-streams opened by the cold pass
  size_t   coldBytes = 0;       // bytes the cold pass read
};

// Case-insensitive, ALL-of-terms text match: the needle is split on whitespace
// and every term must appear. "bilge pump" matches "the Bilge Pump serial" -
// the substring match it replaces missed both the case and the word order.
// Empty needle matches everything.
bool epiTextMatch(const std::string& hay, const std::string& needle);

// Ranked variant for content search: 0 when not every term is present (same
// all-terms gate as epiTextMatch), else a BM25-lite score = summed term
// frequency (no IDF - the file corpus is tiny and hard-capped, so document
// frequency adds cost without ranking value). Higher = more on-topic.
int  textMatchScore(const std::string& hay, const std::string& needle);

// Numeric suffix of a device row id ("m0000a1f3" -> 0xa1f3); 0 when it does not
// parse. Ids are minted monotonically, so the suffix orders rows chronologically.
uint32_t epiIdSuffix(const std::string& id);

// The store interface. The device backs it with the SD append-log; tests + the
// portable reference use InMemoryEpisodicStore below.
class EpisodicStore {
 public:
  virtual ~EpisodicStore() = default;
  virtual void addSession(const EpisodicSession& s) = 0;
  virtual bool setSessionStatus(const std::string& id, const std::string& status) = 0;
  virtual void addMessage(const EpisodicMessage& m) = 0;
  virtual std::vector<EpisodicSession> sessions(const std::string& status = "") const = 0;
  virtual std::vector<EpisodicMessage> query(const MsgQuery& q) const = 0;
  // Same query, plus what it reached. Default: delegate + report nothing, so a
  // store with no cold tier (the in-memory reference) needs no override.
  virtual std::vector<EpisodicMessage> query(const MsgQuery& q, EpiQueryInfo* info) const {
    if (info) *info = EpiQueryInfo();
    return query(q);
  }
  virtual int  messageCount() const = 0;
};

// Portable reference impl (also the device's fallback when no SD is present).
// Bounded: keeps at most `cap` most-recent messages (ring semantics) so a
// host-less device can't OOM; the SD append-log store has no such cap.
class InMemoryEpisodicStore : public EpisodicStore {
 public:
  explicit InMemoryEpisodicStore(int cap = 2000) : cap_(cap) {}
  void addSession(const EpisodicSession& s) override;
  bool setSessionStatus(const std::string& id, const std::string& status) override;
  void addMessage(const EpisodicMessage& m) override;
  std::vector<EpisodicSession> sessions(const std::string& status = "") const override;
  std::vector<EpisodicMessage> query(const MsgQuery& q) const override;
  int messageCount() const override { return (int)msgs_.size(); }

  // Whole-store binary blob for device persistence (LittleFS /data/episodic.bin),
  // mirroring VectorMemory::serialize. Versioned magic; deserialize is tolerant
  // (loads whatever parses cleanly, returns false on truncation/garbage so the
  // caller can log). Round-tripped in host tests. deserialize REPLACES contents.
  std::string serialize() const;
  bool        deserialize(const std::string& blob);

 private:
  int cap_;
  std::vector<EpisodicSession> sessions_;
  std::vector<EpisodicMessage> msgs_;  // insertion order (oldest first)
};

}  // namespace orch
}  // namespace nimbus
