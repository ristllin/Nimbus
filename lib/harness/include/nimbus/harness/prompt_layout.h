#pragma once
#include <ArduinoJson.h>

#include <string>

// prompt_layout - the cacheable shape of a head turn's prompt.
//
// A provider caches a request by its PREFIX: the bytes up to a cache point are
// billed at a fraction of the price when the next request starts with the same
// bytes. The head prompt is therefore composed in three parts, in this order:
//
//   1. static  - role and field guide, HOW YOU RUN, owner preferences,
//                capabilities, the memory explainer and the stable catalogs. It
//                carries the DATE only (no minute clock), so it changes when the
//                device config, the tool list or the day changes - not per turn.
//   2. summary - the conversation summary. It changes only at compaction.
//   3. dynamic - the minute clock, the speaker line, running memory, the tool
//                loop note, sessions, the recent conversation, the scratchpad and
//                the recalled memories: everything that changes per turn.
//
// compose.cpp joins the parts into the one `instructions` string the engine
// already hands every provider, separated by kPartSplit. A provider that can
// cache in parts splits it again (Anthropic: one system block per part; OpenAI:
// the static part as a developer message, the dynamic part leading the user
// message; Mistral: the system message, then the dynamic part leading the user
// message). A provider that sends the prompt as one string flattens it first, so
// the marker byte never reaches a wire (pinned by the wire tests).
namespace agent {
namespace layout {

// ASCII record separator: never part of a prompt section. join() strips it from
// every part, so the first two occurrences are always the real split points.
inline constexpr char kPartSplit = '\x1E';

struct PromptParts {
  std::string stat;      // the static block
  std::string summary;   // the conversation summary ("" = never compacted)
  std::string dyn;       // the per-turn block
  bool split = false;    // false = a legacy single string (everything in stat)
};

// static + kPartSplit + summary + kPartSplit + dynamic (marker bytes removed from
// each part first).
std::string join(const PromptParts& p);
// The inverse. A string with no marker is a legacy single prompt: stat = all of
// it, split = false (sub-agent briefs, the compaction prompt, host rigs).
PromptParts split(const std::string& instructions);
// The same text in the same order with the markers removed, for a provider path
// that sends the prompt as one string.
std::string flat(const std::string& instructions);
// The same text with each split shown as a labelled line, for the turn anatomy
// page and the PROMPT? console dump.
std::string display(const std::string& instructions);

// The user message a split prompt sends: the dynamic block, then the turn input.
std::string userWithDynamic(const PromptParts& p, const std::string& inputs);

// ---- Anthropic Messages ----------------------------------------------------
// Fill `system` with one text block per non-empty part. The static and summary
// blocks carry a cache point; the dynamic block (or, for a legacy string, the
// only block) carries one only when it is the sole block. Returns the number of
// cache points written (at most 2).
int putAnthropicSystem(ArduinoJson::JsonArray system, const PromptParts& p);
// Rewrite every string message content as one text block, so a message renders
// with the same bytes whether or not it is the last one (the one that carries
// the moving cache point).
void textAsBlocks(ArduinoJson::JsonArray messages);
// Put a cache point on the last content block of the last message. Within a
// turn each tool round then reads the previous round's request from the cache.
// Returns 1 when a point was written. Call textAsBlocks() first.
int cacheLastMessage(ArduinoJson::JsonArray messages);
// Count the cache points in a request body (tools, system and messages). The
// API accepts at most kAnthropicMaxBreakpoints.
inline constexpr int kAnthropicMaxBreakpoints = 4;
int countAnthropicBreakpoints(ArduinoJson::JsonVariantConst request);

// ---- OpenAI Responses ------------------------------------------------------
// Add the static block (and the summary, when present) to `input` as developer
// messages. The caller then adds the user message from userWithDynamic().
void putOpenAIDeveloper(ArduinoJson::JsonArray input, const PromptParts& p);

}  // namespace layout
}  // namespace agent
