#pragma once
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/tool_registry.h"   // ToolRegistry, ToolResult, Principal

// notes - engine 2's notes (PRD section 4 "Notes", lane RB-N1): short 'key: value' lines a
// person keeps, and the `note` tool that sets and removes them.
//
//   - A key is a slug [a-z0-9-]{1,32}; one value per key across both shelves; a value is one
//     line of 1 to 300 B with no control characters. A rule is stored and shown as
//     'key [rule]: value', a fact as 'key: value'.
//   - Pinned notes are in every request (the binding passes pinnedBlock() as
//     RequestFacts::pinnedNotes); unpinned notes are outside the request, found by search
//     (scope=notes, lane RB-N3, through unpinned()).
//   - When the pinned shelf has no room the note is stored unpinned and the result says so;
//     when neither has room the call is an error naming the largest keys; nothing is ever
//     evicted to make room.
//   - A set or remove from a turn that read outside content (Origin::Tainted) or that no
//     person's message started (Origin::Unattended) is kept on the person's pending shelf for
//     the owner's approval and changes no note.
//   - Each person has their own three files (pinned, unpinned, pending), chosen by the binding
//     from the Principal alone (personKey); no argument names a person or a path.
//   - A save never waits on the network: NotesIo has file operations only, so a note is kept
//     on the first call whatever the embeddings or the card are doing (the owner's files live
//     on internal flash; a member's on the card, refused in one line when there is none).
//
// Portable, Arduino-free, host-tested (test/test_notes, nimbusd tests/test_hosted_notes.cpp).
namespace nimbus {
namespace orch {
namespace notes {

inline constexpr size_t kKeyMax = 32;
inline constexpr size_t kValueMax = 300;
inline constexpr size_t kLargestNamed = 3;     // keys a "both full" error names
inline constexpr size_t kFsBlock = 4096;       // the device's LittleFS block size

enum class Kind : uint8_t { Fact = 0, Rule = 1 };
enum class Shelf : uint8_t { Pinned = 0, More = 1, Pending = 2 };
const char* shelfSlug(Shelf s);   // "pinned" | "more" | "pending"

// Who started the turn a call comes from, as the binding knows it from the turn.
enum class Origin : uint8_t {
  Attended = 0,     // a person's own message (or the console): the call runs
  Tainted = 1,      // the turn read outside content: kept for approval
  Unattended = 2,   // a routine, a sub-agent report, a notice, a resume: kept for approval
};

// Bytes each shelf may hold, as the file (every line with its '\n').
struct Caps {
  size_t pinned = 0;
  size_t more = 0;
  size_t pending = 0;
};
// The owner on a device: internal flash. Unpinned is 32,724 B, the most 8 LittleFS blocks hold
// (flashBound below), so all three files plus one atomic copy stay within 80 KB.
inline constexpr Caps kOwnerCaps{4096, 32724, 2048};
// A member on a device: the card.
inline constexpr Caps kMemberCaps{1024, 8192, 1024};
// Every tenant on nimbusd: the volume.
inline constexpr Caps kHostedCaps{2048, 65536, 2048};

// LittleFS blocks a file of `bytes` takes (CTZ skip list: block i >= 1 begins with ctz(i) + 1
// four-byte pointers); 0 for an empty file.
size_t fsBlocks(size_t bytes, size_t block = kFsBlock);
// The most flash a person's notes can take at once, in bytes: their directory's metadata pair
// (two blocks), each shelf full, and one atomic copy of the largest shelf in flight (the
// binding writes one file at a time).
size_t flashBound(const Caps& c, size_t block = kFsBlock);

// The person a Principal is: "owner" for the owner, the chat id for a member ([0-9-], at most
// 24 characters, as Telegram ids are), "" when the Principal names no person the notes can key
// on (nothing is read or written for it).
std::string personKey(const Principal& who);

// The storage seam a binding supplies. Files are chosen by (personKey, shelf) only.
class NotesIo {
 public:
  virtual ~NotesIo() = default;
  // Is this person's storage there (a member's card)? false: every call is refused in one line.
  virtual bool available(const std::string& person) = 0;
  // The whole file; a missing file is true with `out` empty. false: it could not be read.
  virtual bool read(const std::string& person, Shelf s, std::string& out) = 0;
  // Replace the whole file atomically (a reader sees the old or the new file, never a mix);
  // false leaves the old file.
  virtual bool write(const std::string& person, Shelf s, const std::string& bytes) = 0;
  // This person's caps.
  virtual Caps caps(const std::string& person) = 0;
};

struct Note {
  std::string key;
  std::string value;
  Kind kind = Kind::Fact;
};

// One kept call, waiting for the owner.
struct PendingNote {
  std::string id;          // "p1", "p2", ... never reused in the person's pending file
  bool remove = false;     // a remove (else a set)
  int pin = -1;            // a set's pin: 1, 0, or -1 when the call left it out
  Note note;               // a remove carries the key only
  bool kindGiven = true;   // a set's kind was in the call (else an existing note keeps its kind)
  std::string why;         // "outside" | "unattended"
};

// ---- pure rules (exposed for the tests) ---------------------------------------------------
bool validKey(const std::string& key);
// One line, 1 to kValueMax bytes, valid UTF-8, not blank, no control characters (C0, DEL, C1,
// U+2028, U+2029).
bool validValue(const std::string& value);
std::string formatLine(const Note& n);   // "key: value" or "key [rule]: value"
// false for a line that is not a note (it is kept as it is by every rewrite, never dropped).
bool parseLine(const std::string& line, Note& out);
// "1,812"
std::string groupedBytes(size_t n);

// The arguments `note` takes, in schema order.
const std::vector<const char*>& noteArgs();

// Reads the running turn's origin when a call comes in (the registry handler sees only the
// Principal). Null: Attended.
using OriginFn = std::function<Origin()>;

class Notebook {
 public:
  explicit Notebook(NotesIo& io) : io_(io) {}

  // The `note` tool: {op: set|remove, key, value?, kind?: rule|fact, pin?: bool}. A set with pin
  // left out keeps an existing note on its shelf and pins a new one; kind left out keeps an
  // existing note's kind (a new one is a fact). A refused allocation is an error result.
  ToolResult call(ArduinoJson::JsonObjectConst args, const Principal& who, Origin origin);
  // Keep a `note` call for approval without running it (RailsDeps::pending for a `note` call
  // on a tainted turn): ok when kept.
  ToolResult keep(ArduinoJson::JsonObjectConst args, const Principal& who, Origin why);

  // The pinned block for RequestFacts::pinnedNotes: the pinned file's note lines, in file order
  // ("" with none, or when the storage is not there). Never more than the pinned cap.
  std::string pinnedBlock(const Principal& who);
  // The person's unpinned notes (search scope=notes). false: storage not there or unreadable.
  bool unpinned(const Principal& who, std::vector<Note>& out);
  // The person's kept calls, oldest first.
  bool pending(const Principal& who, std::vector<PendingNote>& out);
  // The owner's answer to a kept call: approve drops it from the pending file and runs it as an
  // attended call (a refused run puts it back); reject drops it. Neither checks who answers: the
  // binding offers them to the owner only.
  ToolResult approve(const Principal& who, const std::string& id);
  ToolResult reject(const Principal& who, const std::string& id);

  // Register `note` with the manifest's description and schema (plain::toolDecl("note")).
  // The registry must not outlive this object.
  void registerInto(ToolRegistry& reg, const std::string& description, const std::string& schema,
                    OriginFn origin);

 private:
  struct Shelves;
  struct Request;
  ToolResult run(const Request& r, const std::string& person);
  ToolResult runSet(const Request& r, const std::string& person, Shelves& s);
  ToolResult runRemove(const Request& r, const std::string& person, Shelves& s);
  ToolResult keepRequest(const Request& r, const std::string& person, Origin why);
  bool load(const std::string& person, Shelves& s);
  NotesIo& io_;
};

}  // namespace notes
}  // namespace orch
}  // namespace nimbus
