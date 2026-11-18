#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/orch/tx/status.h"

// tx/item.h - the transcript item (PRD section 2), the unit the plain engine stores
// before it renders a request. On disk an item is one JSON object per line, short
// keys, at most kMaxLineBytes, last key z = crc32 of the bytes before it, so a torn
// tail is dropped. q is a per-chat sequence number; order comes from q, never t.
//
// Seam declared by lane RB-B0a; the codec body (orch_tx_item.cpp) is lane RB-T1's.
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
};

// The e item: what the turn cost and why it ended.
struct TurnEndFacts {
  std::string cause;            // "reply" | "limit:rounds" | "provider" | ...
  uint16_t rounds = 0;
  uint16_t tools = 0;
  uint16_t headRequests = 0;
  uint16_t probeRequests = 0;   // truth probe requests, their own column
  uint32_t promptTokens = 0;
  uint32_t outputTokens = 0;
  uint32_t cacheReadTokens = 0;
  uint32_t cacheWriteTokens = 0;
  uint32_t microUsd = 0;        // spend in millionths of a USD
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
  std::string handle;              // r bulk: where the payload lives (b/<turn>.txb + offset)
  uint32_t bytes = 0;              // r: payload size
  uint32_t lines = 0;              // r: payload lines
  std::string st;                  // r: the stub text, computed once at capture (<= 400 B)

  // h
  NoteClass note = NoteClass::Unknown;

  // s
  std::string day;                 // "YYYY-MM-DD" (local ledger day)
  uint64_t thru = 0;               // the last q the digest covers
  bool ext = false;                // archived out of the block

  // x
  uint64_t stub = 0;               // the new pointer: items at or below it render as stubs
  std::string why;                 // tx::moveWhySlug
  std::vector<uint64_t> sup;       // superseded result q's

  // d
  std::vector<int64_t> msgIds;

  // e
  TurnEndFacts end;
};

inline constexpr size_t kMaxLineBytes = 60000;      // a longer line is refused, never cut
inline constexpr size_t kTier1OwnerText = 12288;    // owner text kept inline
inline constexpr size_t kTier1AssistantText = 12288;
inline constexpr size_t kTier1CallArgs = 1024;      // longer string arguments go to bulk
inline constexpr size_t kTier1Receipt = 2048;       // internal results kept inline
inline constexpr size_t kTier1LedgerReceipt = 4096; // ledger tools: a 15-row day keeps its ids
inline constexpr size_t kLongPasteHead = 4096;      // the part of a long paste the message carries

// Encode one item as one line: a JSON object with the kind's short keys, last key z =
// crc32, ending in '\n'. TooLarge when the line would pass kMaxLineBytes.
Status encodeLine(const Item& item, std::string& out);
// Decode one stored line (with or without its '\n'). Torn when the crc does not match or
// the object does not close: the caller drops it.
Status decodeLine(const char* data, size_t len, Item& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
