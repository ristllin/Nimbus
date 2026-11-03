#include "nimbus/orch/reply_guard.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

#include "nimbus/mem_cap.h"   // utf8CapLen - a quoted excerpt stays valid UTF-8

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
bool memoryIdIn(const std::string& t) {
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

bool jobIdIn(const std::string& t) { return t.find("result-job") != std::string::npos; }

bool storageWordingIn(const std::string& t) {
  return t.find("read back confirmed") != std::string::npos ||
         t.find("stored on the device as") != std::string::npos;
}

// A device file path: a run of path bytes with a slash, ending in a file suffix.
// URLs are external, not plumbing: a match preceded by "http", "://" or "www."
// does not count.
bool devicePathIn(const std::string& t) {
  static const char* const kSuffix[] = {".md", ".csv", ".txt"};
  for (const char* suf : kSuffix) {
    for (size_t at = t.find(suf); at != std::string::npos; at = t.find(suf, at + 1)) {
      const size_t end = at + std::strlen(suf);
      if (end < t.size() && isWordChar(t[end])) continue;
      const size_t slash = t.rfind('/', at);
      if (slash == std::string::npos) continue;   // a bare name, not a path
      size_t b = slash;
      while (b > 0 && (isWordChar(t[b - 1]) || t[b - 1] == '.' || t[b - 1] == '-')) b--;
      if (slash == b) continue;                   // "/foo.md": no name before the slash
      // A URL is external, not plumbing: the token may carry its own host
      // ("www.site.com/a.md") or sit behind a scheme ("https://host/a.md").
      const std::string tok = t.substr(b, end - b);
      const std::string pre = t.substr(b > 12 ? b - 12 : 0, b > 12 ? 12 : b);
      if (tok.rfind("www.", 0) == 0 || pre.find("://") != std::string::npos) continue;
      return true;
    }
  }
  return false;
}

}  // namespace

bool hasPlumbing(const std::string& reply) {
  const std::string t = lowered(reply);
  return memoryIdIn(t) || jobIdIn(t) || storageWordingIn(t) || devicePathIn(t);
}

std::string plumbingCheck(const std::string& finalTurnJson) {
  JsonDocument d;
  if (deserializeJson(d, finalTurnJson) != DeserializationError::Ok) return std::string();
  const char* reply = d["reply"] | "";
  if (!hasPlumbing(reply)) return std::string();
  return "[REPLY CHECK] Your reply shows internal plumbing: a memory or job id, a file "
         "path, or storage wording like \"read back confirmed\". The owner reads plain "
         "words only. Rewrite the answer without them: say what the thing is for the "
         "owner, or leave it out. Do not mention this check.";
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
