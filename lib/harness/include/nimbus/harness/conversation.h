#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"       // EpisodicMessage
#include "nimbus/orch/history_rows.h"   // rowIdAfter - the one row-id order

// conversation - what the head reads as "this conversation", and the /clear
// boundary that ends it (CUM-511).
//
// /clear used to answer "Cleared this conversation" while the next turn still
// carried the previous 12 messages: the RECENT CONVERSATION window is rebuilt
// every turn straight from the chat's episodic history, and nothing marked
// where the owner asked to start over. Now /clear writes a BOUNDARY row into
// the chat's own history and every conversation read stops there. The history
// itself is never edited - it is the owner's record (and memory.episodic still
// searches all of it, like long-term memory recall).
namespace agent {

// The boundary row a /clear leaves in the chat's session: kind=message,
// role=system, this tag - so every conversation query (message kinds) already
// returns it and no extra read is needed to find it.
extern const char kClearBoundaryTag[];   // "ev:clear"
extern const char kClearBoundaryText[];  // the row's text (owner-visible in the history)
bool isClearBoundary(const nimbus::orch::EpisodicMessage& m);

// The live part of a chat's rows (`newestFirst` = MsgQuery order): rows NEWER
// than the newest /clear boundary, minus `inFlightId` (the turn's own user row,
// which arrives as [USER]) and minus captured job-engine deliveries (they render
// in the chat but are not conversation the model reads - see isJobDelivery),
// capped at `maxRows` (0 = no cap). Still newest-first.
std::vector<nimbus::orch::EpisodicMessage> liveConversation(
    const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
    const std::string& inFlightId, size_t maxRows);

// The head's "## RECENT CONVERSATION" block over the live conversation (up to
// kRecentTurnsMax rows, oldest first, newest rows kept whole within capBytes -
// CUM-485 derives capBytes from the head model's window; 3000 B is the floor)
// - "" when nothing is live, so a fresh or just-cleared chat (CUM-511
// boundary) carries no conversation at all.
std::string recentConversationBlock(const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
                                    const std::string& inFlightId,
                                    size_t capBytes = 3000);

// ---- the turn-structured history (histV2) -----------------------------------
// The window above re-renders the newest 12 ROWS every turn: older rows shrink
// to previews as the budget fills, the start slides by one row per message, a
// photo shows twice (its media row and the user row that repeats the
// description), seedless turns leave replies with no prompt, and 200 replayed
// copies of one message fill the whole window. The model below fixes the class:
//   * a TURN is the unit: one user half and one assistant half, whatever rows
//     carried them (media rows join their turn by blob ref, assistant rows by
//     their turn: tag, a scheduled or synthesis turn by its trigger row);
//   * each source item is stored once and rendered once: a media row keeps the
//     text record (a photo's description), the user row keeps what the sender
//     sent with a bare label where the item sat, and the render puts the record
//     back in place;
//   * a closed turn's bytes depend only on its own rows, so the rendered history
//     is append-only between boundaries; the start moves only at a /clear, a fold
//     (its anchor row id) or an overflow shift that drops whole turns from high
//     to low water (hysteresis), never one row per message;
//   * consecutive identical owner messages collapse into one row with a count.
// Every tag below is an additive comma-list item, so an older image reads the
// same rows unchanged (its window simply shows the raw rows).
extern const char kAttTag[];       // "att:" + blob refs joined by '.', on the user row
extern const char kSrcTag[];       // "src:" + source key (user row; a trigger row's source)
extern const char kSentTag[];      // "sent:" + unix seconds the sender sent the item
extern const char kOutTag[];       // "out:" + reply | ask | tts, on an assistant row
extern const char kSkillsTag[];   // "skills:" + ids joined by ',', on an assistant row (F1 honesty trail)
extern const char kTriggerTag[];   // "ev:trigger" - a seedless turn's user half

// The value of the first comma-list item starting with `key` ("" if none).
std::string tagValue(const std::string& tags, const char* key);
// `item` as a whole comma-list item.
bool hasTagItem(const std::string& tags, const char* item);
// "a.b" -> {"a","b"} (att refs); "" -> {}.
std::vector<std::string> splitRefs(const std::string& joined);

// One exchange as the history shows it. Strings are already rendered, so the
// block form and the message form (G1) read the same bytes.
struct HistTurn {
  std::string firstId;     // smallest row id in the turn
  std::string lastId;      // largest row id: the anchor a fold or a shift records
  std::string user;        // the user half: what was sent, records in place
  std::string assistant;   // reply, then the question it asked; "" = none yet
  std::string did;         // "memory, file" / "nothing"; "" = no fx: tag (older rows)
  uint32_t tsHours = 0;    // the user half's hour (day markers)
  uint32_t sentUnix = 0;   // when the sender sent it (sent: tag; 0 = unknown)
  uint32_t replyHours = 0; // the hour of the turn's last assistant row (0 = none)
  uint32_t repeats = 1;    // consecutive identical owner messages folded in (xN)
  size_t rows = 0;         // distinct rows the turn holds (the row budget)
  bool seedless = false;   // a scheduled, wakeup or synthesis turn (no owner message)
};

struct HistoryOptions {
  std::string inFlightId;     // the turn's own user row: it and its media are [USER]
  std::string startAfter;     // rows with an id at or before this are not history
  int32_t utcOffsetMin = 0;   // local day markers and send times
};

// Row ids are "m" + 8 hex digits from one boot-stable counter: compare as numbers.
using nimbus::orch::rowIdAfter;

// Move a short heap string's buffer to at least kSpillBytes, so on the device
// (where allocations under 128 B stay in scarce internal RAM and larger ones go
// to PSRAM) the rows and turns the history holds for a turn cost PSRAM, not
// internal heap. Strings that fit the small-string buffer are left alone.
constexpr size_t kSpillBytes = 128;
void spillToPsram(std::string& s);

// The chat's rows (message and media kinds, newest first, as the window query
// returns them; `repeats`, when given, has one entry per row: how many identical
// consecutive owner rows the reader folded into it) -> the live turns, oldest
// first. Stops at the newest /clear boundary and at `startAfter`; job deliveries
// are not conversation; the in-flight turn is left out.
std::vector<HistTurn> historyTurns(const std::vector<nimbus::orch::EpisodicMessage>& newestFirst,
                                   const HistoryOptions& opt,
                                   const std::vector<uint32_t>* repeats = nullptr);

// Two rows the reader may fold into one (xN): consecutive owner messages with
// the same text and no attachment, sent close together on one day - with both
// send times known, at most kRepeatWindowSec apart on the same local day
// (`utcOffsetMin`; 0 = the UTC day); without them, in the same hour.
constexpr uint32_t kRepeatWindowSec = 120;
bool isRepeatRow(const nimbus::orch::EpisodicMessage& newer,
                 const nimbus::orch::EpisodicMessage& older, int32_t utcOffsetMin = 0);

// One turn as RECENT CONVERSATION lines. `prev` is the turn rendered just before
// it (nullptr for the first): it decides the day marker and whether the item
// arrived late (sent before the previous reply).
std::string renderTurnLines(const HistTurn& t, const HistTurn* prev, int32_t utcOffsetMin);

// The message form (G1 builds provider messages from it): one user and one
// assistant message per turn, strict alternation, the summary pair first.
struct HistMsg {
  bool user = true;
  std::string text;
};
std::vector<HistMsg> renderHistory(const std::vector<HistTurn>& turns, const std::string& summary,
                                   int32_t utcOffsetMin);

// Budgets of the verbatim history: past high water (bytes or rows) the oldest
// whole turns drop until the rest fits low water. The newest kHistoryKeepTurns
// turns always stay (the newest owner message and the last reply).
struct HistoryBudget {
  size_t highBytes = 8192;   // ContextBudget::recentConvBytes
  size_t highRows = 64;
  size_t lowBytes() const { return highBytes / 2; }
  size_t lowRows() const { return highRows / 2; }
};
constexpr size_t kHistoryKeepTurns = 2;
constexpr size_t kHistoryMessageCap = 6144;   // one message's fixed clip (UTF-8 safe)
extern const char kHistoryHeader[];

struct HistoryWindow {
  std::string block;       // "" when nothing is live
  std::string softStart;   // the soft start after this call (forward only)
  bool shifted = false;    // the start moved now: rows are hidden until a fold
  size_t bytes = 0;        // rendered turn bytes kept
  // The kept turns themselves (R3/G1: renderHistory turns them into the
  // history messages the providers send as items).
  std::vector<HistTurn> kept;
};
// Fit `turns` (already after `softStart`) into the budget, oldest first.
HistoryWindow fitHistory(const std::vector<HistTurn>& turns, const std::string& softStart,
                         const HistoryBudget& b, int32_t utcOffsetMin);

// The fold's slice (H3): of the turns after the previous anchor, the oldest
// ones beyond the newest `keepBytes` of verbatim history, rendered oldest first
// up to `capBytes`. `thru` is the last row the digest covers (the new anchor).
// Empty when there is nothing new worth a paid summary: fewer than
// kFoldMinNewBytes beyond the kept tail (L19-26: a flood of one repeated line
// collapses to one row and never folds). A manual /compact folds every turn,
// whatever its size.
constexpr size_t kFoldMinNewBytes = 1024;
struct FoldSlice {
  std::string digest;
  std::string thru;
  size_t turns = 0;
};
FoldSlice foldSlice(const std::vector<HistTurn>& turns, size_t keepBytes, size_t capBytes,
                    bool manual, int32_t utcOffsetMin);

// Consecutive identical owner rows (newest first) as one row with " (xN)" - the
// fold input of a chat that has no turn-structured history yet.
std::vector<nimbus::orch::EpisodicMessage> collapseRepeats(
    const std::vector<nimbus::orch::EpisodicMessage>& newestFirst);

}  // namespace agent
