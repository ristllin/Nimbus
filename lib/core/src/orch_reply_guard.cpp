#include "nimbus/orch/reply_guard.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

#include "nimbus/mem_cap.h"           // utf8CapLen - a quoted excerpt stays valid UTF-8
#include "nimbus/orch/action_rules.h"  // actionRuleTable - the request-advertised action names

namespace nimbus {
namespace orch {

namespace {

std::string lowered(const std::string& s) {
  std::string out(s);
  for (char& c : out) c = (char)std::tolower((unsigned char)c);
  return out;
}

bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '\'' || c == '_'; }

// `needle` in `hay` at whole-word edges, from `from`; npos when absent.
size_t findWord(const std::string& hay, const char* needle, size_t from = 0) {
  const size_t n = std::strlen(needle);
  for (size_t at = hay.find(needle, from); at != std::string::npos; at = hay.find(needle, at + 1)) {
    const bool startOk = at == 0 || !isWordChar(hay[at - 1]);
    const bool endOk = at + n >= hay.size() || !isWordChar(hay[at + n]);
    if (startOk && endOk) return at;
  }
  return std::string::npos;
}

bool hasWord(const std::string& hay, const char* needle) { return findWord(hay, needle) != std::string::npos; }

template <size_t N>
bool hasAnyWord(const std::string& hay, const char* const (&words)[N]) {
  for (const char* w : words)
    if (hasWord(hay, w)) return true;
  return false;
}

// The reply with its wrapping punctuation and brackets off, lowercased.
std::string fillerCore(const std::string& reply) {
  std::string t = lowered(reply);
  auto wrap = [](char c) { return std::isspace((unsigned char)c) || std::strchr("<>[]{}()\"'`*_.:!-", c) != nullptr; };
  size_t b = 0, e = t.size();
  while (b < e && wrap(t[b])) b++;
  while (e > b && wrap(t[e - 1])) e--;
  return t.substr(b, e - b);
}

// A reply that is only these words is filler. Words a real question can have as its whole
// answer ("ok", "done", "yes", "n/a", "string", "TBD") are not on it.
const char* const kFiller[] = {
    "placeholder", "placeholder text", "placeholder reply", "placeholder answer", "placeholder response",
    "reply", "your reply", "your reply here", "reply here", "reply text", "the reply", "your response",
    "your response here", "response here", "your answer", "your answer here", "answer here", "final answer",
    "your final answer", "insert reply", "insert reply here", "insert answer here", "text here",
    "todo", "lorem ipsum", "example reply", "sample reply",
};

}  // namespace

const char kPlaceholderCorrection[] =
    "[REPLY CHECK] The reply field holds template filler, not an answer. Write the real answer for the owner "
    "now, from what this turn found. If it found nothing, say that plainly and say what you could not do.";

bool isPlaceholderReply(const std::string& reply, const std::string& ownerText) {
  // Only dots or ellipses, or nothing but wrapping marks around nothing.
  if (reply.find_first_not_of(" \t\r\n") == std::string::npos) return false;   // empty is its own case
  const std::string core = fillerCore(reply);
  if (core.empty() || core == "\xE2\x80\xA6") return true;
  bool filler = false;
  for (const char* f : kFiller) filler = filler || core == f;
  if (!filler) return false;
  return lowered(ownerText).find(core) == std::string::npos;
}

std::string placeholderCheck(const std::string& finalTurnJson, const std::string& ownerText) {
  JsonDocument d;
  if (deserializeJson(d, finalTurnJson) != DeserializationError::Ok) return std::string();
  const char* reply = d["reply"] | "";
  return isPlaceholderReply(reply, ownerText) ? std::string(kPlaceholderCorrection) : std::string();
}

namespace {

// Lane POL2: the plumbing an owner must never read. Each pattern is a class the
// 2 Nov signals named: a memory id, a job id, storage wording, a device path.
// mem_[0-9a-f]{4,}: the memory tool's own id shape.
bool memToolIdIn(const std::string& t) {
  for (size_t at = t.find("mem_"); at != std::string::npos; at = t.find("mem_", at + 1)) {
    if (at != 0 && isWordChar(t[at - 1])) continue;
    size_t n = 0;
    while (at + 4 + n < t.size()) {
      const char c = t[at + 4 + n];
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) n++;
      else break;
    }
    if (n >= 4 && (at + 4 + n >= t.size() || !isWordChar(t[at + 4 + n]))) return true;
  }
  return false;
}

// m[0-9a-f]{8}: the store's row id shape.
bool rowIdIn(const std::string& t) {
  for (size_t at = t.find('m'); at != std::string::npos; at = t.find('m', at + 1)) {
    if (at != 0 && isWordChar(t[at - 1])) continue;
    size_t n = 0;
    while (at + 1 + n < t.size()) {
      const char c = t[at + 1 + n];
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) n++;
      else break;
    }
    if (n >= 8 && (at + 1 + n >= t.size() || !isWordChar(t[at + 1 + n]))) return true;
  }
  return false;
}

bool memoryIdIn(const std::string& t) { return memToolIdIn(t) || rowIdIn(t); }

bool jobIdIn(const std::string& t) {
  if (t.find("result-job") != std::string::npos) return true;
  // job[0-9]{4}: a bare job id the owner should never have to read.
  for (size_t at = t.find("job"); at != std::string::npos; at = t.find("job", at + 1)) {
    if (at != 0 && isWordChar(t[at - 1])) continue;
    size_t n = 0;
    while (at + 3 + n < t.size() && t[at + 3 + n] >= '0' && t[at + 3 + n] <= '9') n++;
    if (n >= 4 && (at + 3 + n >= t.size() || !isWordChar(t[at + 3 + n]))) return true;
  }
  return false;
}

bool storageWordingIn(const std::string& t) {
  return t.find("read back confirmed") != std::string::npos ||
         t.find("stored on the device as") != std::string::npos ||
         t.find("memory id") != std::string::npos || t.find("row key") != std::string::npos;
}

// A device file path: a run of path bytes with a slash, ending in a file suffix.
// URLs are external, not plumbing: a match inside a whitespace-delimited word
// that carries a scheme ("http://host/a/b.md") or starts at a "www." host is
// on another machine. Judging by the whole word (not the bytes next to the
// match) keeps deep URL paths external too ("http://host/data/index.json").
bool inUrlWord(const std::string& t, size_t at) {
  size_t b = at;
  while (b > 0 && t[b - 1] != ' ' && t[b - 1] != '\t' && t[b - 1] != '\n' && t[b - 1] != '\r')
    b--;
  size_t e = at;
  while (e < t.size() && t[e] != ' ' && t[e] != '\t' && t[e] != '\n' && t[e] != '\r') e++;
  const std::string word = t.substr(b, e - b);
  return word.find("://") != std::string::npos || word.rfind("www.", 0) == 0;
}

// One suffix's path scan: a slash-delimited name ending in this suffix, and
// not a URL (a token with its own host or behind a scheme is external).
bool pathWithSuffixIn(const std::string& t, const char* suf) {
  for (size_t at = t.find(suf); at != std::string::npos; at = t.find(suf, at + 1)) {
    const size_t end = at + std::strlen(suf);
    if (end < t.size() && isWordChar(t[end])) continue;
    const size_t slash = t.rfind('/', at);
    if (slash == std::string::npos) continue;   // a bare name, not a path
    size_t b = slash;
    while (b > 0 && (isWordChar(t[b - 1]) || t[b - 1] == '.' || t[b - 1] == '-')) b--;
    if (slash == b) continue;                   // "/foo.md": no name before the slash
    if (inUrlWord(t, at)) continue;
    return true;
  }
  return false;
}

bool devicePathIn(const std::string& t) {
  // The device's storage root is plumbing whatever the suffix (" /data/prefs.bin").
  // A URL path is external, not plumbing: a scheme or "www." host anywhere in
  // the word, or a dotted host right before "/data/" ("nationalrail.co.uk/data/
  // disruptions", with or without a scheme), names another machine.
  for (size_t at = t.find("/data/"); at != std::string::npos; at = t.find("/data/", at + 1)) {
    if (inUrlWord(t, at)) continue;
    size_t b = at;
    while (b > 0 && (isWordChar(t[b - 1]) || t[b - 1] == '.' || t[b - 1] == '-')) b--;
    if (b == at || t.substr(b, at - b).find('.') == std::string::npos) return true;
  }
  static const char* const kSuffix[] = {".md", ".csv", ".txt", ".bin", ".json", ".log"};
  for (const char* suf : kSuffix)
    if (pathWithSuffixIn(t, suf)) return true;
  return false;
}

}  // namespace

bool hasPlumbing(const std::string& reply) {
  const std::string t = lowered(reply);
  return memoryIdIn(t) || jobIdIn(t) || storageWordingIn(t) || devicePathIn(t);
}

namespace {
// The correction a plumbing leak gets (model-facing), shared by the orch_turn
// check and the prose check.
const char* const kPlumbingCorrection =
    "[REPLY CHECK] Your reply shows internal plumbing: a memory or job id, a file "
    "path, or storage wording like \"read back confirmed\". The owner reads plain "
    "words only. Rewrite the answer without them: say what the thing is for the "
    "owner, or leave it out. Do not mention this check.";
}  // namespace

std::string plumbingCheckText(const std::string& text) {
  return hasPlumbing(text) ? std::string(kPlumbingCorrection) : std::string();
}

std::string plumbingCheck(const std::string& finalTurnJson) {
  JsonDocument d;
  if (deserializeJson(d, finalTurnJson) != DeserializationError::Ok) return std::string();
  const char* reply = d["reply"] | "";
  const char* ask = d["ask"] | "";
  // The ask field reaches the owner too (it is the question shown), so both are
  // checked: plumbing in either is a leak.
  if (!hasPlumbing(reply) && !hasPlumbing(ask)) return std::string();
  return std::string(kPlumbingCorrection);
}

namespace {

// Past this the message is a brief, not a chat line.
constexpr size_t kChatMaxBytes = 240;

// Words that ask for work wherever they stand.
const char* const kWorkAnywhere[] = {
    "web",      "online",   "internet", "website", "look up", "lookup",  "search",   "google", "browse",
    "news",     "weather",  "forecast", "price",   "stock",   "file",    "files",    "folder", "document",
    "pdf",      "spreadsheet", "csv",   "calendar", "email",  "inbox",   "sub-agent", "subagent", "routine",
    "remind",   "reminder", "research", "summarize", "summarise", "translate",
};

// Verbs that ask for work where a request starts: the first word of a sentence, or after
// "please", "can you" and the like.
const char* const kWorkVerbs[] = {
    "use",    "look",   "find",    "check",   "read",    "open",    "fetch",  "get",      "list",   "show",
    "compare", "calculate", "compute", "write", "draft", "create",  "make",   "build",    "add",    "remove",
    "delete", "update", "edit",    "change",  "set",     "schedule", "send",  "save",     "download", "upload",
    "analyze", "analyse", "review", "fix",    "run",     "test",    "convert", "plan",    "book",   "order",
    "track",  "log",    "monitor", "start",   "spawn",   "generate", "extract", "export", "import",
};
const char* const kAskLeads[] = {
    "please", "pls", "can you", "could you", "would you", "will you", "i need you to", "i want you to",
    "go and", "help me", "you should", "kindly",
};

bool startsWithWorkVerb(const std::string& t, size_t at) {
  while (at < t.size() && std::isspace((unsigned char)t[at])) at++;
  for (const char* v : kWorkVerbs) {
    const size_t n = std::strlen(v);
    if (t.compare(at, n, v) == 0 && (at + n >= t.size() || !isWordChar(t[at + n]))) return true;
  }
  return false;
}

bool workVerbAtASentenceStart(const std::string& t) {
  size_t at = 0;
  while (at < t.size()) {
    if (startsWithWorkVerb(t, at)) return true;
    const size_t next = t.find_first_of(".!?\n", at);
    if (next == std::string::npos) return false;
    at = next + 1;
  }
  return false;
}

bool workVerbAfterALead(const std::string& t) {
  for (const char* lead : kAskLeads)
    for (size_t at = findWord(t, lead); at != std::string::npos; at = findWord(t, lead, at + 1))
      if (startsWithWorkVerb(t, at + std::strlen(lead))) return true;
  return false;
}

bool mostlyNonAscii(const std::string& t) {
  size_t high = 0, letters = 0;
  for (char c : t) {
    high += ((unsigned char)c & 0x80) ? 1 : 0;
    letters += std::isalpha((unsigned char)c) ? 1 : 0;
  }
  return high > letters;
}

bool holdsALink(const std::string& t) {
  if (t.find("://") != std::string::npos || t.find("www.") != std::string::npos) return true;
  for (const char* tld : {".com", ".org", ".net", ".io", ".ai", ".gov", ".edu", ".co.uk"})
    for (size_t at = t.find(tld); at != std::string::npos; at = t.find(tld, at + 1))
      if (at > 0 && std::isalnum((unsigned char)t[at - 1]) &&
          (at + std::strlen(tld) >= t.size() || !std::isalpha((unsigned char)t[at + std::strlen(tld)])))
        return true;
  return false;
}

}  // namespace

namespace {

// Lane ECHO: the request's own words a reply must never carry back to the owner
// (Lumi v5.0.0, 10 Nov: "Reply.orch_turn.call must include all fields. Use empty
// strings/arrays where none. No em dashes. Concise. No new topics."). Tool names
// and turn fields in the folded spelling: '.', '-' and '_' are one separator on
// the wire (the head loop advertises memory_write, the MCP endpoint memory.write).
// Only the names the action table does not already carry (hasToolName scans it
// below, separator-flexibly, so every device.* / memory.* / files.* / ... key is
// covered there): the head loop's own turn/mem/session names and the bare noun
// "scratchpad". The class test iterates both tables, so a name missing from
// either fails it.
const char* const kEchoToolNames[] = {
    "orch.turn", "mem.write", "mem.query", "session.ops", "scratchpad",
};

// Contract phrases: the orch_turn tool description, the strict schema's required
// list, and the correction-round notes the device sends back in-round. Matching is
// plain substring, so a phrase that CONTAINS another kept one is dropped: the
// shorter one catches every sentence the longer would ("must include all fields"
// is caught by "include all fields"). The class test iterates the full original
// wording list, so a trim that loses coverage fails it.
const char* const kEchoContractPhrases[] = {
    "include all fields", "include every field", "include each field",
    "required fields", "empty string", "empty array",
    "orchestrator turn", "ends the turn",
    "mention this check", "mention the check", "do not mention it to the owner",
    "[reply check]", "[turn check]", "[promise check]", "[spawn refused]", "ran out of room",
    "[last attempt",
};

// A short rule sentence aimed at the assistant opens with one of these (or is a
// single word) and holds nothing but assistant-facing words below. Advice to the
// owner ("Don't add salt.", "Keep it short.", "No reply from them yet.") holds a
// word that is neither light nor a rule word and stays.
const char* const kEchoRuleOpeners[] = {
    "no", "never", "always", "be", "use", "keep", "avoid", "remember", "don't", "do", "note", "just",
    "reply", "concise",
};
// Words that only ever describe the assistant's own output, plurals included, as
// one blank-separated blob (one flash string, no pointer table, matched whole-word
// by findWord). Ordinary words an owner-facing answer can hold ("answer", "brief")
// are NOT here: a short sentence whose content is not entirely these words is an
// answer about the world, not an echo of the request.
const char kEchoRuleWords[] =
    "concise topic topics json field fields tool tools ascii emoji bullet bullets markdown recite "
    "orchestrator reply replies em dash dashes";
// Function words that carry no content of their own in a rule sentence, as one
// blank-separated blob. A word outside this set and the rule set is content: the
// sentence is an answer, not an echo.
const char kEchoLightWords[] =
    "it them that this the a an to one all any new only in of for from with on at by her his my your our "
    "their please be is are was do does did yes no or and not so if when while then than too very up out "
    "off there";

// The folded wire spelling: '.', '-' and '_' are one separator on the wire (the
// head loop advertises memory_write, the MCP endpoint memory.write), so fold them
// all to '.' once and match names with the plain whole-word findWord. The mapping
// is 1:1, so positions in the folded copy still index the original.
std::string foldSeparators(const std::string& t) {
  std::string out(t);
  for (char& c : out)
    if (c == '-' || c == '_') c = '.';
  return out;
}

bool hasSeparator(const char* name) { return std::strchr(name, '.') != nullptr; }

// A name in the folded spelling: '.', '-' and '_' in the name all fold to '.', so
// both wire spellings match the folded text ("device.orch_model" and
// "device_orch_model" both fold to "device.orch.model"). Names are short; the
// stack buffer avoids a heap string per row per sentence.
bool foldedNameIn(const std::string& n, const char* name) {
  char buf[48];
  const size_t len = std::strlen(name);
  if (len >= sizeof(buf)) return findWord(n, name) != std::string::npos;
  for (size_t i = 0; i < len; i++) {
    const char c = name[i];
    buf[i] = (c == '-' || c == '_') ? '.' : c;
  }
  buf[len] = '\0';
  return findWord(n, buf) != std::string::npos;
}

bool hasToolName(const std::string& t, bool allowBare) {
  const std::string n = foldSeparators(t);
  for (const char* name : kEchoToolNames) {
    // A bare feature noun ("scratchpad") is also a user-facing word: it counts only
    // inside a terse rule-shaped sentence, never in a longer answer
    // ("I saved it to your scratchpad.").
    if (!allowBare && !hasSeparator(name)) continue;
    if (foldedNameIn(n, name)) return true;
  }
  // The action table is the single source for the request-advertised action names
  // (device.*, turn.*, models.list, memory.*, ...): every row key is an echo when
  // it appears in either wire spelling, and a row added there is caught here with
  // no guard change - the class test iterates the same table.
  size_t rows = 0;
  const ActionRuleRow* table = actionRuleTable(rows);
  for (size_t i = 0; i < rows; i++) {
    const char* key = table[i].key;
    if (!key || !key[0] || !hasSeparator(key)) continue;
    if (foldedNameIn(n, key)) return true;
  }
  return false;
}

// A sentence ends at '.', '!', '?' or a newline - except a dot between two word
// chars, which is part of a token (orch_turn.call, 39.3, e.g.).
bool sentenceEndsAt(const std::string& t, size_t i) {
  const char c = t[i];
  if (c == '!' || c == '?' || c == '\n') return true;
  if (c == '.') {
    if (i > 0 && isWordChar(t[i - 1]) && i + 1 < t.size() && isWordChar(t[i + 1])) return false;
    return true;
  }
  return false;
}

size_t wordCount(const std::string& s) {
  size_t words = 0, at = 0;
  while (at < s.size()) {
    while (at < s.size() && !isWordChar(s[at])) at++;
    if (at >= s.size()) break;
    words++;
    while (at < s.size() && isWordChar(s[at])) at++;
  }
  return words;
}

std::string firstWord(const std::string& s) {
  size_t at = 0;
  while (at < s.size() && !isWordChar(s[at])) at++;
  const size_t b = at;
  while (at < s.size() && isWordChar(s[at])) at++;
  return s.substr(b, at - b);
}

// Both blobs are blank-separated single words, so the existing whole-word findWord
// matches them directly (no per-call string building).
bool isRuleWord(const std::string& word) { return findWord(kEchoRuleWords, word.c_str()) != std::string::npos; }

bool isLightWord(const std::string& w) { return findWord(kEchoLightWords, w.c_str()) != std::string::npos; }

bool opensWithRuleOpener(const std::string& w) {
  for (const char* o : kEchoRuleOpeners)
    if (w == o) return true;
  return false;
}

// The words after the opener: each must be light or a rule word. False on the
// first content word (the sentence is an answer, not an echo).
bool restIsRuleWords(const std::string& s, bool opens, bool* sawRule) {
  size_t at = 0;
  bool first = true;
  while (at < s.size()) {
    while (at < s.size() && !isWordChar(s[at])) at++;
    if (at >= s.size()) break;
    const size_t b = at;
    while (at < s.size() && isWordChar(s[at])) at++;
    const std::string word = s.substr(b, at - b);
    if (first) {
      first = false;
      if (opens && !isRuleWord(word)) continue;   // the opener itself carries no content
    }
    if (isLightWord(word)) continue;
    if (!isRuleWord(word)) return false;
    *sawRule = true;
  }
  return true;
}

// A rule sentence is terse (at most 5 words), opens with a rule opener (or is a
// single word), and every word after the opener is light or a rule word: the
// sentence's whole content is assistant-facing. Any other content word makes it
// an answer about the world ("No reply from them yet.", "No tools required.",
// "Keep it brief.") and it stays.
bool isRuleEcho(const std::string& s) {
  const size_t words = wordCount(s);
  if (words == 0 || words > 5) return false;
  const std::string w = firstWord(s);
  if (w.empty()) return false;
  const bool opens = opensWithRuleOpener(w);
  if (!opens && words != 1) return false;
  bool sawRule = false;
  return restIsRuleWords(s, opens, &sawRule) && sawRule;
}

// One lowercased sentence: does it address the assistant or quote the contract?
bool echoSentence(const std::string& s) {
  if (s.find_first_not_of(" \t\r\n") == std::string::npos) return false;
  // A terse rule-shaped sentence (at most 5 words, opening with a rule opener or a
  // single word) is where a bare tool name counts as an echo.
  const size_t words = wordCount(s);
  const bool ruleShaped = words >= 1 && words <= 5 && (opensWithRuleOpener(firstWord(s)) || words == 1);
  if (hasToolName(s, ruleShaped)) return true;
  for (const char* p : kEchoContractPhrases)
    if (s.find(p) != std::string::npos) return true;
  return isRuleEcho(s);
}

// The next sentence span [from, e) of lowercased `t`; e includes the terminator.
size_t nextSentence(const std::string& t, size_t from, size_t* e) {
  size_t stop = from;
  while (stop < t.size() && !sentenceEndsAt(t, stop)) stop++;
  *e = stop < t.size() ? stop + 1 : t.size();
  return from;
}

}  // namespace

size_t echoToolNameCount() { return sizeof(kEchoToolNames) / sizeof(kEchoToolNames[0]); }
const char* echoToolName(size_t i) { return kEchoToolNames[i]; }

bool echoSentenceSpan(const std::string& reply, size_t* begin, size_t* end) {
  const std::string t = lowered(reply);
  size_t from = 0;
  while (from < t.size()) {
    size_t e = 0;
    nextSentence(t, from, &e);
    if (echoSentence(t.substr(from, e - from))) {
      if (begin) *begin = from;
      if (end) *end = e;
      return true;
    }
    from = e;
  }
  return false;
}

// Only reached when something was dropped: collapse blank-line runs.
std::string collapseBlankRuns(const std::string& out) {
  std::string collapsed;
  collapsed.reserve(out.size());
  for (size_t i = 0; i < out.size();) {
    if (out[i] == '\n') {
      size_t j = i;
      int breaks = 0;
      while (j < out.size() && (out[j] == '\n' || out[j] == ' ' || out[j] == '\t' || out[j] == '\r')) {
        if (out[j] == '\n') breaks++;
        j++;
      }
      if (breaks >= 3) collapsed += "\n\n";
      else collapsed.append(out, i, j - i);
      i = j;
    } else {
      collapsed += out[i++];
    }
  }
  return collapsed;
}

std::string dropEchoSentences(const std::string& reply) {
  const std::string t = lowered(reply);
  // One pass: keep the non-echo sentences' original bytes, note whether any echo
  // was found. A clean reply is returned byte-exact.
  std::string out;
  out.reserve(reply.size());
  bool any = false;
  for (size_t from = 0; from < t.size();) {
    size_t e = 0;
    nextSentence(t, from, &e);
    if (echoSentence(t.substr(from, e - from))) any = true;
    else out.append(reply, from, e - from);
    from = e;
  }
  if (!any) return reply;
  const std::string collapsed = collapseBlankRuns(out);
  const char* ws = " \t\r\n";
  const size_t b = collapsed.find_first_not_of(ws);
  if (b == std::string::npos) return std::string();
  const size_t e = collapsed.find_last_not_of(ws);
  return collapsed.substr(b, e - b + 1);
}

bool asksForWork(const std::string& ownerText) {
  if (ownerText.find_first_not_of(" \t\r\n") == std::string::npos) return false;
  if (ownerText.size() > kChatMaxBytes) return true;
  if (mostlyNonAscii(ownerText)) return true;   // this reader cannot tell: never throttle it
  const std::string t = lowered(ownerText);
  return holdsALink(t) || hasAnyWord(t, kWorkAnywhere) || workVerbAtASentenceStart(t) || workVerbAfterALead(t);
}

namespace {

// Where later work is promised. The words alone prove little ("in the background" is often a
// place in a photo, "still working on" is often the owner's goal, a next turn can be on a road),
// so each kind says what else the sentence must hold.
enum class Cue : uint8_t {
  Turn,         // a later turn of this chat: any first person, or "this will"
  Round,        // the same words as a contest's or a loan's: a first-person future
  Report,       // telling the owner later: a first-person future (a routine can back it)
  Background,   // a first person doing work there, in the same clause
  Effort,       // "working on": only with a first person as its subject
  Wait,         // asks the owner to wait: stands alone
  WaitAlone,    // the same, only where it opens the sentence
};
struct LaterCue {
  const char* text;
  Cue kind;
};
const LaterCue kLaterCues[] = {
    {"next turn", Cue::Turn},          {"later turn", Cue::Turn},          {"following turn", Cue::Turn},
    {"my next message", Cue::Turn},    {"my next reply", Cue::Turn},       {"my next response", Cue::Turn},
    {"next round", Cue::Round},        {"next pass", Cue::Round},          {"report back", Cue::Report},
    {"get back to you", Cue::Report},  {"come back to you", Cue::Report},  {"circle back", Cue::Report},
    {"keep you posted", Cue::Report},  {"follow up with you", Cue::Report}, {"let you know when", Cue::Report},
    {"ping you when", Cue::Report},    {"update you when", Cue::Report},   {"in the background", Cue::Background},
    {"keep working on", Cue::Effort},  {"continue working on", Cue::Effort}, {"still working on", Cue::Effort},
    {"working on it", Cue::Effort},    {"hang tight", Cue::Wait},          {"bear with me", Cue::Wait},
    {"give me a moment", Cue::Wait},   {"give me a minute", Cue::Wait},    {"stand by", Cue::WaitAlone},
    {"one moment", Cue::WaitAlone},
};
const char* const kFirstPersonFuture[] = {
    "i'll", "i\xE2\x80\x99ll", "i will", "i'm going to", "i\xE2\x80\x99m going to", "i am going to", "we'll",
    "we\xE2\x80\x99ll", "we will", "and will",
};
// A later turn can also be promised with the work as the subject: "this will complete next turn".
const char* const kWorkFuture[] = {"this will", "that will", "it will", "will have", "will be"};
const char* const kFirstPersonNow[] = {"i'm", "i\xE2\x80\x99m", "i am"};
// The work "in the background" names, when it is work and not a place.
const char* const kBackgroundWork[] = {
    "run",      "running",  "check",  "checking", "work",   "working", "continue",   "continuing", "finish",
    "finishing", "process", "processing", "search", "searching", "do",  "doing",     "handle",     "handling",
    "monitor",  "monitoring", "research", "researching", "going",
};
// Words that may open a request to wait, and adverbs between a subject and its verb.
const char* const kWaitLeads[] = {"please", "just", "ok", "okay", "so", "alright", "now"};
const char* const kAdverbs[] = {"now", "also", "just", "currently", "then", "still"};
// After these, a next turn or round is a road's or a contest's.
const char* const kNotATurn[] = {"on", "left", "right", "of", "at"};
// The owner is a condition of it, or it asks: not a promise of unasked work.
const char* const kConditional[] = {
    "if you",    "if it",      "when you",  "once you", "after you", "tell me",  "send me", "let me know",
    "ask me",    "reply with", "want me",   "shall i",  "should i",  "would you", "do you",  "your next",
    "unless you", "next time you", "as soon as you",
};
// Negations: a sentence saying it will not happen is the honest one.
const char* const kNegation[] = {
    "not", "won't", "won\xE2\x80\x99t", "cannot", "can't", "can\xE2\x80\x99t", "nothing", "never", "no", "none",
    "isn't", "isn\xE2\x80\x99t", "aren't", "don't", "don\xE2\x80\x99t", "without",
};

// A cue with a negation in the 5 words before it, in the sentence.
bool negatedBefore(const std::string& s, size_t cueAt) {
  size_t words = 0, i = cueAt;
  while (i > 0 && words < 5) {
    while (i > 0 && !isWordChar(s[i - 1])) i--;
    const size_t e = i;
    while (i > 0 && isWordChar(s[i - 1])) i--;
    if (e == i) break;
    words++;
  }
  const std::string before = s.substr(i, cueAt - i);
  return hasAnyWord(before, kNegation);
}

// The word right after a cue, or "".
std::string wordAfter(const std::string& s, size_t from) {
  while (from < s.size() && s[from] == ' ') from++;
  size_t e = from;
  while (e < s.size() && isWordChar(s[e])) e++;
  return s.substr(from, e - from);
}

template <size_t N>
bool oneOf(const std::string& word, const char* const (&list)[N]) {
  for (const char* w : list)
    if (word == w) return true;
  return false;
}

// `s` up to `at`, without the adverbs that end it ("i'm still" + "working on it").
std::string beforeAdverbs(const std::string& s, size_t at) {
  size_t e = at;
  for (;;) {
    while (e > 0 && !isWordChar(s[e - 1])) e--;
    size_t b = e;
    while (b > 0 && isWordChar(s[b - 1])) b--;
    if (b == e || !oneOf(s.substr(b, e - b), kAdverbs)) break;
    e = b;
  }
  return s.substr(0, e);
}

template <size_t N>
bool endsWithAny(const std::string& s, const char* const (&list)[N]) {
  for (const char* words : list) {
    const size_t n = std::strlen(words);
    if (s.size() >= n && s.compare(s.size() - n, n, words) == 0 && (s.size() == n || !isWordChar(s[s.size() - n - 1])))
      return true;
  }
  return false;
}

bool firstPersonIsSubject(const std::string& s, size_t cueAt) {
  const std::string before = beforeAdverbs(s, cueAt);
  return endsWithAny(before, kFirstPersonFuture) || endsWithAny(before, kFirstPersonNow);
}

// The first person at work in the clause that holds the cue, or in the next one when the cue
// opens its clause ("In the background, I'll keep checking.").
bool workInTheBackground(const std::string& s, size_t cueAt, size_t cueLen) {
  size_t from = s.find_last_of(";,:", cueAt);
  from = from == std::string::npos ? 0 : from + 1;
  std::string clause = s.substr(from, cueAt - from);
  if (clause.find_first_not_of(" (\"'") == std::string::npos) clause = s.substr(cueAt + cueLen);
  return (hasAnyWord(clause, kFirstPersonFuture) || hasAnyWord(clause, kFirstPersonNow) || hasAnyWord(clause, kWorkFuture)) &&
         hasAnyWord(clause, kBackgroundWork);
}

// Only words that may open a request stand before the cue.
bool opensTheSentence(const std::string& s, size_t cueAt) {
  size_t i = 0;
  while (i < cueAt) {
    while (i < cueAt && !isWordChar(s[i])) i++;
    size_t e = i;
    while (e < cueAt && isWordChar(s[e])) e++;
    if (e > i && !oneOf(s.substr(i, e - i), kWaitLeads)) return false;
    i = e;
  }
  return true;
}

bool cueBacked(const LaterCue& cue, const std::string& s, size_t at) {
  const size_t len = std::strlen(cue.text);
  switch (cue.kind) {
    case Cue::Turn:
      if (oneOf(wordAfter(s, at + len), kNotATurn)) return false;
      return hasAnyWord(s, kFirstPersonFuture) || hasAnyWord(s, kWorkFuture) || hasAnyWord(s, kFirstPersonNow);
    case Cue::Round:
      return !oneOf(wordAfter(s, at + len), kNotATurn) && (hasAnyWord(s, kFirstPersonFuture) || findWord(s, "this will") != std::string::npos);
    case Cue::Report:     return hasAnyWord(s, kFirstPersonFuture);
    case Cue::Background: return workInTheBackground(s, at, len);
    case Cue::Effort:     return firstPersonIsSubject(s, at);
    case Cue::Wait:       return true;
    case Cue::WaitAlone:  return opensTheSentence(s, at);
  }
  return false;
}

bool sentencePromises(const std::string& s, LaterWork behind) {
  if (s.empty() || s.find('?') != std::string::npos || hasAnyWord(s, kConditional)) return false;
  for (const LaterCue& cue : kLaterCues) {
    if (behind == LaterWork::Standing && cue.kind == Cue::Report) continue;
    const size_t at = findWord(s, cue.text);
    if (at == std::string::npos || negatedBefore(s, at)) continue;
    if (cueBacked(cue, s, at)) return true;
  }
  return false;
}

}  // namespace

namespace {

// Lane QFIX: work a reply can say is happening right now, as a present participle.
const char* const kNowVerbs[] = {
    "logging",  "saving",   "recording", "writing",  "adding",   "updating", "storing",  "noting",  "entering",
    "setting",  "sending",  "creating",  "deleting", "removing", "changing", "pinning",  "scheduling",
};
// The storage verbs among them: opening a sentence and followed by what they store, they announce the
// write even without "now" ("Logging your lunch as roughly 670 kcal"); in a reply the head writes
// instead of calling the tool, that is the whole answer (58 such replies in this lane's live records).
const char* const kStoreVerbs[] = {"logging", "saving", "recording", "storing"};
// What a verb is followed by when it names what it works on: a pronoun, a meal word, a possessive (the
// owner's own item), or an article (a general noun: "Storing the loaf ..."; counted only with a figure).
const char* const kThings[] = {"it",     "that",   "this",  "them",  "these", "those",  "everything", "all",
                               "dinner", "lunch",  "breakfast", "snack", "supper", "brunch"};
const char* const kPossessives[] = {"your", "my"};
const char* const kArticles[] = {"the", "a", "an"};
// What may stand before the participle: "OK, logging it now", "Got it, saving that now", "I'm adding it now".
const char* const kNowLeads[] = {"ok",   "okay", "sure", "alright", "right",  "yes",   "great", "got", "it",    "i",
                                 "i'm",  "m",    "am",   "i'll",    "ll",     "will",  "let",   "me",  "going", "to",
                                 "gonna"};
// Among the leads, the ones that make the base form an announcement: "I'll log that now", "Let me save it now".
const char* const kFutureLeads[] = {"i'll", "ll", "will", "let", "going", "gonna"};
const char* const kBaseVerbs[] = {"log",   "save",   "record", "store",  "add",    "write",    "update", "enter", "set",
                                  "send",  "create", "delete", "remove", "change", "pin",      "schedule", "note", "put"};
// What may follow "now" in an announcement ("Logging it now as a snack", "... now from your description");
// anything else ("Removing the battery now is safe") means "now" belongs to a statement of its own.
const char* const kAfterNow[] = {"as",    "to",    "from", "in",    "into", "for",   "with",  "at",   "on",
                                 "and",   "so",    "then", "using", "by",   "under", "before", "after", "since",
                                 "because", "while"};
// A finite verb. A work verb that opens a sentence with one of these after it is a gerund SUBJECT
// ("Saving a bit of cash each month adds up", "Logging a workout takes ten seconds"), not an announcement,
// and so is the same sentence in the past tense ("Saving this note worked").
const char* const kFinite[] = {
    "is",      "are",     "was",     "were",   "be",     "been",    "am",      "has",     "have",   "had",
    "do",      "does",    "did",     "will",   "would",  "can",     "could",   "should",  "may",    "might",
    "must",    "shall",   "cannot",  "isn't",  "aren't", "wasn't",  "weren't", "doesn't", "don't",  "didn't",
    "won't",   "can't",   "couldn't", "wouldn't", "shouldn't",
    "keeps",   "kept",    "works",   "worked", "takes",  "took",    "makes",   "made",    "adds",   "means",
    "meant",   "helps",   "helped",  "gives",  "lets",   "costs",   "cost",    "requires", "needs", "depends",
    "saves",   "goes",    "went",    "gets",   "becomes", "became", "comes",   "came",    "uses",   "shows",
    "tells",   "lasts",   "lasted",  "protects", "prevents", "avoids", "ensures", "reduces", "increases",
    "improves", "matters", "counts", "runs",   "ran",    "starts",  "started", "stops",   "stopped", "turns",
    "turned",  "brings",  "brought", "stays",  "stayed", "remains", "remained", "allows", "caused", "causes",
    "creates", "leads",   "tends",   "seems",  "seemed", "looks",   "feels",   "appears", "covers", "includes",
    "holds",   "risks",   "frees",   "cools",  "wastes", "slows",   "speeds",  "pays",    "builds", "moves",
    "lists",   "stores",  "records", "logs",   "writes", "sends",   "removes", "deletes", "changes", "updates",
    "sets",    "pins",    "notes",
};
// Past forms of what the owner did or gave ("sent", "told", "gave", "showed", "wrote", "got", "used") are left out
// on purpose: "Logging the whole pack as you sent it" is an announcement with a subordinate clause, not a statement.

bool isClauseBreak(const std::string& s, size_t at) {
  const char c = s[at];
  if (c == ':' || c == ';' || c == ',' || c == '(') return true;
  // An em or en dash (E2 80 94, E2 80 93) breaks the clause too; a curly apostrophe (E2 80 99) does not.
  return c == '\xE2' && at + 2 < s.size() && s[at + 1] == '\x80' && (s[at + 2] == '\x94' || s[at + 2] == '\x93');
}

// The next word of `s` from `at` (advanced past it), "" at the end. `clauseEnd` is set when a
// colon, semicolon, comma, opening bracket or dash lay between the previous word and this one.
std::string nextWord(const std::string& s, size_t& at, bool& clauseEnd) {
  clauseEnd = false;
  while (at < s.size() && !isWordChar(s[at])) {
    if (isClauseBreak(s, at)) clauseEnd = true;
    at++;
  }
  const size_t b = at;
  while (at < s.size() && isWordChar(s[at])) at++;
  return s.substr(b, at - b);
}

// The first word of `s` that is not a lead (the leads are skipped); `future` is set when a future
// lead stood among them ("I'll", "let me").
std::string skipLeads(const std::string& s, size_t& at, bool& future) {
  bool stop = false;
  std::string w = nextWord(s, at, stop);
  while (!w.empty() && oneOf(w, kNowLeads)) {
    future = future || oneOf(w, kFutureLeads);
    w = nextWord(s, at, stop);
  }
  return w;
}

bool isThing(const std::string& w) {
  return oneOf(w, kThings) || oneOf(w, kPossessives) || oneOf(w, kArticles) ||
         (!w.empty() && std::isdigit((unsigned char)w[0]));
}

bool hasDigit(const std::string& s) {
  for (char c : s)
    if (std::isdigit((unsigned char)c)) return true;
  return false;
}

// A finite verb among the words of the clause that starts at `at`: the sentence has a predicate of its own.
bool finiteInClause(const std::string& s, size_t at) {
  for (;;) {
    bool stop = false;
    const std::string w = nextWord(s, at, stop);
    if (w.empty() || stop) return false;
    if (oneOf(w, kFinite)) return true;
  }
}

// A storage verb is announced when what it stores follows at once and the clause has no predicate of its own:
// a pronoun, a meal word, a possessive or a figure ("Logging your lunch as roughly 670 kcal"), or an article
// when the sentence also holds a figure ("Logging a granola bar at 210 kcal"; "Storing the loaf in a bin keeps
// it fresh" has a predicate, "Saving a bit of cash each month adds up" has no figure).
bool storesSomething(const std::string& s, size_t at) {
  bool stop = false;
  const std::string next = nextWord(s, at, stop);
  if (stop || !isThing(next)) return false;
  if (oneOf(next, kArticles) && !hasDigit(s)) return false;
  return !finiteInClause(s, at);
}

// What follows "now" (from `at`, just past it): the end of the sentence or the clause, or a word that
// continues an announcement. "Removing the battery now is safe" does not.
bool announcementContinuesAfterNow(const std::string& s, size_t at) {
  while (at < s.size() && std::isspace((unsigned char)s[at])) at++;
  if (at >= s.size() || s[at] == '.' || s[at] == '!' || isClauseBreak(s, at)) return true;
  bool stop = false;
  return oneOf(nextWord(s, at, stop), kAfterNow);
}

// "now" within six words of `at`, in the same clause, with no predicate of its own before it ("Saving that note
// will take a moment now"), and an announcement's continuation after it. Adding up numbers is arithmetic.
bool saysNowSoon(const std::string& s, size_t at, bool arithmetic) {
  for (int n = 0; n < 6; n++) {
    bool stop = false;
    const std::string w = nextWord(s, at, stop);
    if (w.empty() || stop || oneOf(w, kFinite)) return false;
    if (arithmetic && (w == "up" || w == "together")) return false;
    if (w == "now") return announcementContinuesAfterNow(s, at);
  }
  return false;
}

// "I'll log that now", "Let me save it now": the base verb names what it works on at once ("Let me update you
// on the status now" and "I'll send you the details now" report, they do not write).
bool baseVerbNamesAThing(const std::string& s, size_t at) {
  bool stop = false;
  return isThing(nextWord(s, at, stop)) && !stop;
}

// "logging it now", "saving that to your food log now", "I'll log that now": a work verb that opens the
// sentence (leads aside) with "now" soon after it; or a storage verb that opens it and is followed by what
// it stores. Not a question, not a condition, not a sentence with a predicate of its own.
bool announcesNowIn(const std::string& s) {
  if (s.find('?') != std::string::npos || hasAnyWord(s, kConditional)) return false;
  size_t at = 0;
  bool future = false;
  const std::string w = skipLeads(s, at, future);
  const bool participle = oneOf(w, kNowVerbs);
  if (!participle && !(future && oneOf(w, kBaseVerbs))) return false;
  if (!participle && !baseVerbNamesAThing(s, at)) return false;
  if (participle && oneOf(w, kStoreVerbs) && storesSomething(s, at)) return true;
  return saysNowSoon(s, at, w == "adding" || w == "add");
}

}  // namespace

bool announcesWorkNow(const std::string& reply, size_t* begin, size_t* end) {
  const std::string t = lowered(reply.size() > 16384 ? reply.substr(0, 16384) : reply);
  size_t from = 0;
  while (from < t.size()) {
    const size_t stop = t.find_first_of(".!?\n", from);
    const size_t e = stop == std::string::npos ? t.size() : stop + 1;
    if (announcesNowIn(t.substr(from, e - from))) {
      if (begin) *begin = from;
      if (end) *end = e;
      return true;
    }
    from = e;
  }
  return false;
}

bool ownerOnlyAsks(const std::string& ownerText) {
  const std::string t = lowered(ownerText);
  bool question = false;
  size_t from = 0;
  while (from < t.size()) {
    const size_t stop = t.find_first_of(".!?\n", from);
    const size_t e = stop == std::string::npos ? t.size() : stop + 1;
    const bool isQuestion = stop != std::string::npos && t[stop] == '?';
    size_t words = 0;
    for (size_t at = from; at < e;) {
      bool unused = false;
      if (nextWord(t, at, unused).empty()) break;
      words++;
    }
    if (isQuestion) question = true;
    else if (words >= 3) return false;   // a statement or an instruction: something to act on
    from = e;
  }
  return question;
}

namespace {

// Lane FIX501 (OIR-35), kept lean on purpose: the test-cyd image has under 1 KB of flash
// headroom, so the cue lists are single '|'-separated strings scanned in one pass. The
// pack-photo + landed-row conditions in the claim check carry the context. Fix pass:
// pack words match whole words only ("barbecue" is not "bar", "backpack" is not "pack"),
// the owner asking a how-question themselves ("how many bars is one serving?") suppresses
// the guard, and an ask cue counts only in a question - a '?' or an imperative
// ("tell me ...") anywhere in the reply - so a declarative that merely contains "how much"
// stays clean.
const char kPackAskCues[] =
    "how many|how much|did you eat|whole pack|whole pouch|all of it|portion";
const char kAskImperatives[] = "tell me|let me know";
// The owner's message sent a photo (the word, or the [image ...] tag).
const char kPhotoCues[] = "photo|[image";

bool hasAnyCue(const std::string& t, const char* cues) {
  for (const char* p = cues; *p;) {
    const char* q = std::strchr(p, '|');
    const size_t n = q ? (size_t)(q - p) : std::strlen(p);
    if (n && t.find(p, 0, n) != std::string::npos) return true;
    p = q ? q + 1 : p + n;
  }
  return false;
}

}  // namespace

bool packMealAskedHowMuch(const std::string& ownerText, const std::string& reply) {
  const std::string o = lowered(ownerText);
  if (!hasAnyCue(o, kPhotoCues)) return false;
  // the owner asked a how-question themselves: their question is answered, not corrected
  if (hasWord(o, "how")) return false;
  // a pack or label, as whole words ("barbecue"/"backpack" do not count)
  if (!(hasWord(o, "pack") || hasWord(o, "label"))) return false;
  // The question must be a question: an ask cue plus a '?' or an imperative
  // ("tell me ...", "let me know ..."), not a statement that merely contains a cue.
  const std::string t = lowered(reply);
  return hasAnyCue(t, kPackAskCues) &&
         (t.find('?') != std::string::npos || hasAnyCue(t, kAskImperatives));
}

const char kPackQuestionCorrection[] =
    "[REPLY CHECK] Never ask how much was eaten (undo: re-send the row with 0 kcal).";

bool laterPromiseSpan(const std::string& reply, size_t* begin, size_t* end, LaterWork behind) {
  if (behind == LaterWork::Live) return false;
  // The reply as written: no scan past the claim scan's bound.
  const std::string t = lowered(reply.size() > 16384 ? reply.substr(0, 16384) : reply);
  size_t from = 0;
  while (from < t.size()) {
    size_t stop = t.find_first_of(".!?\n", from);
    // "..." and decimals keep the sentence going only until the next real stop.
    const size_t e = stop == std::string::npos ? t.size() : stop + 1;
    if (sentencePromises(t.substr(from, e - from), behind)) {
      if (begin) *begin = from;
      if (end) *end = e;
      return true;
    }
    from = e;
  }
  return false;
}

const char kLaterPromiseLine[] =
    "(Note: nothing is running for that, so it will not continue on its own. Ask again to continue.)";

std::string laterCorrection(const std::string& excerpt) {
  std::string q;
  for (char c : excerpt) q += (c == '"' || (unsigned char)c < 0x20) ? ' ' : c;
  return "[PROMISE CHECK] Your reply promises later work: \"" + q +
         "\". After this answer nothing continues: no later turn is coming, and no sub-agent, routine or wakeup "
         "stands behind it. Do the work now with your tools, start a sub-agent or a routine for it, or say plainly "
         "what is not done and that the owner can ask to continue. Do not repeat anything already done, and do not "
         "mention this check.";
}

std::string cutRetryNote(const std::string& why, const std::string& partial) {
  std::string note =
      "[LAST ATTEMPT RAN OUT OF ROOM] Your last answer to this round stopped at the output limit (" + why +
      "), so nothing in it ran and the owner has not seen it. Answer again in far fewer words: one tool call at a "
      "time with short arguments, and a final reply under 300 words. Put anything longer in a file.";
  if (partial.empty()) return note + "\n";
  const int keep = utf8CapLen(partial.c_str(), (int)partial.size(), 600);
  // Brackets out: the excerpt can quote outside content, and it must not pose as a block
  // of this message or close the quote.
  std::string quoted = partial.substr(0, (size_t)keep);
  for (char& c : quoted) {
    if (c == '[') c = '(';
    if (c == ']') c = ')';
  }
  return note + " It began:\n" + quoted + "\n[END OF WHAT YOU WROTE]\n";
}

}  // namespace orch
}  // namespace nimbus
