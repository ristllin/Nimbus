#include "nimbus/orch/owner_voice.h"

#include "nimbus/orch/word_match.h"   // whole-word phrase tests (shared with the food ledger)

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
// theirs (OIR-50, 2 Nov: "I never said that", "where did I say that").
const char* const kDisputePhrases[] = {
    "i never said",       "never said that",   "i didn't say",      "i did not say",
    "i never told you",   "never told you that", "i didn't tell you", "i did not tell you",
    "where did i say",    "where i said",      "where i supposedly", "when did i say",
    "show me where",      "not what i said",   "i never mentioned", "i didn't mention",
    "i never sent",       "i never photographed", "i never confirmed", "i didn't confirm",
};

}  // namespace

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
