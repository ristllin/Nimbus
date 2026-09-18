#include "nimbus/orch/router_route.h"

namespace nimbus {
namespace orch {

RouterRoute resolveRouterRoute(const std::string& sel, const std::string& defaultUpstream) {
  RouterRoute r;
  // Split on the FIRST '/' only when it sits at an index > 0 (a leading '/' is not
  // an upstream marker) - the same test the sub-session adapter has always used
  // (indexOf('/') > 0). The remainder keeps any further slashes so a fine-tuned or
  // namespaced model id survives intact.
  const std::string::size_type sl = sel.find('/');
  if (sl != std::string::npos && sl > 0) {
    r.upstream = sel.substr(0, sl);
    r.model    = sel.substr(sl + 1);
  } else {
    r.upstream = defaultUpstream;
    r.model    = sel;
  }
  // The upstream's own API shape decides whether /v1 rides the router path. The
  // fabric upstreams (openai, mistral, anthropic) version their endpoints under
  // /v1 and the router allowlist admits them there. Z.ai's base URL already
  // carries its API prefix (v4), so its router paths have NO /v1 - the router
  // 403s /router/zai/v1/... as endpoint_not_allowed (found live, CUM-374 e2e
  // 2026-09-18; the device cumulo head already encoded this exception by hand,
  // this moves it into the ONE shared rule every consumer resolves through).
  r.basePath = "/router/" + r.upstream + (r.upstream == "zai" ? "" : "/v1");
  return r;
}

}  // namespace orch
}  // namespace nimbus
