#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/log.h"

// plain/prefix.h - the frozen system part of a conversation (lane E2-CACHE).
//
// The request is tools, then system [core][skills index + pinned notes][digests + recent
// files], then the messages. The provider caches by prefix, so any byte that changes in the
// system part makes it write the whole conversation again (about 0.2 USD on the owner's head
// at 74k tokens). The rule this file holds: between two requests of one conversation the
// bytes ahead of its last closed turn are identical unless one of these happened:
//   compaction or the overflow path moved a pointer (the digests, the stub or the drop point),
//   the tool set or the core changed (a tool, a gate, the model, the time zone name),
//   the pinned notes, rules or skills changed from outside this conversation,
//   the file list changed from outside this conversation (another chat, a routine, the web),
//   the provider's cache has expired anyway (the last request is older than its lifetime),
//   /clear (a new conversation: a new chat key, a new record).
// A note the head saves, a file it writes, a chat switch, a restart, a pause, the time of day
// and a routine firing in another chat are not on the list: the blocks rendered at the last
// listed event are sent again, byte for byte, read back from the record beside the
// transcript after a chat switch or a restart.
//
// What the head did in the conversation is already in it as its own calls and results, so a
// frozen block that is behind by those changes loses nothing (ruling 81). At the close of a
// turn that ran calls the parts are read again; a difference is taken as the turn's own (the
// seen marks move, the blocks stay) only when (lane E2-FIX6):
//   - every item of the turn was stored (ruling 116: a write that did not land moves nothing);
//   - it is a pure addition: no line the block showed was removed or edited (a removal or an
//     edit always renders afresh at the next open, so nothing stays shown as in force that is
//     not), and a file line that left the list belongs to a file that still exists as it was;
//   - every added note line is one a successful, stored result of this very turn carries (the
//     note tool's result holds the stored line), and every added file is a path a successful
//     call of this turn named.
// Anything else leaves the seen marks as they were, so the next open renders afresh, named.
// The q of the first turn relied on is kept (ownFrom): once the ladder or a full arena leaves
// that turn out of the request, the blocks render afresh, mid-turn included.
//
// The record: <tx dir>/prefix.txt, written whole as prefix.txt.tmp and renamed. It mirrors
// what was sent; it never holds anything the owner stored (a lost record costs one full
// cache write, nothing else).
namespace agent {
namespace plain {

// Why the system blocks were rendered afresh at a turn open (None: the frozen ones went out).
enum class PrefixRender : uint8_t { None = 0, First = 1, Connector = 2, Compaction = 3, Notes = 4, Files = 5, Expired = 6 };
// The change point's slug ("change:<slug>" in the turn's events); "" for None.
const char* prefixRenderSlug(PrefixRender r);

inline constexpr uint32_t kCacheLongS = 3600;    // the 1-hour lifetime (RenderInput::longCache)
inline constexpr uint32_t kCacheShortS = 300;    // the default 5-minute lifetime
inline constexpr uint32_t kCacheExpiryMarginS = 60;   // expired only past lifetime + margin
inline constexpr size_t kPrefixRecordMax = 64 * 1024;   // a record over this is not stored

// What composition gives at a turn open, by part.
struct PrefixNow {
  SystemBlocks blocks;         // composeSystem's output for the facts now
  uint32_t manifestHash = 0;   // the tools array as composed now
  std::string digestText;      // tx::digestBlock's text (moves with compaction)
  std::string files;           // the recent-files list now
  uint64_t stub = 0;           // the stub pointer
  uint64_t dropped = 0;        // the newest q a full arena let go from RAM
  uint64_t omitted = 0;        // the newest q left out of the request (dropped, or the ladder's rung 3)
  uint64_t now = 0;            // epoch seconds (0: no clock)
  uint64_t lastReqAt = 0;      // the conversation's last head request (0: none known)
  uint32_t lifetimeS = kCacheShortS;
};

// The frozen part and what was last seen of the parts it is built from.
struct FrozenPrefix {
  bool valid = false;
  SystemBlocks blocks;         // what every request sends until the next render
  uint32_t manifestHash = 0;
  std::string digestText;
  std::string seenNotes;       // the notes block as last seen (at a render or a turn close)
  std::string seenFiles;       // the recent-files list as last seen
  uint64_t stub = 0;
  uint64_t dropped = 0;
  uint64_t ownFrom = 0;        // the first q of the oldest turn whose own change the blocks lag by (0: none)
};

// The decision, in the order of the list above. Pure.
PrefixRender decidePrefix(const FrozenPrefix& f, const PrefixNow& now);
// f becomes what `now` renders, seen parts included.
void renderPrefix(FrozenPrefix& f, const PrefixNow& now);

// Is `now` the seen block plus only this turn's own additions? Lines are compared whole, in any
// order (a reorder of the same lines is not a change of what is in force). `frozen` and `seen`
// must both be contained in `now`; each further line must be one of `headers` or appear in one
// of `ownTexts` (the turn's successful, stored results).
bool ownNotesAddition(const std::string& frozen, const std::string& seen, const std::string& now,
                      const std::vector<std::string>& ownTexts, const std::vector<std::string>& headers);
// The same for the recent-files list. `listing` is every file the store holds now as (its line in
// the list, its path): a seen line that left the list must still be one of them (it fell out of
// the list's size, the file is there as it was); an added line's path must be in `ownPaths`.
bool ownFilesAddition(const std::string& seen, const std::string& now,
                      const std::vector<std::pair<std::string, std::string>>& listing,
                      const std::vector<std::string>& ownPaths);

// The record's bytes and back. decode: Torn when the shape or the checksum is wrong.
Status encodePrefix(const FrozenPrefix& f, std::string& out);
Status decodePrefix(const std::string& in, FrozenPrefix& out);

// The record beside the transcript of `chatKey`. load: NotFound only when the folder listing
// answered and holds neither the record nor a whole temporary copy; Io when the listing or the
// read failed, or memory was refused (never "absent"); Torn when it reads but does not decode.
// A card's rename removes the old record before it moves the temporary one (CardTxFiles): a cut
// between the two leaves only the temporary file, the newest whole copy, so load takes it when it
// decodes (lane E2-FIX6). save: Io when a write or the rename was refused (the stored record is
// then the earlier one, or the temporary file is left beside it).
Status loadPrefix(nimbus::orch::tx::TxFiles& files, const std::string& chatKey, FrozenPrefix& out);
Status savePrefix(nimbus::orch::tx::TxFiles& files, const std::string& chatKey, const FrozenPrefix& f);

}  // namespace plain
}  // namespace agent
