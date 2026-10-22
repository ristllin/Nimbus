#include "nimbus/harness/output_caps.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace agent {
namespace providers {

namespace {

#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
uint32_t g_testFirstAttemptCap = 0;   // test and host builds only (setTestFirstAttemptCap)
#endif

// A Claude id's line and version: "claude-opus-4-5" -> ("opus", 45),
// "claude-haiku-5-5" -> ("haiku", 55), "claude-sonnet-5" -> ("sonnet", 50). A
// router pick carries an "anthropic/" prefix. Old "claude-3-5-haiku" ids and
// anything else parse as no line.
struct ClaudeId {
  std::string line;
  int version = 0;   // major*10 + minor
};
ClaudeId parseClaude(std::string model) {
  ClaudeId id;
  if (model.rfind("anthropic/", 0) == 0) model = model.substr(10);
  if (model.rfind("claude-", 0) != 0) return id;
  const size_t a = 7;
  const size_t b = model.find('-', a);
  if (b == std::string::npos || !std::isalpha((unsigned char)model[a])) return id;
  id.line = model.substr(a, b - a);
  const char* p = model.c_str() + b + 1;
  if (!std::isdigit((unsigned char)*p)) return ClaudeId();
  const int major = (int)std::strtol(p, const_cast<char**>(&p), 10);
  int minor = 0;
  // A one- or two-digit group after the major is the minor; a date suffix
  // (8 digits) is not.
  if (*p == '-' && std::isdigit((unsigned char)p[1]) &&
      (!std::isdigit((unsigned char)p[2]) || !std::isdigit((unsigned char)p[3])))
    minor = (int)std::strtol(p + 1, nullptr, 10) % 10;
  id.version = major * 10 + minor;
  return id;
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The provider-side cap for each round kind, before the time clamp.
OutputCap anthropicCap(const std::string& model, RoundKind kind) {
  const bool effort = anthropicTakesEffort(model);
  OutputCap c;
  c.maxTokens = 8192;
  c.retryTokens = 12000;
  // Head rounds think at medium; the fold (a summary) needs little thinking. The
  // retry after a cut steps effort down so more of the budget goes to the answer.
  c.effort = effort ? (kind == RoundKind::Fold ? "low" : "medium") : "";
  c.retryEffort = effort ? "low" : "";
  return c;
}

OutputCap mistralCap(const std::string& model) {
  OutputCap c;
  // About 60 s of output at the slowest measured Mistral rate (65 tok/s, Lumi
  // 13-14 Oct), inside the 120 s round ceiling; the retry gets half again.
  c.maxTokens = 4096;
  c.retryTokens = 6144;
  // A head round answers; it does not reason (Mistral documents "high" and
  // "none"). Only models that take the field get it, and the cut retry keeps it.
  const bool effort = mistralTakesEffort(model);
  c.effort = effort ? "none" : "";
  c.retryEffort = c.effort;
  return c;
}

// The tokens a generation at `rate` can produce inside `roundMs` (connect, TLS, the
// upload and the prompt prefill take the first 10 s). 0 = no time clamp.
uint32_t timeFit(uint32_t roundMs, uint32_t rate) {
  if (roundMs == 0) return 0;
  const uint32_t kPrefillMs = 10000;
  const uint32_t genMs = roundMs > kPrefillMs ? roundMs - kPrefillMs : 0;
  return (uint32_t)((uint64_t)genMs * rate / 1000);
}

uint32_t atMost(uint32_t a, uint32_t b) { return a < b ? a : b; }
uint32_t atLeast(uint32_t a, uint32_t b) { return a > b ? a : b; }

// L19-44: the time clamp keeps the retry ROOMIER than the first attempt. Clamping
// both caps to the same fit collapsed them (a full 120 s round on claude-opus-5:
// 5,500 and 5,500), so the retry after a cut changed only the effort and a Claude
// id that takes no effort had no retry at all; under 50 s the cap fell below
// v4.5.13's 3,000. Now the first attempt takes three quarters of what the round's
// time allows, never under kFirstFloor; the retry takes the whole fit and at least
// a third more than the first attempt; both stay inside the table's own caps.
constexpr uint32_t kFirstFloor = 3000;
void clampToTime(OutputCap& c, uint32_t roundMs, uint32_t rate) {
  if (!roundMs || !c.maxTokens) return;   // no deadline, or no cap sent
  const uint32_t fit = timeFit(roundMs, rate);   // 0 for a round under the prefill: the floors hold
  c.maxTokens = atMost(c.maxTokens, atLeast(kFirstFloor, fit / 4 * 3));
  if (c.retryTokens) c.retryTokens = atMost(c.retryTokens, atLeast(fit, c.maxTokens / 3 * 4));
}

}  // namespace

const char* roundKindSlug(RoundKind k) {
  switch (k) {
    case RoundKind::ToolRound:  return "tool";
    case RoundKind::FinalRound: return "final";
    case RoundKind::SingleShot: return "single";
    case RoundKind::Fold:       return "fold";
  }
  return "?";
}

bool OutputCap::hasRetry() const {
  if (retryTokens == 0) return false;
  return retryTokens != maxTokens || std::strcmp(retryEffort, effort) != 0;
}

bool anthropicTakesEffort(const std::string& model) {
  const ClaudeId id = parseClaude(model);
  if (id.line == "fable" || id.line == "mythos") return true;
  if (id.line == "opus") return id.version >= 45;
  if (id.line == "sonnet") return id.version >= 46;
  if (id.line == "haiku") return id.version >= 50;
  return false;
}

bool mistralTakesEffort(const std::string& model) {
  const std::string m = startsWith(model, "mistral/") ? model.substr(8) : model;
  static const char* const kReasoning[] = {
      "mistral-small-latest", "mistral-small-26", "mistral-medium", "magistral-", "mistral-large-4",
      "mistral-vibe-cli", "labs-leanstral", "glm-", "zai-glm",
  };
  for (const char* p : kReasoning)
    if (startsWith(m, p)) return true;
  return false;
}

uint32_t slowestTokensPerSecond(const std::string& host, const std::string& model) {
  // Floors about 25-30% under the rates measured live on 18 Oct over a whole
  // non-streamed request (evidence/rate-probe.json in the lane notes): Haiku 5.5
  // 175 tok/s, Sonnet 5.5 128, Opus 5 67.5, Mistral Small 128, Mistral Large 78.
  // Fable is the slowest tier and was not measured: it gets the most room for
  // time (the lowest rate).
  if (host == "anthropic") {
    const ClaudeId id = parseClaude(model);
    if (id.line == "haiku") return 120;
    if (id.line == "sonnet") return 90;
    if (id.line == "opus") return 50;
    return 40;   // fable, mythos, unknown: the slowest tier
  }
  if (host == "mistral") {
    if (startsWith(model, "mistral-small") || startsWith(model, "ministral")) return 90;
    if (startsWith(model, "mistral-medium")) return 70;
    return 55;
  }
  return 50;
}

OutputCap outputCapFor(const std::string& host, const std::string& model, RoundKind kind,
                       uint32_t roundMs) {
  OutputCap c;
  if (host == "anthropic") c = anthropicCap(model, kind);
  else if (host == "mistral") c = mistralCap(model);
  // OpenAI: no max_output_tokens - reasoning tokens count toward it and would
  // starve the answer; completeness is read from status:"incomplete" instead.
  // Custom / Z.ai: the head sends no cap either; finish_reason "length" is read.
  else return c;
  clampToTime(c, roundMs, slowestTokensPerSecond(host, model));
#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
  if (g_testFirstAttemptCap && c.maxTokens) c.maxTokens = g_testFirstAttemptCap;
#endif
  return c;
}

#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
bool setTestFirstAttemptCap(uint32_t tokens) {
  g_testFirstAttemptCap = tokens;
  return true;
}
uint32_t testFirstAttemptCap() { return g_testFirstAttemptCap; }
#endif

}  // namespace providers
}  // namespace agent
