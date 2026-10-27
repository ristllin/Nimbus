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
// Named sources (CUM-824, 27 Oct: the owner wrote "Its 3.0mm rn" and the reply
// said "Rain gauge at the office roof: 3.0 mm ... the live gauge says it is heavy",
// with no gauge anywhere) are a class too: a reply that credits a figure or a fact
// to a gauge, a sensor, a label, a receipt, a watch, an app, a website (the nouns in
// claims.cpp kCues) needs that source named this turn by the owner's message, a tool
// result, a sub-agent result or a memory search result (SourceEvidence), and so does a
// source credited by name: a citation ("Source: National Rail Enquiries"), a check
// ("checked live on National Rail"), or "according to"/"per" a name in a sentence about
// now (today, live, currently). Every word of the name must be in those texts. The
// history window is not a source: an earlier reply that named a gauge must never
// back the next one. A credit names the source as where the figure came from
// ("the gauge says", "per the label", "from your watch", "on the app", "Rain gauge:
// 3.0 mm"); a generic mention ("a rain gauge would help"), a hypothetical, a credit
// to the owner ("the label you sent") or one labeled earlier is not a claim. The
// correction names the figures the owner gave, so the rewrite credits the owner.
//
// A log claim ("updated the log", "logged it in your food log") is backed only by
// a row that landed in a .csv or .log file (turn_effects.h kFxLogRow: a files.append
// whose result counts at least one row), never by any other file write (CUM-782).
//
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
  Source,        // a named source: the gauge says, per the label, from your watch, on the app
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
  const uint32_t* sources = nullptr;        // SourceEvidence bits this turn names; null =
                                            // the named-source rule is not checked
  const std::string* owner = nullptr;       // the numbers the owner's own message holds
                                            // this turn (figureNumbers form)
  const std::string* sourceWords = nullptr; // the words this turn's sources hold
                                            // (sourceWordsOf form); null = a named source
                                            // ("Source: National Rail") is not checked
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

// The sources a turn's text names (CUM-824), as bits: a source claim is backed only by
// the bit of the noun it credits. Owner devices (watch, scale, tracker) may also come
// from recalled memories, the owner's own reports (kSrcOwnerDevices).
enum SourceEvidence : uint32_t {
  kSrcGauge       = 1u << 0,    // gauge, rain gauge
  kSrcSensor      = 1u << 1,    // sensor
  kSrcThermometer = 1u << 2,    // thermometer, barometer, hygrometer, anemometer
  kSrcStation     = 1u << 3,    // weather station
  kSrcRadar       = 1u << 4,    // radar
  kSrcMeter       = 1u << 5,    // smart meter, meter
  kSrcLabel       = 1u << 6,    // label
  kSrcReceipt     = 1u << 7,    // receipt, invoice
  kSrcMenu        = 1u << 8,    // menu
  kSrcPackage     = 1u << 9,    // package, packaging, packet
  kSrcWatch       = 1u << 10,   // watch, smartwatch
  kSrcScale       = 1u << 11,   // scale
  kSrcTracker     = 1u << 12,   // tracker
  kSrcApp         = 1u << 13,   // app
  kSrcWebsite     = 1u << 14,   // website, site, web page (a link names one too)
  kSrcFeed        = 1u << 15,   // feed, dashboard
  kSrcScreenshot  = 1u << 16,   // screenshot
  kSrcReport      = 1u << 17,   // report
  kSrcAll         = (1u << 18) - 1,   // a photo this turn: it may show any of them
  kSrcOwnerDevices = kSrcWatch | kSrcScale | kSrcTracker,
  // What a web search reads (a sub-agent result with web runs names these).
  kSrcWebRead = kSrcWebsite | kSrcFeed | kSrcRadar | kSrcReport,
  // What a provider-hosted tool the device cannot see (a connector, a hosted search) may
  // have read: not checked when the head had one this turn.
  kSrcHostable = kSrcWebRead | kSrcApp | kSrcReceipt,
};
// The SourceEvidence bits a text names (whole words; a link names a website).
uint32_t sourceEvidenceOf(const std::string& text);
// The words a source text holds, lowercased, each once, space-separated, at most `cap`
// bytes: what a named source in a reply ("Source: National Rail Enquiries", "according to
// the Met Office ... today") is checked against. A link's parts count as words.
// *clipped (may be null) is set when a word did not fit.
std::string sourceWordsOf(const std::string& text, size_t cap = 8192, bool* clipped = nullptr);
// The noun a source bit stands for, with its article ("a gauge", "an app").
std::string sourceNoun(uint32_t bit);


struct ClaimGap {
  ClaimClass  cls = ClaimClass::Saved;
  std::string excerpt;     // the claim's clause, at most ~100 bytes, valid UTF-8
  std::string missing;     // Figure: the numbers no source holds ("200, 21, 60")
  uint8_t     variant = 0; // which tail line: plain, RAM-only, not pinned, pin no-op, no voice
                           // receipt, queued, no receipt, and the cause lines
  bool        deviceOnly = false;   // only this device could have done it (a Telegram voice
                                    // message from its bot): a hosted tool never backs it
  uint32_t    source = 0;           // Source: the SourceEvidence bit of the noun it credits;
                                    // `missing` then holds the figures the owner gave
  std::string name;                 // Source: the named source(s) no source text holds
                                    // ("National Rail Enquiries"), for a named credit
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

// A quote presented as a memory, a message, a note or a record (CUM-824, the eval's
// mistral-large-latest: 'I recalled a memory from 14 October that you asked me to save:
// "Use Notion for the commute brief"', words nothing held). `seen` is everything the
// turn was given (the prompt with its history, standing rules and notes, the input
// block, the tool results), in normalizedWords form; a quote of four words or more
// whose words are not in it, in order, is a gap (cls Source, `name` = the quote). One
// gap per reply. hasRecordQuote is the cheap test that says whether `seen` is needed.
// `memoryOnly` checks only quotes presented as the device's own memory (a turn with
// provider-hosted tools may have read a message or a record the device never saw).
std::string normalizedWords(const std::string& text);
bool hasRecordQuote(const std::string& reply);
bool recordQuoteGap(const std::string& reply, const std::string& seen, ClaimGap* out, bool memoryOnly = false);

// A reason given when the owner asks why (CUM-824, the eval's mistral-large-latest on
// "Why notion?": "because the standing rule I recalled said to check your Notion commute
// dashboard", "because I recalled you had set up a Notion database", none of it on
// record). Each sentence of the reply that gives a reason ("because", "due to", "the
// fallback", "was configured to", "I recalled", "a memory from", "the only connector")
// must be built from words the turn's records hold: `records` is everything but the
// fixed instructions (the history, the standing rules, the input block, the tool
// results), in normalizedWords form. Two or more content words (five letters or more,
// not the question's own, not a stop word) missing is a gap (cls Cause, `missing` = the
// words). Only a sentence about a past sub-agent is checked: one that names a word of
// `subjects` (the spawn ledger's tool, connector and job names, normalized; "" = none,
// nothing checked), so "sourdough rises because wild yeast ferments the flour" is not.
// asksWhy says whether the owner's text asks why.
bool asksWhy(const std::string& ownerText);
// The gap unrecordedReasonGap returns (the correction then carries the spawn ledger).
bool isUnrecordedReason(const ClaimGap& g);
bool unrecordedReasonGap(const std::string& reply, const std::string& records, const std::string& question,
                         const std::string& subjects, ClaimGap* out);

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
  bool        needsCredit;      // a source noun that counts only when the reply credits it
                                // ("the gauge says", "per the label", "Rain gauge: 3.0 mm")
  bool        namesASource;     // credits the name after it ("Source: BBC Weather")
};
size_t claimCueCount();
ClaimCueInfo claimCue(size_t i);

}  // namespace orch
}  // namespace nimbus
