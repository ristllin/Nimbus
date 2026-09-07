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
  r.basePath = "/router/" + r.upstream + "/v1";
  return r;
}

}  // namespace orch
}  // namespace nimbus
