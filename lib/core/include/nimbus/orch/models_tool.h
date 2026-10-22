#pragma once
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/tool_registry.h"

// models_tool - the read-only models.list tool (owner ruling R5: "give the model
// an option to pull latest models"). The head asks which models a provider offers
// instead of reading a long per-provider list in every prompt: the prompt's
// [AVAILABLE MODELS] block shrinks to one pointer line on a turn where the tool
// is callable (modelsPointerLine).
//
// The tool never opens a connection. It reads the stored lists through
// ModelsToolHandlers::load, and refresh:true only asks for a refresh through
// requestRefresh, which sets a RAM flag; the device's main loop runs the refresh
// between turns on the self-deleting verify task (model_refresh.h holds that
// policy). The rows a turn reads are therefore the cached list with its fetchedAt,
// and the answer says when a refresh was queued.
//
// Also here, because the tool, the orch_model check and the spawn check must say
// the same thing: the "not in <provider> list" texts (an id the cached list does
// not hold, and a router id named on a direct provider, L19-39).
//
// Portable, Arduino-free, host-tested (test_orch_tools, test_models_refresh).
namespace nimbus {
namespace orch {

// The most rows one models.list answer carries (the rest are counted in "more").
constexpr size_t kModelsListMaxRows = 20;

// One model as the head reads it. Unknown values stay empty or 0 and are sent as
// null, so every row carries every field.
struct ModelRow {
  std::string id;          // the id orch_model and a spawn take (cumulo: "<upstream>/<model>")
  std::string name;        // display name from the provider ("" when it gives none)
  std::string line;        // model line or family ("claude-sonnet", "gpt-6", "glm-5")
  char        size = 0;    // 'S' | 'M' | 'L' | 0 (unknown)
  uint32_t    released = 0;   // release date, unix seconds (0 = unknown)
  uint32_t    ctx = 0;        // input context, tokens (0 = unknown)
  std::string lifecycle;      // "active" | "legacy" | "deprecated" ("" reads as active)
  uint16_t    roles = 0;      // ModelRole bitmask (model_catalog.h); 0 = chat roles
  uint32_t    priceIn = 0;    // micro-USD per 1M input tokens (0 with source "none")
  uint32_t    priceOut = 0;   // micro-USD per 1M output tokens
  std::string priceSource;    // "owner" | "router" | "provider" | "est" | "none"
};

// One provider's stored list.
struct ProviderModels {
  std::string provider;            // the slot slug
  uint32_t    fetchedAt = 0;       // unix seconds of the last list fetch (0 = never, or clock unset)
  bool        fetched = false;     // false: no fetched list yet, the rows are the built-in list
  std::vector<ModelRow> rows;      // flagships first
  uint32_t    more = 0;            // models the stored list itself left out for size
};

// What a refresh request did (ModelRefreshQueue::request, model_refresh.h).
enum class RefreshAsk : uint8_t {
  Queued,    // a refresh is now queued for the next quiet moment
  Pending,   // one was already queued or is running
  Recent,    // one started within the hour (the rate limit): nothing queued
  NoKey,     // the provider has no key: nothing to refresh
};
// "queued" | "pending" | "recent" | "no-key" (the tool, the web route and the log).
const char* refreshAskToken(RefreshAsk a);

// The tool's arguments.
struct ModelsListQuery {
  std::string provider;        // "" = every provider with a key
  std::string line;            // keep rows whose line contains this (case-insensitive)
  std::string query;           // keep rows whose id or name contains this (case-insensitive)
  std::string role;            // a ModelRole token ("orchestrator", "vision", ...); "" = chat models
  bool        includeLegacy = false;   // also legacy and deprecated rows
  bool        refresh = false;         // ask for a refresh of the named provider(s)
};
// Read the arguments; false with `err` for a wrong type or an unknown provider or role.
bool parseModelsListQuery(ArduinoJson::JsonObjectConst a, ModelsListQuery& q, std::string& err);

// The refresh answer for one provider, reported beside its fetchedAt.
struct RefreshNote {
  std::string provider;
  RefreshAsk  ask = RefreshAsk::Pending;
};

// The tool result, a compact JSON object:
//   {"providers":[{"provider","fetchedAt","source","total","refresh"}],
//    "models":[{"provider","id","name","line","size","released","ctx","lifecycle",
//               "price":{"in","out","source"}}],
//    "more":<rows that matched but were not shown>, "note":...}
// At most kModelsListMaxRows rows; with several providers they are taken in turn
// so one long list (the router's) cannot crowd out the rest. Prices are USD per
// 1M tokens; dates are UTC ("2026-10-22", fetchedAt "2026-10-22T14:05Z", null
// when unknown).
std::string modelsListJson(const std::vector<ProviderModels>& lists, const ModelsListQuery& q,
                           const std::vector<RefreshNote>& refresh);

// The device side of the tool. None of these may block or open a connection.
struct ModelsToolHandlers {
  // Provider slots with a key, registry order.
  std::function<std::vector<std::string>()> keyedProviders;
  // The stored list of one provider; false when it has none (not even a built-in one).
  std::function<bool(const std::string& provider, ProviderModels& out)> load;
  // Ask for a refresh (a RAM flag the main loop serves between turns).
  std::function<RefreshAsk(const std::string& provider)> requestRefresh;
};

// Register models.list on `reg`. `h` is copied; its targets must outlive `reg`.
void registerModelsTool(ToolRegistry& reg, const ModelsToolHandlers& h);

// [AVAILABLE MODELS] on a turn where models.list is callable: one line naming the
// providers with a key and pointing to the tool.
std::string modelsPointerLine(const std::vector<std::string>& keyedProviders);

// L19-39: `id` is a router id ("<upstream>/<model>") named on a direct provider
// (every slot but cumulo). Those run only on the router.
bool routerIdOnDirectProvider(const std::string& provider, const std::string& id);
// The answer for such an id: "not in <provider> list: use <bare> (<id> is a cumulo
// router id)" when the provider lists the bare id (`listedBare` non-empty), else
// "not in <provider> list: <id> is a cumulo router id; pick one of <provider>'s ids
// from models.list".
std::string routerIdMissText(const std::string& provider, const std::string& id,
                             const std::string& listedBare);

// An orch_model the cached list does not hold: "not in <provider> list cached
// <date>; refresh queued" (fetchedAt 0: "cache date unknown"). The tail follows
// what the refresh request did: queued (Queued, Pending), "refreshed within the
// hour" (Recent).
std::string modelMissText(const std::string& provider, uint32_t fetchedAt, RefreshAsk ask);

// UTC dates for the texts above: "2026-10-22" and "2026-10-22T14:05Z"; "" for 0.
std::string utcDate(uint32_t epoch);
std::string utcMinute(uint32_t epoch);

}  // namespace orch
}  // namespace nimbus
