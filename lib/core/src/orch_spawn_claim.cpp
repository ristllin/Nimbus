#include "nimbus/orch/spawn_claim.h"

#include <cstring>
#include <vector>

namespace nimbus {
namespace orch {

const char kSpawnNotStartedLine[] =
    "(Correction: the sub-agents did not start. Ask me again and I will retry.)";
const char kSpawnSomeNotStartedLine[] =
    "(Correction: some of the sub-agents did not start. Ask me again and I will retry them.)";

namespace {

bool wordByte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '\'' || c == '-';
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// 0 = not a curly quote; 1 = opening (U+201C, E2 80 9C); 2 = closing (U+201D).
int curlyAt(const std::string& s, size_t i) {
  if ((unsigned char)s[i] != 0xE2 || i + 2 >= s.size() || (unsigned char)s[i + 1] != 0x80) return 0;
  const unsigned char c = (unsigned char)s[i + 2];
  return c == 0x9C ? 1 : c == 0x9D ? 2 : 0;
}

// The reply with code spans and quoted passages blanked out (the head quotes
// the owner, and quotes are not its own claims). Curly quotes count too.
std::string unquoted(const std::string& s) {
  std::string out = s;
  bool code = false, quote = false, curly = false;
  for (size_t i = 0; i < out.size(); i++) {
    const char c = out[i];
    const int cq = (code || quote) ? 0 : curlyAt(out, i);
    if (cq) {
      curly = cq == 1;
      out[i] = out[i + 1] = out[i + 2] = ' ';
      i += 2;
      continue;
    }
    if (c == '`' && !quote && !curly) code = !code;
    else if (c == '"' && !code && !curly) quote = !quote;
    else if (!code && !quote && !curly) continue;
    out[i] = ' ';
  }
  return out;
}

bool inSet(const std::string& w, const char* const* set) {
  for (const char* const* p = set; *p; p++)
    if (w == *p) return true;
  return false;
}

// Start verbs, as one word, or two ("kicked off").
const char* const kVerbs[] = {"started", "start", "starting", "starts", "spawned", "spawn",
                              "spawning", "spawns", "launched", "launch", "launching",
                              "dispatched", "dispatching", "deployed", "deploying", "restarted",
                              "respawned", "relaunched", nullptr};
const char* const kVerbsWithOff[] = {"kicked", "kick", "kicking", "fired", "fire", "firing",
                                     "set", "sent", "send", "sending", nullptr};
const char* const kNouns[] = {"sub-agent", "sub-agents", "subagent", "subagents", "researcher",
                              "researchers", nullptr};
// A bare "agent" is a sub-agent only after a count or a determiner ("three
// agents", "an agent"), never after another noun ("the travel agent").
const char* const kAgentQualifiers[] = {"a", "an", "the", "new", "more", "these", "those", "one",
                                        "two", "three", "four", "five", "six", "seven", "eight",
                                        "nine", "ten", "eleven", "twelve", "several", "parallel",
                                        nullptr};
// Second word after "research" / "background" that makes a sub-agent noun.
const char* const kNounTails[] = {"task", "tasks", "job", "jobs", "agent", "agents", "search",
                                  "searches", "run", "runs", nullptr};
const char* const kNegations[] = {"not", "no", "never", "didn't", "couldn't", "haven't", "hasn't",
                                  "wasn't", "weren't", "won't", "can't", "cannot", "don't",
                                  "doesn't", "unable", "failed", "nothing", "none", nullptr};
// A sentence about work started earlier is not about this turn.
const char* const kEarlier[] = {"still", "already", "earlier", "previously", "yesterday",
                                nullptr};

// Offers and conditions: the verb is not a report of something done.
const char* const kOffers[] = {"want me to", "shall i", "should i", "would you like", "do you want",
                               "if you", "if needed", "if that", "i can", "i could", "can i",
                               "let me know", "once you", "when you", "happy to", "could you",
                               nullptr};

bool countWord(const std::string& w) {
  if (w.empty()) return false;
  for (char c : w)
    if (c < '0' || c > '9') return inSet(w, kAgentQualifiers);
  return true;   // a digit run: "3 agents"
}

const char* const kResearchDeterminers[] = {"the", "deep", "this", "some", "your", "new", nullptr};

bool nounAt(const std::vector<std::string>& w, size_t i) {
  const std::string& x = w[i];
  const std::string prev = i > 0 ? w[i - 1] : std::string();
  const std::string next = i + 1 < w.size() ? w[i + 1] : std::string();
  if (inSet(x, kNouns)) return true;
  if (x == "agent" || x == "agents" || x == "worker" || x == "workers") return countWord(prev);
  const bool qualifier = x == "research" || x == "background" || x == "parallel";
  if (qualifier && inSet(next, kNounTails)) return true;
  // "the research", "deep research": the work itself, started as sub-agents.
  if (x == "research") return inSet(prev, kResearchDeterminers);
  return x == "sub" && (next == "agent" || next == "agents");
}

std::vector<std::string> wordsOf(const std::string& sentence) {
  std::vector<std::string> w;
  std::string cur;
  for (char c : sentence) {
    if (wordByte(c)) cur += lower(c);
    else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
  }
  if (!cur.empty()) w.push_back(cur);
  return w;
}

bool offerOrCondition(const std::string& sentence) {
  std::string low;
  low.reserve(sentence.size());
  for (char c : sentence) low += lower(c);
  for (const char* const* p = kOffers; *p; p++)
    if (low.find(*p) != std::string::npos) return true;
  return false;
}

// "put" starts something only with a sub-agent noun right after it ("put the
// six sub-agents into the spawn field"), never "put the research task on hold".
const char* const kParkWords[] = {"hold", "queue", "aside", "pause", "paused", "later", "list", nullptr};

bool putAt(const std::vector<std::string>& w, size_t i) {
  if (w[i] != "put") return false;
  for (const std::string& x : w)
    if (inSet(x, kParkWords)) return false;
  for (size_t k = i + 1; k < w.size() && k <= i + 3; k++)
    if (inSet(w[k], kNouns)) return true;
  return false;
}

bool verbAt(const std::vector<std::string>& w, size_t i) {
  if (inSet(w[i], kVerbs) || putAt(w, i)) return true;
  return inSet(w[i], kVerbsWithOff) && i + 1 < w.size() && w[i + 1] == "off";
}

// Words that start a new clause: a verb and a noun on either side of one are
// not one claim ("I started drafting while three agents reviewed it").
const char* const kClauseBreaks[] = {"while", "but", "although", "though", "whereas", "because",
                                     "whilst", "and", nullptr};

bool clauseClaims(const std::vector<std::string>& w) {
  for (size_t i = 0; i < w.size(); i++) {
    if (!verbAt(w, i)) continue;
    const size_t lo = i >= 6 ? i - 6 : 0;
    const size_t hi = i + 9 < w.size() ? i + 9 : w.size();
    for (size_t k = lo; k < hi; k++)
      if (k != i && nounAt(w, k)) return true;
  }
  return false;
}

bool sentenceClaims(const std::string& sentence, bool question) {
  if (question || offerOrCondition(sentence)) return false;
  const std::vector<std::string> w = wordsOf(sentence);
  // A sentence that negates anything ("are not running", "did not start") or
  // talks about earlier work is not a report of a spawn this turn.
  for (const std::string& x : w)
    if (inSet(x, kEarlier) || inSet(x, kNegations)) return false;
  std::vector<std::string> clause;
  for (const std::string& x : w) {
    if (!inSet(x, kClauseBreaks)) {
      clause.push_back(x);
      continue;
    }
    if (clauseClaims(clause)) return true;
    clause.clear();
  }
  return clauseClaims(clause);
}

}  // namespace

bool replyClaimsSpawn(const std::string& reply) {
  if (reply.empty()) return false;
  const std::string s = unquoted(reply);
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    const char c = i < s.size() ? s[i] : '\n';
    if (c != '.' && c != '!' && c != '?' && c != '\n' && c != ';') continue;
    if (i > start && sentenceClaims(s.substr(start, i - start), c == '?')) return true;
    start = i + 1;
  }
  return false;
}

}  // namespace orch
}  // namespace nimbus
