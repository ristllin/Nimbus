#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/status.h"

// tx/item.h - the transcript item (PRD section 2), the unit the plain engine stores
// before it renders a request. Items are appended and never rewritten. q is a per-chat
// sequence number; order comes from q, never t.
//
// The line (lane RB-T1, lib/core/src/orch_tx_item.cpp): one JSON object, one line,
//   {"k":"u","q":12,"turn":3,"t":1794920520,"x":"...","z":"1a2b3c4d"}\n
// "k" (the kind letter) and "q" (from 1) come first and are always written; then the
// kind's own fields, each only when it differs from its default (h always names its
// class); a list holds at most kMaxListItems values; "z" is last: the crc32 (IEEE) of
// every byte before ',"z":"', as 8 lowercase hex digits. The tail ',"z":"xxxxxxxx"}\n'
// has a fixed width, so a torn tail, a glued line or a flipped byte fails the crc and is
// dropped, never repaired. A line is at most kMaxLineBytes including its newline.
// Strings are stored byte for byte (renderers repair UTF-8; capture never rewrites);
// '"', '\\' and control bytes are escaped, so a line never holds a raw newline.
// Keys (short, per kind):
//   all  k q turn t            u  x tz ch src m parts[{k img|file|voice, ref, mime, b}]
//   t  tr hd pf at ph          a  x rd fin hdl        c  id n args eff rd st
//   i  id n                    r  id n ok cls x hdl b l st
//   k  x rd                    h  x cls (note class)  s  x day thru ext
//   x  stub why sup            d  ids                 e  cause rds tools hr pr pt ot cr cw usd ms
//                                                        ex[[b,ms,st]] ev[] ph mh lp
// Decoding skips keys it does not know and keys another kind writes (the format only
// grows), answers Torn for a framing or crc failure (a cut, a glued line, a flipped byte)
// and InvalidArgument for a whole line it cannot read (k not first, an unknown kind
// letter, no q, a malformed value, a list over its cap, nesting over 64).
//
// This struct is the decoded value type. The RAM arena (arena.h) keeps encoded bytes,
// never one std::string per item.
namespace nimbus {
namespace orch {
namespace tx {

// The 12 kinds, by their on-disk letter. The letters are frozen: they are the "k"
// value of every stored line.
enum class ItemKind : char {
  User = 'u',         // owner message
  TurnOpen = 't',     // turn header: trigger, head, profile, attempt, visible photos
  Assistant = 'a',    // assistant text of one round
  Call = 'c',         // tool call
  Intent = 'i',       // write-ahead intent of an effect tool (crash safety)
  Result = 'r',       // tool result: receipt inline, or bulk by handle
  RoundMeta = 'k',    // opaque round meta (signed thinking of the open turn)
  DeviceNote = 'h',   // a '[device] ...' note the model sees
  Digest = 's',       // one day's digest
  PointerMove = 'x',  // a stub pointer move
  Delivery = 'd',     // what was sent: message ids
  TurnEnd = 'e',      // turn end facts
};

// Every kind once, for property tests over the whole class (AGENTS.md section 3).
inline constexpr ItemKind kAllItemKinds[] = {
    ItemKind::User,      ItemKind::TurnOpen,   ItemKind::Assistant,   ItemKind::Call,
    ItemKind::Intent,    ItemKind::Result,     ItemKind::RoundMeta,   ItemKind::DeviceNote,
    ItemKind::Digest,    ItemKind::PointerMove, ItemKind::Delivery,   ItemKind::TurnEnd,
};
inline constexpr size_t kItemKindCount = sizeof(kAllItemKinds) / sizeof(kAllItemKinds[0]);

inline char kindLetter(ItemKind k) { return static_cast<char>(k); }
inline bool kindFromLetter(char c, ItemKind& out) {
  for (ItemKind k : kAllItemKinds)
    if (kindLetter(k) == c) {
      out = k;
      return true;
    }
  return false;
}

// r.cls: rcpt (a receipt kept whole in tier 1), bulk (tier 2, by handle), out (outside
// content: always rendered as a stub once its own turn closes).
enum class ResultClass : uint8_t { Receipt = 0, Bulk = 1, Outside = 2 };
inline const char* resultClassSlug(ResultClass c) {
  switch (c) {
    case ResultClass::Receipt: return "rcpt";
    case ResultClass::Bulk: return "bulk";
    case ResultClass::Outside: return "out";
  }
  return "out";
}

// h.cls: why the device wrote a note.
enum class NoteClass : uint8_t { Agent = 0, Routine = 1, Notice = 2, Unknown = 3, Limit = 4, Omit = 5, Gap = 6 };
inline const char* noteClassSlug(NoteClass c) {
  switch (c) {
    case NoteClass::Agent: return "agent";
    case NoteClass::Routine: return "routine";
    case NoteClass::Notice: return "notice";
    case NoteClass::Unknown: return "unknown";
    case NoteClass::Limit: return "limit";
    case NoteClass::Omit: return "omit";
    case NoteClass::Gap: return "gap";
  }
  return "unknown";
}

// An attachment of an owner message (u.parts).
enum class PartKind : uint8_t { Image = 0, File = 1, Voice = 2 };
struct Part {
  PartKind kind = PartKind::Image;
  std::string ref;    // photo ref "p<ref8>", or a file path "project/name"
  std::string mime;   // "image/jpeg", "text/csv", ...
  uint32_t bytes = 0;
};

// One exchange of a turn, for the e item's per-exchange sizes and timings.
struct ExchangeFacts {
  uint32_t requestBytes = 0;
  uint32_t ms = 0;
  uint16_t httpStatus = 0;
  // The request by reference (the dossier, lane RB-E3): with the turn's prompt and manifest
  // hashes, what plain::rebuildRequest needs to render this request again byte for byte from
  // the stored items, instead of a copy of the request. bodyHash 0: no reference was recorded
  // and only the three figures above are stored.
  uint64_t fromQ = 0;           // the item range the request rendered: every item the engine
  uint64_t thruQ = 0;           // held, from the oldest resident one to the newest
  uint32_t items = 0;           // how many items that range held
  uint64_t stub = 0;            // the stub pointer
  uint64_t closedThrough = 0;   // the overflow ladder's state (plain::LadderState)
  uint64_t stubThrough = 0;
  uint64_t omitThrough = 0;
  uint32_t maxTokens = 0;       // the output cap the request carried
  bool toolChoiceNone = false;  // a guard's last round
  uint32_t bodyHash = 0;        // fnv1a-32 of its path, body and spliced bytes (never a header)
  std::string effort;           // the reasoning effort it carried ("" = the provider default)
  std::string omission;         // the ladder's omission line ("" = none)
};

// The e item: what the turn cost and why it ended.
struct TurnEndFacts {
  std::string cause;            // "reply" | "limit:rounds" | "provider" | ...
  uint16_t rounds = 0;
  uint16_t tools = 0;
  uint16_t headRequests = 0;
  uint16_t probeRequests = 0;   // truth probe requests, their own column
  uint32_t promptTokens = 0;     // every request of the turn summed, in the provider's own meaning
  uint32_t outputTokens = 0;
  uint32_t cacheReadTokens = 0;
  uint32_t cacheWriteTokens = 0;
  uint32_t microUsd = 0;        // spend in millionths of a USD
  // The whole prompt of the turn's last answered request (orch::TokenUsage::maxRequestIn: input
  // plus cache reads and writes, whatever each provider leaves in or out of its input count);
  // 0 when no request was answered or the line predates it. Compaction's hot trigger reads this,
  // never the summed promptTokens (lane RB-H81, ruling 91 T4-ADV-01).
  uint32_t lastPromptTokens = 0;
  uint32_t ms = 0;
  std::vector<ExchangeFacts> exchanges;
  std::vector<std::string> events;   // change points and guard events, by slug
  uint32_t promptHash = 0;
  uint32_t manifestHash = 0;
};

// The decoded item. Common fields first, then the fields each kind uses; a field a
// kind does not use stays at its default and is not written.
struct Item {
  uint64_t q = 0;                  // per-chat sequence number: the only order
  ItemKind kind = ItemKind::User;
  uint32_t turn = 0;
  uint64_t t = 0;                  // epoch seconds when stored; 0 = clock not set
  std::string x;                   // the text: u, a, h, s; r inline result; k opaque meta

  // u
  int16_t tzMinutes = 0;
  std::string ch;                  // channel slug ("tg", "web", "voice", ...)
  std::vector<uint64_t> src;       // Telegram update ids folded into this message
  uint64_t m = 0;                  // the day-stream row id it mirrors
  std::vector<Part> parts;

  // t
  std::string trigger;             // plain::triggerSlug
  std::string head;                // "host/model"
  std::string profile;             // tx::profileSlug
  uint8_t attempt = 0;
  std::vector<std::string> photos; // visible photo refs

  // a
  uint8_t rd = 0;                  // round
  bool fin = false;                // the turn's final text

  // c, i, r (id and n are shared: the call id and the tool name)
  std::string id;
  std::string n;
  std::string args;                // c: the arguments JSON (or a long-argument stub)
  bool eff = false;                // c: an effect tool
  bool ok = true;                  // r
  ResultClass cls = ResultClass::Receipt;   // r
  std::string handle;              // r bulk, and an a text over the message cap: where the whole
                                   // lives (b/<turn>.txb + offset)
  uint32_t bytes = 0;              // r: payload size
  uint32_t lines = 0;              // r: payload lines
  std::string st;                  // r: the stub text, computed once at capture (<= 400 B); c: the
                                   // arguments with each long string as its stub (tx::shownArgs)

  // h
  NoteClass note = NoteClass::Unknown;

  // s
  std::string day;                 // "YYYY-MM-DD" (the local day)
  uint64_t thru = 0;               // the last q the digest covers
  bool ext = false;                // archived out of the block

  // x
  // The new stub pointer. At or below it only tier-2 bulk renders as its stub (bulk and
  // outside results, string arguments over 1,024 B, pixels, document bodies); receipts,
  // owner and assistant text stay verbatim until compacted (PRD section 2, Ruling 16).
  uint64_t stub = 0;
  std::string why;                 // tx::moveWhySlug
  std::vector<uint64_t> sup;       // superseded result q's

  // d
  std::vector<int64_t> msgIds;

  // e
  TurnEndFacts end;
};

inline constexpr size_t kMaxLineBytes = 60000;      // with its newline; a longer line is refused, never cut
inline constexpr size_t kTier1OwnerText = 12288;    // owner text kept inline
inline constexpr size_t kTier1AssistantText = 12288;
inline constexpr size_t kTier1CallArgs = 1024;      // longer string arguments go to bulk
inline constexpr size_t kTier1Receipt = 2048;       // internal results kept inline
inline constexpr size_t kLongPasteHead = 4096;      // the part of a long paste the message carries

inline constexpr size_t kLineTailBytes = 16;        // ',"z":"xxxxxxxx"}' before the newline
inline constexpr size_t kMaxListItems = 256;        // values in one list field (src, parts, ids, ...)

// Encode one item as one line: a JSON object with the kind's short keys, last key z =
// crc32, ending in '\n'. TooLarge (out empty) when the line would pass kMaxLineBytes;
// InvalidArgument (out empty) for a kind outside the 12, q 0 or a list over its cap.
Status encodeLine(const Item& item, std::string& out);

// Where encodeLineTo writes: the arena writes straight into its block, so an append
// allocates nothing. put is called with consecutive pieces of the line.
class LineSink {
 public:
  virtual void put(const char* data, size_t len) = 0;

 protected:
  ~LineSink() = default;
};
// Encode one item into `sink` and set `lineBytes` to the full line length (newline
// included). The sink receives every byte even when the line is too long: on TooLarge
// (line over kMaxLineBytes) or InvalidArgument the caller discards what it got.
Status encodeLineTo(const Item& item, LineSink& sink, size_t& lineBytes);
// Decode one stored line (with or without its '\n'). Torn when its framing or crc fails:
// the caller drops it. InvalidArgument for a crc-valid line it cannot read (see above).
// `out` is cleared in place first (its strings keep their capacity); on failure its
// contents are unspecified.
Status decodeLine(const char* data, size_t len, Item& out);

// What a stored line says about itself without decoding its strings: the framing and crc
// are checked, k must come first, and only k, q, turn and t are read (other values are
// skipped, not checked: a line checkLine accepts can still fail decodeLine, which only a
// writer bug or a newer format produces). No allocation; used by the restart fill and by
// scans that look for the newest s and x items.
struct LineHeader {
  uint64_t q = 0;
  uint64_t t = 0;
  uint32_t turn = 0;
  ItemKind kind = ItemKind::User;
};
Status checkLine(const char* data, size_t len, LineHeader& out);

// The send-time stamp every rendered message carries (Ruling 11, RB-REC5): orch::day::stamp
// (day_start.h, the one function engine 1 uses too) in brackets, for `epoch` seconds at
// `tzMinutes` east of UTC (the offset valid at that time) and the device's day start:
//   "[2026-11-17 Tue 13:02]"                                at or after the day start
//   "[2026-11-18 Wed 00:30 (day 2026-11-17 until 04:00)]"   before it
//   "[time unknown]"                                        the clock was never set
// The year and the weekday are always there. Writes a NUL-terminated string of at most
// kStampMaxBytes - 1 characters. InvalidArgument (and "") when `cap` is under
// kStampMaxBytes or the offset is beyond +-18 h. No allocation.
inline constexpr size_t kStampMaxBytes = 52;
Status messageStamp(uint64_t epoch, int16_t tzMinutes, uint16_t dayStartMin, char* out, size_t cap);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
