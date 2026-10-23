#pragma once
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// prompt_layout - the cacheable shape of a head turn's request.
//
// A provider caches a request by its PREFIX: the bytes up to a cache point are
// billed at a fraction of the price when the next request starts with the same
// bytes. A head turn is therefore sent in this order, most stable first:
//
//   1. tools   - the full tool set, the same bytes on every round of every turn
//                of one head configuration (the forced final round included).
//   2. static  - role and field guide, HOW YOU RUN, owner preferences,
//                capabilities, the memory explainer and the stable catalogs. It
//                carries the DATE only (no minute clock).                 [cache]
//   3. shelf   - the skills shelf and the standing preferences: they change on
//                an owner or skill edit, not per turn ("" = none).        [cache]
//   4. history - the conversation as real messages: the summary pair (when the
//                chat was compacted), then one user and one assistant message
//                per past turn, appended turn by turn. Only a compaction, a
//                /clear or an overflow shift rewrites it.     [cache on the last]
//   5. the newest user message - the per-turn block (minute clock, speaker,
//                running memory, the tool-loop note, sessions, scratchpad,
//                recalled memories), then the turn input, then any photos.
//   6. in-turn rounds - tool calls and results, appended; the last block of the
//                request carries the moving cache point.
//
// compose.cpp joins parts 2-5 (and the turn's cache settings) into the one
// `instructions` string the engine already hands every provider, separated by
// kPartSplit; the history messages are introduced by kMsgSplit. A provider that
// caches in parts splits it again (Anthropic: system blocks, then the history
// messages; OpenAI: developer messages, then the history items; Mistral: the
// system message, then the history messages). A provider that sends the prompt
// as one string flattens it first (the history becomes a text block), so no
// marker byte ever reaches a wire (pinned by the wire tests).
namespace agent {
namespace layout {

// ASCII record separator between parts and group separator before each history
// message: never part of a prompt section. join() strips both from every text,
// so the markers are always the real split points.
inline constexpr char kPartSplit = '\x1E';
inline constexpr char kMsgSplit = '\x1D';

// One history message. Strict alternation, user first, assistant last.
struct Msg {
  bool user = true;
  std::string text;
};

// The cache settings of one head turn (the TTL table and the per-chat key).
struct CacheMeta {
  bool longTtl = false;   // Anthropic: the static, shelf and history points live 1 hour
  std::string key;        // prompt_cache_key for OpenAI and Mistral ("" = none)
};

struct PromptParts {
  std::string stat;           // the static block
  std::string shelf;          // skills shelf + standing preferences ("" = none)
  std::vector<Msg> history;   // the summary pair and the past turns, oldest first
  std::string dyn;            // the per-turn block
  CacheMeta cache;
  bool split = false;         // false = a legacy single string (everything in stat)
};

// static | shelf | history | dynamic | cache settings (marker bytes removed from
// every text first).
std::string join(const PromptParts& p);
// The inverse. A string with no marker is a legacy single prompt: stat = all of
// it, split = false (sub-agent briefs, the compaction prompt, host rigs).
PromptParts split(const std::string& instructions);
// The same text in the same order with the markers removed and the history as a
// "## RECENT CONVERSATION" block, for a provider path that sends one string.
std::string flat(const std::string& instructions);
// The history messages as that text block ("" when there are none).
std::string historyText(const std::vector<Msg>& history);
// The same text with each part shown under a labelled line, for the turn anatomy
// page and the PROMPT? console dump.
std::string display(const std::string& instructions);
// The labelled lines display() puts at the splits (the turn anatomy legend and
// the bench legs find the parts by them).
inline constexpr const char* kDisplayShelfLabel = "\n[----- static block ends; shelf block (skills and standing preferences) -----]\n";
inline constexpr const char* kDisplayHistoryLabel = "\n[----- conversation history: one message per line, user and assistant in turn -----]\n";
inline constexpr const char* kDisplayDynLabel = "\n[----- cached blocks end; newest user message: per-turn block -----]\n";

// The user message a split prompt sends: the dynamic block, then the turn input.
std::string userWithDynamic(const PromptParts& p, const std::string& inputs);
// The history pair a compacted chat starts with.
std::vector<Msg> summaryPair(const std::string& summary);
// Bytes the history messages carry (texts only).
size_t historyBytes(const std::vector<Msg>& history);

// ---- the TTL table (Anthropic) ----------------------------------------------
// A 5-minute cache point is refreshed by every request that reads it, so a chat
// whose turns come less than 5 minutes apart keeps it warm for free. Between 5
// and 60 minutes it expires before the next turn and every turn re-writes the
// whole prompt at 1.25x; a 1-hour point costs 2x to write and pays from its
// first read. Past an hour neither helps. So: 1 hour when the chat's median gap
// between turns is 5-60 minutes (at least kTtlMinGaps gaps seen), else 5
// minutes. The moving in-turn point is always 5 minutes (rounds are seconds
// apart), and a 1-hour point always comes before a 5-minute one, as the API
// requires.
inline constexpr uint32_t kLongTtlMinGapMs = 5u * 60u * 1000u;
inline constexpr uint32_t kLongTtlMaxGapMs = 60u * 60u * 1000u;
inline constexpr size_t kTtlMinGaps = 2;
bool longTtlForGaps(const uint32_t* gapsMs, size_t n);

// Turn start times per chat, for the TTL table. RAM only (a restart starts over
// on 5 minutes); a few chats, the oldest chat replaced first. Single task.
class TurnGaps {
 public:
  static constexpr size_t kChats = 4;
  static constexpr size_t kGaps = 7;   // the median of the last 7 gaps
  // Record a turn start in `chat` at `nowMs` (a monotonic clock that may wrap).
  void note(const std::string& chat, uint32_t nowMs);
  // The TTL table for `chat` over the gaps recorded so far.
  bool longTtl(const std::string& chat) const;
  size_t gapsFor(const std::string& chat) const;

 private:
  struct Chat {
    uint32_t key = 0;
    uint32_t lastMs = 0;
    uint32_t gaps[kGaps] = {};
    uint8_t n = 0, next = 0;
    bool used = false;
  };
  Chat chats_[kChats];
  uint8_t evict_ = 0;
  const Chat* find(uint32_t key) const;
};

// The per-chat cache key: "nb-" + 16 hex digits of a hash of the device and the
// chat (derived per turn, nothing stored).
std::string cacheKey(const std::string& device, const std::string& chat);

// ---- Anthropic Messages ----------------------------------------------------
// The cache_control object of one cache point ({"type":"ephemeral"}, plus
// "ttl":"1h" when longTtl).
void putCacheControl(ArduinoJson::JsonObject block, bool longTtl);
// Fill `system` with the static block and the shelf block, each with a cache
// point (the TTL from p.cache). A legacy string is one block with one point.
// Returns the number of cache points written (at most 2).
int putAnthropicSystem(ArduinoJson::JsonArray system, const PromptParts& p);
// Append the history messages (one text block each, never empty) to `messages`;
// the last one's block carries a cache point. Returns the points written (0-1).
int putAnthropicHistory(ArduinoJson::JsonArray messages, const PromptParts& p);
// Rewrite every string message content as one text block, so a message renders
// with the same bytes whether or not it is the last one (the one that carries
// the moving cache point).
void textAsBlocks(ArduinoJson::JsonArray messages);
// Put a 5-minute cache point on the last content block of the last message.
// Within a turn each tool round then reads the previous round's request from
// the cache. Returns 1 when a point was written. Call textAsBlocks() first.
int cacheLastMessage(ArduinoJson::JsonArray messages);
// Count the cache points in a request body (tools, system and messages). The
// API accepts at most kAnthropicMaxBreakpoints.
inline constexpr int kAnthropicMaxBreakpoints = 4;
int countAnthropicBreakpoints(ArduinoJson::JsonVariantConst request);
// True when no 1-hour point follows a 5-minute one in tools, system, messages
// order (the API refuses that order).
bool anthropicTtlOrderValid(ArduinoJson::JsonVariantConst request);

// ---- OpenAI Responses ------------------------------------------------------
// Add the static block and the shelf as developer messages and the history as
// message items to `input`. Explicit cache breakpoints (GPT-5.6 message-boundary
// caching) on the static block, the shelf and the last history user message;
// the request's implicit breakpoint takes the fourth write. The caller then adds
// the user message from userWithDynamic().
void putOpenAIInput(ArduinoJson::JsonArray input, const PromptParts& p);
// Cache breakpoints in an OpenAI request body (at most 3 explicit ones).
int countOpenAIBreakpoints(ArduinoJson::JsonVariantConst request);
// The request's cache key field (prompt_cache_key) when the turn has one.
void putCacheKey(ArduinoJson::JsonObject request, const PromptParts& p);

// ---- Mistral chat completions ----------------------------------------------
// The system message (static block, then the shelf) and the history messages,
// appended to `messages`. Mistral caches a matching prefix on its own; the
// request's prompt_cache_key keeps one chat's turns on one cache.
void putMistralPrefix(ArduinoJson::JsonArray messages, const PromptParts& p);

}  // namespace layout
}  // namespace agent
