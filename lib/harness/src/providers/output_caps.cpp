#include "nimbus/harness/output_caps.h"

#include "nimbus/orch/model_defaults.h"   // lane DEF: a background turn thinks little

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace agent {
namespace providers {

namespace {

#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
uint32_t g_testFirstAttemptCap = 0;   // test and host builds only (setTestFirstAttemptCap)
int g_testCutRounds = 0;              // ... (setTestCutRounds)
uint32_t g_testCutTokens = 0;
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
  // Lane DEF: a background turn (a routine, a wakeup, the dream) pins low effort too:
  // nobody waits on it, and its thinking is what a silent run still bills.
  const bool background =
      nimbus::orch::currentTurnRole() == nimbus::orch::TurnRole::Background;
  c.effort = effort ? ((kind == RoundKind::Fold || background) ? "low" : "medium") : "";
  c.retryEffort = effort ? "low" : "";
  return c;
}

OutputCap mistralCap(const std::string& model) {
  OutputCap c;
  // A model that thinks spends its output budget on it, and a cut ends the turn badly.
  // Measured live 1 Nov: Large 4 at "low" wrote a 1,800-token guide in 18 s, and with no
  // effort field it was cut at 4,000. The time clamp below holds these inside the 60 s
  // at which Mistral's gateway closes a request (kMistralRoundMs), so a real round gets
  // less than these figures: test_mistral_caps_are_the_documented_numbers pins what it gets.
  const std::string m = startsWith(model, "mistral/") ? model.substr(8) : model;
  const bool fast = startsWith(m, "mistral-large-4") || startsWith(m, "mistral-small") ||
                    startsWith(m, "mistral-medium") || startsWith(m, "magistral-");
  c.maxTokens = fast ? 6000 : 4096;
  c.retryTokens = fast ? 12000 : 6144;
  // Only models that take the field get it, at the lowest value each takes, and the cut
  // retry keeps it.
  c.effort = mistralHeadEffort(model);
  c.retryEffort = c.effort;
  return c;
}

// GLM always reasons and Z.ai's gateway closes a request at 60 s (60.6 s, no response,
// both GLM models, 1 Nov). Measured 61 to 73 tok/s, so about 2,400 tokens fit a round of
// 50 s, and the retry after a cut keeps a little more.
OutputCap zaiCap() {
  OutputCap c;
  c.maxTokens = 2400;
  c.retryTokens = 3000;
  c.effort = "low";
  c.retryEffort = "low";
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

const char* modelFitNote(const std::string& provider, const std::string& model) {
  if (provider != "mistral" && provider != "cumulo") return "";
  const std::string m = startsWith(model, "mistral/") ? model.substr(8) : model;
  if (!startsWith(m, "mistral-small")) return "";
  return "Mistral Small is quick and cheap for chat, but it can say a file was changed without changing it and "
         "it stalls on food math. For daily use, pick Mistral Large 4.";
}

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

const char* mistralHeadEffort(const std::string& model) {
  if (!mistralTakesEffort(model)) return "";
  const std::string m = startsWith(model, "mistral/") ? model.substr(8) : model;
  // These refuse "none" with 400 code 3051 and list low, medium and high (recorded
  // 1 Nov). With the field dropped they think at their default and get cut.
  for (const char* p : {"mistral-large-4", "zai-glm", "glm-"})
    if (startsWith(m, p)) return "low";
  return "none";   // every other reasoning model refuses "low" (the same recording)
}

const char* sameHostNonReasoningModel(const std::string& host, const std::string& model) {
  if (host != "mistral") return "";
  const std::string m = startsWith(model, "mistral/") ? model.substr(8) : model;
  // Only Large 4 is forced to reason (it refuses effort "none"); every other Mistral
  // head runs at "none", so a cut there is a long answer, not reasoning, and the next
  // provider - not a smaller same-provider model - is the right escape.
  if (!startsWith(m, "mistral-large-4")) return "";
  return "mistral-medium-latest";   // the default Mistral head, at effort "none"
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
    if (startsWith(model, "mistral-small") || startsWith(model, "ministral")) return 130;   // 156 to 198, 1 Nov
    if (startsWith(model, "mistral-medium")) return 130;   // 196, 1 Nov
    if (startsWith(model, "mistral-large-4")) return 100;   // 101 to 210, 1 Nov
    return 55;
  }
  if (host == "zai") return 60;   // 61 to 73, 1 Nov
  return 50;
}

OutputCap outputCapFor(const std::string& host, const std::string& model, RoundKind kind,
                       uint32_t roundMs) {
  OutputCap c;
  if (host == "anthropic") c = anthropicCap(model, kind);
  else if (host == "mistral") c = mistralCap(model);
  else if (host == "zai") c = zaiCap();
  // OpenAI: no max_output_tokens - reasoning tokens count toward it and would
  // starve the answer; completeness is read from status:"incomplete" instead.
  // Custom: the head sends no cap either; finish_reason "length" is read.
  else return c;
  clampToTime(c, roundMs, slowestTokensPerSecond(host, model));
#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
  if (g_testFirstAttemptCap && c.maxTokens) c.maxTokens = g_testFirstAttemptCap;
  if (g_testCutRounds > 0 && c.maxTokens) {
    g_testCutRounds--;
    c.maxTokens = g_testCutTokens;
    c.retryTokens = g_testCutTokens + 16;   // a retry that differs, and is cut again
  }
#endif
  return c;
}

#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)
bool setTestFirstAttemptCap(uint32_t tokens) {
  g_testFirstAttemptCap = tokens;
  return true;
}
uint32_t testFirstAttemptCap() { return g_testFirstAttemptCap; }
bool setTestCutRounds(int rounds, uint32_t tokens) {
  g_testCutRounds = rounds;
  g_testCutTokens = tokens;
  return true;
}
#endif

}  // namespace providers
}  // namespace agent
