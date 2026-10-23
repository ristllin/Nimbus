#pragma once
// claims - find what a reply says the turn DID, and compare it with what the
// turn recorded (turn_effects.h).
//
// The honesty rule used to be prompt text only, and replies kept stating
// effects that never happened: a deliverable "written and waiting" with no
// file, label figures the caption never gave, sub-agents "started" that never
// reached the queue, a scratchpad "loaded every turn, reboot or not" whose save
// had failed a second earlier. The device holds the evidence before delivery,
// so it now checks.
//
// The scanner is deterministic: a hand-written token matcher over one
// constexpr cue table (in flash), no std::regex, one linear pass, allocations
// only through the working (PSRAM) allocator. Scoping rules, each measured as
// necessary on the 16-18 Oct replies:
//   R1  a negation counts only within 4 tokens before the cue, in the same
//       clause ("haven't logged"); a sentence-wide filter missed "reboot or
//       not" and "so it can never decay". A negative quantifier as the
//       clause's subject negates all of it ("nothing in the logs shows it was
//       saved", "no record shows I saved it");
//   R2  questions, offers, conditionals and futures are not claims ("want me
//       to", "I'll", "if you", "once", "to be saved", "probably"), and an
//       "or" clause without the assistant as subject shares the one before
//       it ("it may have been lost, or saved elsewhere");
//   R3  spawn backing comes from the enqueue result, never the dispatch log
//       (dispatch runs after the turn ends);
//   R4  a scratchpad write backs "noted", and also brings the durability check;
//   R5  text inside quotes or code is skipped (the head often quotes the owner);
//   R6  English only: a reply that is mostly non-ASCII letters is not scanned.
// A send claim about a Telegram delivery is backed only by a send that carries
// Telegram's receipt (turn_effects.h kFxReceipt: the message_id Telegram answered
// with), never by a send tool's own words (22 Oct: "sent to Telegram" backed "a
// voice message is on its way" and nothing arrived). A voice-message claim needs a
// voice message's receipt; a text, document or photo send without a receipt (it was
// only queued) backs no "sent" either. A send that is not a Telegram send (a share
// link) backs it as before.
//
// Cause claims (19 Oct: "your Telegram client is re-sending a corrupt,
// partially-uploaded item", "my vision path is working", none of it on record) are
// a class of their own: a cause about the owner's phone, app or connection, about a
// file being damaged, about the sender repeating himself, or about the assistant's
// own subsystems (it timed out, it is out of credit, it is working) needs a tool
// result or a device record behind it this turn (CauseEvidence). Hedges ("probably",
// "might be", "looks like") do not exempt a cause: a guess about the owner's phone
// is the failure. Questions, offers, conditions and negations still do.
// Precision first: a missed claim costs nothing new, a false one would correct
// a true reply. A claim about another person's action ("the photo you sent"),
// a past reference ("already logged") and an adjective ("your logged runs") are
// not claims of this turn.
//
// Portable, Arduino-free, host-tested (test_orch_claims).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/psram_alloc.h"
#include "nimbus/orch/turn_effects.h"

namespace nimbus {
namespace orch {

enum class ClaimClass : uint8_t {
  Saved = 0,     // logged, saved, noted, stored, recorded
  Deliverable,   // written and waiting, saved as <name>, I've written/drafted
  Spawned,       // started/spawned/launched ... sub-agents
  Sent,          // sent, shared, spoken aloud, reminder set, scheduled
  Changed,       // pinned, switched, updated, changed, turned off
  Durable,       // survives reboot, reboot or not, never decay, every turn
  Checked,       // verified, I checked, looked it up, live data
  Figure,        // a figure credited to a source: (label), per the label, watch said
  Cause,         // a cause: your phone/app/connection, a damaged file, it timed out, it works
  kCount
};

const char* claimClassSlug(ClaimClass c);

struct Claim {
  ClaimClass cls = ClaimClass::Saved;
  uint16_t   cue = 0;      // index of the matched row in the cue table
  uint32_t   begin = 0;    // byte span of the clause (or table row) holding the claim
  uint32_t   end = 0;
  uint8_t    count = 0;    // a number the claim states ("six sub-agents"); 0 = none
};

struct ClaimScan {
  std::vector<Claim, WorkingAllocator<Claim>> claims;
  bool skippedNonEnglish = false;   // R6: the reply was not scanned
};

// Scan a reply (any size; text past kScanMax bytes is not scanned).
static constexpr size_t kScanMax = 16384;
ClaimScan scanClaims(const std::string& reply);

// What the claim check compares a reply against.
struct ClaimEvidence {
  const TurnEffects* fx = nullptr;          // this turn's ledger (pending + recorded)
  const std::string* grounding = nullptr;   // the sources a figure may come from:
                                            // the owner's text and captions, this
                                            // turn's tool outputs, sourced results
  const std::string* recalled = nullptr;    // memories recalled into the prompt: a source
                                            // only for what the owner reported ("your
                                            // watch said"), never for a label figure
  uint32_t causeEvidence = 0;               // CauseEvidence bits this turn's records carry
};

// What this turn's records (tool results, the turn's input block: the owner's text,
// photo labels, device notices) show about causes, as bits. A cause claim is backed
// only by the bit it needs (claims.cpp kCues): "your connection" by a network failure
// on record, "corrupt" by a record that says so. kEvHealthRead comes from the ledger
// (a status or health tool ran), never from text.
enum CauseEvidence : uint32_t {
  kEvNetwork      = 1u << 0,   // a network failure: no answer, timed out, connect failed
  kEvClient       = 1u << 1,   // a record naming the owner's phone, app or client
  kEvCorrupt      = 1u << 2,   // a damaged, partial or undecodable file
  kEvDeviceRepeat = 1u << 3,   // the device repeated a delivery (offset not saved, redelivered)
  kEvFailure      = 1u << 4,   // any failure on record (a photo not read, a refusal, HTTP 4xx/5xx)
  kEvHealthRead   = 1u << 5,   // a status or health read ran this turn
  kEvBilling      = 1u << 6,   // a provider out of credit
  kEvTimeout      = 1u << 7,   // a timeout on record
  kEvImageFail    = 1u << 8,   // a photo that could not be read or fetched
};
// The CauseEvidence bits one record shows (the newest 32 KB of it).
uint32_t causeEvidenceOf(const std::string& record);

struct ClaimGap {
  ClaimClass  cls = ClaimClass::Saved;
  std::string excerpt;     // the claim's clause, at most ~100 bytes, valid UTF-8
  std::string missing;     // Figure: the numbers no source holds ("200, 21, 60")
  uint8_t     variant = 0; // which tail line: plain, RAM-only, not pinned, pin no-op, no voice
                           // receipt, queued, no receipt, and the cause lines
  bool        deviceOnly = false;   // only this device could have done it (a Telegram voice
                                    // message from its bot): a hosted tool never backs it
};

// The numbers in a source text, normalized and space-separated ("20 3 1,500"
// -> "20 3 1500"), each once, at most `cap` bytes: all the figure check reads
// of a source, so a turn keeps this compact form instead of the source text.
std::string figureNumbers(const std::string& text, size_t cap = 4096);

// The gaps between the claims and the evidence, at most one per claim, in reply
// order. No ledger (fx null) means nothing can be checked: no gaps.
std::vector<ClaimGap> claimGaps(const std::string& reply, const ClaimScan& scan,
                                const ClaimEvidence& ev);

// True when `text` (a reply, or a memory about to be written) states a cause that
// `causeEvidence` does not back; *excerpt (may be null) gets the first such clause.
// The apply-time refusal of a memory write that would store an unrecorded cause as a
// fact reads it (19 Oct: "Roy's Telegram client had a stuck outbox" was stopped only
// by the embedding outage).
bool unbackedCause(const std::string& text, uint32_t causeEvidence, std::string* excerpt = nullptr);

// The unsourced-result rail (sub-agent provenance): the turn read a live-data
// sub-agent result that no lookup backed, ran no lookup of its own, and the
// reply does not say the data was not checked. Returns that gap (cls Checked,
// empty excerpt) when it applies.
bool unsourcedResultGap(const std::string& reply, const TurnEffects& fx, ClaimGap* out);
// True when the reply already says its data was not checked ("not checked",
// "unverified", "from memory", "my guess", ...).
bool replyHedgesUnchecked(const std::string& reply);

// Model-facing correction for one in-turn correction round: names each claim and
// the turn's recorded effects, and says to do the work now or rewrite the reply.
std::string claimCorrection(const std::vector<ClaimGap>& gaps, const TurnEffects& fx);

// Owner-facing tail: one calm line per gap class, each on its own paragraph, in
// a fixed class order. "" when there are no gaps.
std::string claimTail(const std::vector<ClaimGap>& gaps);
// The tail line for one class (exposed for tests and for lines shared with other
// rails); `g` supplies the figure numbers.
std::string claimTailLine(const ClaimGap& g);

// One cue row, exposed so the table property tests iterate the single source.
struct ClaimCueInfo {
  ClaimClass  cls;
  const char* phrase;   // lowercase words; '*' ends a prefix word; '#' is any number
  bool        needsSelf;   // counts only with the assistant (or a passive) as subject
  bool        needsFaultWord;   // a cause cue that counts only with a fault word in its sentence
  bool        needsRecordNoun;  // a save cue that counts only with a record noun near ("updated the log")
};
size_t claimCueCount();
ClaimCueInfo claimCue(size_t i);

}  // namespace orch
}  // namespace nimbus
