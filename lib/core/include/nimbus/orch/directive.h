#pragma once
#include <string>

// The owner directive: the owner's standing preferences for how the assistant
// communicates and behaves. It is USER-owned text, immutable by the model, and
// injected into the orchestrator system prompt below the platform rules (see
// compose.cpp / assembleContext). Stored in NVS under `sysPrompt`; capped to
// nimbus::orch::kMemDirectiveMax bytes on every write and re-capped UTF-8-safely
// on read.
//
// This header is the SINGLE source of the shipped default text: the prompt
// injector and the web/wizard UI both read it (the UI over /api/orch's
// `directiveDefault`) so the default is never duplicated.
namespace nimbus {
namespace orch {

// Compiled-in default owner directive. Shipped as the zero-effort baseline: a
// fresh device (empty stored value) and "Revert to default" both fall back to
// it. Copy-clean (AGENTS.md section 6): no em dash, US English, calm and direct.
extern const char* const kOwnerDirectiveDefault;

// The effective owner directive: the stored text when the owner set one, else
// the shipped default. An empty stored value means "use the shipped default", so
// a future default change reaches every device that never overrode it.
std::string effectiveDirective(const std::string& stored);

}  // namespace orch
}  // namespace nimbus
