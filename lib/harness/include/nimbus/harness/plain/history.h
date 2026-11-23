#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/notes.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/tx/search.h"

// plain/history.h - the engine-2 tools search and view (lane E2-SRCH; PRD section 4 tools 7 and 9,
// the subset that makes the device's own texts true: "search finds them" on the omission line, the
// import's notes and the summary; "view id=r<q>" on stubs; "view id=p<ref>" on photos).
//
//   search {query, scope?: history|notes, from?, to?, before?}
//     history (default): this conversation's stored transcript and, for a chat's first conversation,
//     the earlier engine's rows the import did not bring over verbatim (tx/search.h), all words,
//     the whole range in one window when it fits (lane E2-SRCH2), ranked (the phrase, then one line,
//     then two lines; newest first among equals), at most twelve hits, each "id when who: excerpt",
//     then how many more matches and their dates, or how far the reading went, with the cursor
//     (before=...), or that it read to the start;
//     notes: the person's unpinned notes, all words, five at a time (before=n<index>).
//   view {id, offset?, limit?}
//     r<q>: any stored message, note, summary, call or result of this conversation, its whole text
//     paged like read_file (next_offset and the bytes left); m<row>: one earlier-engine row of this
//     chat; p<ref>: a photo of this conversation, whose pixels the request shows inside this result
//     while its turn is open (the wire, viewedPhoto).
//
// The conversation is the turn's: TurnCtx::chatKey and TurnCtx::who, never an argument. A member's
// turn reads only its own chat (who.ns names the chat); a generation /clear started (<chat>_c<n>) has
// no earlier-engine rows, and a cleared conversation's transcript is another key, never read. The
// tools are answered through the turn (answer(), called from the binding's dispatch before the
// registry); the registry entries only carry the declarations and refuse every other route (the LAN,
// a direct dispatch with no turn). A result taken from outside content is outside again
// (ToolOutput::outside), so the rails see it as they saw it first.
//
// Reads only; each read is one short card hold. The result is a few KB at most (twelve excerpts of
// tx::kSearchExcerptBytes; a view page of at most the read_file cap). One line per call goes to the log with the
// card time and bytes.
namespace agent {
namespace plain {

inline constexpr size_t kViewPageMax = 32768;    // read_file's cap
inline constexpr size_t kViewPageFloor = 2048;   // read_file's floor (ruling 95)
inline constexpr size_t kNotesPerPage = 5;

struct HistoryDeps {
  nimbus::orch::tx::TxFiles* files = nullptr;   // the transcripts (/mem/tx)
  // The earlier engine's rows of one chat (generation 0 only); null or a null answer: none.
  std::function<nimbus::orch::tx::OldRows*(const std::string& chatId)> oldRows;
  // The person's unpinned notes; null: scope notes answers that none are kept.
  std::function<bool(const nimbus::orch::Principal& who, std::vector<nimbus::orch::notes::Note>& out)> notes;
  // The local offset at an instant, minutes east of UTC (stamps and the date bounds); null: UTC.
  std::function<int16_t(uint64_t epoch)> tzAt;
  std::function<uint32_t()> nowMs;   // card time; null: bytes only
  // Whether a photo's stored copy can be shown again (the binding's photo hook); null: never.
  std::function<bool(const std::string& ref)> photoReadable;
  // Lane E2-SRCH: a test or a host may lower the card budget of one search call.
  nimbus::orch::tx::SearchLimits limits;
};

class HistoryTools {
 public:
  explicit HistoryTools(HistoryDeps d) : d_(std::move(d)) {}

  // The declarations of search and view on `reg` (plain::toolDecl); their registry handlers refuse
  // in one line: these tools run only through answer().
  void registerInto(nimbus::orch::ToolRegistry& reg) const;
  // True when `call` is search or view: `out` is its answer. False: not one of these tools.
  bool answer(const ToolCall& call, const TurnCtx& turn, ToolOutput& out);

  // What the last call read (the bench's figures).
  const nimbus::orch::tx::ReadStats& lastCard() const { return card_; }

 private:
  ToolOutput search(const ToolCall& call, const TurnCtx& turn, ArduinoJson::JsonObjectConst a);
  bool askOf(const TurnCtx& turn, ArduinoJson::JsonObjectConst a, nimbus::orch::tx::SearchAsk& ask) const;
  std::string answerText(const nimbus::orch::tx::SearchOutcome& o, const nimbus::orch::tx::Status& st) const;
  ToolOutput searchNotes(const TurnCtx& turn, ArduinoJson::JsonObjectConst a, const std::string& query);
  ToolOutput view(const ToolCall& call, const TurnCtx& turn, ArduinoJson::JsonObjectConst a);
  ToolOutput viewItem(const TurnCtx& turn, uint64_t q, size_t offset, size_t limit);
  ToolOutput viewOld(const TurnCtx& turn, uint64_t row, size_t offset, size_t limit);
  ToolOutput viewPhoto(const TurnCtx& turn, const std::string& ref);
  nimbus::orch::tx::OldRows* oldFor(const TurnCtx& turn) const;
  int16_t tz(uint64_t t) const { return d_.tzAt ? d_.tzAt(t) : 0; }

  HistoryDeps d_;
  nimbus::orch::tx::ReadStats card_;
};

// The ref a view result names when it is a photo line (kViewPhoto + "p<ref>" + ...), else "".
// The wire reads it to show that photo's pixels inside the result while its turn is open.
std::string viewedPhoto(const std::string& toolName, const std::string& resultText);

}  // namespace plain
}  // namespace agent
