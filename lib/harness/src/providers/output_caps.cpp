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

OutputCap mistralCap() {
  OutputCap c;
  // About 60 s of output at the slowest measured Mistral rate (65 tok/s, Lumi
  // 13-14 Oct), inside the 120 s round ceiling; the retry gets half again.
  c.maxTokens = 4096;
  c.retryTokens = 6144;
  return c;
}

uint32_t clampToTime(uint32_t tokens, uint32_t roundMs, uint32_t rate) {
  if (tokens == 0 || roundMs == 0) return tokens;
  const uint32_t kPrefillMs = 10000;   // connect, TLS, upload and prompt prefill
  const uint32_t kFloor = 2048;        // a final round still gets room for a whole turn
  const uint32_t genMs = roundMs > kPrefillMs ? roundMs - kPrefillMs : 0;
  uint32_t fit = (uint32_t)((uint64_t)genMs * rate / 1000);
  if (fit < kFloor) fit = kFloor;
  return tokens < fit ? tokens : fit;
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
  else if (host == "mistral") c = mistralCap();
  // OpenAI: no max_output_tokens - reasoning tokens count toward it and would
  // starve the answer; completeness is read from status:"incomplete" instead.
  // Custom / Z.ai: the head sends no cap either; finish_reason "length" is read.
  else return c;
  const uint32_t rate = slowestTokensPerSecond(host, model);
  c.maxTokens = clampToTime(c.maxTokens, roundMs, rate);
  c.retryTokens = clampToTime(c.retryTokens, roundMs, rate);
#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
  if (g_testFirstAttemptCap && c.maxTokens) c.maxTokens = g_testFirstAttemptCap;
#endif
  return c;
}

#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
void setTestFirstAttemptCap(uint32_t tokens) { g_testFirstAttemptCap = tokens; }
uint32_t testFirstAttemptCap() { return g_testFirstAttemptCap; }
#endif

}  // namespace providers
}  // namespace agent
