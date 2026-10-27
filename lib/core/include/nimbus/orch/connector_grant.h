#pragma once
#include <cstdint>

#include "nimbus/orch/tool_registry.h"   // Principal

// connector_grant - whose connectors may ride a provider request (CUM-828).
//
// The owner's connectors (remote MCP servers, first-party Gmail / Calendar / Drive,
// Studio connectors) carry the owner's own sign-ins. A request made for anyone else -
// a Telegram member's or a guest's turn, a sub-agent one of them started, or the turn
// that reads that sub-agent's result - carries the provider's built-in tools only
// (web search, the code sandbox), exactly like a request over the shared Cumulo router.
//
// The grant is the calling task's, set for the length of a turn (TurnEngine::runTurn,
// runFold) and of a sub-agent dispatch (JobEngine::dispatchSpawn) from the principal
// of the chat it runs for. Every attach builder (attach*Wire, the one path every
// provider request and every product uses) reads it, so no request path can carry
// the owner's connectors for someone else. With no scope set (the device's own
// probes and checks) the grant is the owner's.
//
// Thread-local: the device runs one turn and one dispatch at a time on its turn task,
// and a request answered on another task meanwhile reads its own (unset) grant.
namespace nimbus {
namespace orch {

enum class ConnectorGrant : uint8_t {
  Owner = 0,      // the owner's connectors and the built-ins
  BuiltinsOnly,   // the provider's built-in tools only
};

// The grant a principal's requests get: the owner's for the owner (an admin, never
// the LAN endpoint), built-ins only for anyone else, an unapproved chat included.
ConnectorGrant connectorGrantFor(const Principal& who);

// The calling task's grant (Owner when no scope is set).
ConnectorGrant currentConnectorGrant();

// Sets the calling task's grant for its lifetime; scopes nest.
class ConnectorGrantScope {
 public:
  explicit ConnectorGrantScope(ConnectorGrant g);
  ~ConnectorGrantScope();
  ConnectorGrantScope(const ConnectorGrantScope&) = delete;
  ConnectorGrantScope& operator=(const ConnectorGrantScope&) = delete;

 private:
  ConnectorGrant prev_;
};

// What an attach builder attaches: built-ins only when its route asks for it (the
// Cumulo router) or the calling task's grant does.
inline bool attachBuiltinsOnly(bool routeBuiltinsOnly) {
  return routeBuiltinsOnly || currentConnectorGrant() == ConnectorGrant::BuiltinsOnly;
}

}  // namespace orch
}  // namespace nimbus
