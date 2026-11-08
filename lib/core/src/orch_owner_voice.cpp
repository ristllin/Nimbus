#include "nimbus/orch/owner_voice.h"

#include "nimbus/orch/word_match.h"   // whole-word phrase tests (shared with the food ledger)
#include "nimbus/mem_cap.h"            // utf8CapLen: a quoted excerpt never ends inside a character

namespace nimbus {
namespace orch {

namespace {

// The curly apostrophe (U+2019) as the straight one, so "didn’t" matches "didn't".
std::string straightApostrophes(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (i + 2 < s.size() && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
        (unsigned char)s[i + 2] == 0x99) {
      out += '\'';
      i += 2;
      continue;
    }
    out += s[i];
  }
  return out;
}

// What an owner writes when the head credited them with words or an act that was never
// theirs (OIR-50, 2 Nov: "I never said that", "where did I say that"). Only first-person
// denials that point at the claim: "I didn't tell you, but I got the job", "My boss never
// said that" and "I never sent you the invoice, can you remind me?" are not disputes.
const char* const kDisputePhrases[] = {
    "i never said that",       "i never said it",          "i never said so",        "i never said i",
    "i never said anything",   "i didn't say that",        "i did not say that",     "i didn't say i",
    "i did not say i",         "i never told you that",    "i never told you i",     "i never told you anything",
    "where i supposedly",      "when i supposedly",        "i supposedly said",      "i never mentioned that",
    "i never mentioned it",    "i never mentioned any",    "i never photographed that", "i never photographed it",
    "i never photographed any", "i never confirmed that",  "i never confirmed it",   "i didn't confirm that",
    "i did not confirm that",  "i never sent that",        "i never sent you that",  "i never sent you any",
    "that's not what i said",  "that is not what i said",  "that was not what i said", "that wasn't what i said",
    "when did i ever",         "where did i ever",         "when have i ever",       "since when did i",
};

// The question forms ("Where did I say that?"): a dispute only when the pointer at the
// claim ends the clause. "Where did I say the meeting was?" and "When did I tell you that
// I was moving?" ask the record something; the head answers them from it.
const char* const kDisputeQuestions[] = {
    "where did i say that",     "when did i say that",      "where did i say it",      "when did i say it",
    "where did i say so",       "when did i say so",        "where did i tell you that", "when did i tell you that",
    "where did i tell you so",  "where did i mention that", "when did i mention that", "where did i mention it",
    "where did i send that",    "when did i send that",     "when did i send you that", "where did i send you that",
};

// Does `phrase` occur in `lower` as whole words with only spaces and punctuation after it
// up to the clause's end?
bool endsClause(const std::string& lower, const std::string& phrase) {
  for (size_t at = lower.find(phrase); at != std::string::npos; at = lower.find(phrase, at + 1)) {
    if (at > 0 && words::isWordChar(lower[at - 1])) continue;
    size_t e = at + phrase.size();
    while (e < lower.size() && lower[e] == ' ') e++;
    if (e >= lower.size() || std::string(".,!?;:)\n").find(lower[e]) != std::string::npos) return true;
  }
  return false;
}

// Words too common to name a topic (4+ letters; shorter words never count).
const char* const kCommonWords[] = {
    "that", "this", "with", "have", "will", "your", "yours", "from", "they", "them", "their", "there",
    "then", "than", "what", "when", "where", "which", "while", "been", "were", "just", "like", "some",
    "more", "also", "here", "about", "know", "want", "need", "make", "sure", "good", "great", "time",
    "today", "tomorrow", "yesterday", "later", "earlier", "sometime", "soon", "please", "thanks", "thank",
    "really", "maybe", "something", "anything", "everything", "nothing", "would", "could", "should",
    "into", "over", "after", "before", "again", "only", "very", "much", "many", "each", "every", "other",
    "another", "because", "does", "doing", "done", "going", "come", "take", "give", "tell", "said", "says",
    "okay", "yeah", "sorry", "hello", "morning", "evening", "night", "week", "still", "even", "well",
    "back", "next", "last", "first", "right", "made", "ask", "asked", "message", "reply", "answer",
    "image", "photo", "attached", "voice", "note", "file", "sent", "nimbus", "can't", "don't", "it's",
    "i'm", "i'll", "you're", "let's", "mine", "ours", "these", "those", "being", "doesn't", "didn't",
    // sentiment, not a topic ("I love tech trivia" is about tech trivia; "Love the choice!" is not)
    "love", "loved", "loves", "lovely", "enjoy", "enjoyed", "hate", "prefer", "fancy", "nice", "cool",
    "awesome", "glad", "happy", "interesting", "honestly", "actually", "definitely", "probably",
};

// What an owner writes to point back at an earlier message, to ask after one that got no
// reply (a ping), or to ask about a span that holds it (a recap).
const char* const kBackReferences[] = {
    "earlier", "before", "i mentioned", "i said", "i asked", "i told you", "my last message",
    "my previous message", "my other message", "you didn't answer", "you did not answer",
    "you never answered", "didn't reply", "did not reply", "still waiting", "as i said",
    "are you there", "you there", "anyone there", "still there", "did you get", "did you see",
    "did you read", "any answer", "any reply", "any update", "any news", "ping",
    "so far", "have i had", "did i have", "have i eaten", "did i eat", "what have i", "what did i",
    "recap", "catch me up", "my total", "in total",
};

// An earlier message that asks the head something now is owed its answer: a question or a
// direct request. One deferred to some later moment ("ask me sometime") is a remark.
const char* const kDirectAsks[] = {
    "can you", "could you", "would you", "will you", "please", "tell me", "remind me", "let me know",
    "do you know",
};
// The first word of a clause that asks the head to act now ("Log my lunch", "Here is my
// lunch photo", "To be clear: forget the check-in"), after a lead-in word.
const char* const kRequestVerbs[] = {
    "log", "save", "add", "remember", "remind", "set", "delete", "remove", "forget", "stop", "check",
    "find", "show", "tell", "send", "update", "cancel", "book", "schedule", "track", "note", "record",
    "store", "write", "make", "give", "list", "look", "search", "start", "turn", "change", "edit",
    "move", "put", "keep", "read", "open", "pin", "replace", "fix", "explain", "summarize", "summarise",
    "translate", "convert", "count", "estimate", "plan", "help", "here",
};
const char* const kLeadIns[] = {"please", "also", "and", "then", "now", "just", "oh", "ok", "okay", "so", "hey", "hi"};
const char* const kDeferrals[] = {
    "sometime", "some time", "someday", "some day", "one day", "at some point", "another time",
    "whenever you",
};

// What a sentence does when it takes an earlier remark up rather than report a fact: a
// promise, an offer, an acknowledgement, a deferral, an aside (a question is one too).
const char* const kMoves[] = {
    "i'll", "i will", "i'd love", "want me to", "would you like", "shall i", "how about", "happy to",
    "let me know", "noted", "remember", "good to know", "sounds fun", "sounds like fun", "love that",
    "sometime", "some time", "anytime", "any time", "next time", "another time", "one day",
    "take you up", "by the way", "speaking of", "fun fact", "did you know", "here's one", "here is one",
};

bool commonWord(const std::string& w) {
  for (const char* c : kCommonWords)
    if (w == c) return true;
  return false;
}

// A word character, or an apostrophe inside a word ("didn't").
bool tokenChar(char c) { return words::isWordChar(c) || c == '\''; }

// The token of `lower` that starts at or after `i` (advanced past it), apostrophes at its
// ends trimmed; "" at the end of the text.
std::string nextToken(const std::string& lower, size_t& i) {
  while (i < lower.size() && !tokenChar(lower[i])) i++;
  const size_t b = i;
  while (i < lower.size() && tokenChar(lower[i])) i++;
  std::string w = lower.substr(b, i - b);
  while (!w.empty() && w.back() == '\'') w.pop_back();
  while (!w.empty() && w.front() == '\'') w.erase(0, 1);
  return w;
}

// A topic word: 4+ bytes, at least one letter, not a common word.
bool topicWord(const std::string& w) {
  if (w.size() < 4 || commonWord(w)) return false;
  for (char c : w)
    if (c >= 'a' && c <= 'z') return true;
  return false;
}

// The topic words of a lowercase text.
std::vector<std::string> topicWords(const std::string& lower) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < lower.size()) {
    std::string w = nextToken(lower, i);
    if (topicWord(w)) out.push_back(std::move(w));
  }
  return out;
}

bool holdsWord(const std::string& lower, const std::string& w) { return words::hasPhrase(lower, w); }

bool listed(const std::string& w, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (w == list[i]) return true;
  return false;
}

// A clause of `lower` (split at . ! ? : ; , and line breaks, after any leading "(...)" day
// or time label) opens with a request verb.
bool opensWithARequest(const std::string& lower) {
  size_t i = 0;
  while (i < lower.size() && (lower[i] == ' ' || lower[i] == '(')) {
    if (lower[i] == '(') {
      const size_t close = lower.find(')', i);
      if (close == std::string::npos) break;
      i = close;
    }
    i++;
  }
  constexpr size_t nVerbs = sizeof(kRequestVerbs) / sizeof(kRequestVerbs[0]);
  constexpr size_t nLeads = sizeof(kLeadIns) / sizeof(kLeadIns[0]);
  bool clauseStart = true;
  while (i < lower.size()) {
    if (std::string(".!?:;,\n").find(lower[i]) != std::string::npos) {
      clauseStart = true;
      i++;
      continue;
    }
    if (!tokenChar(lower[i])) {
      i++;
      continue;
    }
    const std::string w = nextToken(lower, i);
    if (!clauseStart || listed(w, kLeadIns, nLeads)) continue;
    if (listed(w, kRequestVerbs, nVerbs)) return true;
    clauseStart = false;
  }
  return false;
}

bool asksNow(const std::string& lower) {
  if (words::anyPhrase(lower, kDeferrals)) return false;
  return lower.find('?') != std::string::npos || words::anyPhrase(lower, kDirectAsks) || opensWithARequest(lower);
}

// This message brings the earlier ones up without naming them: a pointer back, a ping, a
// recap, or a bare question with no topic of its own ("Hello?", "??").
bool asksAfterEarlier(const std::string& now) {
  if (words::anyPhrase(now, kBackReferences)) return true;
  return now.find('?') != std::string::npos && topicWords(now).empty();
}

// The topic words of `earlier` that `now` (this message) does not hold; empty when the
// earlier message is owed its answer or shares a topic word with this one.
std::vector<std::string> unaskedWords(const std::string& earlier, const std::string& now) {
  std::vector<std::string> out;
  if (asksNow(earlier)) return out;
  for (const std::string& w : topicWords(earlier)) {
    if (holdsWord(now, w)) return std::vector<std::string>();
    out.push_back(w);
  }
  return out;
}

// A sentence echoes an earlier remark when it holds two of the remark's topic words (its
// only one, when it has one): "tech trivia", not "the bagel's weight" against "My weight
// was down a kilo". One shared noun is chance (the stress corpus, evidence/scope-corpus2).
bool echoes(const std::string& sentence, const std::vector<std::string>& topic) {
  std::vector<std::string> held;
  for (const std::string& w : topic) {
    bool again = false;
    for (const std::string& h : held) again = again || h == w;
    if (!again && holdsWord(sentence, w)) held.push_back(w);
  }
  const size_t need = topic.size() < 2 ? topic.size() : 2;
  return need > 0 && held.size() >= need;
}

// A sentence that makes a move on what it mentions (not one that reports a fact).
bool makesAMove(const std::string& sentence) {
  return sentence.find('?') != std::string::npos || words::anyPhrase(sentence, kMoves);
}

}  // namespace

int unansweredTopicSpan(const std::string& reply, const std::string& ownerText,
                        const std::vector<std::string>& unanswered, size_t* begin, size_t* end) {
  if (reply.empty() || unanswered.empty()) return -1;
  const std::string now = words::lowerAscii(straightApostrophes(ownerText));
  if (asksAfterEarlier(now)) return -1;
  // Spans are the reply's as written (straightApostrophes shortens a curly apostrophe to
  // one byte), so each sentence is folded on its own. No scan past the claim scan's bound.
  const std::string raw = reply.size() > 16384 ? reply.substr(0, 16384) : reply;
  for (size_t m = unanswered.size(); m-- > 0;) {   // the newest earlier message first
    const std::vector<std::string> topic =
        unaskedWords(words::lowerAscii(straightApostrophes(unanswered[m])), now);
    if (topic.empty()) continue;
    size_t from = 0;
    while (from < raw.size()) {
      const size_t stop = raw.find_first_of(".!?\n", from);
      const size_t e = stop == std::string::npos ? raw.size() : stop + 1;
      const std::string sentence = words::lowerAscii(straightApostrophes(raw.substr(from, e - from)));
      if (echoes(sentence, topic) && makesAMove(sentence)) {
        if (begin) *begin = from;
        if (end) *end = e;
        return (int)m;
      }
      from = e;
    }
  }
  return -1;
}

std::string replyScopeCorrection(const std::string& earlier, const std::string& excerpt) {
  auto quoted = [](const std::string& s, int cap) {
    const int keep = utf8CapLen(s.c_str(), (int)s.size(), cap);
    std::string q;
    for (char c : s.substr(0, (size_t)keep)) q += (c == '"' || (unsigned char)c < 0x20) ? ' ' : c;
    if ((size_t)keep < s.size()) q += "...";
    return q;
  };
  return "[REPLY SCOPE CHECK] Your reply takes up an earlier message of theirs that got no reply (\"" +
         quoted(earlier, 160) + "\"), which this message does not bring up: \"" + quoted(excerpt, 240) +
         "\". Answer only this message: leave the earlier one out entirely (no answer, acknowledgement, "
         "promise, offer or question about it). Do not mention this check.";
}

bool disputesAttribution(const std::string& text) {
  const std::string lower = words::lowerAscii(straightApostrophes(text));
  if (words::anyPhrase(lower, kDisputePhrases)) return true;
  for (const char* q : kDisputeQuestions)
    if (endsClause(lower, q)) return true;
  return false;
}

std::string renderOwnerVoiceHint(const std::string& text) {
  std::string out =
      "[REPLY SCOPE] Answer this message. An earlier question or request of theirs shown with [no reply] "
      "is still owed its answer, unless they left it for later. Any other earlier message (a remark, an "
      "invitation, one already answered) and a memory are context, not requests still waiting for an "
      "answer: the reply adds no question, offer or promise on a topic this message does not ask about. "
      "[OPEN QUESTIONS] keep their own rule.\n";
  if (disputesAttribution(text))
    out += "[DISPUTED CLAIM] This message says they never said or did something you said they did. "
           "Search the episodic log first. If no message of theirs says it, say that plainly and that "
           "you were wrong, and describe your earlier claim in your own words: no \"you told me\", "
           "\"you said\", \"you mentioned\", \"you photographed\" or \"you confirmed\" anywhere in the "
           "reply, not in quotes and not in the apology.\n";
  return out;
}

}  // namespace orch
}  // namespace nimbus
