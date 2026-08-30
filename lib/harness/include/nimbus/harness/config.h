#pragma once
#include <cstdint>
#include <functional>
#include <string>

// HarnessConfig - the harness's read-mostly view of device configuration,
// clustered by consumer. This replaces direct agent::store:: (NVS) access in
// every lifted function; the device implements it over store:: in
// src/agent/store_config.cpp, host tests over an in-memory map.
//
// SECURITY RAIL, structural: there are NO setters here for provider keys, the
// orchestrator-host priority list, or orchHost. Those are human-only writes
// (web UI / provisioning console) and the harness cannot reach them by
// construction - the same property the protected-key list in
// orch_device_actions enforces at the wire is now a property of the type.
// The ONE provider-side write is setConvId (per-host conversation continuity),
// and the ONE model-writable routing knob remains DeviceExec.applyConfig's
// sub-priority path (validated device-side).
namespace agent {

struct ProviderConfig {
  // Credential presence + material (read-only). key() returns the secret the
  // adapters must place in auth headers; it is never logged by the harness.
  std::function<bool(const std::string& host)> hasKey;
  std::function<std::string(const std::string& host)> key;
  // CUM-211: is ANY provider configured on this device - including router/relay
  // providers (Cumulo, Z.ai) that hasKey() above does NOT report because they are
  // not single-shot head hosts. Device-truth over every key slot. Used only to
  // decide the "no provider set up" honest reply, so a device with a real verified
  // provider (e.g. Cumulo) is never told it has none. Nullable: when unset, the
  // engine falls back to walking the head hosts (host tests without this wired).
  std::function<bool()> anyKeyed;

  std::function<std::string()> orchHost;           // "" => top of priority
  // CUM-242 / CUM-201 deterministic source: with NO BYOK head keyed, a verified
  // router key (Cumulo, then Z.ai) is the fallback head that runs the whole
  // assistant - "one key, one balance." Returns the router host ("cumulo"/"zai")
  // or "" when none is keyed. Nullable: unset on host rigs (they fall to the bare
  // priority head, unchanged). A keyed BYOK head still wins over this.
  std::function<std::string()> routerFallbackHost;
  std::function<std::string()> providerPriority;   // orchestrator-host list
  std::function<std::string()> subPriority;        // sub-session provider list
  std::function<std::string(const std::string& host)> orchModel;
  std::function<std::string(const std::string& host)> subModel;
  std::function<std::string(const std::string& host)> modelChoices; // live-verified csv, "" = none
  std::function<std::string()> fallbackRules;      // fallback rule set JSON (v1); "" => size-class defaults

  // Per-host conversation continuity, stored "host|convId".
  std::function<std::string()> convId;
  std::function<void(const std::string&)> setConvId;

  // Custom/proxy endpoint (backend "custom"): base URL, key, wire convention,
  // model. Empty base = not configured.
  std::function<std::string()> customBase, customKey, customConv, customModel;
};

struct LoopConfig {
  std::function<bool()> toolLoopOn;
  std::function<int()>  rounds;      // clamped device-side (1..32)
  std::function<int()>  deadlineS;   // 30..3600
  std::function<int()>  resultCap;   // per-tool-result bytes
  std::function<int()>  totalCap;    // cumulative tool bytes
  // Mid-turn provider failover (Stage 2 phase 5): when true AND hosts.fabric is
  // registered, LOOP turns run the engine-owned multi-provider fabric loop - a
  // failed round switches provider and re-runs against the shared transcript.
  // Nullable = OFF (host rigs unaffected); the device wires NVS `midFail`.
  std::function<bool()> midTurnFailover;
};

struct BudgetConfig {
  std::function<bool(const std::string& host)> overBudget;
  // `tag` = spend attribution ("turn" | "synthesis" | "loop:<id>") - the engine
  // knows the turn's source, the ledger files it (device: recordProviderTokens
  // tag overload). Consumers that don't care may ignore it.
  // cacheRead/cacheWrite (v4.1.3 prism): tokens Anthropic EXCLUDES from
  // tokensIn but bills (0.1x / 1.25x). The engine passes them only for hosts
  // with that exclusion semantic; consumers meter them into the $ estimate.
  std::function<void(const std::string& host, uint32_t tokensIn, uint32_t tokensOut,
                     uint32_t cacheRead, uint32_t cacheWrite, const std::string& tag)>
      recordTokens;
};

struct HarnessConfig {
  ProviderConfig provider;
  LoopConfig     loop;
  BudgetConfig   budget;

  std::function<bool()> ttsEnabled;      // "Voice replies" master toggle
  std::function<std::string()> deviceName;
  // N11: select the simplified v2 system prompt (NVS `promptV2`, default off).
  // Nullable = off (v1, byte-identical) so host rigs and un-wired devices are
  // unaffected. The engine reads it into ComposeInputs::promptV2 before compose.
  std::function<bool()> promptV2;
};

}  // namespace agent
