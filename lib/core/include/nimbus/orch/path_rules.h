#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// path_rules - the engine-2 file tools' path rule and reserved paths (PRD section 4).
//
// A path is exactly the file store's two segments, 'project/name' under /mem/files. The
// rule is a rule, never a guess:
//   - the path splits on '/' into exactly two non-empty segments; a leading '/', an empty
//     segment, '.', '..' or a third segment is an error naming the form;
//   - inside a segment every character is made valid one for one: a Latin-1 letter folds
//     to its ASCII letter (U+00E4 a, U+00DF ss), any other character the store or the card's
//     FAT refuses (space, control, non-ASCII, \ : * ? " < > |) becomes '-', a leading dot
//     becomes '_', trailing dots are dropped (FAT drops them);
//   - a segment longer than its limit (project 24, name 48) keeps its extension and ends
//     in '-' plus 8 hex of its hash, so two long names never collide;
//   - a valid path is never changed, so making valid twice changes nothing.
// The result names the stored path; the tools state it when it differs from the asked one.
//
// Reserved paths (matched without letter case: the card's FAT opens names that way). Each
// is reserved because something other than the file tools writes it, never because of
// what a person keeps in it: every other path, whatever it holds (a log, a table, notes),
// is the person's own file store (ruling 41).
//   skills/<id>.md       the skill store's approved version, read-only (skill_save writes it);
//                        every other name under skills/ is the skill store's too
//   export-pre-v2/*      what the migration to engine 2 exported from the old engine's own
//                        stores (PRD section 9), read-only
//   inbox/*              files sent in a chat, written by the device, readable
//   agents/*             agent reports, written by agent jobs, readable
// Portable, Arduino-free, host-tested (test_path_rules).
namespace nimbus::orch {

inline constexpr size_t kStoreProjectMax = 24;
inline constexpr size_t kStoreNameMax = 48;
// The longest extension kept whole when a name is shortened (".markdown" is 9 with its dot).
inline constexpr size_t kStoreExtMax = 10;

enum class PathError : uint8_t {
  None = 0,
  Empty,        // nothing was asked
  Absolute,     // starts with '/'
  NotTwoParts,  // one segment, three or more, or an empty segment
  DotSegment,   // a segment is '.' or '..'
  NothingLeft,  // a segment had no character the store can keep
};

// A model's path made into the store's two segments.
struct StorePath {
  PathError error = PathError::None;
  std::string project, name;   // valid store segments when ok()
  bool changed = false;        // the stored path differs from the asked one
  bool ok() const { return error == PathError::None; }
  std::string str() const { return project + "/" + name; }
};

// The store path for `asked` (PathError says why there is none).
StorePath storePathFor(std::string_view asked);
// One segment made valid for a store limit ("" when nothing usable is left). Applied to a
// valid segment it returns the segment unchanged.
std::string validSegmentFor(std::string_view segment, size_t maxLen);
// The same one-for-one character mapping, without the length rule (list_files' prefix).
std::string validCharsFor(std::string_view text);

// The reserved families. A path outside them is the person's own file store.
enum class Reserved : uint8_t { None = 0, Skill, Archive, Inbox, Agents };

struct PathRule {
  Reserved kind = Reserved::None;
  bool writable = true;      // the file tools may write it
  const char* owner = "";    // the tool that writes it ("" = none, or the person's own store)
  std::string skillId;       // Reserved::Skill: the skill id ("" when the name is not <id>.md)
};

// The rule for a store path (segments as stored).
PathRule pathRuleFor(std::string_view project, std::string_view name);
// The skill store's id rule (SkillStore::validId): a-z A-Z 0-9 - _, 1 to 23 bytes.
bool skillIdValid(std::string_view id);

}  // namespace nimbus::orch
