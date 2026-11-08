#pragma once
#include <cstddef>
#include <string>
#include <vector>

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

// Does the owner's message dispute something the head said they said or did? A
// first-person denial that points at the claim ("I never said that", "I didn't say I",
// "That's not what I said", "When did I ever ...") or a question form whose pointer ends
// the clause ("Where did I say that?"). A recall question ("Where did I say the meeting
// was?"), someone else's words ("My boss never said that") and a plain fact ("I never
// sent you the invoice") are not. Lexical, whole words, case-insensitive; the curly
// apostrophe a phone keyboard types counts too.
bool disputesAttribution(const std::string& text);

// The per-turn lines for a human message (each ends with a newline): [REPLY SCOPE] on
// every one (answer this message; an earlier question or request shown with [no reply] is
// still owed unless left for later; any other earlier message and a memory are context),
// and [DISPUTED CLAIM] when disputesAttribution(text) (search first; with no message of
// theirs behind the claim, say so and that you were wrong, in your own words, without the
// attribution wording, not even quoted).
std::string renderOwnerVoiceHint(const std::string& text);

// The reply-scope check (OIR-06). An earlier owner remark that got no reply reads to a
// head as still waiting for one: on the 48e166c2 leg mistral-large-4-0 answered "I love
// tech trivia, ask me sometime." inside a reply that logged a lunch ("I'll quiz you
// sometime!") in 4 of 5 reps, and the per-turn line alone left 3 to 5 of 10. The check
// finds a sentence of `reply` that
//   - holds a topic word (4+ letters, not a common or sentiment word) of one of
//     `unanswered` (earlier owner messages with no reply, oldest first), and
//   - makes a move on it (a question, promise, offer, acknowledgement, deferral or
//     aside: "I'll", "noted", "want me to", "sometime", "by the way"), not one that
//     reports a fact ("You have had chicken salad and a latte, 870 kcal."),
// while that earlier message is a remark, not something owed now (a question or a direct
// request is owed unless deferred, "ask me sometime"), and this message (`ownerText`)
// neither shares a topic word with it nor asks after it (a pointer back: "I mentioned",
// "earlier"; a ping: "Hello?", "are you there", "did you get"; a recap: "so far", "have I
// had"; a bare question with no topic of its own). Returns the index of the earlier
// message taken up, or -1; `begin`/`end` (may be null) get the sentence's byte span.
int unansweredTopicSpan(const std::string& reply, const std::string& ownerText,
                        const std::vector<std::string>& unanswered, size_t* begin = nullptr,
                        size_t* end = nullptr);

// The correction the reply gets while a round can still run (model-facing).
std::string replyScopeCorrection(const std::string& earlier, const std::string& excerpt);

}  // namespace orch
}  // namespace nimbus
