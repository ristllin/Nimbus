#include "nimbus/orch/moderation.h"

namespace nimbus {
namespace orch {

const char* modGateName(ModGate g) {
  switch (g) {
    case ModGate::InboundText: return "inbound";
    case ModGate::OutboundReply: return "outbound";
    case ModGate::WorldContent: return "world";
  }
  return "?";
}

const char* modActionName(ModAction a) {
  switch (a) {
    case ModAction::Allow: return "allow";
    case ModAction::Block: return "block";
    case ModAction::MarkUntrusted: return "mark-untrusted";
  }
  return "?";
}

const char* modProviderName(ModProvider p) {
  switch (p) {
    case ModProvider::None: return "none";
    case ModProvider::Cumulo: return "cumulo";
    case ModProvider::Mistral: return "mistral";
  }
  return "?";
}

bool gateApplies(ModGate g, Role role, const ModConfig& cfg) {
  if (role == Role::Admin) return false;   // admin traffic is never classified
  switch (g) {
    case ModGate::InboundText: return cfg.inbound;
    case ModGate::OutboundReply: return cfg.outbound;
    case ModGate::WorldContent: return cfg.injection;
  }
  return false;
}

FailMode failModeFor(ModGate g) {
  // Inbound guest text fails CLOSED (a message we could not screen does not run a
  // turn). Outbound and world content fail OPEN (better to answer / show data than
  // go mute on a classifier outage; the world gate still marks).
  return g == ModGate::InboundText ? FailMode::Closed : FailMode::Open;
}

ModAction decide(ModGate g, ClassifierVerdict v) {
  switch (v) {
    case ClassifierVerdict::Unchecked:
    case ClassifierVerdict::Allow:
      return ModAction::Allow;
    case ClassifierVerdict::Flag:
      // World content is marked, never blocked; the text gates block a flag.
      return g == ModGate::WorldContent ? ModAction::MarkUntrusted : ModAction::Block;
    case ClassifierVerdict::Error:
      if (g == ModGate::WorldContent) return ModAction::MarkUntrusted;  // fail-open WITH marking
      return failModeFor(g) == FailMode::Closed ? ModAction::Block : ModAction::Allow;
  }
  return ModAction::Allow;
}

bool outboundExempt(bool systemProvenance) {
  // Provenance-only, fail-closed: the exemption is the flag and nothing else. There
  // is no text argument on purpose - a reply's bytes can never satisfy this, so a
  // guest-steered model reply (systemProvenance == false) is always screened.
  return systemProvenance;
}

ModProvider pickProvider(bool hasCumuloKey, bool hasMistralKey) {
  if (hasCumuloKey) return ModProvider::Cumulo;
  if (hasMistralKey) return ModProvider::Mistral;
  return ModProvider::None;
}

// --- injection heuristics ---------------------------------------------------

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Case-insensitive substring search (needle is already lowercase).
static bool ciContains(const std::string& hay, const char* needle) {
  const size_t n = std::char_traits<char>::length(needle);
  if (n == 0 || n > hay.size()) return false;
  for (size_t i = 0; i + n <= hay.size(); i++) {
    size_t j = 0;
    for (; j < n; j++) if (lc(hay[i + j]) != needle[j]) break;
    if (j == n) return true;
  }
  return false;
}

bool looksLikeInjection(const std::string& text) {
  // Lowercase needles; the scan lowercases the haystack char-by-char. These are the
  // load-bearing shapes of a prompt-injection payload smuggled inside fetched or
  // summarized world content (a web page, a document, a sub-agent result).
  static const char* kPatterns[] = {
      "ignore previous instructions",
      "ignore all previous",
      "ignore the above",
      "disregard previous",
      "disregard the above",
      "ignore your instructions",
      "forget your instructions",
      "forget all previous",
      "you are now",
      "new instructions:",
      "system prompt",
      "system:",
      "assistant:",
      "<|im_start|>",
      "<|system|>",
      "[system]",
      "reveal your prompt",
      "print your instructions",
      "override your",
  };
  for (const char* p : kPatterns)
    if (ciContains(text, p)) return true;
  return false;
}

}  // namespace orch
}  // namespace nimbus
