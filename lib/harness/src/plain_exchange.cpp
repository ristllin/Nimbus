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
#include "nimbus/harness/plain/texts.h"
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/compact.h"   // isContextOverflowError: each provider's words for a size refusal
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_refusals.h"
#include "nimbus/orch/router_route.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/wire_limits.h"
#include "nimbus/utf8.h"
#include "providers/plain_body.h"   // the body check counts strings as the writer does

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
  if (status == 413 || ((status == 400 || status == 422) && orch::isContextOverflowError(body.substr(0, 4096)))) return ExchangeErr::ContextRefused;
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
  // The contract parks the router only by its reason: a routed 401, 402 or 403 that names
  // none (a proxy, a balancer) is a per-call failure.
  v.err = routed && status >= 401 && status <= 403 ? ExchangeErr::Other : statusErr(status, body);
  return v;
}

const char* providerName(const std::string& slug) {
  namespace n = texts::device_notes;
  if (slug == "anthropic") return n::kAnthropic;
  if (slug == "openai") return n::kOpenAI;
  if (slug == "mistral") return n::kMistral;
  if (slug == "zai") return n::kZai;
  if (slug == "cumulo") return n::kRouter;
  return n::kProvider;
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
      if (!st.ok() && out_.requests == 0) return st;
      if (!st.ok()) {   // a render that fails after a request: the answer in hand still goes
        verdict_ = Verdict{};
        verdict_.err = ExchangeErr::Other;
        return finish();
      }
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
  bool fits(uint32_t ms) const {
    const uint32_t left = remaining();
    return ms <= left && left - ms >= kMinRequestMs;
  }

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
    // A custom host that speaks the Messages API takes its output cap from Anthropic's table: that
    // API requires max_tokens, and the custom row has none (RB-W1b).
    tableHost_ = capHost_ == "custom" && in->turn->dialect == Dialect::AnthropicMessages ? "anthropic" : capHost_;
    // No cap given: the head's released row, never the 85,000 B floor by accident (ruling 83).
    bodyCap_ = req_.bodyCap ? req_.bodyCap : orch::requestBudgetFor(slug_, in->turn->model, routed_).wireBodyBytes;
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
    const providers::OutputCap table = providers::tableCap(tableHost_, model_, kind());
    Status st = renderAt(table.maxTokens);
    if (!st.ok()) return st;
    providers::RoundLoad load;
    load.bodyBytes = bodyLength(http_);
    load.imageBytes = load.bodyBytes - http_.body.size();
    load.routed = routed_;
    load.fragmented = deps_.platform->largestFreeBlock && providers::heapFragmented(deps_.platform->largestFreeBlock());
    cap_ = providers::outputCapFor(tableHost_, model_, kind(), minU(window_, remaining()), load);
    // the cut retry takes the retry cap of this request's own fit (the time left now)
    uint32_t tokens = cutRetried_ ? cap_.retryTokens : cap_.maxTokens;
    if (routerCap_ && (!tokens || routerCap_ < tokens)) tokens = routerCap_;
    // a retry that would send what the cut request sent is not taken: it would be cut again
    noRoom_ = cutRetried_ && tokens <= cutSentTokens_ && head_.effort == cutSentEffort_;
    if (tokens != in_.maxTokens) st = renderAt(tokens);
    http_.cappedResend = routerCap_ != 0 && tokens == routerCap_;
    return st;
  }

  // Render until the body fits its cap (the ladder climbs a rung each time), then check it.
  // `sendable` false: nothing fits and nothing was sent.
  Status prepare(bool& sendable) {
    for (;;) {
      Status st = renderFitted();
      if (!st.ok()) return st;
      if (noRoom_) return Status::success();   // the cut answer in hand is delivered
      if (remaining() == 0) {                   // no time left to send anything
        verdict_ = Verdict{};
        verdict_.err = ExchangeErr::Transport;
        return Status::success();
      }
      if (bodyLength(http_) <= bodyCap_) break;
      if (!req_.climb || capClimbs_ >= kMaxCapClimbs || !req_.climb(ClimbAsk{Climb::OverCap, bodyLength(http_), bodyCap_})) {
        verdict_ = Verdict{};
        verdict_.err = ExchangeErr::ContextRefused;
        httpStatus_ = 0;
        return Status::success();
      }
      ++capClimbs_;
      ++out_.climbs;
    }
    if (!body::detail::stringsWithinGuard(http_.body, orch::kJsonStrGuard) || !nimbus::utf8Valid(http_.body))
      return Status::fail(Err::InvalidArgument, "plain::runExchange.bodyCheck");
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
    recordRequest(in_, http_, out_.ref);   // the dossier keeps this request by reference (RB-E3)
    const bool answered = deps_.transport->exec(http_, resp, err) && resp.status > 0;
    httpStatus_ = answered ? resp.status : 0;
    verdict_ = classify(httpStatus_, resp.body, routed_);
    if (verdict_.err == ExchangeErr::None) read(resp);
    else if (verdict_.err == ExchangeErr::Other) refusedBody_ = resp.body.substr(0, 4096);
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
      case ExchangeErr::Other: return retryRefusedRef();
      case ExchangeErr::Transport:
      case ExchangeErr::Server:
      case ExchangeErr::RateLimited:
        return verdict_.action == orch::RouterAction::None ? retrySameHost() : retryRouter();
      default: return false;
    }
  }

  void wait(uint32_t ms) {
    deps_.platform->delayMs(ms);
    out_.waitedMs += ms;
  }

  bool retrySameHost() {
    constexpr size_t kLastWait = sizeof(kRetryWaitMs) / sizeof(kRetryWaitMs[0]) - 1;
    const uint32_t ms = kRetryWaitMs[sameHost_ < kLastWait ? sameHost_ : kLastWait];
    if (sameHost_ >= kMaxSameHostRetries || !fits(ms)) return false;
    ++sameHost_;
    wait(ms);
    return true;
  }

  bool retryRouter() {
    if (verdict_.action == orch::RouterAction::FailOver) {
      out_.failOver = true;
      return false;
    }
    if (routerResent_) return false;
    if (verdict_.action == orch::RouterAction::RetryCapped && verdict_.capTokens >= orch::kRouterCapFloorTokens) {
      if (!fits(0)) return false;
      routerResent_ = true;
      routerCap_ = (uint32_t)std::min<int64_t>(verdict_.capTokens, UINT32_MAX);
      return true;
    }
    const uint32_t ms = verdict_.retryAfterMs;
    if (ms == 0 || ms > orch::kRouterInlineRetryMaxMs || !fits(ms)) return false;
    routerResent_ = true;
    wait(ms);
    return true;
  }

  // The one retry of a cut answer, at the retry cap of the cut request's own fit.
  bool retryCut() {
    if (cutRetried_ || !cap_.retryTokens || !fits(0)) return false;
    cutRetried_ = true;
    cutSentTokens_ = in_.maxTokens;
    cutSentEffort_ = head_.effort;
    cutEffort_ = cap_.retryEffort;
    return true;
  }

  // A file reference the provider no longer holds: the caller forgets it and the same stored
  // state renders once more without it (inline bytes or the stub line).
  bool retryRefusedRef() {
    if (refResent_ || !req_.refusedRef || (httpStatus_ != 400 && httpStatus_ != 404) || !fits(0)) return false;
    if (!req_.refusedRef(httpStatus_, refusedBody_)) return false;
    refResent_ = true;
    return true;
  }

  bool retryClimb() {
    if (refusedClimbed_ || !req_.climb || !fits(0) || !req_.climb(ClimbAsk{Climb::Refused, 0, bodyCap_})) return false;
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
    out_.status = err == ExchangeErr::Cut       ? StatusLine::CutAnswer
                  : err == ExchangeErr::Blocked ? StatusLine::ModerationBlock
                                                : StatusLine::ProviderError;
    out_.errorLine = exchangeLine(err, slug_, httpStatus_);
    return Status::success();
  }

  const ExchangeDeps& deps_;
  const ExchangeRequest& req_;
  ExchangeResult& out_;
  RenderInput in_;
  HeadEndpoint head_;
  std::string slug_, capHost_, tableHost_, model_;
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
  bool cutRetried_ = false, refusedClimbed_ = false, routerResent_ = false, refResent_ = false;
  std::string refusedBody_;
  uint32_t routerCap_ = 0;
  uint32_t cutSentTokens_ = 0;
  std::string cutSentEffort_;
  const char* cutEffort_ = "";
  bool noRoom_ = false;
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

// The owner's line for a failed exchange (texts.h device_notes): what happened, then the
// one next step; no provider text (a provider's message is outside content and stays in the log).
std::string exchangeLine(ExchangeErr err, const std::string& slug, int httpStatus) {
  namespace n = texts::device_notes;
  const std::string who = std::string(n::kNoReply) + providerName(slug);
  const std::string code = httpStatus > 0 ? n::kHttp + std::to_string(httpStatus) + ")" : "";
  switch (err) {
    case ExchangeErr::None: return "";
    case ExchangeErr::Transport: return who + n::kUnreachable;
    case ExchangeErr::Server: return who + n::kServerError + code + n::kTryLater;
    case ExchangeErr::RateLimited: return who + n::kLimiting + code + n::kTryLater;
    case ExchangeErr::ContextRefused: return std::string(n::kNoReply) + n::kTooLarge;
    case ExchangeErr::KeyRefused: return who + n::kKeyRefused + code + n::kCheckKey;
    case ExchangeErr::Cut: return n::kCut;
    case ExchangeErr::Malformed: return who + n::kUnreadable;
    case ExchangeErr::Blocked: return n::kBlocked;
    case ExchangeErr::Other: break;
  }
  return who + n::kRefused + code + ".";
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
