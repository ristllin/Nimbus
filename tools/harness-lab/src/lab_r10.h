#pragma once
#include <string>
#include <vector>

// lab_r10 - engine 2 against the real providers (lane E2-R10, router design section 12). PAID: every
// request reaches the provider with a real key.
//
//   nimbus-lab --engine=2 r10 <provider> <model> <out dir> <dotenv file> <key var> <parts> [photo model]
//
// <provider> anthropic | openai | mistral | zai. The key is read from the dotenv file inside this
// process (never from the environment, never printed). <parts> is a comma list:
//   engine    three owner turns through the plain loop (nimbusd's PlainRig, the real system text and
//             tools): S1 a text turn, S2 a tool call and its result, S3 a second turn; the chat is
//             saved as <out dir>/<provider>/chat.tx for a handoff
//   handoff:<file>   one owner turn on this head over a chat another head stored (S9)
//   codec     the route goldens' transcripts rendered by the codec for this head and sent as they
//             are: S4 the guard round, S5 a 64-token cap, S6 a small PNG, S7 a wrong key and an
//             unknown model, S8 the structured summarizer, S10 photos after view results, plus
//             foreign ids and reasoning, a stored cut answer and a second turn after tools
// Every request and answer body is written under <out dir>/<provider>/ (no header, so no key).
namespace lab {
int cmdR10(const std::vector<std::string>& args);
}
