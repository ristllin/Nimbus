#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/item.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/status.h"

// tx/migrate.h - the conversation's move into /mem/tx and the re-entry rule (PRD section 9,
// "v6 stores" and "Going back and forward again"). Bodies: lane RB-MG1a
// (lib/core/src/orch_tx_migrate.cpp). Engine 2 only (tools/engine/engine2_only.txt).
//
// What a migration makes of one chat (the old store is only read, never written):
//   rows at or below the old summary's pointer (thru)
//       -> digest 0: one s item rebuilt by one small-model call (the binding's Digest0Fn: the
//          compaction driver's planSlice and summarizer text, lib/harness plain/compact.h
//          digest0Asker), accepted only when it holds no path-like token, no figure copied from
//          a tool result (a figure in a tool row of those rows that no owner row holds) and at
//          most kDigestMaxBytes per day covered. It covers one anchor note (an h item naming the
//          earlier conversation), so the block shows the digest and not the note.
//       -> otherwise (digest 0 unset, refused or not accepted) the old summary itself is carried
//          as digest 0 (ruling 84 item 6, lane RB-H81): verbatim, at most kOldSummaryMaxBytes
//          (v5.1.2's summary cap), fenced and dated as the earlier engine's summary whose paths
//          and totals may be stale, in digest parts of its day that fit the full digest block
//          (each part covers the anchor and the parts before it). It is also exported to
//          export-pre-v2/summary-<day>-<chatKey>.md (ExportFn), an extra, never a replacement.
//   rows after the pointer (the rows between the two old pointers first, then the rest)
//       -> one import batch: one device note, then the owner and assistant rows as text
//          items (u with m = the row id, a), one turn per owner row, closed by an e item;
//          tool, trace and log rows are not replayed. The newest rows that fit importBudget
//          (per head: importBudgetFor, at least the head's full due mark) are imported; the older ones are named in the same note ("The M earlier messages
//          before <t> are not shown: search finds them"). Imported rows between the pointers
//          are the oldest after thru, so the compaction driver folds them first.
//
// The re-entry rule: /mem/tx/<chatKey>/migrated.json records the newest row a migration
// accounted for (hi). At every engine-2 start migrate() runs again; with a marker it is a
// re-entry: the rows after hi that engine 2 did not write (DayRow::ours, the "e2" tag) are
// imported as one more batch behind one note. Nothing at or below hi is ever imported again,
// and a start with nothing new writes nothing.
//
// Resumable at every step. Each step is planned into the marker (written by temp and rename,
// crc-checked) before its first item is appended; a batch's items are a pure function of its
// plan and the rows, so after a reset the stored lines from the plan's start are compared
// with the regenerated ones, line for line, and the batch goes on after the last that
// matches. A refused append stops the step (NotStored) with nothing half-done; the next call
// goes on. Digest 0 is asked at most kDigest0Tries times in all (the count is in the marker
// before each ask); the accepted text is in the marker before its item is stored, so a reset
// never asks twice for one answer.
//
// Contract for the binding (RB-MG1b, RB-I1, RB-I2): call migrate() for a chat before its
// engine opens the log, and let the engine store items for that chat only after it answered
// Done (an engine-2 start with no card runs RAM-only, and the next start migrates first).
// Rows engine 2 writes to the day streams carry the tag "e2" and come back with ours set.
// After Done the engine boots the chat as usual (TxLog::open, fill, loadDigests).
//
// Memory: one page of kRowPage rows, the plan and one encoded line at a time; no buffer
// grows with the store (a 6,000-row month is read in pages). Every entry point turns a
// refused allocation into Err::TooLarge with the marker and the log consistent (ruling 30):
// the next call resumes.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr uint32_t kDigest0Tries = 2;          // asks of digest 0, every start together
inline constexpr size_t kOldSummaryMaxBytes = 6144;   // v5.1.2's summary cap: the most carried as digest 0
inline constexpr size_t kRowPage = 64;                // rows per RowSource::read
inline constexpr size_t kMarkerMaxBytes = 8192;       // migrated.json with its crc tail
inline constexpr const char* kExportDir = "export-pre-v2";

// The device-written lines of a migration (model-facing: catalogued by
// tools/release_gate/gen_model_text_catalog.py, E2_EXTRA_SOURCES, class device_notes).
namespace migrate_text {
inline constexpr char kDevice[] = "[device] ";
inline constexpr char kEarlierThrough[] = "The conversation through ";
inline constexpr char kEarlierUndated[] = "The earlier conversation";
inline constexpr char kEarlierKept[] = " was kept by the earlier engine; search finds it.";
inline constexpr char kSummarySaved[] = " Its old summary is saved in ";
inline constexpr char kSummaryNotSaved[] = " Its old summary was not saved.";
inline constexpr char kEnd[] = ".";
inline constexpr char kOneTurn[] = " turn";
inline constexpr char kTurns[] = " turns";
inline constexpr char kBetween[] = " between ";
inline constexpr char kAnd[] = " and ";
inline constexpr char kAnswered[] = " answered by the earlier engine; their tool work is not shown.";
inline constexpr char kWas[] = " was";
inline constexpr char kWere[] = " were";
inline constexpr char kNamedThe[] = " The ";
inline constexpr char kNamedOne[] = " earlier message";
inline constexpr char kNamedMany[] = " earlier messages";
inline constexpr char kNamedBefore[] = " before ";
inline constexpr char kNamedNotShown[] = " not shown: search finds them.";
inline constexpr char kIs[] = " is";
inline constexpr char kAre[] = " are";
// The fence of the old summary carried as digest 0 (RB-H81, ruling 84 item 6).
inline constexpr char kOldOpen[] = "[the earlier engine's summary, as of ";
inline constexpr char kOldStale[] = "; its file paths and totals may be out of date] ";
inline constexpr char kOldMore[] = "[the earlier engine's summary, continued] ";
inline constexpr char kOldClose[] = " [end of the earlier engine's summary]";
inline constexpr char kOldCut[] = " [the earlier engine's summary goes on in ";
inline constexpr char kOldCutEnd[] = "]";
}  // namespace migrate_text

// ---- the old store, as the binding reads it ---------------------------------------------

enum class RowRole : uint8_t { Owner = 0, Assistant = 1, Tool = 2, Other = 3 };

// One day-stream row of this chat (EpisodicMessage): owner rows are role user (message, image,
// audio, transcript), assistant rows role assistant kind message; tool output rows are Tool;
// raw model responses, system and log rows are Other.
struct DayRow {
  uint64_t id = 0;        // the row id's number ("m0000a1f3" -> 0xa1f3); rises with time
  uint64_t t = 0;         // epoch seconds (the day stream keeps hours: tsHours * 3600); 0 = unknown
  RowRole role = RowRole::Other;
  bool photo = false;     // an image row: its text is the photo's description
  // The photo's ref ("p<ref8>", the content-hash blob) when the binding knows it (RB-H81): the row
  // becomes an owner message with the photo as its part and the description as the part's note,
  // so the later-turn stub carries it with a view pointer (ruling 84 item 5).
  std::string photoRef;
  bool ours = false;      // engine 2 wrote it (tag "e2"): never imported
  std::string text;
};

// The day streams of one chat, read forward (the store's `after` query). Read only.
class RowSource {
 public:
  virtual ~RowSource() = default;
  // At most `max` rows with after < id <= upTo (upTo 0: no bound), oldest first; `end` is true
  // when no row of that range follows them. Io when the store did not answer.
  virtual Status read(uint64_t after, uint64_t upTo, size_t max, std::vector<DayRow>& out, bool& end) = 0;
  // The newest row id of this chat (0: none).
  virtual Status newest(uint64_t& id) = 0;
};

// The old engine's summary record of this chat (FoldStore, /data/chatsum.txt), copied.
struct OldFold {
  std::string summary;      // ChatFold::summary ("" = none)
  uint64_t thru = 0;        // ChatFold::thru as a number: the newest row it covers (0 = none)
  uint64_t shift = 0;       // ChatFold::softStart as a number: the overflow shift (0 = none)
  uint64_t foldEpoch = 0;   // ChatFold::lastFoldEpoch
};

// ---- digest 0 ---------------------------------------------------------------------------

struct Digest0Ask {
  std::string summary;      // the old summary
  uint64_t at = 0;          // the newest row it covers (else the fold's time): the digest's day
  uint64_t now = 0;
  int16_t tzMinutes = 0;
  uint16_t dayStartMin = 240;
  uint32_t attempt = 0;     // 0, 1, ...: the binding's ladder rung
};
struct Digest0Answer {
  std::string summary;      // the model's whole answer
  std::string text;         // the digest text the compaction driver would store from it
  std::string day;          // "YYYY-MM-DD"
};
// One small-model call. Not ok: the answer was not taken (the driver's rule: cut, thin, a
// failed exchange); the migration counts the try.
using Digest0Fn = std::function<Status(const Digest0Ask&, Digest0Answer&)>;
// Write a file into the owner's file store (replace); false when refused.
using ExportFn = std::function<bool(const std::string& path, const std::string& text)>;

// Why digest 0 was not accepted (slugs on the outcome and in the dry run).
enum class Digest0Verdict : uint8_t { Accepted = 0, NoSummary, Asked, Size, Path, Figure, Text };
const char* digest0VerdictSlug(Digest0Verdict v);

// The acceptance rule beyond the driver's own (the driver's: not cut, not thin, a valid digest
// text): no path-like token in `summary`, at most kDigestMaxBytes per covered day, and no
// figure of `summary` that `toolFigure` says came from a tool result. Pure.
Digest0Verdict checkDigest0(const std::string& summary, uint32_t daysCovered,
                            const std::function<bool(const std::string& figure)>& toolFigure);
// The figures of a text: digit runs with their decimal and group separators ("1,450", "12.5"),
// two characters or more, leaving out the parts of a date or a clock time ("2026-11-17",
// "13:02", a number next to a month name, a year 1900 to 2099). Pure.
std::vector<std::string> figuresOf(const std::string& text);
// A token that names a file or a place in one: a slash or backslash between two
// characters ("notes/boat.md", "/data", "C:\x"), a URL, or a name with an extension of 2 to 4
// letters or digits ("boat.md", "photo.jpeg"). Pure.
bool hasPathLikeToken(const std::string& text);

// ---- the migration ----------------------------------------------------------------------

struct MigrateDeps {
  TxFiles* files = nullptr;
  std::string chatKey;
  RowSource* rows = nullptr;
  OldFold fold;
  Digest0Fn digest0;         // unset: digest 0 is not asked (the omission line stands in)
  ExportFn exportText;       // unset or refused: the note says the old summary was not saved
  uint64_t now = 0;          // epoch seconds (0: the clock is not set)
  int16_t tzMinutes = 0;
  uint16_t dayStartMin = 240;
  // The chat's head (RB-H81): sizes the import. Required unless importBudget is set.
  HeadKey head;
  // Stored line bytes of the imported u and a items; 0: importBudgetFor(head, the arena), never a
  // fixed figure (owner ruling 81).
  size_t importBudget = 0;
  const TxArena* arena = nullptr;   // RAM items not stored yet: every q stays above them
};

// The import budget of a head (owner ruling 81, ruling 83 item 8): its full due mark H
// (budgetFor(head, Full).marks.high), clamped only by the arena the restart fill reads it into,
// less a quarter kept for compaction's room (a fit, stated: at 589,824 B that is 442,368 B, over
// every head's H today). 0 when the head is not named.
size_t importBudgetFor(const HeadKey& head, size_t arenaBytes);

enum class MigrationStep : uint8_t { NotStarted = 0, Digest0 = 1, Rows = 2, Done = 3 };
const char* migrationStepSlug(MigrationStep s);

struct MigrationOutcome {
  MigrationStep step = MigrationStep::NotStarted;   // Done when this call finished
  bool reentry = false;            // a marker was there: only rows since were looked at
  bool digest0 = false;            // digest 0 is stored (a rewrite, or the old summary carried)
  bool carried = false;            // digest 0 is the old summary itself, fenced (no rewrite taken)
  Digest0Verdict verdict = Digest0Verdict::NoSummary;
  bool exported = false;           // the old summary went to its export file
  uint32_t asks = 0;               // digest-0 calls this call made
  uint32_t turns = 0;              // turns imported by this call
  uint32_t imported = 0;           // rows imported by this call
  uint32_t named = 0;              // rows named in a note by this call
  uint32_t items = 0;              // items appended by this call
  bool overtaken = false;          // a batch found other items where its own belonged
  uint64_t hi = 0;                 // the newest row accounted for
};

// Run every step that is left (a fresh migration, a resumed one, or a re-entry). Ok with
// out.step Done; NotStored or Io when the storage refused (the next call goes on); TooLarge
// for a refused allocation; InvalidArgument for missing deps or a bad chat key.
Status migrate(const MigrateDeps& deps, MigrationOutcome& out);

// The dry-run report: counts and anomaly classes, never row content. Writes nothing and
// asks nothing.
struct MigrationReport {
  bool reentry = false;
  uint32_t rows = 0;               // rows of the chat (every role)
  uint32_t conversation = 0;       // owner and assistant rows
  uint32_t toolRows = 0;           // tool, trace and log rows (not replayed)
  uint32_t days = 0;               // local days the rows span
  uint32_t covered = 0;            // conversation rows at or below the old pointer
  uint32_t betweenPointers = 0;    // conversation rows between the two old pointers
  uint32_t toImport = 0;           // conversation rows the next batch imports
  uint32_t toName = 0;             // conversation rows it names instead (over the budget)
  uint32_t ours = 0;               // rows engine 2 wrote
  uint32_t anomalies = 0;
  std::vector<std::string> anomalyClasses;   // slugs, each once
};
Status dryRun(const MigrateDeps& deps, MigrationReport& out);

// True when /mem/tx/<chatKey> holds a finished migration (or items without a marker): the
// next start is a re-entry.
Status isReentry(TxFiles& files, const std::string& chatKey, bool& out);

// ---- the import batch, pure (also used by the dry run and the tests) ----------------------

// One batch's plan: rows lo < id <= hi that are not ours, the ones up to `cut` named.
struct ImportPlan {
  uint64_t q0 = 0;            // q of its note
  uint32_t turnBase = 0;      // its turns are turnBase + 1, ...
  uint64_t lo = 0, cut = 0, hi = 0;
  uint32_t turns = 0;         // imported turns
  uint32_t imported = 0;      // imported rows
  uint64_t firstT = 0, lastT = 0;   // first and last imported row
  uint32_t named = 0;         // named rows (lo < id <= cut)
  uint64_t namedLastT = 0;    // the newest named row (0: none or no clock)
  int16_t tzMinutes = 0;
};
// The batch's note text (the PRD's line, and the named rows when there are any).
std::string importNote(const ImportPlan& plan);
// The items of a batch from its rows (in id order; rows outside (cut, hi], ours, tool and
// other rows are skipped): `emit` gets each item with the row id it stands for (0 for the
// note and the e items); stop by returning false. Deterministic: the same plan and rows give
// the same items byte for byte.
class BatchBuilder {
 public:
  using Emit = std::function<bool(const Item&, uint64_t row)>;
  explicit BatchBuilder(const ImportPlan& plan) : plan_(plan) {}
  // Items for the next rows; `last` after the final page closes the open turn.
  Status feed(const std::vector<DayRow>& rows, bool last, const Emit& emit);
  uint64_t nextQ() const { return q_; }

 private:
  bool put(const Item& it, uint64_t row, const Emit& emit);
  bool closeTurn(const Emit& emit);
  bool note(const Emit& emit);
  bool row(const DayRow& r, const Emit& emit);
  const ImportPlan& plan_;
  uint64_t q_ = 0;
  uint32_t turn_ = 0;
  bool open_ = false;
  bool noted_ = false;
  uint64_t lastT_ = 0;
  bool stopped_ = false;
};

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
