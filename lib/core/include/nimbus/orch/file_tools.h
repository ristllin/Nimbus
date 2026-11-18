#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "nimbus/orch/file_store.h"      // FileStore - the index, ownership and quotas
#include "nimbus/orch/path_rules.h"      // StorePath, PathRule - the path rule
#include "nimbus/orch/tool_registry.h"   // ToolRegistry, ToolResult, Principal

// file_tools - the engine-2 plain file tools (PRD section 4): read_file, write_file,
// append_file, edit_file and list_files. Each does what its name says:
//   - read_file returns the whole file up to the head's read cap, with exact paging:
//     following next_offset rebuilds the file byte for byte at every cap, and every cut
//     falls on a UTF-8 boundary, after a line end when one sits in the page's second half;
//   - write_file creates a file, or replaces one only when if_exists=replace;
//   - append_file only appends (it never replaces, re-keys or skips a row);
//   - edit_file replaces exact text, and zero matches or several without all=true are errors;
//   - list_files lists newest first, 40 at a time, with the true remainder;
//   - every write returns what changed; an unknown argument is an error naming the
//     accepted ones; nothing refuses on a heuristic; paths follow path_rules.h.
//
// The library holds the rules; a binding (the device's file store, nimbusd's) supplies
// the card through FileToolsIo. Card time is bounded by construction, so no single
// card-lock hold exceeds the PRD's 150 ms gate at the content caps (Ruling 19):
//   - every raw card op runs inside locked(), and each locked() is one hold;
//   - reads go 8 KB per hold, part-file writes 4 KB per hold; an append is one op of at
//     most contentCap bytes; the swap and the index entry update share one commit hold;
//     the index rewrite (persistIndex) has a hold of its own.
// Portable, Arduino-free, host-tested (test_file_tools).
namespace nimbus::orch {

// Bytes per read hold and per part-file write hold.
inline constexpr size_t kFileReadSlice = 8192;
inline constexpr size_t kFileWriteSlice = 4096;
// The smallest read_file limit.
inline constexpr size_t kFileReadMinLimit = 16;

struct FileToolsConfig {
  // write_file content, append_file text, and edit_file old and new, per call: 12,000 B
  // on 120 s hosts, 4,096 B on 60 s hosts (PRD section 4), as write_file's schema states.
  size_t contentCap = 12000;
  // The most one read_file returns (the head's budget for one result).
  size_t readCap = 32768;
  // The largest file edit_file rewrites.
  size_t editMax = 262144;
  // Files up to this size get lines= in read_file and append_file results.
  size_t lineCountMax = 131072;
  // list_files entries per call.
  size_t listMax = 40;
};

// The card seam a binding supplies. The raw ops (readAt .. persistIndex) are called only
// inside locked(); a binding takes its card lock in locked() and never inside a raw op.
class FileToolsIo {
 public:
  virtual ~FileToolsIo() = default;
  // Run fn under the card lock: one hold. False when there is no store (no card); fn
  // did not run.
  virtual bool locked(const std::function<void()>& fn) = 0;
  // The file index. Valid inside locked() only.
  virtual FileStore& index() = 0;
  // Read at most len bytes of the stored file from offset into out (replacing it).
  virtual bool readAt(const std::string& project, const std::string& name, uint64_t offset, size_t len,
                      std::string& out) = 0;
  // Add bytes to the end of the stored file, creating it when it is missing. All or
  // nothing: on false the file is as it was (a torn append is cut back to the old size).
  virtual bool append(const std::string& project, const std::string& name, std::string_view bytes) = 0;
  // Write bytes to this file's part file (first: start it empty). The part file is the
  // file tools' own (never the web upload's), outside the index; one write per path runs
  // at a time (the binding serializes file tool calls, or names the part per call).
  virtual bool partWrite(const std::string& project, const std::string& name, std::string_view bytes,
                         bool first) = 0;
  // Put the finished part file in place of the stored file, with no moment where the
  // file is gone (file_swap.h). False: the stored file is as it was.
  virtual bool partSwap(const std::string& project, const std::string& name) = 0;
  // Remove the part file (after a refusal or a failed write).
  virtual void partDiscard(const std::string& project, const std::string& name) = 0;
  // Write index().dump() durably.
  virtual bool persistIndex() = 0;
  // Room for a file of `addBytes` at project/name; a binding may reclaim regenerable
  // copies here (FileStore::planEviction), each removal counting toward this one hold's
  // time. Default: the index's own rule.
  virtual bool roomFor(const std::string& project, const std::string& name, uint64_t addBytes, std::string& err) {
    return !index().wouldExceed(project, name, addBytes, err);
  }
  // Epoch seconds (0 = clock unknown).
  virtual uint32_t now() = 0;
  // The approved version of skill `id` (skills/<id>.md); false when there is none. Not
  // a card op of this seam: the binding reads its skill store (SkillStore::liveBody).
  virtual bool skillBody(const std::string& id, std::string& out) {
    (void)id;
    (void)out;
    return false;
  }
  // A listed time ("2026-11-17 13:02"); the default is UTC.
  virtual std::string timeText(uint32_t epoch);
};

// What the manifest shows for one tool.
struct FileToolDecl {
  std::string description;
  std::string schema;
};
using FileToolDeclFn = std::function<FileToolDecl(const char* tool)>;

// The arguments a tool accepts, in schema order (empty for a name that is not a file
// tool). A call with any other key is an error naming these; the manifest's schema for
// the tool must offer exactly these properties.
const std::vector<const char*>& fileToolArgs(const char* tool);

class FileTools {
 public:
  FileTools(FileToolsIo& io, const FileToolsConfig& cfg) : io_(io), cfg_(cfg) {}

  ToolResult read(ArduinoJson::JsonObjectConst args, const Principal& who);
  ToolResult write(ArduinoJson::JsonObjectConst args, const Principal& who);
  ToolResult append(ArduinoJson::JsonObjectConst args, const Principal& who);
  ToolResult edit(ArduinoJson::JsonObjectConst args, const Principal& who);
  ToolResult list(ArduinoJson::JsonObjectConst args, const Principal& who);

  // Register the five tools with the description and schema the manifest shows for each
  // (RB-E2's plain::toolDecl and plain::declSchema; contentCap must be the cap that schema
  // states). The registry must not outlive this object.
  void registerInto(ToolRegistry& reg, const FileToolDeclFn& decl);

  const FileToolsConfig& config() const { return cfg_; }

 private:
  struct Target;
  struct Page;
  struct WriteMode;
  bool resolve(ArduinoJson::JsonObjectConst args, Target& t, std::string& err) const;
  bool lookup(Target& t, const Principal& who, int& state);
  bool readSpan(const Target& t, uint64_t from, uint64_t to, std::string& out);
  void countFile(const Target& t, Page& p);
  ToolResult readSkill(const Target& t, size_t offset, size_t limit);
  ToolResult readStored(Target& t, const Principal& who, uint64_t offset, size_t limit, const std::string& note);
  bool writeCheck(Target& t, const Principal& who, const WriteMode& m, uint64_t newBytes, std::string& err);
  bool writePart(const Target& t, const std::string& bytes);
  ToolResult writeBytes(Target& t, const std::string& bytes, const Principal& who, const WriteMode& m);
  ToolResult appendText(Target& t, const std::string& text, const Principal& who);
  ToolResult editText(Target& t, const std::string& oldText, const std::string& newText, bool all,
                      const Principal& who);
  std::string linesNote(const Target& t, uint64_t size);
  std::string persisted();

  FileToolsIo& io_;
  FileToolsConfig cfg_;
};

// ---- pure rules, exposed for the property tests -------------------------------------

// Where a page of `limit` bytes starting at `from` ends inside `window` (the bytes from
// `from`, at most limit of them; atEnd: the window reaches the end of the file). Returns
// the page length: the whole window at the end of the file; else after the last line end
// when it keeps at least half the limit; else the last UTF-8 boundary; never 0.
size_t pageCut(std::string_view window, size_t limit, bool atEnd);
// FNV-1a 64 continued over more bytes: fnvContinue(blob hash of A, B) is the blob hash
// of A+B (blob_store.h), so an append keeps the index hash without reading the file.
uint64_t fnvContinue(uint64_t hash, std::string_view bytes);
inline constexpr uint64_t kFnvBasis = 1469598103934665603ULL;
// The 8 hex a result shows for a content hash ("unknown" for 0).
std::string sha8Of(uint64_t hash);

}  // namespace nimbus::orch
