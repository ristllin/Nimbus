#pragma once
#include <ArduinoJson.h>

#include <functional>
#include <string>

#include "nimbus/orch/rbac.h"            // Role / Perms / Quota
#include "nimbus/orch/vector_memory.h"   // kOwnerNs / kSharedNs / kMcpNs
#include <vector>

#include "nimbus/orch/world.h"  // ToolInfo (name+description) for the manifest

// tool_registry - the on-device tool backend + a minimal MCP (Model Context
// Protocol) JSON-RPC 2.0 dispatcher. Every tool is name + description + JSON
// input-schema + handler + a uniform ToolResult{success, output, error};
// dispatch validates the name, runs the handler, and returns a structured
// result.
//
// This is the SINGLE source of truth for tools (locked decision: an on-device
// MCP server exposing memory.* / session.* / device.*). Two consumers read it:
//   - the orchestrator bridge advertises list() to the LLM as native
//     function-calling tools and routes tool calls back through dispatch();
//   - an external MCP client (Ph4, over the LAN) speaks JSON-RPC to handleRpc().
// Both go through the same registry, so the capability manifest, the LLM's tool
// list, and what actually executes can never drift apart.
//
// Portable + Arduino-free (ArduinoJson only, host-safe) -> unit-tested via
// pio test -e native. Handlers are injected by the device (Ph2+ wires the real
// memory/session handlers); the registry itself owns no device state.
namespace nimbus {
namespace orch {

// The uniform tool result. `output` is the tool's text result; `error` is set
// (and success=false) on failure. Never throws - a handler reports failure in-band.
struct ToolResult {
  bool        success = true;
  std::string output;
  std::string error;

  static ToolResult ok(const std::string& out) { return {true, out, ""}; }
  static ToolResult fail(const std::string& err) { return {false, "", err}; }
};

// WHO is making this call (v3.7.0 - the per-principal data boundary). A tool
// handler cannot decide what a caller may read or write without this, and it
// CANNOT be ambient state: dispatch happens concurrently on the turn task
// (tg_poll) and the web/MCP task (AsyncTCP), so a file-static "current
// principal" would race between them. It is therefore threaded explicitly
// through every dispatch path.
//
//   ns    - the data namespace this caller reads/writes by default. For a turn
//           it is the routing chat ("1001", "web", "voice"…); for the LAN MCP
//           endpoint it is kMcpNs. EMPTY means "unattributed" and is treated as
//           the most restrictive case by policy, never as a wildcard.
//   owner - this caller may read/write the SHARED namespace (device-level
//           facts). The device sets it for the owner's chats and the
//           token-authenticated local surfaces; a Telegram member never has it.
struct Principal {
  std::string ns;
  bool        owner = false;    // == role Admin; kept as the fast path every rail reads
  Role        role = Role::Unknown;
  Quota       quota;            // this tenant's explicit overrides (0 = role default)
  uint32_t    pinsUsed = 0;     // permanent entries already held (pin budget)

  bool valid() const { return !ns.empty(); }
  Perms perms() const { return permsFor(role); }
};

// The reserved namespace constants (kOwnerNs / kSharedNs / kMcpNs) live in
// vector_memory.h - the lowest layer that persists them.
// NOTE: there is deliberately no principalForChat(chatId, isOwner) helper.
// It existed, it guessed `role = isOwner ? Admin : User`, and its comment
// promised the device would "refine" that from the tenant table. Nothing did,
// so every RBAC rail read a role nobody had set and roles/quotas did nothing on
// the real turn path. A Principal must be built from an actual role - use
// principalForRole below, whose caller has to supply one.

inline Principal principalForRole(const std::string& chatId, Role r, const Quota& q = {}) {
  const bool admin = (r == Role::Admin);
  Principal p;
  p.ns = nsForChat(chatId, admin);
  p.owner = admin;
  p.role = r;
  p.quota = q;
  return p;
}

// A handler receives the validated call arguments as a JSON object view (a view
// into a document the dispatcher keeps alive for the call), plus the calling
// Principal, and returns a result.
using ToolHandler = std::function<ToolResult(ArduinoJson::JsonObjectConst args,
                                             const Principal& who)>;

struct Tool {
  std::string name;         // "memory.search"
  std::string description;  // one-line, shown to the model
  std::string schemaJson;   // JSON Schema for arguments (MCP inputSchema); may be "{}"
  ToolHandler handler;
  // ADVERTISEMENT scope (W14) - never a security boundary, which stays the
  // handler's own check (see the ToolPolicy note below). This exists because
  // the composed prompt listed EVERY tool to EVERY conversation: a guest was
  // told the assistant could call skill.save / tenant.set_role / loop.create,
  // it tried, the handler refused, and the guest got a confused walk-back -
  // plus a description of the owner's admin surface. `adminOnly` keeps such a
  // tool out of a non-admin turn's list, restoring "advertised == callable".
  bool adminOnly = false;
};

class ToolRegistry {
 public:
  // ---- ToolPolicy (dispatch-level allow/deny/gate) --------------------------
  // ENFORCEMENT, not visibility (the ESP-Claw lesson: hiding a tool from the
  // advertisement is not a security boundary - the dispatch path must refuse).
  // Every execution route funnels through dispatch() (direct calls AND
  // handleRpc's tools/call), so a Deny/Gated verdict here is authoritative: the
  // handler NEVER runs, and the caller gets a failed ToolResult carrying the
  // reason (never a throw). Default is Allow - with no table entry and no
  // resolver installed, behavior is byte-identical to the pre-policy registry.
  //
  // Two sources, checked in order:
  //   1. the static per-tool table (setPolicy) - fixed rules;
  //   2. the resolver hook (setPolicyResolver) - dynamic context (e.g. the
  //      device denies loop.create while a scheduled turn is running).
  // The first non-Allow verdict wins. This is the P7 autonomy-contract
  // substrate: Gated is reserved for a future owner-approval flow and today
  // refuses exactly like Deny (distinct kind kept so callers can tell them
  // apart when that flow lands).
  struct Verdict {
    enum Kind : uint8_t { Allow = 0, Deny = 1, Gated = 2 };
    Kind        kind = Allow;
    std::string reason;

    static Verdict allow() { return Verdict{}; }
    static Verdict deny(const std::string& r)  { return Verdict{Deny, r}; }
    static Verdict gated(const std::string& r) { return Verdict{Gated, r}; }
  };
  using PolicyResolver = std::function<Verdict(const std::string& name)>;

  // Set/replace the static verdict for one tool name (Allow removes the entry).
  void setPolicy(const std::string& name, Verdict v);
  void clearPolicies() { policies_.clear(); }
  // Install (or clear, with nullptr) the dynamic resolver.
  void setPolicyResolver(PolicyResolver r) { resolver_ = std::move(r); }
  // The effective verdict for a name: table first, then resolver, else Allow.
  Verdict policyFor(const std::string& name) const;

  // Register (or replace) a tool. schemaJson defaults to an empty object schema.
  void add(const std::string& name, const std::string& description,
           ToolHandler handler, const std::string& schemaJson = "{}");
  // Mark an already-registered tool admin-only for ADVERTISEMENT purposes
  // (W14). Call it right after add(); an unknown name is a no-op, so a renamed
  // tool degrades to "advertised to everyone" (visible + still refused by its
  // handler) rather than silently vanishing from every prompt.
  void setAdminOnly(const std::string& name, bool v = true);
  bool isAdminOnly(const std::string& name) const;

  bool has(const std::string& name) const;
  int  size() const { return (int)tools_.size(); }

  // For the capability manifest (the tool summaries): stable order = add
  // order.
  std::vector<ToolInfo> manifest() const;

  // A tool's full advertisement material (name + description + arguments JSON-Schema),
  // for building a provider function-`tools[]` array so the tool is genuinely CALLABLE
  // by the head model in a tool-use loop. Unlike manifest() (name+description only, the
  // read-only capability blurb), this carries the schema. Stable add order. Each
  // adapter wraps these into its own shape (Anthropic input_schema / OpenAI+Mistral
  // function.parameters).
  struct Spec {
    std::string name;
    std::string description;
    std::string schemaJson;   // JSON Schema for the arguments ("{}" if none)
  };
  std::vector<Spec> toolSpecs() const;
  // The specs THIS caller may actually call - admin-only tools are omitted for a
  // non-admin principal (W14). Use this on every advertisement path; the
  // unfiltered toolSpecs() above stays for the web/debug surfaces that
  // deliberately show the whole registry.
  std::vector<Spec> toolSpecsFor(const Principal& who) const;

  // Execute a tool by name with a JSON-object arguments view. Unknown name -> a
  // failed ToolResult (never throws).
  // `who` identifies the caller (see Principal). Every execution route carries
  // it - direct dispatch AND handleRpc's tools/call - so a handler can always
  // scope its data access without consulting ambient state.
  ToolResult dispatch(const std::string& name, ArduinoJson::JsonObjectConst args,
                      const Principal& who) const;

  // ---- MCP JSON-RPC 2.0 endpoint ----
  // Handle one JSON-RPC request string and return the response string. Supports:
  //   "tools/list"  -> { tools: [{name, description, inputSchema}] }
  //   "tools/call"  -> params {name, arguments} -> { content:[{type:"text",
  //                    text}], isError } (MCP call result; a failed ToolResult
  //                    maps to isError=true with the error text)
  //   "ping"        -> {}
  // Errors: parse error -> -32700; not an object / bad request -> -32600;
  // unknown method -> -32601. A request with no "id" is a notification and
  // returns "" (no response), per JSON-RPC.
  std::string handleRpc(const std::string& requestJson, const Principal& who) const;

 private:
  const Tool* find(const std::string& name) const;
  std::vector<Tool> tools_;

  struct PolicyEntry { std::string name; Verdict verdict; };
  std::vector<PolicyEntry> policies_;
  PolicyResolver           resolver_;
};

}  // namespace orch
}  // namespace nimbus
