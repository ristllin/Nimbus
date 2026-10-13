#pragma once
// reply_text - the answer text of a provider reply, whatever shape its content takes.
//
// A chat-completions message.content is usually a string, but a reasoning model can
// answer with a list of chunks: Mistral's mistral-large-4-0 through the Cumulo router
// returns [{"type":"thinking","thinking":[...]},{"type":"text","text":"..."}] (bench,
// 13 Oct 2026), and every reader that cast content to a string read "" and reported
// "couldn't read its result" for a helper that had run and charged. An Anthropic
// Messages reply can likewise lead with a thinking block before its text.
#include <ArduinoJson.h>

#include <string>

namespace nimbus {
namespace orch {

// chat-completions message.content: the string itself, or the "text" chunks of a
// chunk list joined in order ("thinking" and every other chunk type skipped).
// Anything else (null, a number, a list with no text chunk) -> "".
std::string chatContentText(ArduinoJson::JsonVariantConst content);

// Anthropic Messages "content": every {"type":"text"} block's text, joined in order
// with a newline ("thinking", "tool_use" and other blocks skipped). A plain string
// is returned as is. Anything else -> "".
std::string messagesContentText(ArduinoJson::JsonVariantConst content);

}  // namespace orch
}  // namespace nimbus
