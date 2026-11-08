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
// theirs (OIR-50, 2 Nov: "I never said that", "where did I say that"). Only the forms that
// dispute a claim: "I didn't tell you, but I got the job" and "show me where the file is"
// are not disputes, so "didn't tell you" and "show me where" alone never count.
const char* const kDisputePhrases[] = {
    "i never said that",     "i never said it",       "i never said i",      "never said that",
    "i didn't say that",     "i did not say that",    "i didn't say i",      "i did not say i",
    "i never told you that", "i never told you i",    "where did i say",     "where did i tell you",
    "when did i say",        "when did i tell you",   "where i supposedly",  "not what i said",
    "i never mentioned that", "i never photographed", "i never confirmed",   "i didn't confirm that",
    "i never sent that",     "i never sent you",
};

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
};

// What an owner writes to point back at an earlier message.
const char* const kBackReferences[] = {
    "earlier", "before", "i mentioned", "i said", "i asked", "i told you", "my last message",
    "my previous message", "my other message", "you didn't answer", "you did not answer",
    "you never answered", "didn't reply", "did not reply", "still waiting", "as i said",
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

// The topic words of `earlier` that `now` (this message) does not hold; empty when this
// message brings the earlier one up (a shared topic word, or a pointer back to it).
std::vector<std::string> unaskedWords(const std::string& earlier, const std::string& now) {
  std::vector<std::string> out;
  if (words::anyPhrase(now, kBackReferences)) return out;
  for (const std::string& w : topicWords(earlier)) {
    if (holdsWord(now, w)) return std::vector<std::string>();
    out.push_back(w);
  }
  return out;
}

}  // namespace

int unansweredTopicSpan(const std::string& reply, const std::string& ownerText,
                        const std::vector<std::string>& unanswered, size_t* begin, size_t* end) {
  if (reply.empty() || unanswered.empty()) return -1;
  const std::string now = words::lowerAscii(straightApostrophes(ownerText));
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
      for (const std::string& w : topic) {
        if (!holdsWord(sentence, w)) continue;
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
  return words::anyPhrase(words::lowerAscii(straightApostrophes(text)), kDisputePhrases);
}

std::string renderOwnerVoiceHint(const std::string& text) {
  std::string out =
      "[REPLY SCOPE] Answer this message only. An earlier message (answered, or shown with [no reply]) "
      "and a memory are context, not requests still waiting for an answer: the reply adds no question, "
      "offer or fact on a topic this message does not ask about. [OPEN QUESTIONS] keep their own rule.\n";
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
