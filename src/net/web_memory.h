#pragma once
#include <ESPAsyncWebServer.h>

// web_memory - the memory dashboard + LAN MCP endpoint routes (Part B Ph3/Ph4),
// kept in their own translation unit so webui.cpp stays a thin shell. Registered
// from beginWeb() via registerMemoryRoutes(server).
//
// Routes (all JSON; the page polls them):
//   GET  /api/mem/stats                 vector/scratch/episodic counts + embed cfg
//   GET  /api/mem/vector?query=&limit=  browse (importance-desc) or semantic search
//   POST /api/mem/vector  op=delete|flush|flushnp|dedupe|permanent [&id=]
//   GET  /api/mem/scratchpad            rendered scratchpad
//   POST /api/mem/scratchpad            edit (proxied to the memory.scratchpad tool)
//   GET  /api/mem/config  · PUT /api/mem/config    retrieval/decay knobs
//   GET  /api/mem/episodic?kind=&limit= episodic browse
//   GET  /api/mem/embedcfg · POST /api/mem/embedcfg  set-once embed config (+reset)
//   POST /mcp                           JSON-RPC 2.0 -> memory::handleMcp (LAN MCP)
//
// SECURITY: same LAN/AP HTTP exposure as the rest of the config surface (no auth
// yet - a known open item). The model still can't reach provider keys/host here;
// these endpoints touch only memory engines + the embed config.
namespace nimbus::net {

void registerMemoryRoutes(AsyncWebServer& server);

}  // namespace nimbus::net
