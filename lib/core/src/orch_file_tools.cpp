#include "nimbus/orch/file_tools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "nimbus/orch/rbac.h"   // effectiveQuota - the per-person byte limit
#include "nimbus/utf8.h"        // utf8Lead, utf8CapLen - cuts on character boundaries

namespace nimbus::orch {

// ---- model-facing text --------------------------------------------------------------
// Every result and error string a model reads from these tools, in one place, until they
// move into lane RB-E2's lib/harness/include/nimbus/harness/plain/texts.h (the
// descriptions and schemas are already there: the binding registers with them). "{}"
// marks where fill() puts a value. Facts only: a result never tells the model what to do.
namespace texts {
namespace tool_results {
// errors
const char kArgs[] = "{} takes {}; not read: {}";
const char kMissingArg[] = "{} is missing; {} takes {}";
const char kNotString[] = "{} is not a string";
const char kNotCount[] = "{} is not a whole number, 0 or more";
const char kNotBool[] = "{} is not true or false";
const char kNotLine[] = "line is not a whole number, 1 or more";
const char kIfExists[] = "if_exists is fail or replace";
const char kPath[] = "path \"{}\" is not project/name (two parts, no leading /, no . or ..)";
const char kNoCaller[] = "no file store for this caller";
const char kNoCard[] = "no file store (no card)";
const char kCardRead[] = "card read failed";
const char kCardWrite[] = "card write failed; nothing changed";
const char kNoFile[] = "no file {}";
const char kNoSkill[] = "no approved skill {}";
const char kReadOnly[] = "{} is read-only here: {}";
const char kOwnerSkill[] = "skill_save changes skills (group skills)";
const char kOwnerArchive[] = "an export the migration wrote; no tool writes it";
const char kOwnerInbox[] = "the device writes files sent in a chat";
const char kOwnerAgents[] = "agent jobs (agent_start) write agent reports";
const char kOthers[] = "{} belongs to someone else here";
const char kExists[] = "{} exists ({} B, sha8={}); if_exists=replace replaces it";
const char kTooLong[] = "{} is {} B; a call here takes at most {} B";
const char kTooLongMore[] = "; append_file adds more";
const char kQuota[] = "{} B of files would pass your {} B";
const char kRoom[] = "file store full: {}";
const char kPastEnd[] = "offset {} is past the end (size={})";
const char kNotText[] = "{} is not text ({})";
const char kPdf[] = "{} is a PDF; its text is not readable yet";
const char kBinary[] = "binary data";
const char kEmptyText[] = "text is empty";
const char kEditTooBig[] = "{} is {} B; edit_file takes files up to {} B";
const char kChanged[] = "{} changed during the edit; nothing written";
const char kAfter[] = "after: {} is not listed";
// results
const char kReadHead[] = "{} size={} lines={} sha8={} bytes {}-{}";
const char kTornNote[] = "; {}";
const char kLimitNote[] = " (limit {} B here)";
const char kAskedNote[] = " (asked \"{}\")";
const char kSkillNote[] = " (approved skill)";
const char kNextOffset[] = "\nnext_offset={}";
const char kWrote[] = "wrote {} size={} sha8={}";
const char kReplaced[] = "; replaced {} B";
const char kNewFile[] = "; new file";
const char kAskedSemi[] = "; asked \"{}\"";
const char kAppended[] = "appended {} B to {}; file is {} B";
const char kLines[] = ", {} lines";
const char kNoNewline[] = "; ends without a newline";
const char kIndexNote[] = "; file index not saved";
const char kListLine[] = "{} {} B {}";
const char kShared[] = " shared";
const char kMore[] = "more: {}";
const char kMoreAfter[] = "more: {} (after={})";
const char kNoFiles[] = "no files";
const char kNoFilesUnder[] = "no files under {}";
const char kTimeUnknown[] = "time?";
}  // namespace tool_results
}  // namespace texts

namespace {

namespace R = texts::tool_results;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;

const std::vector<const char*> kReadArgs = {"path", "offset", "limit", "column", "from", "to"};
const std::vector<const char*> kWriteArgs = {"path", "content", "if_exists"};
const std::vector<const char*> kAppendArgs = {"path", "text"};
const std::vector<const char*> kEditArgs = {"path", "old", "new", "all", "line"};
const std::vector<const char*> kListArgs = {"prefix", "after"};

// A template with each "{}" replaced by the next value.
std::string fill(const char* tpl, std::initializer_list<std::string> vals) {
  std::string out;
  auto it = vals.begin();
  for (const char* p = tpl; *p; ++p) {
    if (p[0] == '{' && p[1] == '}' && it != vals.end()) {
      out += *it++;
      ++p;
    } else {
      out += *p;
    }
  }
  return out;
}

std::string num(uint64_t v) { return std::to_string(v); }

std::string joined(const std::vector<const char*>& names, const char* sep) {
  std::string out;
  for (const char* n : names) out += (out.empty() ? "" : sep) + std::string(n);
  return out;
}

// What the model asked, as a result shows it: no control bytes or quotes, at most 96 B.
std::string shown(std::string_view s) {
  std::string out(s.substr(0, size_t(nimbus::utf8CapLen(s.data(), int(std::min<size_t>(s.size(), 4096)), 96))));
  for (char& c : out)
    if ((unsigned char)c < 0x20 || c == '"') c = '?';
  return out;
}

bool onlyKnown(JsonObjectConst a, const char* tool, const std::vector<const char*>& accepted, std::string& err) {
  std::string unknown;
  for (auto kv : a) {
    const char* k = kv.key().c_str();
    const bool known = std::any_of(accepted.begin(), accepted.end(), [k](const char* n) { return std::strcmp(n, k) == 0; });
    if (!known) unknown += (unknown.empty() ? "" : ", ") + shown(k);
  }
  if (unknown.empty()) return true;
  err = fill(R::kArgs, {tool, joined(accepted, ", "), unknown});
  return false;
}

struct ArgReader {
  JsonObjectConst a;
  const char* tool;
  const std::vector<const char*>& accepted;
  std::string err;

  bool str(const char* key, bool required, std::string& out, bool* given = nullptr) {
    JsonVariantConst v = a[key];
    if (given) *given = !v.isNull();
    if (v.isNull()) {
      if (required) err = fill(R::kMissingArg, {key, tool, joined(accepted, ", ")});
      return !required;
    }
    if (!v.is<const char*>()) {
      err = fill(R::kNotString, {key});
      return false;
    }
    const ArduinoJson::JsonString js = v.as<ArduinoJson::JsonString>();
    out.assign(js.c_str(), js.size());   // a \u0000 inside is kept, never a cut
    return true;
  }
  bool count(const char* key, uint64_t& out, bool& given) {
    JsonVariantConst v = a[key];
    given = !v.isNull();
    if (!given) return true;
    if (!v.is<uint64_t>() || v.is<bool>()) {
      err = fill(R::kNotCount, {key});
      return false;
    }
    out = v.as<uint64_t>();
    return true;
  }
  bool flag(const char* key, bool& out) {
    JsonVariantConst v = a[key];
    if (v.isNull()) return true;
    if (!v.is<bool>()) {
      err = fill(R::kNotBool, {key});
      return false;
    }
    out = v.as<bool>();
    return true;
  }
};

const char* reservedText(Reserved k) {
  switch (k) {
    case Reserved::Skill: return R::kOwnerSkill;
    case Reserved::Archive: return R::kOwnerArchive;
    case Reserved::Inbox: return R::kOwnerInbox;
    default: return R::kOwnerAgents;
  }
}

bool callerReads(const Principal& who) { return who.valid() && (who.owner || who.perms().readOwn); }
bool callerWrites(const Principal& who) { return who.valid() && (who.owner || who.perms().writeOwn); }
bool mayReadShared(const Principal& who) { return who.owner || who.perms().readShared; }

uint64_t hashOf(std::string_view bytes) { return fnvContinue(kFnvBasis, bytes); }

size_t countLines(std::string_view s) {
  const size_t nl = size_t(std::count(s.begin(), s.end(), '\n'));
  return nl + ((!s.empty() && s.back() != '\n') ? 1 : 0);
}

bool isPdf(std::string_view name) {
  if (name.size() < 4) return false;
  std::string ext(name.substr(name.size() - 4));
  for (char& c : ext) c = char(std::tolower((unsigned char)c));
  return ext == ".pdf";
}

// list_files' prefix: each segment mapped like a stored one, '/' kept.
std::string prefixFor(std::string_view prefix) {
  std::string out;
  size_t at = 0;
  while (at <= prefix.size()) {
    const size_t k = prefix.find('/', at);
    out += validCharsFor(prefix.substr(at, k == std::string_view::npos ? std::string_view::npos : k - at));
    if (k == std::string_view::npos) break;
    out += '/';
    at = k + 1;
  }
  return out;
}

bool startsFolded(std::string_view s, std::string_view p) {
  if (s.size() < p.size()) return false;
  for (size_t i = 0; i < p.size(); ++i)
    if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)p[i])) return false;
  return true;
}

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
void civil(int64_t days, int& y, unsigned& m, unsigned& d) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = unsigned(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = int(int64_t(yoe) + era * 400 + (m <= 2));
}

struct Listed {
  std::string path;
  uint32_t bytes, at;
  bool shared;
};

}  // namespace

// ---- pure rules -------------------------------------------------------------------

size_t pageCut(std::string_view window, size_t limit, bool atEnd) {
  const std::string_view w = window.substr(0, std::min(window.size(), limit));
  if (atEnd || w.empty()) return w.size();
  const size_t nl = w.rfind('\n');
  if (nl != std::string_view::npos && nl + 1 >= limit / 2) return nl + 1;
  // The last character that starts in the window: cut before it when it does not end there.
  const size_t low = w.size() > 4 ? w.size() - 4 : 0;
  for (size_t i = w.size(); i-- > low;) {
    const unsigned char c = (unsigned char)w[i];
    if ((c & 0xC0) == 0x80) continue;
    const size_t len = nimbus::utf8Lead(c).len;
    return (len > 1 && i + len > w.size() && i > 0) ? i : w.size();
  }
  return w.size();
}

uint64_t fnvContinue(uint64_t hash, std::string_view bytes) {
  for (unsigned char c : bytes) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string sha8Of(uint64_t hash) {
  if (!hash) return "unknown";
  char buf[12];
  std::snprintf(buf, sizeof buf, "%08llx", (unsigned long long)(hash >> 32));
  return buf;
}

std::string FileToolsIo::timeText(uint32_t epoch) {
  if (!epoch) return R::kTimeUnknown;
  int y = 0;
  unsigned m = 0, d = 0;
  civil(int64_t(epoch / 86400), y, m, d);
  const unsigned secs = epoch % 86400;
  char buf[24];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02u:%02u", y, m, d, secs / 3600, (secs / 60) % 60);
  return buf;
}

// ---- the call's target ----------------------------------------------------------------

struct FileTools::Target {
  const char* tool = "";
  std::string asked;
  StorePath sp;          // project/name; after a lookup, in the store's letter case
  PathRule rule;
  FileEntry entry;       // after a lookup that found a readable file
  std::string path() const { return sp.str(); }
};

struct FileTools::Page {
  std::string whole;     // the whole file, when the read already holds it
  bool haveWhole = false;
  std::string lines = "?";
  uint64_t hash = 0;
};

// How a write meets what is stored: replace an existing file (else it is an error), or
// expect exactly the bytes edit_file read (else the file changed under the edit).
struct FileTools::WriteMode {
  bool replace = false;
  bool expect = false;
  uint32_t bytes = 0;
  uint64_t hash = 0;
  uint32_t at = 0;   // every write stamps its time, so a same-size change of a file with no hash shows too
};

bool FileTools::resolve(JsonObjectConst args, Target& t, std::string& err) const {
  const ArduinoJson::JsonString js = args["path"].as<ArduinoJson::JsonString>();
  t.asked.assign(js.c_str(), js.size());
  t.sp = storePathFor(t.asked);
  if (!t.sp.ok()) {
    err = fill(R::kPath, {shown(t.asked)});
    return false;
  }
  t.rule = pathRuleFor(t.sp.project, t.sp.name);
  return true;
}

// The lookup hold. state: 0 missing, 1 readable by this caller, 2 there but not readable
// (it reads exactly like a missing file). False: no card.
bool FileTools::lookup(Target& t, const Principal& who, int& state) {
  state = 0;
  return io_.locked([&] {
    FileStore& s = io_.index();
    const FileEntry* e = s.find(t.sp.project, t.sp.name);
    if (!e) return;
    state = s.readableBy(*e, who.ns, who.owner, mayReadShared(who)) ? 1 : 2;
    if (state != 1) return;
    t.entry = *e;
    t.sp.project = plainCopy(e->project);
    t.sp.name = plainCopy(e->name);
  });
}

// ---- reads -----------------------------------------------------------------------------

bool FileTools::readSpan(const Target& t, uint64_t from, uint64_t to, std::string& out) {
  out.clear();
  bool ok = true;
  for (uint64_t at = from; ok && at < to; at += kFileReadSlice) {
    const size_t len = size_t(std::min<uint64_t>(kFileReadSlice, to - at));
    std::string piece;
    const bool held = io_.locked([&] { ok = io_.readAt(t.sp.project, t.sp.name, at, len, piece); });
    ok = ok && held && piece.size() == len;
    out += piece;
  }
  return ok;
}

void FileTools::countFile(const Target& t, Page& p) {
  p.hash = t.entry.hash;
  if (t.entry.bytes > cfg_.lineCountMax) return;
  std::string all;
  if (p.haveWhole) all = p.whole;
  else if (!readSpan(t, 0, t.entry.bytes, all)) return;
  p.lines = num(countLines(all));
  if (!p.hash) p.hash = hashOf(all);
}

namespace {

std::string readHeader(const std::string& path, uint64_t size, const std::string& lines, uint64_t hash, uint64_t a,
                       uint64_t b) {
  return fill(R::kReadHead, {path, num(size), lines, sha8Of(hash), num(a), num(b)});
}

std::string pageOf(const std::string& head, const std::string& window, size_t cut, uint64_t a, uint64_t size) {
  std::string out = head + "\n" + window.substr(0, cut);
  if (a + cut < size) out += fill(R::kNextOffset, {num(a + cut)});
  return out;
}

}  // namespace

ToolResult FileTools::readSkill(const Target& t, size_t offset, size_t limit) {
  std::string body;
  if (t.rule.skillId.empty()) return ToolResult::fail(fill(R::kNoFile, {t.path()}));
  if (!io_.skillBody(t.rule.skillId, body)) return ToolResult::fail(fill(R::kNoSkill, {t.rule.skillId}));
  if (offset > body.size()) return ToolResult::fail(fill(R::kPastEnd, {num(offset), num(body.size())}));
  const std::string window = body.substr(offset, limit);
  const size_t cut = pageCut(window, limit, offset + window.size() >= body.size());
  const std::string lines = body.size() <= cfg_.lineCountMax ? num(countLines(body)) : "?";
  std::string head =
      readHeader("skills/" + t.rule.skillId + ".md", body.size(), lines, hashOf(body), offset, offset + cut);
  head += R::kSkillNote;
  return ToolResult::ok(pageOf(head, window, cut, offset, body.size()));
}

namespace {

// read_file's arguments; table: column, from or to was given.
struct ReadCall {
  std::string path;
  uint64_t offset = 0, limit = 0;
  bool hasOffset = false, hasLimit = false, table = false;
  aids::CsvQuery q;
};

bool readArgs(JsonObjectConst args, ReadCall& c, std::string& err) {
  ArgReader in{args, "read_file", kReadArgs, ""};
  bool col = false, from = false, to = false;
  const bool ok = in.str("path", true, c.path) && in.count("offset", c.offset, c.hasOffset) &&
                  in.count("limit", c.limit, c.hasLimit) && in.str("column", false, c.q.column, &col) &&
                  in.str("from", false, c.q.from, &from) && in.str("to", false, c.q.to, &to);
  c.table = col || from || to;
  err = in.err;
  return ok;
}

}  // namespace

ToolResult FileTools::read(JsonObjectConst args, const Principal& who) {
  std::string err;
  ReadCall c;
  Target t;
  t.tool = "read_file";
  if (!onlyKnown(args, t.tool, kReadArgs, err) || !readArgs(args, c, err) || !resolve(args, t, err))
    return ToolResult::fail(err);
  if (!callerReads(who)) return ToolResult::fail(R::kNoCaller);
  // Ruling 95: a page has a floor. A limit under kFileReadFloor is raised to it, a limit over
  // readCap held to it, and the head says which limit the page used.
  const uint64_t asked = c.hasLimit ? c.limit : cfg_.readCap;
  const size_t eff = size_t(std::min<uint64_t>(std::max<uint64_t>(asked, kFileReadFloor), cfg_.readCap));
  const std::string note = c.hasLimit && eff != c.limit ? fill(R::kLimitNote, {num(eff)}) : "";
  const size_t offset = size_t(std::min<uint64_t>(c.offset, SIZE_MAX / 2));
  if (c.table) {
    c.q.fromByte = c.offset;
    c.q.limit = eff;
    err = aids::checkCsvQuery(c.q);
    return err.empty() ? readTable(t, who, c.q, note) : ToolResult::fail(err);
  }
  if (t.rule.kind != Reserved::Skill) return readStored(t, who, c.offset, eff, note);
  ToolResult r = readSkill(t, offset, eff);
  if (r.success) r.output.insert(r.output.find('\n'), note);
  return r;
}

// The lookup and the kind checks every text read makes: a file this caller reads, stored,
// not a PDF, an image, audio or binary data. False with err.
bool FileTools::textFile(Target& t, const Principal& who, std::string& err) {
  int state = 0;
  if (!lookup(t, who, state)) return (err = R::kNoCard).empty();
  if (state != 1) return (err = fill(R::kNoFile, {t.path()})).empty();
  const FileKind kind = fileKindForName(t.sp.name);
  if (isPdf(t.sp.name)) return (err = fill(R::kPdf, {t.path()})).empty();
  if (kind == FileKind::Image || kind == FileKind::Audio)
    return (err = fill(R::kNotText, {t.path(), fileKindName(kind)})).empty();
  std::string head;
  if (!readSpan(t, 0, std::min<uint64_t>(t.entry.bytes, kTextSniffBytes), head)) return (err = R::kCardRead).empty();
  if (!fileReadableAsText(t.sp.name, head)) return (err = fill(R::kNotText, {t.path(), R::kBinary})).empty();
  return true;
}

ToolResult FileTools::readStored(Target& t, const Principal& who, uint64_t offset, size_t limit,
                                 const std::string& note) {
  std::string err, window, last;
  if (!textFile(t, who, err)) return ToolResult::fail(err);
  const uint64_t size = t.entry.bytes;
  if (offset > size) return ToolResult::fail(fill(R::kPastEnd, {num(offset), num(size)}));
  if (!readSpan(t, offset, std::min<uint64_t>(size, offset + limit), window)) return ToolResult::fail(R::kCardRead);
  const bool atEnd = offset + window.size() >= size;
  if (atEnd && !window.empty()) last = window.substr(window.size() - 1);
  else if (size && !readSpan(t, size - 1, size, last)) return ToolResult::fail(R::kCardRead);
  const size_t cut = pageCut(window, limit, atEnd);
  Page p;
  p.haveWhole = offset == 0 && window.size() == size;
  if (p.haveWhole) p.whole = window;
  countFile(t, p);
  std::string line = readHeader(t.path(), size, p.lines, p.hash, offset, offset + cut) + note;
  if (t.sp.changed) line += fill(R::kAskedNote, {shown(t.asked)});
  if (aids::endsTorn(size, last.empty() ? '\n' : last[0])) line += fill(R::kTornNote, {aids::kTornTail});
  return ToolResult::ok(pageOf(line, window, cut, offset, size));
}

namespace {

// The stored file as file_aids.h reads it: one card-lock hold per call, hashed as it goes.
struct CardSource : aids::AidSource {
  FileToolsIo& io;
  const StorePath& sp;
  uint64_t hash = kFnvBasis;
  std::string piece;
  CardSource(FileToolsIo& i, const StorePath& p) : io(i), sp(p) {}
  // Past the stored size is the end of the file (a short read), so a file that shrank under
  // an edit reads as changed, never as a card fault.
  bool readAt(uint64_t offset, size_t len, char* out, size_t& got) override {
    bool ok = false;
    got = 0;
    piece.clear();
    const auto read = [&] {
      const FileEntry* e = io.index().find(sp.project, sp.name);
      const uint64_t size = e ? e->bytes : 0;
      ok = offset >= size || io.readAt(sp.project, sp.name, offset, size_t(std::min<uint64_t>(len, size - offset)), piece);
      if (offset >= size) piece.clear();
    };
    if (!io.locked(read) || !ok) return false;
    got = std::min(piece.size(), len);
    std::memcpy(out, piece.data(), got);
    hash = fnvContinue(hash, std::string_view(out, got));
    return true;
  }
};

// An approved skill's text as a source (read_file on skills/<id>.md with column or sum).
struct TextSource : aids::AidSource {
  std::string_view text;
  explicit TextSource(std::string_view t) : text(t) {}
  bool readAt(uint64_t offset, size_t len, char* out, size_t& got) override {
    got = offset < text.size() ? std::min(len, size_t(text.size() - offset)) : 0;
    std::memcpy(out, text.data() + std::min<uint64_t>(offset, text.size()), got);
    return true;
  }
};

}  // namespace

// read_file with column/from/to (ruling 97): the CSV scan, then exactly a plain read's page of
// the selected rows: the same head (the part's file bytes a-b), the header in the part that
// holds it and the selected rows as stored, and next_offset=<b> when a selected row follows.
ToolResult FileTools::readTable(Target& t, const Principal& who, const aids::CsvQuery& q, const std::string& note) {
  std::string err, path = t.path(), body;
  uint64_t size = 0, hash = 0;
  aids::CsvScanOutcome o;
  if (t.rule.kind == Reserved::Skill) {
    if (t.rule.skillId.empty() || !io_.skillBody(t.rule.skillId, body))
      return ToolResult::fail(fill(R::kNoSkill, {t.rule.skillId}));
    TextSource src(body);
    path = "skills/" + t.rule.skillId + ".md";
    size = body.size();
    hash = hashOf(body);
    o = aids::scanCsv(src, size, path, q);
  } else {
    if (!textFile(t, who, err)) return ToolResult::fail(err);
    CardSource src(io_, t.sp);
    size = t.entry.bytes;
    o = aids::scanCsv(src, size, path, q);
    hash = t.entry.hash ? t.entry.hash : src.hash;
  }
  if (!o.ok) return ToolResult::fail(o.error);
  const aids::CsvScanResult& r = o.result;
  if (q.fromByte > r.bytes) return ToolResult::fail(fill(R::kPastEnd, {num(q.fromByte), num(r.bytes)}));
  std::string line = readHeader(path, r.bytes, num(r.lines), hash, r.partFrom, r.partTo) + note;
  if (t.sp.changed) line += fill(R::kAskedNote, {shown(t.asked)});
  if (r.tornLine) line += fill(R::kTornNote, {aids::kTornTail});
  std::string out = line + "\n" + r.body;
  if (r.more) out += fill(R::kNextOffset, {num(r.partTo)});
  return ToolResult::ok(out);
}

// ---- writes ----------------------------------------------------------------------------

namespace {

bool quotaOk(const FileStore& s, const Principal& who, const FileEntry* cur, uint64_t newBytes, std::string& err) {
  const Quota q = effectiveQuota(who.role, who.quota);
  if (who.owner || !q.maxBytes) return true;
  const bool mine = cur && (cur->owner.empty() ? who.ns == kOwnerNs : plainCopy(cur->owner) == who.ns);
  const uint64_t used = uint64_t(s.bytesFor(who.ns)) - (mine ? cur->bytes : 0) + newBytes;
  if (used <= q.maxBytes) return true;
  err = fill(R::kQuota, {num(used), num(q.maxBytes)});
  return false;
}

// The entry a write leaves: whose file it is and whether it is shared stay; the content
// facts are new; a provider's cached upload id goes with the old content.
FileEntry entryAfter(const FileEntry* cur, const StorePath& sp, const Principal& who, uint64_t bytes, uint64_t hash,
                     uint32_t now) {
  FileEntry e;
  if (cur) {
    e = *cur;
  } else {
    e.project = sp.project;
    e.name = sp.name;
    e.owner = who.ns;
  }
  e.bytes = uint32_t(bytes);
  e.createdAt = now;
  e.kind = fileKindForName(plainCopy(e.name));
  e.hash = hash;
  e.providerTag.clear();
  e.providerFileId.clear();
  return e;
}

}  // namespace

// Inside a hold: may this caller put newBytes at the target now?
bool FileTools::writeCheck(Target& t, const Principal& who, const WriteMode& m, uint64_t newBytes,
                           std::string& err) {
  FileStore& s = io_.index();
  const FileEntry* cur = s.find(t.sp.project, t.sp.name);
  if (!s.writeAllowed(t.sp.project, t.sp.name, who.ns, who.owner)) {
    err = fill(R::kOthers, {t.path()});
    return false;
  }
  if (m.expect && (!cur || cur->bytes != m.bytes || cur->hash != m.hash || cur->createdAt != m.at)) {
    err = fill(R::kChanged, {t.path()});
    return false;
  }
  if (cur && !m.replace) {
    err = fill(R::kExists, {t.path(), num(cur->bytes), sha8Of(cur->hash)});
    return false;
  }
  if (!quotaOk(s, who, cur, newBytes, err)) return false;
  std::string why;
  if (s.wouldExceed(t.sp.project, t.sp.name, newBytes, why)) {
    err = fill(R::kRoom, {why});
    return false;
  }
  if (cur) {   // card ops use the stored letter case (a case-sensitive host would make a second file)
    t.sp.project = plainCopy(cur->project);
    t.sp.name = plainCopy(cur->name);
  }
  return true;
}

bool FileTools::writePart(const Target& t, const std::string& bytes) {
  bool ok = true;
  size_t at = 0;
  do {
    const std::string_view slice = std::string_view(bytes).substr(at, kFileWriteSlice);
    const bool held = io_.locked([&] { ok = io_.partWrite(t.sp.project, t.sp.name, slice, at == 0); });
    ok = ok && held;
    at += slice.size();
  } while (ok && at < bytes.size());
  return ok;
}

// The index rewrite, alone in its hold. "" when saved.
std::string FileTools::persisted() {
  bool saved = false;
  io_.locked([&] { saved = io_.persistIndex(); });
  return saved ? std::string() : std::string(R::kIndexNote);
}

ToolResult FileTools::writeBytes(Target& t, const std::string& bytes, const Principal& who, const WriteMode& m) {
  std::string err;
  bool ok = false;
  // 1. The checks, and room (a binding may reclaim regenerable copies here).
  const bool card = io_.locked([&] {
    ok = writeCheck(t, who, m, bytes.size(), err);
    std::string why;
    if (ok && !io_.roomFor(t.sp.project, t.sp.name, bytes.size(), why)) {
      err = fill(R::kRoom, {why});
      ok = false;
    }
  });
  if (!card) return ToolResult::fail(R::kNoCard);
  if (!ok) return ToolResult::fail(err);
  // 2. The part file, 4 KB per hold.
  if (!writePart(t, bytes)) {
    io_.locked([&] { io_.partDiscard(t.sp.project, t.sp.name); });
    return ToolResult::fail(R::kCardWrite);
  }
  // 3. The commit: the checks again, the swap, the index entry.
  uint64_t replaced = 0;
  bool existed = false;
  const uint64_t hash = hashOf(bytes);
  io_.locked([&] {
    ok = writeCheck(t, who, m, bytes.size(), err);
    if (!ok) return;
    FileStore& s = io_.index();
    const FileEntry* cur = s.find(t.sp.project, t.sp.name);
    if (!io_.partSwap(t.sp.project, t.sp.name)) {
      err = R::kCardWrite;
      ok = false;
      return;
    }
    existed = cur != nullptr;
    replaced = cur ? cur->bytes : 0;
    const FileEntry e = entryAfter(cur, t.sp, who, bytes.size(), hash, io_.now());
    t.sp.project = plainCopy(e.project);
    t.sp.name = plainCopy(e.name);
    std::string addErr;
    s.add(e, addErr);   // cannot refuse: writeCheck ran wouldExceed in this same hold
  });
  if (!ok) {
    io_.locked([&] { io_.partDiscard(t.sp.project, t.sp.name); });
    return ToolResult::fail(err);
  }
  // 4. The index rewrite, alone.
  std::string out = fill(R::kWrote, {t.path(), num(bytes.size()), sha8Of(hash)});
  out += existed ? fill(R::kReplaced, {num(replaced)}) : std::string(R::kNewFile);
  if (t.sp.changed) out += fill(R::kAskedSemi, {shown(t.asked)});
  out += persisted();
  return ToolResult::ok(out);
}

namespace {

// Shared argument rules of the three write tools.
bool writeTarget(const Principal& who, const PathRule& rule, const std::string& path, std::string& err) {
  if (!callerWrites(who)) {
    err = R::kNoCaller;
    return false;
  }
  if (rule.writable) return true;
  err = fill(R::kReadOnly, {path, reservedText(rule.kind)});
  return false;
}

bool withinCap(const char* key, size_t size, size_t cap, std::string& err, bool more = false) {
  if (size <= cap) return true;
  err = fill(R::kTooLong, {key, num(size), num(cap)}) + (more ? R::kTooLongMore : "");
  return false;
}

}  // namespace

ToolResult FileTools::write(JsonObjectConst args, const Principal& who) {
  std::string err, path, content, ifExists = "fail";
  Target t;
  t.tool = "write_file";
  if (!onlyKnown(args, t.tool, kWriteArgs, err)) return ToolResult::fail(err);
  ArgReader in{args, t.tool, kWriteArgs, ""};
  if (!in.str("path", true, path) || !in.str("content", true, content) || !in.str("if_exists", false, ifExists))
    return ToolResult::fail(in.err);
  if (ifExists != "fail" && ifExists != "replace") return ToolResult::fail(fill(R::kIfExists, {shown(ifExists)}));
  if (!resolve(args, t, err) || !withinCap("content", content.size(), cfg_.contentCap, err, true) ||
      !writeTarget(who, t.rule, t.path(), err))
    return ToolResult::fail(err);
  WriteMode m;
  m.replace = ifExists == "replace";
  return writeBytes(t, content, who, m);
}

std::string FileTools::linesNote(const Target& t, uint64_t size) {
  if (size > cfg_.lineCountMax) return std::string();
  std::string all;
  if (!readSpan(t, 0, size, all)) return std::string();
  return num(countLines(all));
}

// Step 1 of an append, one hold: the checks and room for `add` more bytes; before is the
// stored size. False with err (no card is kNoCard).
bool FileTools::appendRoom(Target& t, const Principal& who, uint64_t add, uint64_t& before, std::string& err) {
  WriteMode m;
  m.replace = true;
  bool ok = false;
  const bool card = io_.locked([&] {
    const FileEntry* cur = io_.index().find(t.sp.project, t.sp.name);
    before = cur ? cur->bytes : 0;
    std::string why;
    ok = writeCheck(t, who, m, before + add, err);
    if (ok && !io_.roomFor(t.sp.project, t.sp.name, before + add, why)) ok = (err = fill(R::kRoom, {why})).empty();
  });
  if (!card) err = R::kNoCard;
  return card && ok;
}

// Step 3, one hold: the checks again, the append, the index entry. size is the new size.
bool FileTools::appendBytes(Target& t, const Principal& who, const std::string& bytes, bool& fresh, uint64_t& size,
                            std::string& err) {
  WriteMode m;
  m.replace = true;
  bool ok = false;
  io_.locked([&] {
    FileStore& s = io_.index();
    const FileEntry* cur = s.find(t.sp.project, t.sp.name);
    size = uint64_t(cur ? cur->bytes : 0) + bytes.size();
    if (!writeCheck(t, who, m, size, err)) return;
    if (!io_.append(t.sp.project, t.sp.name, bytes)) return void(err = R::kCardWrite);
    fresh = cur == nullptr;
    const uint64_t hash = cur ? (cur->hash ? fnvContinue(cur->hash, bytes) : 0) : hashOf(bytes);
    const FileEntry e = entryAfter(cur, t.sp, who, size, hash, io_.now());
    t.sp.project = plainCopy(e.project);
    t.sp.name = plainCopy(e.name);
    std::string addErr;
    s.add(e, addErr);
    ok = true;
  });
  return ok;
}

ToolResult FileTools::appendText(Target& t, const std::string& text, const Principal& who) {
  std::string err, tail;
  uint64_t before = 0, size = 0;
  bool fresh = false;
  if (!appendRoom(t, who, text.size(), before, err)) return ToolResult::fail(err);   // the commit checks the fence too
  // Step 2, one hold: the file's last bytes, for the newline fence and the identical-line fact.
  const uint64_t from = before - std::min<uint64_t>(before, aids::kAppendTailBytes);
  if (before && !readSpan(t, from, before, tail)) return ToolResult::fail(R::kCardRead);
  const bool fence = aids::endsTorn(before, tail.empty() ? '\n' : tail.back());
  const std::string bytes = fence ? "\n" + text : text;
  if (!appendBytes(t, who, bytes, fresh, size, err)) return ToolResult::fail(err);
  const std::string index = persisted();
  const std::string lines = linesNote(t, size);
  std::string out = fill(R::kAppended, {num(bytes.size()), t.path(), num(size)});
  if (!lines.empty()) out += fill(R::kLines, {lines});
  if (fresh) out += R::kNewFile;
  if (text.back() != '\n') out += R::kNoNewline;
  if (t.sp.changed) out += fill(R::kAskedSemi, {shown(t.asked)});
  // The lines the file had before: what it has now, less the text's own.
  const uint64_t had = lines.empty() ? 0 : std::stoull(lines) - countLines(text);
  out += aids::appendFacts(tail, from == 0, before, had, text).note;
  return ToolResult::ok(out + index);
}

ToolResult FileTools::append(JsonObjectConst args, const Principal& who) {
  std::string err, path, text;
  Target t;
  t.tool = "append_file";
  if (!onlyKnown(args, t.tool, kAppendArgs, err)) return ToolResult::fail(err);
  ArgReader in{args, t.tool, kAppendArgs, ""};
  if (!in.str("path", true, path) || !in.str("text", true, text)) return ToolResult::fail(in.err);
  if (!resolve(args, t, err)) return ToolResult::fail(err);
  if (text.empty()) return ToolResult::fail(R::kEmptyText);
  if (!withinCap("text", text.size(), cfg_.contentCap, err) || !writeTarget(who, t.rule, t.path(), err))
    return ToolResult::fail(err);
  return appendText(t, text, who);
}

namespace {

// The part file and the commit as file_aids.h's edit writes them: 4 KB per hold; the swap
// hold runs the binding's checks again (whose file, quota, room, unchanged since pass 1)
// and updates the index entry with the new size and hash.
struct EditSink : aids::AidSink {
  FileToolsIo& io;
  const StorePath& sp;
  std::function<bool(uint64_t bytes, uint64_t hash, std::string& err)> commit;
  std::function<bool(uint64_t bytes, std::string& err)> check;
  uint64_t bytes = 0, hash = kFnvBasis;
  std::string err;
  EditSink(FileToolsIo& i, const StorePath& p) : io(i), sp(p) {}
  bool partWrite(std::string_view b, bool first) override {
    bool ok = false;
    if (first) bytes = 0, hash = kFnvBasis;
    if (!io.locked([&] { ok = io.partWrite(sp.project, sp.name, b, first); }) || !ok) return false;
    bytes += b.size();
    hash = fnvContinue(hash, b);
    return true;
  }
  bool partSwap() override {
    bool ok = false;
    return io.locked([&] { ok = commit(bytes, hash, err); }) && ok;
  }
  void partDiscard() override {
    io.locked([&] { io.partDiscard(sp.project, sp.name); });
  }
  bool admit(uint64_t newSize, size_t) override {
    bool ok = false;
    return io.locked([&] { ok = check(newSize, err); }) && ok;
  }
  std::string refusal() const override { return err; }
};

}  // namespace

ToolResult FileTools::editText(Target& t, const aids::EditRequest& e, const Principal& who) {
  int state = 0;
  if (!lookup(t, who, state)) return ToolResult::fail(R::kNoCard);
  if (state != 1) return ToolResult::fail(fill(R::kNoFile, {t.path()}));
  const uint32_t was = t.entry.bytes;
  if (was > cfg_.editMax) return ToolResult::fail(fill(R::kEditTooBig, {t.path(), num(was), num(cfg_.editMax)}));
  WriteMode m;
  m.replace = m.expect = true;
  m.bytes = was;
  m.hash = t.entry.hash;
  m.at = t.entry.createdAt;
  std::string err;
  bool ok = false;
  if (!io_.locked([&] { ok = writeCheck(t, who, m, was, err); })) return ToolResult::fail(R::kNoCard);
  if (!ok) return ToolResult::fail(err);
  CardSource src(io_, t.sp);
  EditSink sink(io_, t.sp);
  sink.check = [&](uint64_t bytes, std::string& why) {
    if (bytes > cfg_.editMax) return (why = fill(R::kEditTooBig, {t.path(), num(bytes), num(cfg_.editMax)})).empty();
    std::string room;
    if (!writeCheck(t, who, m, bytes, why)) return false;
    if (!io_.roomFor(t.sp.project, t.sp.name, bytes, room)) return (why = fill(R::kRoom, {room})).empty();
    return true;
  };
  sink.commit = [&](uint64_t bytes, uint64_t hash, std::string& why) {
    if (!writeCheck(t, who, m, bytes, why)) return false;
    std::string room;
    if (!io_.roomFor(t.sp.project, t.sp.name, bytes, room)) return (why = fill(R::kRoom, {room})).empty();
    if (!io_.partSwap(t.sp.project, t.sp.name)) return false;
    FileStore& s = io_.index();
    const FileEntry next = entryAfter(s.find(t.sp.project, t.sp.name), t.sp, who, bytes, hash, io_.now());
    std::string addErr;
    s.add(next, addErr);   // cannot refuse: writeCheck ran wouldExceed in this same hold
    return true;
  };
  const aids::EditOutcome o = aids::editFile(src, sink, was, t.entry.hash, t.path(), e);
  if (!o.ok) return ToolResult::fail(o.error);
  return ToolResult::ok(aids::renderEdit(t.path(), o) + persisted());
}

ToolResult FileTools::edit(JsonObjectConst args, const Principal& who) {
  std::string err, path;
  aids::EditRequest e;
  uint64_t line = 0;
  bool hasLine = false;
  Target t;
  t.tool = "edit_file";
  if (!onlyKnown(args, t.tool, kEditArgs, err)) return ToolResult::fail(err);
  ArgReader in{args, t.tool, kEditArgs, ""};
  if (!in.str("path", true, path) || !in.str("old", true, e.oldText) || !in.str("new", true, e.newText) ||
      !in.flag("all", e.all) || !in.count("line", line, hasLine))
    return ToolResult::fail(in.err);
  if (hasLine && (line == 0 || line > UINT32_MAX)) return ToolResult::fail(R::kNotLine);
  e.line = uint32_t(line);
  if (!resolve(args, t, err)) return ToolResult::fail(err);
  err = aids::checkEdit(e);
  if (!err.empty()) return ToolResult::fail(err);
  if (!withinCap("old", e.oldText.size(), cfg_.contentCap, err) ||
      !withinCap("new", e.newText.size(), cfg_.contentCap, err) || !writeTarget(who, t.rule, t.path(), err))
    return ToolResult::fail(err);
  return editText(t, e, who);
}

// ---- list ------------------------------------------------------------------------------

namespace {

std::vector<Listed> visibleFiles(FileStore& s, const Principal& who, const std::string& prefix) {
  std::vector<Listed> out;
  for (const FileEntry* e : s.list()) {
    if (!s.readableBy(*e, who.ns, who.owner, mayReadShared(who))) continue;
    std::string path = plainCopy(e->project) + "/" + plainCopy(e->name);
    if (!startsFolded(path, prefix)) continue;
    out.push_back({std::move(path), e->bytes, e->createdAt, e->shared && !s.ownedBy(*e, who.ns, false)});
  }
  return out;
}

bool sameFolded(const std::string& a, const std::string& b) { return a.size() == b.size() && startsFolded(a, b); }

}  // namespace

ToolResult FileTools::list(JsonObjectConst args, const Principal& who) {
  std::string err, prefix, after;
  const char* tool = "list_files";
  if (!onlyKnown(args, tool, kListArgs, err)) return ToolResult::fail(err);
  ArgReader in{args, tool, kListArgs, ""};
  if (!in.str("prefix", false, prefix) || !in.str("after", false, after)) return ToolResult::fail(in.err);
  if (!callerReads(who)) return ToolResult::fail(R::kNoCaller);
  std::vector<Listed> files;
  const std::string want = prefixFor(prefix);
  if (!io_.locked([&] { files = visibleFiles(io_.index(), who, want); })) return ToolResult::fail(R::kNoCard);
  std::sort(files.begin(), files.end(), [](const Listed& a, const Listed& b) {   // outside the hold
    return a.at != b.at ? a.at > b.at : a.path < b.path;
  });
  size_t from = 0;
  if (!after.empty()) {
    const auto it = std::find_if(files.begin(), files.end(), [&](const Listed& l) { return sameFolded(l.path, after); });
    if (it == files.end()) return ToolResult::fail(fill(R::kAfter, {shown(after)}));
    from = size_t(it - files.begin()) + 1;
  }
  const size_t to = std::min(files.size(), from + cfg_.listMax);
  std::string out;
  for (size_t i = from; i < to; i++) {
    out += fill(R::kListLine, {files[i].path, num(files[i].bytes), io_.timeText(files[i].at)});
    out += (files[i].shared ? R::kShared : "") + std::string("\n");
  }
  if (from == to && from == 0) out += (want.empty() ? std::string(R::kNoFiles) : fill(R::kNoFilesUnder, {shown(want)})) + "\n";
  const size_t more = files.size() - to;
  out += more ? fill(R::kMoreAfter, {num(more), files[to - 1].path}) : fill(R::kMore, {num(0)});
  return ToolResult::ok(out);
}

// ---- registration ----------------------------------------------------------------------

const std::vector<const char*>& fileToolArgs(const char* tool) {
  static const std::vector<const char*> none;
  const std::string t = tool;
  if (t == "read_file") return kReadArgs;
  if (t == "write_file") return kWriteArgs;
  if (t == "append_file") return kAppendArgs;
  if (t == "edit_file") return kEditArgs;
  if (t == "list_files") return kListArgs;
  return none;
}

void FileTools::registerInto(ToolRegistry& reg, const FileToolDeclFn& decl) {
  using A = JsonObjectConst;
  FileToolDecl d = decl("read_file");
  reg.add("read_file", d.description, [this](A a, const Principal& w) { return read(a, w); }, d.schema);
  d = decl("write_file");
  reg.add("write_file", d.description, [this](A a, const Principal& w) { return write(a, w); }, d.schema);
  d = decl("append_file");
  reg.add("append_file", d.description, [this](A a, const Principal& w) { return append(a, w); }, d.schema);
  d = decl("edit_file");
  reg.add("edit_file", d.description, [this](A a, const Principal& w) { return edit(a, w); }, d.schema);
  d = decl("list_files");
  reg.add("list_files", d.description, [this](A a, const Principal& w) { return list(a, w); }, d.schema);
}

}  // namespace nimbus::orch
