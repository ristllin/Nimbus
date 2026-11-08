#pragma once
#include <string>

// owner_voice - the owner-voice rules at the point of use (lane MLQ, 8 Nov).
//
// The static role states them once (compose.cpp ORCH_OWNER_VOICE_RULE: never raise a
// topic the owner did not bring up; a time-sensitive brief cites the primary source),
// and the food playbook states the retraction rule (never repeat "you told me"). On the
// 48e166c2 owner-regression leg mistral-large-4-0 broke all three while
// claude-haiku-5-5 kept them: it follows the per-turn user message, where nothing said
// it (an earlier owner message that got no reply read as still waiting for one) or
// where the per-turn framing pulled the other way ([FRESH RESULTS]: "with only the
// sources they name"). These lines put each rule next to the message it governs, in
// the per-turn turn input of a real human message only: the cached blocks stay
// byte-identical, and a routine's prompt or a sub-agent's text never gets them.
namespace nimbus {
namespace orch {

// Does the owner's message dispute something the head said they said or did ("I never
// said that", "show me where I said it", "I didn't tell you that")? Lexical, whole
// words, case-insensitive; the curly apostrophe a phone keyboard types counts too.
bool disputesAttribution(const std::string& text);

// The per-turn lines for a human message (each ends with a newline): [REPLY SCOPE] on
// every one (answer this message; an earlier message, answered or not, and a memory are
// context, not topics to raise), and [DISPUTED CLAIM] when disputesAttribution(text)
// (search first; with no message of theirs behind the claim, say so and that you were
// wrong, in your own words, without the attribution wording, not even quoted).
std::string renderOwnerVoiceHint(const std::string& text);

}  // namespace orch
}  // namespace nimbus
