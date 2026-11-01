#pragma once
#include <cstddef>
#include <string>

// reply_guard - what a head's answer must never be, and how honest the turn's shape is.
//
//   - isPlaceholderReply: template filler written where an answer belongs. Hosted
//     Virtual Nimbus, 1 Nov: a forced final round on mistral-large-4-0 answered
//     {"reply":"placeholder",...} and the device delivered the word to the owner.
//   - asksForWork: a message that asks for work, not a chat line. The same turn was a
//     real task ("Use a web lookup ...") that the plain-chat cap of 2 tool rounds
//     throttled; lookups, reads and writes keep the configured rounds.
//   - laterPromiseSpan: a reply that promises work for later (a next turn, the
//     background, a report back) in a sentence the owner is not a condition of. Lumi,
//     31 Oct: "I'll rewrite the whole file to correct it next turn." Nothing runs after
//     a final answer unless a sub-agent, a routine or a wakeup does.
//   - cutRetryNote: what a round that ran out of room is told when it runs again.
//
// Deterministic token matchers over flash tables, English only like the claim scan.
// Portable, Arduino-free, host-tested (test_reply_guard).
namespace nimbus {
namespace orch {

// True when the whole reply is template filler. `ownerText`: a word the owner's own
// message holds is an answer ("Say the word placeholder").
bool isPlaceholderReply(const std::string& reply, const std::string& ownerText = std::string());

// The correction a placeholder answer gets (model-facing).
extern const char kPlaceholderCorrection[];

// The final-answer check over an orch_turn's JSON: its `reply` field read loosely.
// "" when it is an answer, else kPlaceholderCorrection.
std::string placeholderCheck(const std::string& finalTurnJson, const std::string& ownerText);

// True when the owner's message asks for work: an action, a lookup, a link, a long
// brief, or words this reader cannot judge. False only for what reads as plain chat.
bool asksForWork(const std::string& ownerText);

// The sentence holding a promise of later work, as a byte span; false when there is
// none. `begin`/`end` may be null.
bool laterPromiseSpan(const std::string& reply, size_t* begin = nullptr, size_t* end = nullptr);

// The owner line a promise with nothing behind it gets, and the correction it gets while
// a round can still run (model-facing; `excerpt` is the sentence).
extern const char kLaterPromiseLine[];
std::string laterCorrection(const std::string& excerpt);

// The note a round gets when it runs again after a cut. `why` is the cut's error line;
// `partial` what the model wrote before it (quoted back in part, "" = nothing).
std::string cutRetryNote(const std::string& why, const std::string& partial);

}  // namespace orch
}  // namespace nimbus
