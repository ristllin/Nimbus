#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <vector>

// model_catalog - the PORTABLE, capability-aware model catalog (host-tested via
// pio test -e native). It owns the pure decisions the old cap-8 harvest in
// src/agent/provider_verify.cpp did inline, plus everything that harvest could
// not express: per-model ROLE classification (orchestrator, sub-agent, embedding,
// vision, STT, TTS, image), CAPABILITY fields (from the provider API where it
// supplies them - Anthropic max_input_tokens/max_tokens, Mistral capabilities -
// and id-family heuristics otherwise), a size class S/M/L (shared with the
// fallback rule engine), and a stable JSON shape for GET /api/models.
//
// NO Arduino / store / TLS here. The device layer (src/agent/provider_verify.cpp)
// still owns the live GET /v1/models fetch + NVS cache; it hands the raw response
// body to parseModelsList() and serializes the result with modelsToJson(). Tests
// replay recorded fixtures per provider (test/test_model_catalog/).
//
// The 8-id cap is gone: every chat-capable model is listed (flagships first), and
// embedding/audio/image models now appear under their own roles instead of being
// dropped. A model the key cannot actually use is marked usable=false by the
// device usability probe and omitted from GET /api/models by default.
namespace nimbus {
namespace orch {

// Roles a model can fill. Bitmask - one model can carry several (a chat model is
// both orchestrator and sub-agent; a multimodal chat model adds vision).
enum ModelRole : uint16_t {
  RoleOrchestrator = 1u << 0,
  RoleSubAgent     = 1u << 1,
  RoleEmbedding    = 1u << 2,
  RoleVision       = 1u << 3,
  RoleStt          = 1u << 4,
  RoleTts          = 1u << 5,
  RoleImage        = 1u << 6,
};

// Wire capabilities. "streaming" is what the PROVIDER supports; the device itself
// still consumes responses non-streamed (see docs/provider-wire.md).
enum ModelCap : uint16_t {
  CapTools     = 1u << 0,  // function / tool calling (the turn contract needs this)
  CapVision    = 1u << 1,  // image input
  CapStreaming = 1u << 2,
  CapJson      = 1u << 3,  // structured / json output
  CapEmbedding = 1u << 4,
  CapAudioIn   = 1u << 5,  // speech-to-text
  CapAudioOut  = 1u << 6,  // text-to-speech
};

inline constexpr size_t kModelNameMax = 48;   // display name bytes kept (UTF-8 safe)
inline constexpr size_t kModelLineMax = 16;   // model line bytes kept

struct ModelInfo {
  std::string id;
  uint16_t    roles = 0;         // ModelRole bitmask
  uint16_t    caps = 0;          // ModelCap bitmask
  bool        usable = true;     // usability-probe verdict (true until a probe says otherwise)
  bool        probed = false;    // has the one-shot usability probe run for this id?
  uint32_t    ctxTokens = 0;     // input context window (API-reported or family heuristic)
  uint32_t    maxOutTokens = 0;  // 0 when unknown
  char        size = 0;          // 'S' | 'M' | 'L' | 0 (unclassified)
  std::string family;            // coarse id-family bucket ("gpt-5", "claude-opus", ...)
  bool        apiCaps = false;   // capability/window fields came from the API, not heuristics
  bool        deprecated = false;
  std::string upstream;          // cumulo only: upstream provider slug; "" otherwise
  // Lane F (the model store): what the provider says about the model, cleaned
  // (untrusted text). All optional: a provider that does not say leaves them empty.
  std::string name;              // display name ("Claude Opus 5.5"), at most kModelNameMax bytes
  std::string line;              // model line ("opus", "haiku", "fable"), Anthropic only today
  uint32_t    created = 0;       // release time, unix seconds (0 = not given)
  std::string life;              // lifecycle word: "active" | "deprecated" | "" (not given)
  uint32_t    retires = 0;       // announced shutdown or retirement, unix seconds (0 = none)
  bool        priced = false;    // the row carried its own price (the Cumulo router's)
  uint32_t    priceIn = 0;       // that price, micro-USD per 1M input tokens
  uint32_t    priceOut = 0;      // micro-USD per 1M output tokens

  bool hasRole(ModelRole r) const { return (roles & r) != 0; }
  bool hasCap(ModelCap c) const { return (caps & c) != 0; }
};

// ---- pure classification (also used by the fallback rule engine) -------------
// OpenAI generation number of a "gpt-<N>..." id (gpt-4o -> 4, gpt-5.5 -> 5,
// gpt-6-astra -> 6); 0 for anything else (o-series, gpt-realtime, other vendors).
// The ONE place the "gpt-5 and newer" rule lives: the size class, the vision
// heuristic, the family bucket, the context table, the Responses reasoning gate
// and the device harvest ordering all key on it, so a new generation (gpt-6-astra,
// 2026-09) is classified the day it appears instead of drifting per call site.
int gptGeneration(const std::string& id);
// Whether a Mistral model takes image input, by its name: the chat families with
// vision (small, medium, large, ministral, magistral, pixtral), never codestral,
// devstral, voxtral, the OCR, embedding or moderation models. The ONE Mistral vision
// name rule: the catalog's vision role and the head's native photos both read it
// (mistral-small-latest and mistral-large-latest both read a photo's pixels on the
// API, 26 Oct 2026), so the two can never disagree again.
bool mistralVisionByName(const std::string& id);
// True when `id` is in the provider's current flagship family - the ids the
// device lists first in the model dropdown (openai: gpt-5 and newer generations;
// zai: glm-5 and newer). Non-flagship families (gpt-4o, o-series, glm-4.x) are
// still usable, just not preferred.
bool isFlagshipFamily(const std::string& provider, const std::string& id);
// Coarse family bucket for an id (lowercased matching). "" if unrecognizable.
std::string modelFamily(const std::string& provider, const std::string& id);
// Size class 'S' | 'M' | 'L', or 0 when the id gives no signal.
char modelSizeClass(const std::string& provider, const std::string& id);
// Full classification of one id (roles + caps + size + family + heuristic window).
// Provider is the slug: openai | anthropic | mistral | cumulo | zai | custom.
ModelInfo classifyModel(const std::string& provider, const std::string& id);

// Like classifyModel but honours the Cumulo "<upstream>/<id>" convention (splits
// and classifies against the upstream, tagging ModelInfo.upstream). For any other
// provider it is classifyModel. Lets a caller rebuild a catalog from a bare id
// list (e.g. the harvested CSV) when a full /models body is unavailable.
ModelInfo classifyCatalogEntry(const std::string& provider, const std::string& id);

// ---- parse a provider /v1/models response into a catalog --------------------
// `body` may carry HTTP response headers ahead of the JSON (tolerated - the JSON
// body is located first). `alloc` backs the parse doc (device: PSRAM; host: null
// => default). Providers:
//   openai/anthropic - id list; Anthropic also reads max_input_tokens / max_tokens
//                      / capabilities when the API returns them (apiCaps=true).
//   mistral          - capabilities + deprecation + aliases metadata.
//   cumulo/zai       - OpenAI-compatible id list (cumulo tags each model's upstream
//                      when the id is prefixed "<upstream>/<id>" or upstreamHint set;
//                      the router's GET /router/models also carries a size_class
//                      metadata field, which overrides the size heuristic).
// Non-chat families are still returned, classified under their own role; nothing
// is capped. Flagships sort first. Returns the number of models parsed.
size_t parseModelsList(const std::string& provider, const std::string& body,
                       std::vector<ModelInfo>& out,
                       ArduinoJson::Allocator* alloc = nullptr);

// ---- usability probe verdict -------------------------------------------------
// A model your key cannot actually run must never appear (the phantom-model
// complaint). The device does one cheap probe call per candidate; this maps the
// probe's HTTP status + error body to a verdict. Unusable is reserved for the
// model itself being rejected (unknown model / no access), NOT a transient fault -
// a network/rate-limit/server error is Unknown and must not demote a model.
enum class ProbeVerdict { Usable, Unusable, Unknown };
ProbeVerdict probeVerdict(int httpStatus, const std::string& errBody);

// ---- serialization for GET /api/models --------------------------------------
// Append each model as an object to `arr`. When includeUnusable is false, models
// with usable=false are skipped (the default "a model your key cannot use never
// appears"). Roles/caps render as string arrays / boolean maps per the contract.
void modelsToJson(const std::vector<ModelInfo>& models, JsonArray arr,
                  bool includeUnusable);

// Inverse of modelsToJson: rebuild ModelInfo entries from a persisted /api/models
// model array (device NVS cache round-trip). Returns the number appended.
size_t modelsFromJson(JsonArrayConst arr, std::vector<ModelInfo>& out);

// Canonical role token list (index-stable), for the contract's top-level "roles".
const char* const* roleTokens(int& countOut);
// Token &lt;-&gt; bit helpers (used by tests + the fallback engine's capability predicate).
const char* roleToken(ModelRole r);
const char* capToken(ModelCap c);

// ==== lane F: the streamed fetch and the model store ===========================
// Every provider list (and the Cumulo router's, ~95 KB) is read as a STREAM: the
// reader hands each element of the body's "data" array over on its own, so a body of
// any size is parsed in bounded memory and a body cut short still yields every row
// that arrived whole (with `cut` set). The rows land in the model store, a LittleFS
// file per provider (/data/models_<slug>.txt, one line per model, no NVS), which is
// the ONE list the prompt's short list, the checks (model_selectable.h), the picker
// and GET /api/models read.

// ---- untrusted text ----------------------------------------------------------
// A model id the device may store, show and send: 1-64 chars of [A-Za-z0-9._:/-].
// Anything else (a control byte, quote, space, or a tab that would split a store
// line) drops the row.
bool cleanModelId(const std::string& id);
// Display text from an untrusted body: control bytes, tabs and DEL dropped, cut to at
// most `maxBytes` on a UTF-8 boundary.
std::string cleanModelText(const std::string& s, size_t maxBytes);
// "2026-10-07T18:00:00Z" or "2026-10-23" (UTC) to unix seconds; 0 when unreadable.
uint32_t parseIsoTime(const char* s);
// One slot per model across dot and dash spellings of its version: a '.' between two
// digits reads as '-' ("claude-opus-5.5" and "claude-opus-5-5" share the key
// "claude-opus-5-5"). Only the separator: "gpt-5.6" and "gpt-5.6-sol" stay distinct.
std::string sameModelKey(const std::string& id);
// Non-chat FAMILIES that cannot run the turn contract (embeddings, audio, image,
// moderation, deep research, fill-in-middle ...), by id substring, provider-
// independent: the one list the short lists, the checks and the router read.
bool neverChatFamily(const std::string& id);
// A router row the device can call at all: a selector some upstream runs
// (routableSelector) that is not the router's ":batch" price twin of a model.
bool routerRowKept(const std::string& selector);
// A router selector in its upstream's CURRENT generation (openai/zai: the flagship
// families; anthropic: anything but claude-3; mistral and the rest: all), the rows a
// cut list keeps first. The one rule router_catalog.h orders by.
bool routerFlagshipSelector(const std::string& selector);

// ---- the streamed body ---------------------------------------------------------
// The largest single "data" element kept (an Anthropic row with its capabilities is
// ~1.5 KB); a larger one is skipped and counted.
inline constexpr size_t kModelRowMaxBytes = 8192;

class ModelsBodyReader {
 public:
  // Called once per complete object element of the top-level "data" array.
  using RowFn = void (*)(void* ctx, const char* obj, size_t len);
  ModelsBodyReader(RowFn fn, void* ctx) : fn_(fn), ctx_(ctx) {}
  // Feed the next chunk. HTTP response headers may precede the JSON (skipped).
  void feed(const char* p, size_t n);
  bool complete() const { return closed_; }          // the top-level object closed
  bool hasMore() const { return hasMore_; }           // top-level "has_more": true
  const std::string& lastId() const { return lastId_; }   // top-level "last_id"
  size_t bytes() const { return bytes_; }
  size_t oversize() const { return oversize_; }       // elements over kModelRowMaxBytes

 private:
  void jsonChar(char c);
  void captureChar(char c);
  void stringChar(char c);
  void topChar(char c);
  bool topSeparator(char c);
  void nestedChar(char c);
  void topValueChar(char c);
  RowFn fn_;
  void* ctx_;
  size_t bytes_ = 0, oversize_ = 0;
  // header skip
  bool inJson_ = false, headers_ = false, lineEmpty_ = true;
  // JSON structure
  int depth_ = 0;
  bool inStr_ = false, esc_ = false, closed_ = false, started_ = false;
  bool inData_ = false;                // inside the top-level "data" array
  int dataDepth_ = 0;                  // the array's own depth
  bool capturing_ = false, skipping_ = false;
  std::string row_;
  // top-level key/value tracking (depth 1)
  bool expectKey_ = false, inKey_ = false, afterColon_ = false;
  std::string key_, val_;
  bool inVal_ = false, valStr_ = false;
  bool hasMore_ = false;
  std::string lastId_;
};

// The device's malloc sends every allocation of 128 B or more to PSRAM and keeps smaller
// ones on the scarce internal SRAM (main.cpp heap_caps_malloc_extmem_enable(128)). A
// model string longer than the in-object buffer (15 chars) but under 128 B would land
// there, once per row of an untrusted list: each such string is given 128 B of capacity,
// so a list of any length costs internal SRAM nothing. Harmless on the host.
inline constexpr size_t kPsramStringFloor = 128;
void keepOffInternalHeap(ModelInfo& m);
// dst = [p, p + n), on PSRAM when longer than the in-object buffer (capacity kept).
void assignOffInternal(std::string& dst, const char* p, size_t n);

// ---- one fetch's rows ------------------------------------------------------------
// The most rows one fetch holds before the store's own bound: far above any real
// provider or router list (the router's ~95 KB body has about 300 rows it can run),
// and a ceiling on a hostile body. Past it a router chat row of a current generation
// (routerFlagshipSelector) is still taken, up to twice the bound, so rows listed late
// behind thousands of others keep the default and each upstream's flagships.
inline constexpr size_t kHarvestCollectMax = 512;

struct ModelHarvest {
  std::string provider;
  uint32_t    now = 0;            // unix seconds; 0 = clock unset (no retire date applied)
  std::vector<ModelInfo> rows;
  size_t seen = 0;                // "data" elements read
  size_t hostile = 0;             // dropped: an id or row the device will not keep
  size_t retired = 0;             // dropped: past its announced shutdown
  size_t overflow = 0;            // dropped: past kHarvestCollectMax
  size_t oversize = 0;            // dropped: over kModelRowMaxBytes
  size_t notRun = 0;              // router rows no upstream runs, and ":batch" price twins
  bool   cut = false;             // a page did not arrive whole
  bool   hasMore = false;         // the provider holds pages that were not read
  ArduinoJson::Allocator* alloc = nullptr;   // the per-row parse (device: PSRAM)
};

// Classify one "data" element (its JSON object text) into `h`: roles, capabilities,
// name, line, dates, lifecycle and (router) price; hostile ids, non-agentic families,
// deprecated Mistral rows and models past their shutdown are left out and counted.
void harvestRow(ModelHarvest& h, const char* obj, size_t len);
// Feed a whole body (one page) through a reader into `h`; sets `cut` when the body did
// not close. Returns the reader's hasMore (the caller fetches the next page).
bool harvestBody(ModelHarvest& h, const char* body, size_t len, std::string* lastId = nullptr);
// After the last page: duplicates out (the first stays), flagships first (L, M, S,
// unclassified, input order kept within a rank).
void finishHarvest(ModelHarvest& h);

// ---- the model store: /data/models_<slug>.txt -----------------------------------------
// Line 1 is the header, "#models v1 fetchedAt=<unix s, 0 when the clock was unset>
// fw=<firmware version> total=<models listed> more=<not kept> cut=<0|1> src=<api|legacy>"
// (space-separated key=value; readers ignore keys they do not know). Every other line
// is one model, tab-separated:
//   id name line size created ctx maxOut life retires roles caps flags priceIn priceOut
// `id` is the selector (a router row's "<upstream>/<model>"), roles and caps are hex
// bitmasks, flags letters (u usable, p probed, a API capabilities, d deprecated) and the
// prices are empty when the row carried none. About 85 B per model.
// Bound: at most kModelStoreMaxRows models and kModelStoreMaxBytes per provider; what
// does not fit is counted in `more` (docs/reference/config-and-nvs.md states it).
inline constexpr size_t kModelStoreMaxRows  = 160;
inline constexpr size_t kModelStoreMaxBytes = 20480;

struct ModelStoreMeta {
  uint32_t    fetchedAt = 0;   // unix seconds of the fetch; 0 = unknown (clock unset, or legacy)
  std::string fw;              // firmware version that wrote the file
  uint32_t    total = 0;       // models the provider listed that the device could keep
  uint32_t    more = 0;        // of those, not kept (the bound)
  bool        cut = false;     // the list arrived incomplete (a cut body, unread pages)
  std::string src;             // "api" (a fetch) | "legacy" (merged from an older build)
  std::string choices;         // the short list (CSV) the prompt line and quick picks show
};

// "/data/models_<slug>.txt" for a safe slug (1-10 of [a-z0-9]), else "".
std::string modelStorePath(const char* slug);
std::string serializeModelStore(const ModelStoreMeta& meta, const std::vector<ModelInfo>& rows);
// Parse a store file. Rows that do not read back whole are skipped. A router (cumulo)
// row's selector is split into `upstream` + `id` again. False when `text` has no header.
bool parseModelStore(const std::string& provider, const std::string& text, ModelStoreMeta& meta,
                     std::vector<ModelInfo>* rows);
// The header alone (line 1), no rows read. False when there is none.
bool parseModelStoreHeader(const std::string& text, ModelStoreMeta& meta);
// One model line (no newline) into `out` (every field reset first). False when the line
// is not a whole model row.
bool parseModelStoreLine(const std::string& provider, const char* line, size_t len, ModelInfo& out);
// Each model row of a store text in file order, through ONE reused ModelInfo (no row
// vector: the device scans its cached text this way on every check). `fn` returns
// false to stop. Returns the rows visited.
template <typename Fn>
size_t forEachStoreRow(const std::string& provider, const std::string& text, Fn fn) {
  ModelInfo m;
  size_t n = 0, at = text.find('\n');   // line 1 is the header
  while (at != std::string::npos && at + 1 < text.size()) {
    const size_t b = at + 1;
    at = text.find('\n', b);
    const size_t e = at == std::string::npos ? text.size() : at;
    if (e == b || text[b] == '#' || !parseModelStoreLine(provider, text.data() + b, e - b, m)) continue;
    ++n;
    if (!fn(static_cast<const ModelInfo&>(m))) break;
  }
  return n;
}
// Bound rows to the store: at most `maxRows` and `maxBytes` once serialized. Trims by
// model LINE (each line's newest first, round-robin), never one whole line first (the
// Fable trim), then restores the input order. Returns how many rows were dropped.
size_t boundModelRows(std::vector<ModelInfo>& rows, size_t maxRows = kModelStoreMaxRows,
                      size_t maxBytes = kModelStoreMaxBytes);

// Whether a finished fetch may replace the stored list: never an empty one, and never
// one cut short in transit while a list is stored (the last good list stays). A list
// whose further pages were left unread (Anthropic past kModelPagesMax, 400 models) does
// store: every page it read arrived whole, the store keeps at most 160 rows anyway, and
// its header says the list is incomplete.
bool harvestStorable(const ModelHarvest& h, bool haveStored);
// The one log line every fetch writes: rows read, bytes, pages, kept of total, and
// whether the bound was reached, the body was cut short or pages were left unread.
std::string harvestLogLine(const ModelHarvest& h, const ModelStoreMeta& meta, size_t bytes,
                           int pages);

// ---- the older build's lists (the first boot of this build) ------------------------------
// v4.5.x kept the short list in NVS (mch_<slug>) and a JSON catalog in
// /data/mcat_<slug>.json, trimmed by size (Lumi's lost claude-fable-5-1, critique C3).
// Merge both into a store text: an existing store's rows (else the JSON catalog's),
// then every short-list id not yet there. The JSON file is only read (a rollback keeps
// reading it). "" when nothing new would be written.
std::string mergeLegacyLists(const std::string& provider, const std::string& storeText,
                             const std::string& mcatJson, const std::string& mchCsv,
                             const std::string& fw);
// The NVS short list may go only on a boot whose image was already valid when it
// started (never an OTA trial boot, which may still roll back) and only once the store
// holds the list.
inline bool legacyListErasable(bool imageValidAtBoot, bool haveStore, bool haveNvsList) {
  return imageValidAtBoot && haveStore && haveNvsList;
}

// ---- what the store offers ---------------------------------------------------------------
// The selector a pick sends: "<upstream>/<id>" for a router row, else the id.
inline std::string modelSelector(const ModelInfo& m) {
  return m.upstream.empty() ? m.id : m.upstream + "/" + m.id;
}
// A row that may be offered and accepted now for a chat role: usable, carries
// RoleOrchestrator or RoleSubAgent (`role`, 0 = either), and not past its retire date
// (`now` 0 = unknown: no date applies).
bool rowOffered(const ModelInfo& m, uint16_t role, uint32_t now);
// The short list a direct provider's prompt line and quick picks show, from its store
// rows: offered chat models the provider's own filter keeps (openai/zai: the current
// flagship generations; anthropic: claude-*; mistral: chat + tools), not deprecated,
// newest first, at most kDirectChoicesMax. "" when nothing qualifies. The router's is
// router_catalog::storeChoices.
inline constexpr size_t kDirectChoicesMax = 8;
std::string directChoices(const std::string& provider, const std::vector<ModelInfo>& rows,
                          uint32_t now);

// The concrete model behind a "-latest" alias, from a store text: the row whose id is
// `alias` names it (Mistral's /v1/models gives each alias the model it points at, e.g.
// mistral-large-latest -> "mistral-large-2512"; the harvest keeps that as the row's
// name). "" when `alias` is no -latest alias, no row holds it, or its name is not a
// model id (a display name such as "Claude Opus 5.5" never answers). Mistral's chat
// answers echo the alias it was asked for, never the model behind it (measured 26 Oct
// 2026), so this is how the device learns which model really answered.
std::string concreteModelName(const std::string& storeText, const std::string& alias);
// Does `id` end in "-latest" (a provider alias for its current model)?
bool isLatestAlias(const std::string& id);

// What a store text says about `id` taking image input: 1 = its row's API-reported
// capabilities include vision, 0 = they do not, -1 = no row for `id`, or its
// capabilities are the id heuristic's (an OpenAI or Z.ai list, a legacy merge), -2 = no
// list at all (an empty store text).
int storeImageInput(const std::string& provider, const std::string& storeText, const std::string& id);

// Honest freshness for the prompt's model line: "(list from 2026-10-22, may miss newer
// models)", "(list date unknown, may miss newer models)" for a store written while the
// clock was unset (or merged from an older build), "(built-in list, not fetched yet)"
// with no store.
std::string modelListNote(bool haveStore, const ModelStoreMeta& meta);
// Unix seconds from a raw clock reading, or 0 while the clock is not set yet (anything
// before 2020-09-13): the one "now" the store, its freshness and its retire dates use.
inline uint32_t calendarNowOrZero(int64_t t) { return t > 1600000000 ? (uint32_t)t : 0; }
// "YYYY-MM-DD" (UTC) of unix seconds; "" for 0.
std::string isoDate(uint32_t unixSeconds);

// ---- price per model ----------------------------------------------------------------------
// What a row costs, and where that figure comes from: the router's own price for a
// router row ("router"), else the built-in estimate for that model ("builtin",
// model_price_defaults.h), else "unknown" (the picker says "price unknown"). A
// provider-wide default is never shown as a model's price.
struct ModelPrice {
  uint32_t    in = 0, out = 0;   // micro-USD per 1M tokens
  const char* src = "unknown";   // "router" | "builtin" | "unknown"
};
ModelPrice modelPrice(const std::string& provider, const ModelInfo& m);

// One GET /api/models row with the store fields (name, line, created, life, retires)
// and the price (priceIn, priceOut, priceSrc: "router" | "builtin" | "unknown"). The
// chat roles are listed only while rowOffered() holds for them (`now`, 0 = unknown), so
// the picker never lists a model the checks refuse. The three-argument modelsToJson
// above stays for callers without a provider (nimbusd).
void modelRowToJson(const std::string& provider, const ModelInfo& m, JsonObject o, uint32_t now);
void modelsToJson(const std::string& provider, const std::vector<ModelInfo>& models,
                  JsonArray arr, bool includeUnusable, uint32_t now = 0);

}  // namespace orch
}  // namespace nimbus
