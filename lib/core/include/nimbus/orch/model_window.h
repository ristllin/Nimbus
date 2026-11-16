#pragma once
#include <cstdint>
#include <string>

// model_window - the per-model context window table and the compaction trigger math it
// feeds. Moved out of compact.h/orch_compact.cpp unchanged (lane P0 of the history fix,
// 16 Nov 2026) so the window's owner edits it alone; compact.h includes this header, so
// every caller compiles as before.
namespace nimbus {
namespace orch {

// ---- per-model context windows ----------------------------------------------
// Family-prefix table, deliberately conservative: an unknown model gets
// kCtxDefaultTokens so a wrong guess compacts EARLY (cheap), never late (400s).
// The device may layer an NVS override on top (store::ctxTokensOverride).
constexpr uint32_t kCtxDefaultTokens = 100000;

// Returns the context window (tokens) for a provider/model pair.
// Matching: exact family prefixes on the known catalogs - "claude-*" 200K,
// "gpt-5*" 272K, "gpt-6*" and newer 922K, "o<digit>*" 200K, "mistral-*" 128K -
// else the default. A router selector ("<upstream>/<model>", what the Cumulo
// picker stores) is split by resolveRouterRoute first and read as (upstream,
// bare model); provider "cumulo" with a bare id reads as the router's default
// upstream (openai). Another provider's id is split only when its prefix is a
// router upstream (openai/anthropic/mistral/zai).
uint32_t modelCtxTokens(const std::string& provider, const std::string& model);

// The listed window: what the device's stored model list (the VN: its harvested
// catalog) says `model` takes on `host`, in tokens (listedCtxTokens, model_catalog.h);
// 0 = the list does not say, or no lookup is installed (host tests). Installed by the
// device (src/agent/model_refresh.cpp) and the VN (nimbusd rig.h). Nothing reads it
// yet: lane B prefers it over the family table above.
using ListedCtxLookup = uint32_t (*)(const char* host, const char* model);
void installListedCtxLookup(ListedCtxLookup f);
uint32_t listedWindowTokens(const std::string& host, const std::string& model);

// ---- trigger math ------------------------------------------------------------
// threshold = min(ctx − reservedOut − kCompactBufferTokens, ownerCapTokens).
// reservedOut is the caller's REQUESTED max_tokens (the head asks ~3-4K, not the
// model max). ownerCapTokens (NVS compactAtK, default 60K) exists because a desk
// assistant gains nothing from a 150K tail - compacting earlier keeps every turn
// faster and cheaper. 0 disables the owner cap (window math only). The result is
// floored at kCompactMinThreshold so a tiny/misconfigured window can't make the
// device compact on every turn.
constexpr uint32_t kCompactBufferTokens  = 13000;
constexpr uint32_t kCompactMinThreshold  = 1000;

uint32_t compactThreshold(uint32_t ctxTokens, uint32_t reservedOutTokens,
                          uint32_t ownerCapTokens);

}  // namespace orch
}  // namespace nimbus
