#pragma once
#include <string>

// router_route - the ONE pure rule for turning a Cumulo router model selector
// ("<upstream>/<model>") into router coordinates (upstream, bare model, base
// path). Kept portable (NO Arduino) so the on-device orchestrator HEAD and the
// sub-session adapter share a single resolution and can never drift apart.
//
// Why this exists (CUM-369): the router addresses each upstream under
// /router/<upstream>/v1 and PRICES the bare model id. The router's own catalog
// ids are "<upstream>/<model>", and pricing resolve(<upstream>, <model>)
// family-matches the remainder AFTER the slash (the cloud rule requires the priced
// model to start with its family, so a prefixed id can never match). The
// sub-session adapter already split the selector this way; the head sent the
// selector verbatim to a hardcoded openai base, so a device whose owner picked a
// prefixed id from the dropdown priced 403 model_not_priced and lost the assistant
// head entirely (money fail-closed). Both paths now resolve through this one
// function: a new caller that forgets to split fails the host test, not the field.

namespace nimbus {
namespace orch {

struct RouterRoute {
  std::string upstream;  // route segment, e.g. "openai" / "anthropic" (never empty)
  std::string model;     // bare model id the router prices (the selector remainder)
  std::string basePath;  // "/router/<upstream>/v1" - the request base path prefix
};

// Resolve a router selector into (upstream, model, basePath). Split semantics are
// byte-identical to the sub-session CumuloAdapter: the FIRST '/' at an index > 0
// splits the selector - the leading segment is the upstream, everything after it
// (nested slashes included) is the model. A selector with no '/' (or a leading
// '/') keeps `defaultUpstream` and is sent as the model unchanged, so a bare id
// like "gpt-4o" still routes to the openai upstream exactly as before. An empty
// selector yields an empty model; callers gate on that (BadRequest / a default).
RouterRoute resolveRouterRoute(const std::string& sel,
                               const std::string& defaultUpstream = "openai");

}  // namespace orch
}  // namespace nimbus
