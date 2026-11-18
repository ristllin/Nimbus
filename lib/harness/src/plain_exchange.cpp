// One exchange of the plain engine (plain/exchange.h), lane RB-EX: the output cap for the
// real body, the body budget and check, the post, the retry policy per error class, cut
// handling, and the qualification rule for fallback and ladder targets (PRD section 7).
#include "nimbus/harness/plain/exchange.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/platform.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_refusals.h"
#include "nimbus/orch/router_route.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/wire_limits.h"
#include "nimbus/utf8.h"

namespace agent {
namespace plain {

namespace {

namespace orch = nimbus::orch;
namespace hq = nimbus::orch::headqual;

// The provider slugs of the qualification table.
constexpr const char* kSlugs[] = {"anthropic", "openai", "mistral", "zai", "cumulo", "custom"};

std::string lower(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = (char)std::tolower((unsigned char)c);
  return o;
}

// Each provider's words for "the prompt is too large", recorded 1 Nov 2026 (lane OVF, the
// same markers as the old engine's isContextOverflowError, which engine 2 does not link).
bool namesContext(const std::string& body) {
  const std::string e = lower(body.substr(0, 4096));
  static const char* const kMarkers[] = {
      "context_length_exceeded", "maximum context length", "prompt is too long", "too many total text bytes",
      "prompt exceeds max length", "prompt_too_long", "context window", "exceed context limit",
      "too large for model",
  };
  for (const char* m : kMarkers)
    if (e.find(m) != std::string::npos) return true;
  return false;
}

// What one answer means for the policy.
struct Verdict {
  ExchangeErr err = ExchangeErr::None;
  orch::RouterAction action = orch::RouterAction::None;   // a routed refusal's contract action
  uint32_t retryAfterMs = 0;
  int64_t capTokens = -1;
};

// A routed refusal that names a reason the contract knows: decided by the reason.
bool routerVerdict(const std::string& body, Verdict& v) {
  const orch::RouterAnswer a = orch::readRouterAnswer(body);
  if (!a.row) {
    if (a.reason.empty()) return false;
    v.err = ExchangeErr::Other;   // a reason this image does not know: a per-call failure
    return true;
  }
  v.action = a.action();
  v.retryAfterMs = a.retryAfterMs;
  v.capTokens = a.maxAffordableOutputTokens;
  switch (v.action) {
    case orch::RouterAction::ParkBilling:
    case orch::RouterAction::ParkAuth: v.err = ExchangeErr::KeyRefused; break;
    case orch::RouterAction::RetryLater:
    case orch::RouterAction::RetryCapped: v.err = ExchangeErr::RateLimited; break;
    case orch::RouterAction::FailOver: v.err = ExchangeErr::Server; break;
    default: v.err = a.reason == "request_too_large" ? ExchangeErr::ContextRefused : ExchangeErr::Other;
  }
  return true;
}

// A direct head's answer, by its status and the provider's words.
ExchangeErr statusErr(int status, const std::string& body) {
  if (status == 401 || status == 402 || status == 403 || orch::isBillingRefusal(status, body))
    return ExchangeErr::KeyRefused;
  if (status == 413 || ((status == 400 || status == 422) && namesContext(body))) return ExchangeErr::ContextRefused;
  if (status == 429) return ExchangeErr::RateLimited;
  if (status == 408) return ExchangeErr::Transport;
  return status >= 500 ? ExchangeErr::Server : ExchangeErr::Other;
}

Verdict classify(int status, const std::string& body, bool routed) {
  Verdict v;
  if (status >= 200 && status < 300) return v;
  if (status <= 0) {
    v.err = ExchangeErr::Transport;
    return v;
  }
  if (routed && routerVerdict(body, v)) return v;
  v.err = statusErr(status, body);
  return v;
}

const char* providerName(const std::string& slug) {
  if (slug == "anthropic") return "Anthropic";
  if (slug == "openai") return "OpenAI";
  if (slug == "mistral") return "Mistral";
  if (slug == "zai") return "Z.ai";
  if (slug == "cumulo") return "The Cumulo router";
  return "The model's provider";
}

// Every JSON string of the body (splices excluded) holds at most the guard's text: a
// two-byte escape is one byte of text, a \uXXXX at most three.
bool stringsWithinGuard(const std::string& b) {
  bool in = false;
  size_t text = 0;
  for (size_t i = 0; i < b.size(); ++i) {
    const char c = b[i];
    if (!in) {
      if (c == '"') {
        in = true;
        text = 0;
      }
      continue;
    }
    if (c == '"') {
      in = false;
      continue;
    }
    if (c == '\\' && i + 1 < b.size()) text += b[++i] == 'u' ? 3 : 1;
    else ++text;
    if (text > orch::kJsonStrGuard) return false;
  }
  return true;
}

uint32_t minU(uint32_t a, uint32_t b) { return a < b ? a : b; }

// The state of one exchange: its requests, waits and retries.
class Exchange {
 public:
  Exchange(const ExchangeDeps& d, const ExchangeRequest& r, ExchangeResult& o) : deps_(d), req_(r), out_(o) {}

  Status run() {
    out_ = ExchangeResult{};
    Status st = setUp();
    if (!st.ok()) return st;
    for (;;) {
      bool sendable = false;
      st = prepare(sendable);
      if (!st.ok()) return st;
      if (!sendable) return finish();
      send();
      if (!again()) return finish();
    }
  }

 private:
  uint32_t now() const { return deps_.platform->nowMs(); }
  uint32_t elapsed() const { return now() - t0_; }
  uint32_t remaining() const {
    const uint32_t e = elapsed();
    return e >= req_.deadlineMs ? 0 : req_.deadlineMs - e;
  }
  // A wait of `ms` and then a request still fit the wall clock.
  bool fits(uint32_t ms) const { return remaining() >= ms + kMinRequestMs; }

  Status setUp() {
    const RenderInput* in = req_.render;
    if (!deps_.transport || !deps_.platform || !deps_.platform->nowMs || !deps_.platform->delayMs || !req_.wire ||
        !in || !in->turn || !in->head || req_.deadlineMs == 0)
      return Status::fail(Err::InvalidArgument, "plain::runExchange");
    t0_ = now();
    in_ = *in;
    head_ = *in->head;
    in_.head = &head_;
    slug_ = providerSlugOf(*in->turn, *in->head);
    capHost_ = slug_;
    model_ = in->turn->model;
    routed_ = in->head->routed || slug_ == "cumulo";
    if (slug_ == "cumulo") {   // the cap of a routed request is its upstream's
      const orch::RouterRoute route = orch::resolveRouterRoute(model_);
      capHost_ = route.upstream;
      model_ = route.model;
    }
    window_ = req_.timeoutMs ? req_.timeoutMs : orch::readWindowMsFor(capHost_, model_, routed_);
    bodyCap_ = req_.bodyCap ? req_.bodyCap : orch::tx::kFloorHeadBodyCap;
    return Status::success();
  }

  providers::RoundKind kind() const {
    return in_.toolChoiceNone ? providers::RoundKind::FinalRound : providers::RoundKind::ToolRound;
  }

  Status renderAt(uint32_t tokens) {
    in_.maxTokens = tokens;
    http_ = HttpRequest{};
    return req_.wire->render(in_, http_);
  }

  // The cap the next request carries: main's load-aware cap for this body inside the time
  // left (rendered once at the table's cap to measure the body), the retry cap after a cut,
  // or the router's clamp.
  Status renderFitted() {
    if (cutRetried_ && *cutEffort_) head_.effort = cutEffort_;
    const providers::OutputCap table = providers::tableCap(capHost_, model_, kind());
    Status st = renderAt(table.maxTokens);
    if (!st.ok()) return st;
    providers::RoundLoad load;
    load.bodyBytes = bodyLength(http_);
    load.imageBytes = load.bodyBytes - http_.body.size();
    load.routed = routed_;
    load.fragmented = deps_.platform->largestFreeBlock && providers::heapFragmented(deps_.platform->largestFreeBlock());
    cap_ = providers::outputCapFor(capHost_, model_, kind(), minU(window_, remaining()), load);
    uint32_t tokens = cutRetried_ ? cutTokens_ : cap_.maxTokens;
    if (routerCap_ && (!tokens || routerCap_ < tokens)) tokens = routerCap_;
    return tokens == in_.maxTokens ? st : renderAt(tokens);
  }

  // Render until the body fits its cap (the ladder climbs a rung each time), then check it.
  // `sendable` false: nothing fits and nothing was sent.
  Status prepare(bool& sendable) {
    for (;;) {
      Status st = renderFitted();
      if (!st.ok()) return st;
      if (bodyLength(http_) <= bodyCap_) break;
      if (!req_.climb || capClimbs_ >= kMaxCapClimbs || !req_.climb(Climb::OverCap)) {
        verdict_ = Verdict{};
        verdict_.err = ExchangeErr::ContextRefused;
        httpStatus_ = 0;
        return Status::success();
      }
      ++capClimbs_;
      ++out_.climbs;
    }
    if (!stringsWithinGuard(http_.body) || !nimbus::utf8Valid(http_.body))
      return Status::fail(Err::InvalidArgument, "plain::runExchange body check");
    sendable = true;
    return Status::success();
  }

  void send() {
    http_.timeoutMs = minU(window_, remaining());
    HttpResponse resp;
    std::string err;
    ++out_.requests;
    out_.requestBytes = (uint32_t)bodyLength(http_);
    out_.maxTokens = in_.maxTokens;
    const bool answered = deps_.transport->exec(http_, resp, err) && resp.status > 0;
    httpStatus_ = answered ? resp.status : 0;
    verdict_ = classify(httpStatus_, resp.body, routed_);
    if (verdict_.err == ExchangeErr::None) read(resp);
  }

  void read(const HttpResponse& resp) {
    ParsedStep step;
    if (!req_.wire->parse(resp, step).ok()) {
      verdict_.err = ExchangeErr::Malformed;
      return;
    }
    usage_ += step.usage;
    if (step.stop == StopReason::MaxTokens) {
      verdict_.err = ExchangeErr::Cut;
      for (ToolCall& c : step.calls) c.complete = false;   // cut arguments are never dispatched
      cutStep_ = std::move(step);
      haveCut_ = true;
      return;
    }
    step_ = std::move(step);
  }

  // Whether the exchange sends again (after any wait), by the class of the last answer.
  bool again() {
    switch (verdict_.err) {
      case ExchangeErr::Cut: return retryCut();
      case ExchangeErr::ContextRefused: return retryClimb();
      case ExchangeErr::Transport:
      case ExchangeErr::Server:
      case ExchangeErr::RateLimited:
        return verdict_.action == orch::RouterAction::None ? retrySameHost() : retryRouter();
      default: return false;
    }
  }

  bool wait(uint32_t ms) {
    deps_.platform->delayMs(ms);
    out_.waitedMs += ms;
    return true;
  }

  bool retrySameHost() {
    constexpr size_t kLastWait = sizeof(kRetryWaitMs) / sizeof(kRetryWaitMs[0]) - 1;
    const uint32_t ms = kRetryWaitMs[sameHost_ < kLastWait ? sameHost_ : kLastWait];
    if (sameHost_ >= kMaxSameHostRetries || !fits(ms)) return false;
    ++sameHost_;
    return wait(ms);
  }

  bool retryRouter() {
    if (verdict_.action == orch::RouterAction::FailOver) {
      out_.failOver = true;
      return false;
    }
    if (routerResent_ || !fits(verdict_.retryAfterMs)) return false;
    routerResent_ = true;
    if (verdict_.action == orch::RouterAction::RetryCapped && verdict_.capTokens >= orch::kRouterCapFloorTokens) {
      routerCap_ = (uint32_t)std::min<int64_t>(verdict_.capTokens, UINT32_MAX);
      return true;
    }
    if (verdict_.retryAfterMs == 0 || verdict_.retryAfterMs > orch::kRouterInlineRetryMaxMs) return false;
    return wait(verdict_.retryAfterMs);
  }

  // The one retry of a cut answer, at the retry cap of the cut request's own fit.
  bool retryCut() {
    if (cutRetried_ || !cap_.retryTokens || !fits(0)) return false;
    cutRetried_ = true;
    cutTokens_ = cap_.retryTokens;
    cutEffort_ = cap_.retryEffort;
    return true;
  }

  bool retryClimb() {
    if (refusedClimbed_ || !req_.climb || !fits(0) || !req_.climb(Climb::Refused)) return false;
    refusedClimbed_ = true;
    ++out_.climbs;
    return true;
  }

  Status finish() {
    ExchangeErr err = verdict_.err;
    if (err != ExchangeErr::None && haveCut_) {   // the cut answer in hand is delivered
      err = ExchangeErr::Cut;
      step_ = std::move(cutStep_);
    }
    out_.err = err;
    out_.httpStatus = httpStatus_;
    out_.retries = out_.requests ? out_.requests - 1 : 0;
    out_.cutRetried = cutRetried_;
    out_.ms = elapsed();
    out_.parkHost = verdict_.err == ExchangeErr::KeyRefused;   // a park holds even when a cut is delivered
    step_.usage = usage_;
    out_.step = std::move(step_);
    if (err == ExchangeErr::None) return Status::success();
    out_.status = err == ExchangeErr::Cut ? StatusLine::CutAnswer : StatusLine::ProviderError;
    out_.errorLine = exchangeLine(err, slug_, httpStatus_);
    return Status::success();
  }

  const ExchangeDeps& deps_;
  const ExchangeRequest& req_;
  ExchangeResult& out_;
  RenderInput in_;
  HeadEndpoint head_;
  std::string slug_, capHost_, model_;
  bool routed_ = false;
  uint32_t t0_ = 0, window_ = 0, bodyCap_ = 0;
  providers::OutputCap cap_;
  HttpRequest http_;
  Verdict verdict_;
  int httpStatus_ = 0;
  ParsedStep step_, cutStep_;
  bool haveCut_ = false;
  nimbus::orch::TokenUsage usage_;
  uint8_t sameHost_ = 0, capClimbs_ = 0;
  bool cutRetried_ = false, refusedClimbed_ = false, routerResent_ = false;
  uint32_t routerCap_ = 0;
  uint32_t cutTokens_ = 0;
  const char* cutEffort_ = "";
};

}  // namespace

Status runExchange(const ExchangeDeps& deps, const ExchangeRequest& req, ExchangeResult& out) {
  Exchange ex(deps, req, out);
  return ex.run();
}

ExchangeErr classifyAnswer(int status, const std::string& body, bool routed) {
  return classify(status, body, routed).err;
}

std::string providerSlugOf(const TurnCtx& turn, const HeadEndpoint& head) {
  for (const char* s : kSlugs)
    if (turn.host == s) return s;
  const std::string h = lower(head.host);
  if (h == "api.anthropic.com") return "anthropic";
  if (h == "api.openai.com") return "openai";
  if (h == "api.mistral.ai") return "mistral";
  if (h == "api.z.ai" || h == "open.bigmodel.cn") return "zai";
  return head.routed ? "cumulo" : "custom";
}

// The owner's line for a failed exchange. Plain words, the next step, no provider text
// (a provider's message is outside content and stays in the log).
std::string exchangeLine(ExchangeErr err, const std::string& slug, int httpStatus) {
  const std::string who = providerName(slug);
  const std::string code = httpStatus > 0 ? " (HTTP " + std::to_string(httpStatus) + ")" : "";
  switch (err) {
    case ExchangeErr::None: return "";
    case ExchangeErr::Transport: return "No reply: the connection to " + who + " failed. Send the message again to retry.";
    case ExchangeErr::Server: return "No reply: " + who + " had a server error" + code + ". Try again in a minute.";
    case ExchangeErr::RateLimited: return "No reply: " + who + " is limiting requests" + code + ". Try again in a minute.";
    case ExchangeErr::ContextRefused: return "No reply: the conversation is too large for this model. Send it again shorter.";
    case ExchangeErr::KeyRefused:
      return "No reply: " + who + " refused the key or the account has no credit" + code + ". Check the key and credit.";
    case ExchangeErr::Cut: return "The reply above was cut at the length limit.";
    case ExchangeErr::Malformed: return "No reply: " + who + " sent an answer the device could not read. Try again.";
    case ExchangeErr::Blocked: return "The reply was blocked by moderation.";
    case ExchangeErr::Other: break;
  }
  return "No reply: " + who + " refused the request" + code + ".";
}

bool targetQualified(const HeadTarget& t, hq::Line line) {
  return hq::lookup(t.host.c_str(), t.model.c_str(), line, t.wire).state == hq::State::Qualified;
}

Status pickFallback(const FallbackFacts& facts, int& index) {
  index = -1;
  if (!facts.order) return Status::fail(Err::InvalidArgument, "plain::pickFallback");
  if (!facts.freshAttempt || facts.effectRun || !facts.headParked) return Status::success();
  for (size_t i = 0; i < facts.order->size(); ++i) {
    const HeadTarget& t = (*facts.order)[i];
    if (!t.parked && targetQualified(t, facts.line)) {
      index = (int)i;
      break;
    }
  }
  return Status::success();
}

Status qualifiedTargets(const std::vector<HeadTarget>& ladder, hq::Line line, std::vector<HeadTarget>& out) {
  std::vector<HeadTarget> kept;
  for (const HeadTarget& t : ladder)
    if (targetQualified(t, line)) kept.push_back(t);
  out.swap(kept);
  return Status::success();
}

}  // namespace plain
}  // namespace agent
