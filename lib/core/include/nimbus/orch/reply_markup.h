#pragma once
// reply_markup - keeps text that is not the owner's content out of every reply.
//
// The class: decoder-format leaks, reasoning, wrapper tags and tool-call markup
// reach a channel. Three seams cover every path:
//   1. the contract parse (repairLeakedParams, run inside parseTurn): a model
//      that slips from JSON tool input into its XML tool-call format ends `reply`
//      with "</reply>" or "</parameter>" and writes the LATER fields as
//      <parameter name="memory">...</parameter> blocks inside that string. The
//      tail is cut, and the fields it carried are recovered (only where the
//      document had none), so the turn's side effects are not lost.
//      Measured live on claude-opus-5 (2 of 32 turns, 17-18 Oct).
//   2. the shape read (stripReasoningPreamble / unwrapWholeReply, run first in
//      parseHeadReply): "<think>...</think>{json}" and "<orch_turn>{json}
//      </orch_turn>" are turns, not prose with raw JSON in them.
//   3. the reply seam (cleanReplyMarkup): every delivered string goes through a
//      closed deny-list, so whatever slipped past 1 and 2 never reaches the owner.
//
// Never eat content: unknown <word> tokens (<provider>, List<String>, a < b, <3)
// and anything inside a code span or fence stay byte-exact. Only the closed
// deny-list below is removed. Portable, Arduino-free, host-tested
// (test_reply_markup); a reply with no '<' is returned unchanged.

#include <ArduinoJson.h>

#include <cstddef>
#include <string>

#include "nimbus/orch/turn.h"   // TurnField / LeakRepair

namespace nimbus {
namespace orch {

// Formatting F1: cut leaked tool markup from the top-level strings (reply,
// memory, ask) of a parsed orch_turn and recover the fields the tail carried.
// The cut fires only on the STRUCTURAL rule: from the first marker to the end,
// the tail holds nothing but markers, whitespace and <parameter name="F"> value
// blocks. Prose that merely mentions a tag and goes on is left alone. A field
// present and non-null in the document is never overwritten; a container value
// that does not parse to its schema type is dropped (counted, not guessed).
// Nested strings (a spawn task, a memory content, scratchpad items) are cut at a
// leak too, but nothing is recovered from depth.
void repairLeakedParams(ArduinoJson::JsonObject root, LeakRepair& rep);
// The two halves of repairLeakedParams, for the parse normalizer: the top
// strings first (so a recovered field exists before container coercion), the
// nested strings after it (so a session_ops list repaired from a JSON string has
// its tasks checked too).
void repairLeakedTop(ArduinoJson::JsonObject root, LeakRepair& rep);
void repairLeakedNested(ArduinoJson::JsonObject root, LeakRepair& rep);

// Formatting F3: drop leading reasoning blocks (<think>, <thinking>,
// <reasoning>, any case, any attributes or namespace prefix) through their close
// tag. An unclosed block loses only its opening tag: losing a real answer is
// worse than showing reasoning. `strippedBytes` (optional) reports the bytes cut.
std::string stripReasoningPreamble(const std::string& text, size_t* strippedBytes = nullptr);

// Formatting F3: the WHOLE trimmed text is exactly one wrapper pair
// (<orch_turn>, <reply>, <answer>, <response>, <output>, <final>) -> its inner
// text. Anything else is returned unchanged.
std::string unwrapWholeReply(const std::string& text);

// Formatting F4, the reply seam. Closed lists, case-insensitive, any namespace
// prefix, code spans and fences skipped byte-exact:
//   reasoning   think, thinking, reasoning        block removed with its content
//   tool calls  function_calls, function_results, invoke, tool_call,
//               tool_result, tool_use, arg_key, arg_value, and <|...|> tokens
//                                                  block removed with its content
//   parameters  <parameter ...>, </parameter>      tag removed, text kept
//   orch_turn   <orch_turn>, </orch_turn>          tag removed, text kept
//   wrappers    reply, answer, response, output, final - removed only when they
//               open the text or close it (prose about the HTML <output> element
//               keeps its words)
// An unclosed block loses only its tag. Idempotent, never grows the text, and
// returns the input unchanged when it holds no '<'.
std::string cleanReplyMarkup(const std::string& text);

// The class predicate the tests (and a debug log line) share: does `delivered`
// carry text that is not the owner's content? A deny-list tag outside code, a
// <|...|> token, a wrapper pair around the whole text, or raw orch_turn JSON
// ({"reply": or "session_ops":[ outside code). `why` names the first hit.
bool hasStrayMarkup(const std::string& delivered, std::string* why = nullptr);

}  // namespace orch
}  // namespace nimbus
