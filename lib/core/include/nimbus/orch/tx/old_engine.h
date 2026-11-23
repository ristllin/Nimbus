#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/compact.h"       // ChatFold
#include "nimbus/orch/episodic.h"      // EpisodicMessage, MsgQuery, EpiQueryInfo
#include "nimbus/orch/notes.h"         // Notebook
#include "nimbus/orch/tx/migrate.h"    // RowSource, DayRow, OldFold

// tx/old_engine.h - what engine 2 reads from the earlier engine at a chat's first start (lane
// E2-MIG). Read only: nothing here writes the episodic store, the fold record or the rules file,
// so the way back to the earlier image finds every byte it wrote.
//
//   EpisodicRowSource  tx::RowSource over the episodic store's forward query (MsgQuery::after,
//                      cold scan on): pages of at most `max` rows, oldest first, one chat. A cold
//                      page the store stops early (its read budget) is continued from the byte
//                      cursor it handed back, so the migration sees every row once, in id order.
//   dayRowOf           one EpisodicMessage as a migration row (the RB-MG1a mapping).
//   oldFoldOf          the fold record copied into tx::OldFold.
//   importRules        the earlier engine's active standing rules into the owner's notes, as
//                      pinned rules, once each (a marker records what was copied).
//
// Engine 2 only (tools/engine/engine2_only.txt). Portable, Arduino-free, host-tested
// (test/test_e2_migration).
namespace nimbus {
namespace orch {
namespace tx {

// "m0000a1f3" -> 0xa1f3 (0: not a row id).
uint64_t rowIdNumber(const std::string& id);
// 0xa1f3 -> "m0000a1f3".
std::string rowIdString(uint64_t n);

// Owner rows: role user (message, image, file, audio, transcript); assistant rows: role assistant
// kind message; tool rows: role tool or kind tool_output; everything else Other. `ours` is the
// "e2" tag. A photo row carries its description as text (photoRef stays empty: engine 2 on the
// device does not read the earlier engine's photo blobs yet).
DayRow dayRowOf(const EpisodicMessage& m);
// The tag list holds `tag` as one whole comma-separated entry.
bool hasTag(const std::string& tags, const char* tag);

OldFold oldFoldOf(const ChatFold& f);

// The fold record's pointer names a row engine 2 wrote (its compaction mirrors its own digests
// into the record, so the earlier image reads them on the way back): it says nothing about the
// earlier engine's rows, which a first start must then not take as summarized. The pointers are
// cleared (the summary stays: digest 0 carries it) and true is returned; the import then reads
// every row of the chat, newest first within its budget. Reads one row; Io when the store did
// not answer.
Status dropMirroredPointer(RowSource& rows, OldFold& fold, bool& dropped);

// Lane E2-FIX2B (finding P5): engine 2 no longer writes the earlier engine's fold record. A build
// before it replaced a chat's record with its mirror (counters 0, the epoch field carrying
// ";m<row>", its own cumulative summary as the text). The caller has shown that the record's
// pointer names a row engine 2 wrote (dropMirroredPointer); the counters alone are also the earlier
// engine's after its own fold, so the text must parse as engine 2's summary (parseSummary). Then
// `out` is `blob` with that one record put back from `copy` (/mem/tx-pre-e2/chatsum.txt, the record
// as the earlier engine left it); every other record stays byte for byte. False, `out` empty:
// nothing to put back (no such record, another shape, or the copy has no record of the chat: a
// record is never removed). Pure.
bool restoreFoldRecord(const std::string& blob, const std::string& copy, const std::string& chatId, std::string& out);
// The same, from a read of the shared record file (lane E2-FIX3, finding R7): `blob` must be the
// file's full `fileSize`, since `out` replaces the whole file; a short read puts nothing back.
bool restoreFoldRecordFrom(const std::string& blob, size_t fileSize, const std::string& copy, const std::string& chatId,
                           std::string& out);

// The store's forward query, called under the binding's lock (the device takes the card lock
// around it).
using EpiQueryFn = std::function<std::vector<EpisodicMessage>(const MsgQuery& q, EpiQueryInfo& info)>;

class EpisodicRowSource : public RowSource {
 public:
  EpisodicRowSource(std::string sessionId, EpiQueryFn query) : sid_(std::move(sessionId)), query_(std::move(query)) {}
  Status read(uint64_t after, uint64_t upTo, size_t max, std::vector<DayRow>& out, bool& end) override;
  Status newest(uint64_t& id) override;
  // Store calls made, and spans the card would not give back (named in the device log).
  uint32_t queries() const { return queries_; }
  const std::vector<std::string>& unreadable() const { return unreadable_; }

  static constexpr uint32_t kMaxQueriesPerRead = 256;   // a read that gets no row this often: Io

 private:
  // One store page from `cursor` into `out`; the cursor after it.
  std::string page(const std::string& cursor, uint64_t after, uint64_t upTo, size_t max, std::vector<DayRow>& out,
                   bool& end);
  std::string sid_;
  EpiQueryFn query_;
  uint64_t lastServed_ = 0;   // the newest row the previous read handed out
  std::string resume_;        // the store's continuation token after it ("" = the row id)
  uint32_t queries_ = 0;
  std::vector<std::string> unreadable_;
};

// ---- the standing rules -----------------------------------------------------------------

// One active rule of the earlier engine (prefs.txt: not pending, moved or retired).
struct OldRule {
  uint32_t id = 0;     // its "p<id>"
  std::string text;
};

struct RulesImportOutcome {
  uint32_t added = 0;      // new pinned rule notes
  uint32_t updated = 0;    // a rule the earlier engine changed, the note untouched on engine 2
  uint32_t kept = 0;       // already copied, or the owner's note on engine 2 left as it is
  uint32_t removed = 0;    // the earlier engine no longer has the rule, the note untouched: removed (P6)
  uint32_t refused = 0;    // the notebook refused it (named in the log; tried again next start)
  std::string firstError;
};

// The note key of a rule ("rule-p3").
std::string ruleNoteKey(uint32_t id);

// Copy the rules into the owner's notes. `marker` is the record of earlier copies (one line per
// rule: "p<id> <fnv1a-32 of the text copied, hex>"); `markerOut` is the record to keep, written
// by the caller after this returns. Per rule:
//   - copied before with the same text: nothing;
//   - the earlier engine changed its text: the note is set to the new text only when it still
//     holds the text that was copied (an edit or a removal on engine 2 is the owner's and stays);
//   - never copied: set as a pinned rule note unless a note with its key is there already.
// A refused set is not recorded, so the next start tries it again. Lane E2-FIX2B (finding P6): a
// copied rule the earlier engine no longer has active (deleted, retired, moved, evicted) takes its
// note away when the note still holds the copied text, and leaves the record (it comes back as new
// if it is active again); a note the owner edited on engine 2 stays, his. A note he removed on
// engine 2 is never put back.
void importRules(const std::vector<OldRule>& rules, const std::string& marker, notes::Notebook& nb,
                 const Principal& owner, std::string& markerOut, RulesImportOutcome& out);

// Lane E2-FIX3 (finding R5, ruling 116 item 2): the earlier engine's active rules as read from its
// file (/data/prefs.txt, the store's own parser), and whether the read is whole: the blob is the
// file's full length, within the store's cap, and every rule line in it parsed.
struct RulesRead {
  std::vector<OldRule> rules;
  bool whole = false;
};
RulesRead oldRulesFrom(const std::string& blob, size_t fileSize);
// importRules over a read: one that is not whole changes nothing and keeps the record as it is,
// since only a whole read can show a rule gone (and a cut line would read as a changed rule).
void importRules(const RulesRead& read, const std::string& marker, notes::Notebook& nb, const Principal& owner,
                 std::string& markerOut, RulesImportOutcome& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
