#include "adapter_factory.h"
#include "anthropic_adapter.h"
#include "openai_adapter.h"
#include "mistral_adapter.h"
#include "custom_adapter.h"
#include "zai_adapter.h"
#include "cumulo_adapter.h"
#include "../store.h"
#include "../../sys/agent_log.h"

#include <cstring>

// Ported from Nuage-Solide src/agent/adapters/adapter_factory.cpp (Head
// Orchestrator v2). backendColor(RGB) -> backendHue (0-255) so the accent maps
// straight onto nimbus::attn::Event.accentHue. All four adapters (anthropic,
// openai, mistral, custom) implement host + sub-session dispatch/poll/cancel.

namespace agent {

static AnthropicAdapter s_anthropic;
static OpenAIAdapter    s_openai;
static MistralAdapter   s_mistral;   // host + sub-session ported 2026-07
static CustomAdapter    s_custom;
static ZaiAdapter       s_zai;       // Z.ai GLM (OpenAI-compatible sub-session)
static CumuloAdapter    s_cumulo;    // Cumulo router (per-role upstream)

// Parse "code:openai,research:openai,ops:anthropic" from config.
static void bindCategories(HeavyFabric& fabric, const String& cfg) {
  int start = 0;
  while (start < (int)cfg.length()) {
    int comma = cfg.indexOf(',', start);
    if (comma < 0) comma = cfg.length();
    String pair = cfg.substring(start, comma);
    int colon   = pair.indexOf(':');
    if (colon > 0) {
      String cat = pair.substring(0, colon);
      String be  = pair.substring(colon + 1);
      cat.trim(); be.trim();
      fabric.bindCategory(cat.c_str(), be.c_str());
      alogf("fabric: %s -> %s", cat.c_str(), be.c_str());
    }
    start = comma + 1;
  }
}

void fabricInit(HeavyFabric& fabric) {
  fabric.registerAdapter(&s_anthropic);
  fabric.registerAdapter(&s_openai);
  fabric.registerAdapter(&s_mistral);  // dispatch checks the key (Auth when absent)
  if (store::hasCustom()) {
    fabric.registerAdapter(&s_custom);   // backend "custom" - proxy endpoint
    alogf("fabric: custom backend (conv=%s)", store::customConv().c_str());
  }
  if (store::hasZaiKey()) {
    fabric.registerAdapter(&s_zai);      // backend "zai" - Z.ai GLM
    alog("fabric: zai backend (GLM)");
  }
  if (store::hasCumuloKey()) {
    fabric.registerAdapter(&s_cumulo);   // backend "cumulo" - router
    alog("fabric: cumulo backend (router)");
  }
  // Legacy category bindings. The v2.x sub-session router (orchestrator.cpp
  // dispatchSpawn) routes by the per-spawn provider / store::subPriority(), NOT by
  // these category bindings, so HeavyFabric::dispatch() is no longer the spawn
  // path. Kept so the agentFabricCfg getter stays live and adapterFor() lookups
  // work. A binding to "mistral" resolves to no adapter (stub not registered) and
  // returns NotFound cleanly.
  bindCategories(fabric, store::agentFabricCfg());
  alog("fabric: initialized");
}

uint8_t backendHue(const char* backend) {
  // Hues equivalent to the Nuage-Solide RGB accents (converted via HSV):
  //   anthropic cyan (0,180,255) -> 140 ; openai green (0,220,80) -> 100 ;
  //   mistral amber (255,140,0) -> 23 ; custom/unknown white -> 255.
  if (strncmp(backend, "anthropic", 9) == 0) return 140;
  if (strncmp(backend, "openai",    6) == 0) return 100;
  if (strncmp(backend, "mistral",   7) == 0) return 23;
  if (strncmp(backend, "zai",       3) == 0) return 200;   // violet-ish (GLM)
  if (strncmp(backend, "cumulo",    6) == 0) return 170;   // blue (the cloud)
  return 255;   // custom / unknown: white (nimbus::attn treats 255 as unknown)
}

}  // namespace agent
