#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// fallback_rules - the PORTABLE device fallback rule engine (host-tested). Consumes
// the BINDING v1 shared schema agreed with the cloud (C3, CUM-105): an admin edits
// one rule set; the server and the device both apply it. This module owns the pure
// decisions - parse the rule JSON, match a failing turn against the predicates, and
// pick the next (provider, model) target - with NO Arduino/store/TLS. The device
// layer loads the JSON from NVS (or the Cumulo cloud) and supplies the keyed/failed
// predicates.
//
// Schema (v1): every `match` predicate is an ARRAY (any-of); an omitted/empty
// predicate matches anything. `model` entries allow a trailing-'*' prefix glob.
// Action is an ordered `to[]` of {provider, model?} (model omitted = that
// provider's same-size-class default). Unknown fields are tolerated and ignored
// (forward compat; the SERVER rejects them, the device does not).
namespace nimbus {
namespace orch {

// The shared errorClass enum (lower_snake tokens on the wire). Device FabricErr
// maps onto this: remote_fail -> server_error, (network stays), parse_fail is a
// hard never-fallback and is NOT represented here.
enum class ErrorClass : uint8_t {
  None = 0,
  RateLimited,
  Timeout,
  ServerError,
  Network,
  Auth,
  BadRequest,
  NotFound,
  Unsupported,
  Unpriced,
  Disabled,
};
const char* errorClassToken(ErrorClass e);
ErrorClass errorClassFromToken(const std::string& t);

struct FallbackTarget {
  std::string provider;
  std::string model;  // "" = the provider's same-size-class default
};

struct FallbackRule {
  std::string id;
  std::string description;
  bool enabled = true;
  // match predicates - any-of; empty vector = match anything.
  std::vector<std::string> provider;
  std::vector<std::string> model;       // exact id or trailing-'*' prefix glob
  std::vector<std::string> sizeClass;   // "small" | "medium" | "large"
  std::vector<std::string> capability;  // chat | tools | vision | embeddings | ...
  std::vector<std::string> errorClass;  // tokens above
  std::vector<FallbackTarget> to;       // ordered substitution targets
};

struct FallbackRuleSet {
  int version = 1;
  std::vector<FallbackRule> rules;
};

// The turn being routed, expressed in schema tokens. Filled by the caller.
struct TurnContext {
  std::string provider;    // the failing provider slug
  std::string model;       // the failing model id
  std::string sizeClass;   // "small" | "medium" | "large" | "" (unknown)
  std::string capability;  // primary capability of the turn ("chat" for a head turn)
  ErrorClass  errorClass = ErrorClass::None;
  bool        embeddings = false;  // embeddings NEVER fall back cross-provider
};

// A resolved fallback decision.
struct FallbackChoice {
  bool found = false;
  std::string ruleId;      // which rule fired
  FallbackTarget target;   // where to go
};

// ---- parse / serialize -------------------------------------------------------
// Parse the v1 rule JSON (tolerant: unknown fields ignored, malformed rules
// dropped). `body` may carry HTTP headers ahead of the JSON. Returns rule count.
size_t parseFallbackRules(const std::string& body, FallbackRuleSet& out,
                          ArduinoJson::Allocator* alloc = nullptr);
// Serialize a rule set to the v1 shape (for GET /api/fallbacks).
void fallbackRulesToJson(const FallbackRuleSet& rs, JsonObject out);

// ---- matching + selection ----------------------------------------------------
// Does a rule's `match` accept this context? (enabled + every present predicate
// matches any-of; model supports a trailing-'*' glob.)
bool ruleMatches(const FallbackRule& rule, const TurnContext& ctx);

// Walk the rule set and pick the first target that (a) matches, (b) is not the
// (provider,model) that just failed, and (c) `isAvailable(provider, model)` says
// is usable (keyed/priced/enabled/funded - the caller decides). Embeddings and a
// parse_fail context never fall back (returns found=false). Honors the binding's
// "first matching rule, walk its to[]; exhausted -> next matching rule".
FallbackChoice selectFallback(
    const FallbackRuleSet& rs, const TurnContext& ctx,
    const std::function<bool(const std::string& provider, const std::string& model)>& isAvailable);

// ---- defaults + helpers ------------------------------------------------------
// The shipped size-class defaults: one rule per class whose to[] walks `priority`
// (provider slugs, e.g. from store::providerPriority). Reproduces the pre-engine
// midFail behavior (walk the priority list) when no admin rules exist.
FallbackRuleSet defaultRuleSet(const std::vector<std::string>& priority);

// Map the device FabricErr integer (nimbus::harness FabricErr order) to a class.
// parse_fail -> None (the engine treats None as "never fall back").
ErrorClass errorClassFromFabric(int fabricErr);

// Full-word size class ('S'|'M'|'L' -> "small"|"medium"|"large"; 0 -> "").
std::string sizeClassWord(char sizeClass);

// The model-facing context note recorded on a fallback (mention only if relevant).
std::string fallbackNote(const std::string& fromProvider, const std::string& fromModel,
                         const std::string& toProvider, const std::string& toModel,
                         ErrorClass reason);

}  // namespace orch
}  // namespace nimbus
