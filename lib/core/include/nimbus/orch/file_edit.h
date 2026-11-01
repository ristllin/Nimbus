#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// file_edit - the portable rules behind files.edit (host-tested; the device
// and nimbusd only move bytes).
//
// The only way to change a stored document used to be a whole-file rewrite:
// files.read of every byte, then artifact.save of every byte - twice over the
// prompt, and a whole-file buffer on the device. files.edit substitutes an
// exact span (a phrase, a whole line's text) in place: the caller streams the
// file through the matcher in bounded chunks, the device writes the substituted
// stream to a temp sibling and renames it over the original - the original is
// untouched until the last byte is written, and nothing is ever held whole in
// memory (the matcher carries at most one chunk plus the unsettled tail).
namespace nimbus::orch {

// One edit call carries at most this much find text and replacement (a line, a
// paragraph - never a file), and replaces at most this many matches (a runaway
// span like "e" on a big file is refused with the count, not applied blindly).
inline constexpr size_t kEditFindMaxBytes = 2048;
inline constexpr size_t kEditReplaceMaxBytes = 4096;
inline constexpr int kEditMaxMatches = 200;

// What the caller asked for. all=false (the default) replaces the FIRST
// `count` matches (count 1 = the surgical default); all=true replaces every
// match (the hard cap above still guards the runaway case).
struct EditPolicy {
  bool all = false;
  int count = 1;
};

// The decision after the file was scanned and before any byte is rewritten.
// ok=false means nothing is written; `err` says why in plain words.
struct EditDecision {
  bool ok = false;
  std::string err;
  size_t matches = 0;    // matches found in the whole file
  int apply = 0;         // matches this call replaces
  int64_t sizeDelta = 0; // (replaceLen - findLen) * apply: the file's growth
};

// Validate the find/replace spans and the policy before anything is opened:
// "" when the edit may proceed to the scan (the same checks decideEdit makes).
std::string validateEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol);

// Validate the spans and the policy against what the scan counted, and decide
// how many matches this call replaces. first-mode replaces min(count,
// matches); all-mode replaces every match but refuses (with the count) when
// the file holds more than kEditMaxMatches.
EditDecision decideEdit(size_t findLen, size_t replaceLen, const EditPolicy& pol,
                        size_t matches);

// The streaming matcher. Feed the file through it in chunks of any size - the
// carries never exceed one chunk plus (find.size() - 1) unsettled bytes, so a
// match that straddles a chunk boundary is found exactly where it lies.
//
//   countOnly: counts matches, emits nothing (the scan pass);
//   replace:   the first `apply` matches are substituted with `replace`;
//              once `apply` is reached the rest copies through unchanged
//              (no further matching), and the substituted text is never
//              rescanned (a replacement cannot match itself).
//
// Overlapping matches are applied left to right: after a substitution the scan
// resumes after the replaced span.
class TextEditStream {
 public:
  TextEditStream(const std::string& find, const std::string& replace, bool countOnly);

  // Consume one chunk of the file. Settled output (everything that can no
  // longer start or extend a match - substituted where the policy applies) is
  // appended to `out`; countOnly leaves `out` alone.
  void feed(const char* data, size_t len, std::string& out);
  // The file ended: scan and emit whatever is left (a partial match at the end
  // of the file is never a match - it copies through).
  void finish(std::string& out);

  size_t found() const { return found_; }     // matches seen so far
  size_t applied() const { return applied_; } // substitutions made so far
  // The unsettled carry (for the caller's own bounds; always < find + chunk).
  size_t carry() const { return carry_.size(); }
  // Replace mode: substitute at most n matches (the decision's `apply`);
  // 0 = no limit.
  void setApplyLimit(size_t n) { applyLimit_ = n; }

 private:
  void scan(std::string& out, bool atEof);

  std::string find_;
  std::string replace_;
  bool countOnly_;
  std::string carry_;
  size_t found_ = 0;
  size_t applied_ = 0;
  size_t applyLimit_ = 0;   // 0 = no limit (countOnly scans the whole file)
  bool done_ = false;       // apply limit reached: the rest copies through
};

}  // namespace nimbus::orch
