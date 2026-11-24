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
#include "nimbus/harness/plain/route.h"
#include "nimbus/harness/plain/texts.h"
#include "nimbus/harness/providers.h"   // kRoutedDeadlineHeader, kRoutedDeadlineSlackMs
#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/compact.h"   // isContextOverflowError: each provider's words for a size refusal
#include "nimbus/orch/provider_error.h"   // providerErrorText: the message of a provider's error body
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/router_refusals.h"
#include "nimbus/orch/tx/budget.h"
#include "nimbus/orch/tx/rules.h"   // kPhotoCapBytes: a photo against the body cap
#include "nimbus/orch/wire_errors.h"   // classifyProvider: the router's one error order
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
  orch::caps::ErrClass cls = orch::caps::ErrClass::BadRequest;   // the classifier's class (when it classed one)
  uint16_t refusedFields = 0;   // the optional fields a 400 or 422 refused (caps::refused::)
};

// A routed refusal that names a reason the contract knows: decided by the reason.
bool routerVerdict(const std::string& body, Verdict& v) {
  const orch::RouterAnswer a = orch::wire::readRouterAnswerE2(body);
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

// By the order of ROUTER-DESIGN.md section 9 (nimbus/orch/wire_errors.h), with the marker rows
// of `provider` (a routed head's upstream; "" = the generic rules alone, as before the router).
Verdict classify(int status, const std::string& body, bool routed, const std::string& provider) {
  namespace w = orch::wire;
  using E = ExchangeErr;
  // By orch::caps::ErrClass: a refused field, an unknown model, a bad request and a refusal are Other.
  static constexpr ExchangeErr kByClass[] = {E::Transport,  E::RateLimited, E::Server, E::KeyRefused,
                                             E::KeyRefused, E::KeyRefused,  E::ContextRefused, E::Other,
                                             E::Other,      E::Other,       E::Other};
  static_assert(sizeof(kByClass) / sizeof(kByClass[0]) == (size_t)orch::caps::ErrClass::Refusal + 1, "one per class");
  Verdict v;
  const bool ok = status >= 200 && status < 300;
  if (!ok && status > 0 && routed && routerVerdict(body, v)) return v;
  // The contract parks the router only by its reason: a routed 401, 402 or 403 that names
  // none (a proxy, a balancer) is a per-call failure.
  if (routed && status >= 401 && status <= 403) {
    v.err = ExchangeErr::Other;
    return v;
  }
  const w::Classified c = w::classifyProvider(w::markerSetFor(provider), status, body);
  if (!c.error) return v;
  v.err = kByClass[(size_t)c.cls];
  v.cls = c.cls;
  v.refusedFields = c.cls == orch::caps::ErrClass::RefusedField ? c.refused : 0;
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
    base_ = *in;
    head_ = *in->head;
    base_.head = &head_;
    refused_ = deps_.refused ? deps_.refused : &ownRefused_;
    slug_ = headSlugOf(*in->turn, *in->head);
    return routeOf(*in->turn);
  }

  // The route (plain/route.h): the upstream and bare model of a routed head, the cap table's host,
  // the rows; a head no slug names keeps an empty route and needs the caller's body cap.
  Status routeOf(const TurnCtx& turn) {
    const bool routeOk = resolveRoute(head_, turn, route_, refused_).ok();
    capHost_ = route_.provider;
    tableHost_ = route_.capHost;
    model_ = route_.model;
    routed_ = route_.routed;
    // The read window is the route's at most: a caller's longer timeout is lowered to it.
    const uint32_t routeWindow = orch::readWindowMsFor(capHost_, model_, routed_);
    window_ = req_.timeoutMs && req_.timeoutMs < routeWindow ? req_.timeoutMs : routeWindow;
    // The body cap: the caller's, or with none given the head's full body (tx::budgetFor), the one
    // fallback figure the turn path uses too; never the 85,000 B floor by accident (ruling 83) and
    // never a head the setup did not name (ruling 91).
    bodyCap_ = req_.bodyCap ? req_.bodyCap : routeOk ? route_.bodyCap : 0;
    if (!bodyCap_) return Status::fail(Err::InvalidArgument, "plain::runExchange.head");
    return Status::success();
  }

  providers::RoundKind kind() const {
    return base_.toolChoiceNone ? providers::RoundKind::FinalRound : providers::RoundKind::ToolRound;
  }

  // One render at `tokens`, through the pre-flight (plain/route.h) every time: the codec reads the
  // route, and what the row would refuse is left out and counted in dropped_.
  Status renderAt(uint32_t tokens) {
    asked_ = tokens;
    in_ = base_;
    in_.maxTokens = tokens;
    in_.bodyCap = bodyCap_;
    dropped_ = preflight(route_, ceiling(), pre_, in_);
    http_ = HttpRequest{};
    const Status st = req_.wire->render(in_, http_);
    dropped_ |= pre_.lateDropped;
    return st;
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
    if (req_.capCeiling) ceilCaps(load);
    // the cut retry takes the retry cap of this request's own fit (the time left now)
    uint32_t tokens = cutRetried_ ? cap_.retryTokens : cap_.maxTokens;
    if (routerCap_ && (!tokens || routerCap_ < tokens)) tokens = routerCap_;
    // a retry that would send what the cut request sent is not taken: it would be cut again
    noRoom_ = cutRetried_ && tokens <= cutSentTokens_ && head_.effort == cutSentEffort_;
    if (tokens != asked_) st = renderAt(tokens);
    http_.cappedResend = routerCap_ != 0 && tokens == routerCap_;
    return st;
  }

  // Lane E2-R4b (router design section 6 item 8): the caller's ceilings (the summarizer's two rungs). The
  // first request carries at most capCeiling; the cut retry at most retryCeiling, inside the time fit, and
  // only when it gives kRetryGainTokens more (the shared retry-gain rule), at the same effort.
  uint32_t ceiling() const { return cutRetried_ && req_.retryCeiling ? req_.retryCeiling : req_.capCeiling; }
  void ceilCaps(const providers::RoundLoad& load) {
    if (!cap_.maxTokens) return;   // an uncapped row sends no cap, the summarizer's included
    cap_.maxTokens = minU(cap_.maxTokens, req_.capCeiling);
    const uint32_t fit = providers::roundFitTokens(tableHost_, model_, minU(window_, remaining()), load);
    const uint32_t retry = minU(fit, req_.retryCeiling ? req_.retryCeiling : req_.capCeiling);
    cap_.retryTokens = retry >= cap_.maxTokens + providers::kRetryGainTokens ? retry : 0;
    cap_.retryEffort = "";
  }

  // What the body cap measures (lane E2-FIX4): the text of the body, and each photo spliced into it
  // as the text its tokens would be (tx::kPhotoCapBytes), never its base64. The cap sizes the
  // conversation in tokens; a photo's base64 is about a hundred times its tokens, and the photo caps
  // (tx::visiblePhotos) bound the pixels a request carries. Counted as base64, four photos in view
  // would push the conversation out of the request.
  static size_t capLength(const HttpRequest& r) { return r.body.size() + r.splices.size() * orch::tx::kPhotoCapBytes; }

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
      const size_t capped = capLength(http_);
      if (capped <= bodyCap_) break;
      if (!req_.climb || capClimbs_ >= kMaxCapClimbs || !req_.climb(ClimbAsk{Climb::OverCap, capped, bodyCap_})) {
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
    // A routed request tells the router when the device gives up on it (x-cumulo-deadline-ms).
    if (routed_ && http_.timeoutMs > providers::kRoutedDeadlineSlackMs)
      http_.headers.emplace_back(providers::kRoutedDeadlineHeader,
                                 std::to_string(http_.timeoutMs - providers::kRoutedDeadlineSlackMs));
    HttpResponse resp;
    std::string err;
    ++out_.requests;
    out_.requestBytes = (uint32_t)bodyLength(http_);
    out_.maxTokens = in_.maxTokens;
    out_.dropped = dropped_;
    recordRequest(in_, http_, out_.ref);   // the dossier keeps this request by reference (RB-E3)
    out_.ref.dropped = dropped_;
    const bool answered = deps_.transport->exec(http_, resp, err) && resp.status > 0;
    httpStatus_ = answered ? resp.status : 0;
    verdict_ = classify(httpStatus_, resp.body, routed_, capHost_);
    refusalAnswer_ = false;
    if (verdict_.err == ExchangeErr::Other) providerVerdict(resp.body);
    if (verdict_.err == ExchangeErr::None && !refusalAnswer_) read(resp);
    else if (verdict_.err == ExchangeErr::Other) refusedBody_ = resp.body.substr(0, 4096);
    // E2-FIX14: a key or credit refusal's own words, so the shared park can tell billing from the key.
    parkBody_ = verdict_.err == ExchangeErr::KeyRefused ? resp.body.substr(0, 4096) : std::string();
    // The reason belongs to this answer only (F7): a later answer of another class clears it, so a
    // line never joins one status to an earlier status's words. From the whole body: a long message
    // still parses.
    refusedReason_ = verdict_.err == ExchangeErr::Other ? refusalReason(resp.body) : std::string();
  }

  // What the provider's class of an Other answer means (router design section 9): a refusal code is
  // the model's refusal answer (nothing to run), an unknown model is marked unusable.
  void providerVerdict(const std::string& body) {
    if (verdict_.cls == orch::caps::ErrClass::UnknownModel) out_.modelUnusable = true;
    if (verdict_.cls != orch::caps::ErrClass::Refusal) return;
    step_ = ParsedStep{};
    step_.stop = StopReason::Refusal;
    step_.refusal = orch::wire::readErrorObject(body).message;
    verdict_.err = ExchangeErr::None;
    refusalAnswer_ = true;
  }

  void read(const HttpResponse& resp) {
    ParsedStep step;
    if (!req_.wire->parse(resp, step).ok()) {
      verdict_.err = ExchangeErr::Malformed;
      return;
    }
    usage_ += step.usage;
    const bool cut = step.stop == StopReason::MaxTokens, full = step.stop == StopReason::Context;
    if (cut || full || step.stop == StopReason::Refusal)
      for (ToolCall& c : step.calls) c.complete = false;   // cut, cut short or refused arguments never run
    if (!cut && !full) {
      step_ = std::move(step);
      return;
    }
    // A cut is retried with more room; a full window climbs a rung (a larger cap cannot help). Either
    // one after a window-full climb is delivered as it stands.
    verdict_.err = full ? ExchangeErr::ContextRefused : ExchangeErr::Cut;
    if (contextClimbed_) verdict_.err = ExchangeErr::Cut;
    cutStep_ = std::move(step);
    haveCut_ = true;
  }

  // Whether the exchange sends again (after any wait), by the class of the last answer.
  bool again() {
    switch (verdict_.err) {
      case ExchangeErr::Cut: return retryCut();
      case ExchangeErr::ContextRefused: return retryClimb();
      case ExchangeErr::Other: return retryRefusedField() || retryRefusedRef();
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
    if (cutRetried_ || contextClimbed_ || !cap_.retryTokens || !fits(0)) return false;
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

  // A request refused for an optional field it named (reasoning effort, a schema, tool choice none, ...):
  // remembered for the boot, the route narrowed, the same stored state rendered once more without it.
  // Refusals a caller already owns keep their paths: a photo (the refused-photo line), a file id
  // (refusedRef) and the summarizer's structured form (the compaction driver asks in lines and marks).
  bool retryRefusedField() {
    namespace rf = orch::caps::refused;
    const uint16_t fields = verdict_.refusedFields & (uint16_t)~(rf::kImage | rf::kFileRef | rf::kStructured);
    if (fieldResent_ || !fields || !route_.hasSlot || !fits(0)) return false;
    if (!orch::wire::resendWithout(*refused_, route_.slot, model_.c_str(), routed_, fields)) return false;
    route_.caps = orch::caps::capsFor(route_.slot, model_.c_str(), routed_, refused_);
    route_.refused = refused_->fieldsFor(route_.slot, model_.c_str(), routed_);
    fieldResent_ = true;
    return true;
  }

  bool retryClimb() {
    if (refusedClimbed_ || !req_.climb || !fits(0) || !req_.climb(ClimbAsk{Climb::Refused, 0, bodyCap_})) return false;
    refusedClimbed_ = true;
    contextClimbed_ = haveCut_;   // a window-full answer climbed: its resend is not cut-retried
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
    out_.errorLine = exchangeLine(err, slug_, httpStatus_, err == ExchangeErr::Other ? refusedReason_ : std::string());
    out_.errorLineStored = exchangeLine(err, slug_, httpStatus_);   // what may be kept: no provider words (F4)
    if (out_.modelUnusable)   // lane E2-R4b: the provider does not know the model, said as that (PC-0259)
      out_.errorLine = out_.errorLineStored = model_ + texts::device_notes::kNotAvailableOn + providerName(capHost_) + ".";
    if (err == ExchangeErr::Other) out_.providerBody = refusedBody_;
    if (out_.parkHost) out_.parkBody = parkBody_;
    return Status::success();
  }

  const ExchangeDeps& deps_;
  const ExchangeRequest& req_;
  ExchangeResult& out_;
  RenderInput base_, in_;   // the caller's input (endpoint copied) and the pre-flighted one of the render
  HeadEndpoint head_;
  Route route_;
  PreflightState pre_;
  orch::caps::RefusedFields ownRefused_;
  orch::caps::RefusedFields* refused_ = nullptr;
  uint32_t asked_ = 0;     // the cap the last render was asked for (before the pre-flight)
  uint16_t dropped_ = 0;
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
  bool contextClimbed_ = false, fieldResent_ = false, refusalAnswer_ = false;
  std::string refusedBody_, refusedReason_, parkBody_;
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
  return classify(status, body, routed, std::string()).err;
}

namespace {
// The provider of a known API host, else empty.
std::string knownHostSlug(const std::string& host) {
  const std::string h = lower(host);
  if (h == "api.anthropic.com") return "anthropic";
  if (h == "api.openai.com") return "openai";
  if (h == "api.mistral.ai") return "mistral";
  if (h == "api.z.ai" || h == "open.bigmodel.cn") return "zai";
  return "";
}
// The provider a dialect speaks for: native dialects name theirs, a compatible one is a custom endpoint.
const char* dialectProvider(Dialect d) {
  if (d == Dialect::AnthropicMessages) return "anthropic";
  if (d == Dialect::OpenAIResponses) return "openai";
  return "custom";
}
}  // namespace

std::string headSlugOf(const TurnCtx& turn, const HeadEndpoint& head, bool* byDialect) {
  if (byDialect) *byDialect = false;
  for (const char* s : kSlugs)
    if (turn.host == s) return s;
  const std::string host = head.host.empty() ? turn.host : head.host;
  const std::string known = knownHostSlug(host);
  if (!known.empty() || host.empty()) return known;   // a known host, or nothing names the head
  if (head.routed) return "cumulo";
  const std::string slug = dialectProvider(turn.dialect);
  if (byDialect) *byDialect = slug != "custom";
  return slug;
}

std::string providerSlugOf(const TurnCtx& turn, const HeadEndpoint& head) {
  bool byDialect = false;
  const std::string s = headSlugOf(turn, head, &byDialect);
  return byDialect ? "custom" : s;   // a line never names a provider the endpoint is not
}

namespace {
// A word of letters and digits this long that mixes both, or any word this long, reads as a key or
// a token (words split on every other character, so a dated model id or a JSON path is not one).
constexpr size_t kKeyMixed = 16, kKeyAny = 32;
bool alnum(char c) { return std::isalnum((unsigned char)c) != 0; }

// True when `s` could carry a credential: a key's own markers ("sk-" at the start of a word only),
// or a key-like word.
bool looksLikeSecret(const std::string& s) {
  const std::string low = lower(s);
  for (const char* m : {"bearer", "x-api-key", "api_key", "api-key", "apikey", "authorization", "password", "secret"})
    if (low.find(m) != std::string::npos) return true;
  for (size_t at = low.find("sk-"); at != std::string::npos; at = low.find("sk-", at + 1))
    if (at == 0 || !alnum(low[at - 1])) return true;
  size_t run = 0;
  bool letter = false, digit = false;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i < s.size() && alnum(s[i])) {
      ++run;
      letter |= std::isalpha((unsigned char)s[i]) != 0;
      digit |= std::isdigit((unsigned char)s[i]) != 0;
      continue;
    }
    if (run >= kKeyAny || (run >= kKeyMixed && letter && digit)) return true;
    run = 0;
    letter = digit = false;
  }
  return false;
}

// The code point at s[i] (valid UTF-8, checked before), and its length in `len`.
uint32_t codePointAt(const std::string& s, size_t i, size_t& len) {
  const unsigned char c = (unsigned char)s[i];
  len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
  for (size_t k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
  return cp;
}

// The text as one line of printable ASCII (F2, F6): every control character (C0, DEL, C1, NEL) and
// U+2028 / U+2029 is a space, runs of spaces are one, the ends trimmed; any other non-ASCII code
// point (bidi controls, zero-width, letters) is one '?'. One line by construction.
std::string asciiLineOf(const std::string& raw) {
  std::string text;
  for (size_t i = 0, len = 1; i < raw.size(); i += len) {
    const uint32_t cp = codePointAt(raw, i, len);
    const bool space = cp <= 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029;
    if (space) {
      if (!text.empty() && text.back() != ' ') text += ' ';
    } else {
      text += cp < 0x7F ? (char)cp : '?';
    }
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

// No markup of the reason is read (F1). The line goes out through Telegram's Markdown-to-HTML mapping
// (nimbus::tgHtml) and the web chat's Markdown (ui_js.h _mdInline): a [text](url) link, `code` and
// ``` blocks, **bold**, *italic* and _italic_. Headings, bullets and fences need a line start, which a
// reason never has (the line begins with the device's words). '<', '>' and '&' are escaped there.
std::string literalOf(std::string text) {
  for (size_t i = 0; i < text.size(); ++i) {
    char& c = text[i];
    if (c == '[') c = '(';
    else if (c == ']') c = ')';
    else if (c == '`' || c == '*') c = '\'';
    else if (c == '_' && (i == 0 || text[i - 1] == ' ')) c = '\'';   // _italic_ opens only there
  }
  return text;
}

// At most `max` bytes (ASCII by now), with "..." where it was cut.
std::string cutTo(std::string text, size_t max) {
  if (text.size() <= max) return text;
  text.resize(max - 3);
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text + "...";
}
}  // namespace

std::string refusalReason(const std::string& body) {
  constexpr size_t kBodyMax = 16384;   // a larger body is not parsed: the device keeps its memory
  constexpr size_t kReasonMax = 160;
  if (body.empty() || body.size() > kBodyMax) return std::string();
  const std::string raw = orch::providerErrorText(body);
  if (raw.empty() || !nimbus::utf8Valid(raw)) return std::string();
  const std::string text = asciiLineOf(raw);
  if (text.empty() || looksLikeSecret(text)) return std::string();
  return cutTo(literalOf(text), kReasonMax);
}

namespace {
// "<who> refused the request (HTTP n)", then the provider's reason when there is one.
std::string refusedLine(const std::string& who, const std::string& code, const std::string& reason) {
  namespace n = texts::device_notes;
  if (reason.empty() || code.empty()) return who + n::kRefused + code + ".";
  const char last = reason.back();
  return who + n::kRefused + code + n::kReasonSep + reason + (last == '.' || last == '!' || last == '?' ? "" : ".");
}
}  // namespace

// The owner's line for a failed exchange (texts.h device_notes): what happened, then the
// one next step. `reason` (refusalReason of the provider's body; Other only) rides after the
// status, for the person alone: every kept copy is errorLineStored, the line without it (F4).
std::string exchangeLine(ExchangeErr err, const std::string& slug, int httpStatus, const std::string& reason) {
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
  return refusedLine(who, code, reason);
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
