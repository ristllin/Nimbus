#include "nimbus/orch/connector_grant.h"

#include "nimbus/orch/vector_memory.h"   // kMcpNs

namespace nimbus {
namespace orch {

namespace {
thread_local ConnectorGrant t_grant = ConnectorGrant::Owner;
}  // namespace

ConnectorGrant connectorGrantFor(const Principal& who) {
  const bool owner = who.ns != kMcpNs && who.perms().manageTenants;
  return owner ? ConnectorGrant::Owner : ConnectorGrant::BuiltinsOnly;
}

ConnectorGrant currentConnectorGrant() { return t_grant; }

ConnectorGrantScope::ConnectorGrantScope(ConnectorGrant g) : prev_(t_grant) { t_grant = g; }

ConnectorGrantScope::~ConnectorGrantScope() { t_grant = prev_; }

}  // namespace orch
}  // namespace nimbus
