// One exchange (plain/exchange.h).
// Stub bodies from lane RB-B0a: each answers Err::NotImplemented until lane RB-EX writes them.
#include "nimbus/harness/plain/exchange.h"

namespace agent {
namespace plain {

Status runExchange(const ExchangeDeps&, const ExchangeRequest&, ExchangeResult&) {
  return Status::pending("plain::runExchange");
}

}  // namespace plain
}  // namespace agent
