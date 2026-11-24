#pragma once
#include <cstddef>
#include <cstdint>

// model_caps - the table of wire-shape facts for the engine-2 provider router (lane E2-R1,
// P/analysis-2026-11-24/ROUTER-DESIGN.md sections 3 and 9). Portable (NO Arduino), host-tested
// in test/test_model_caps.
//
// What a row holds: how a model's requests and answers are SHAPED (which photos it takes, which
// tool, reasoning and structured-output settings it accepts, how its ids, cache and usage work).
// What a row does NOT hold: windows, output caps, read windows, rates and prices. Those stay in
// the shared functions both engines read (headWindowTokens, outputCapFor, readWindowMsFor,
// builtinModelPrice, tx::budgetFor), and a host test proves every row resolves there.
//
// The data is generated: tools/model_caps/models.toml (each value with its source, the date it
// was read and whether it was verified) -> tools/model_caps/gen_caps.py (lints) ->
// model_caps_data.h (constexpr, committed). Edit the TOML, never the generated header.
//
// Lookup: capsFor(slot, model, routed) takes the slot's row whose id equals the model (also
// with one version or snapshot suffix, the money-safe rule of model_key.h), else the longest
// prefix row, else the slot's '*' row: the fail-safe defaults (no photos, no structured output,
// no effort, nothing a model might refuse). A routed head reads its upstream's row with the
// router's limits laid over it. The per-boot refused-field memory only ever narrows a row.
//
// Nothing in the firmware calls this yet (lane E2-R1 switches no consumer).
namespace nimbus {
namespace orch {
namespace caps {

// ---- enums (each one byte; the TOML spells them in snake case) ------------------------------
enum class Slot : uint8_t { Anthropic = 0, OpenAI, Mistral, Zai, CustomChat, CustomMessages };
inline constexpr size_t kSlotCount = 6;

enum class Dialect : uint8_t { Messages = 0, Responses, Chat };
enum class Auth : uint8_t { XApiKey = 0, Bearer, None };
enum class CacheKey : uint8_t { None = 0, PromptCacheKey };
enum class MatchKind : uint8_t { Exact = 0, Prefix, Any };

// How the output cap field is sent: always (with the shared function's value), required (the
// wire refuses a request without it), never, or omitted direct with a ceiling when routed.
enum class CapMode : uint8_t { SendAlways = 0, Required, Omit, OmitDirectCeilRouted };
enum class CapParam : uint8_t { MaxTokens = 0, MaxOutputTokens };
enum class IdRule : uint8_t { Passthrough = 0, AnthropicPattern, Mistral9 };
enum class Structured : uint8_t { None = 0, AnthropicOutputConfig, ResponsesTextFormat, ChatJsonSchema };
enum class Thinking : uint8_t { Off = 0, Adaptive, AlwaysOn, BetweenTools };
enum class Replay : uint8_t { None = 0, SignedBlocks, EncryptedItems, ThinkChunks, ReasoningContent };
enum class ReplayScope : uint8_t { OpenTurn = 0, AllTurns };
enum class CacheStyle : uint8_t { None = 0, Automatic, Explicit4, OpenaiBreakpoints, OpenaiImplicit, KeyHint };
enum class UsageMode : uint8_t { InputIncludesCache = 0, InputExcludesCache };

// ---- bitmasks ---------------------------------------------------------------------------------
namespace input {
inline constexpr uint8_t kText = 1, kImage = 2;
}
namespace imgfmt {
inline constexpr uint8_t kJpeg = 1, kPng = 2, kGif = 4, kWebp = 8;
}
namespace choice {
inline constexpr uint8_t kAuto = 1, kNone = 2, kAny = 4, kRequired = 8, kNamed = 16;
}
namespace effort {
inline constexpr uint8_t kNone = 1, kMinimal = 2, kLow = 4, kMedium = 8, kHigh = 16, kXHigh = 32, kMax = 64;
}
// Message-order rules. Every bit but kUserAfterToolOk is a restriction the planner keeps; the
// fail-safe default is every restriction on and the permission off.
namespace order {
inline constexpr uint8_t kEndsOnUser = 1, kResultsFirstInUser = 2, kNoEmptyText = 4, kMergeUsers = 8,
                         kUserAfterToolOk = 16, kNotOnlySystem = 32;
}
namespace flag {
inline constexpr uint8_t kImageInToolResult = 1, kFileRef = 2, kTools = 4, kLongCacheOk = 8, kDropUnderRung = 16;
}

// ---- rows (layouts pinned: gen_caps.py sizes the table with these figures) -------------------
// Strings are offsets into kCapsPool (0 is the empty string).
struct ModelRow {
  uint16_t match;            // pool offset of the id or prefix ("" for the '*' row)
  Slot slot;
  MatchKind kind;
  CapMode capMode;
  CapParam capParam;
  uint8_t inputKinds;        // input::
  uint8_t imageFormats;      // imgfmt::
  uint16_t imageMaxKiB;      // largest decoded image
  uint16_t imagesMax;        // images per request
  uint8_t flags;             // flag::
  uint8_t toolChoice;        // choice:: the values the model accepts
  IdRule idRule;
  Structured structured;
  Structured summarizerStructured;
  Thinking thinking;
  uint8_t effortSet;         // effort:: the values the model accepts (0 = send no effort field)
  Replay replay;
  ReplayScope replayScope;
  CacheStyle cacheStyle;
  uint16_t cacheMinTok;
  UsageMode usageMode;
  uint8_t orderRules;        // order::
};

struct ProviderRow {
  Slot slot;
  Dialect dialect;
  Auth auth;
  CacheKey cacheKey;
  uint16_t host;             // pool offsets
  uint16_t host2;            // a second host tried after the first ("" = none)
  uint16_t basePath;
  uint16_t versionHeader;    // the anthropic-version value ("" = none sent)
  uint16_t emptyText;        // placeholder for an empty history text ("" = none needed)
  uint16_t toolFrameBytes;   // extra bytes per tool over the Anthropic shape
  uint16_t routedImagesMax;  // the router's per-request image count for this upstream (0 = not routable)
  uint8_t markerSet;         // which marker rows are this provider's
  uint8_t pad;
};

// Error markers per provider (design section 9). Code and Type match the body's code or type
// field exactly; Text matches the message case-insensitively. status 0 = any status.
enum class MarkerKind : uint8_t { Code = 0, Type, Text };
enum class ErrClass : uint8_t {
  Transport = 0, RateLimited, Server, KeyAuth, KeyForbidden, Billing, Context, RefusedField,
  UnknownModel, BadRequest, Refusal
};
struct MarkerRow {
  uint8_t markerSet;
  MarkerKind kind;
  ErrClass cls;
  uint8_t pad;
  uint16_t status;
  uint16_t text;             // pool offset
};

// ---- the refused-field memory (per boot, owned by the caller; design section 9) --------------
// One resend without a refused optional field, remembered for the boot. Eight entries of 8 B,
// kept by the caller (the exchange's PSRAM state), so the table adds no static RAM.
namespace refused {
inline constexpr uint16_t kImage = 1, kEffort = 2, kThinking = 4, kStructured = 8, kChoiceNone = 16,
                          kCacheBreakpoint = 32, kReplay = 64, kFileRef = 128, kCapField = 256;
inline constexpr uint16_t kAll = 511;
}  // namespace refused

class RefusedFields {
 public:
  static constexpr size_t kEntries = 8;
  // Remember that (slot, model, routed) refused `fields` (refused:: bits, OR-ed into what it
  // already refused). When every entry is taken the oldest is replaced.
  void note(Slot slot, const char* model, bool routed, uint16_t fields);
  // The fields (slot, model, routed) refused this boot (0 = none).
  uint16_t fieldsFor(Slot slot, const char* model, bool routed) const;
  void clear();

 private:
  struct Entry {
    uint32_t key;    // 0 = free
    uint16_t mask;
    uint16_t pad;
  };
  Entry e_[kEntries] = {};
  uint8_t next_ = 0;
};

// ---- the lookup ---------------------------------------------------------------------------------
struct Caps {
  const ProviderRow* provider = nullptr;
  const ModelRow* matched = nullptr;   // the table row the lookup chose
  ModelRow row{};                      // that row with the routed overlay and the refusals applied
  bool routed = false;                 // false for a custom slot whatever was asked
  bool known = false;                  // matched an id or prefix row, not the '*' row
};

// The tables a lookup reads (the generated one in the firmware; a test may pass its own).
struct CapsTable {
  const ModelRow* models;
  size_t modelCount;
  const ProviderRow* providers;
  size_t providerCount;
  const MarkerRow* markers;
  size_t markerCount;
  const char* pool;
  size_t poolBytes;
};

const CapsTable& builtinCaps();

// The capabilities of `model` on `slot` (an upstream slot for a routed head: "anthropic/x" on
// the router is (Anthropic, "x", true)). `refused` may be null.
Caps capsFor(Slot slot, const char* model, bool routed, const RefusedFields* refused = nullptr);
Caps capsFrom(const CapsTable& t, Slot slot, const char* model, bool routed, const RefusedFields* refused);

// A row with the refused fields taken away (only ever narrows; test_model_caps proves it).
ModelRow narrowed(const ModelRow& r, uint16_t refusedMask);

// The router's limits laid over an upstream row: no provider file ids, at most 3 MiB per
// decoded image, at most the router's image count for the upstream.
inline constexpr uint16_t kRoutedImageMaxKiB = 3072;
ModelRow routedOverlay(const ModelRow& r, const ProviderRow& p);

// "anthropic", "openai", "mistral", "zai" and "custom" (CustomChat) to a slot; false otherwise
// ("cumulo" is not a slot: split its selector with resolveRouterRoute and pass the upstream).
bool slotFromHost(const char* host, Slot* out);
const char* slotName(Slot s);   // the TOML's name: "anthropic", ..., "custom_chat", "custom_messages"

// A pool string of `t` ("" for an offset outside the pool).
const char* poolStr(const CapsTable& t, uint16_t off);

// The marker rows of a provider, contiguous in the table.
struct MarkerSpan {
  const MarkerRow* rows;
  size_t count;
};
MarkerSpan markersOf(const CapsTable& t, const ProviderRow& p);

}  // namespace caps
}  // namespace orch
}  // namespace nimbus
