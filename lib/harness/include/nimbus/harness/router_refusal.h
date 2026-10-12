#pragma once
#include <ArduinoJson.h>

#include <cctype>
#include <string>

#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/orch/router_route.h"

// router_refusal - what a Cumulo head needs ON TOP of the native provider wire
// (CUM-539): knowing WHY the router refused a request, so a refusal becomes an
// honest reply and never a dead assistant. One rule for both heads that run over
// the router: the device's cumulo head (src/agent/orchestrator.cpp) and the
// hosted Virtual Nimbus head (nimbusd/src/rig.h).
//
// The native head turns (orchTurnAnthropic / OpenAI / Mistral, driven over the
// router by ProviderDeps.routerBase) only surface "messages HTTP 403" - the
// router's refusal body ({"error":"model_not_priced","reason":...,"message":...})
// carries its reason as a STRING `error`, which their `error.message` filter
// drops. The engine then maps any 403 to "the provider rejected this device's
// key", which is wrong for a refused MODEL (the key is fine). RouterTap sits
// between the head and the transport and records the router's own reason for the
// first refusal of an attempt, so the head can tell a refused model (switch back
// to a model that works) from an account-wide refusal (say what to do) without
// touching the shared provider code.
namespace agent {
namespace router_refusal {

// The first refusal the router answered within one head attempt.
struct RouterRefusal {
  int status = 0;              // HTTP status; 0 = no refusal seen
  std::string reason;          // the router's machine reason ("model_not_priced", ...)
  std::string message;         // the error text (an upstream's own, passed through)
  bool firstRequest = false;   // it refused the attempt's FIRST request (no tool ran)
  // The refused request continued a provider-side conversation (Mistral's
  // /conversations/<id>): its 404 says that conversation is gone, not the model.
  bool continuedThread = false;
};

// How a refusal should be answered:
//   Selector - the router does not offer the picked model (model_not_priced, a
//              404 for the model or its upstream, or the upstream's own 400
//              "Invalid model" passed through): answer on a model that works and
//              move the pick there;
//   Upstream - that upstream is paused (funding_cap_reached, provider_disabled) or
//              its route is not allowed (endpoint_not_allowed): answer on the router
//              default this turn and KEEP the pick (nothing is wrong with the choice);
//   Tool     - a provider built-in tool is not offered on a paying key
//              (native_tool_not_metered): every model would carry it, so say so;
//   Account  - credits or a plan are missing (402): no model would work;
//   Paused   - the whole router is paused (router_disabled);
//   RateLimit/Auth/Other keep the engine's own reply.
enum class RefusalKind { None, Selector, Upstream, Tool, Account, Paused, RateLimit, Auth, Other };

// An upstream's own answer that the model does not exist, passed through by the
// router (bench, CUM-530: Mistral 400 "Invalid model: mistral/zai-glm-5-3" for a
// router id the router priced but no upstream runs). Case-insensitive.
inline bool namesUnknownModel(const std::string& text) {
  std::string t = text;
  for (char& c : t) c = (char)tolower((unsigned char)c);
  return t.find("invalid model") != std::string::npos || t.find("unknown model") != std::string::npos ||
         t.find("model_not_found") != std::string::npos;
}

// The router's own machine reasons and how each is answered.
inline RefusalKind kindOfReason(const std::string& why) {
  static const struct {
    const char* reason;
    RefusalKind kind;
  } kReasons[] = {{"model_not_priced", RefusalKind::Selector},     {"funding_cap_reached", RefusalKind::Upstream},
                  {"provider_disabled", RefusalKind::Upstream},    {"endpoint_not_allowed", RefusalKind::Upstream},
                  {"native_tool_not_metered", RefusalKind::Tool}, {"router_disabled", RefusalKind::Paused}};
  for (const auto& e : kReasons)
    if (why == e.reason) return e.kind;
  return RefusalKind::None;
}

// A 404 for the model or its upstream (not for a conversation that is gone), or
// the upstream's own 400 naming the model unknown: the router does not offer it.
inline bool refusesTheModel(const RouterRefusal& r) {
  if (r.status == 404) return !r.continuedThread;
  return r.status == 400 && (namesUnknownModel(r.reason) || namesUnknownModel(r.message));
}

inline RefusalKind classifyRefusal(const RouterRefusal& r) {
  if (r.status == 0) return RefusalKind::None;
  const RefusalKind byReason = kindOfReason(r.reason);
  if (byReason == RefusalKind::Selector || refusesTheModel(r)) return RefusalKind::Selector;
  if (byReason != RefusalKind::None) return byReason;
  if (r.status == 429) return RefusalKind::RateLimit;
  if (r.status == 402) return RefusalKind::Account;
  if (r.status == 401) return RefusalKind::Auth;
  return RefusalKind::Other;
}

// The engine renders a failed turn as "That didn't finish - <cause>. Nothing is
// still running; ask again to retry." and maps a cause containing "key",
// "network", "budget" or an "HTTP 4xx/5xx" token to its own generic wording, so
// these causes avoid those words. The engine also cuts a cause at
// kEngineCauseMax characters (lib/harness engine.cpp), so the selector is named
// only when its whole sentence fits; a longer one is "that model", so the fix at
// the end of the sentence is never the part that gets cut.
constexpr size_t kEngineCauseMax = 100;
// Every cause below starts with this, which is how the engine's failover notice
// knows a refusal from an outage (isRefusalCause).
constexpr const char* kCausePrefix = "the Cumulo router ";

inline std::string refusalSentence(RefusalKind k, const std::string& name) {
  switch (k) {
    case RefusalKind::Selector:
      return "the Cumulo router does not offer " + name + " right now. Choose another model under Models";
    case RefusalKind::Upstream:
      return "the Cumulo router cannot run " + name + " right now. Try again shortly";
    case RefusalKind::Tool:
      return "the Cumulo router does not run provider built-in tools on this account. Remove them under Connectors";
    case RefusalKind::Account:
      return "the Cumulo router needs credits or a plan for this account. Add credits in the portal";
    case RefusalKind::Paused:
      return "the Cumulo router is paused right now. Try again in a few minutes";
    default:
      return std::string();
  }
}

inline std::string refusalCause(RefusalKind k, const std::string& selector) {
  const std::string named = refusalSentence(k, selector);
  return named.size() <= kEngineCauseMax ? named : refusalSentence(k, "that model");
}

// A head error that is one of the causes above (a refusal no model could answer),
// not an outage: the engine's failover notice says so instead of "unavailable".
inline bool isRefusalCause(const std::string& err) { return err.rfind(kCausePrefix, 0) == 0; }

// The line that opens a reply answered by a different model than the one picked.
inline std::string fallbackNotice(RefusalKind k, const std::string& picked, const std::string& used) {
  if (k == RefusalKind::Upstream)
    return "The Cumulo router cannot run " + picked + " right now, so " + used +
           " answered this one. Your model choice is kept.";
  return "The Cumulo router does not offer " + picked + " for this key, so " + used +
         " answered instead. Choose another model under Models.";
}

// Prefix `note` to the reply of a head turn's output: the orch_turn JSON's
// "reply" when it parses, else the prose itself (a lenient head may answer in
// prose, which the engine delivers as the reply).
inline void prefixReply(std::string& outJson, const std::string& note) {
  JsonDocument d;
  if (!deserializeJson(d, outJson) && d.is<JsonObject>() && d["reply"].is<const char*>()) {
    const std::string reply = d["reply"].as<const char*>();
    d["reply"] = reply.empty() ? note : note + "\n\n" + reply;
    outJson.clear();
    serializeJson(d, outJson);
    return;
  }
  outJson = outJson.empty() ? note : note + "\n\n" + outJson;
}

// Two selectors that reach the same router model: "openai/gpt-5.6" is the default
// "gpt-5.6" spelled with its upstream, so it is never a "different" fallback.
inline bool sameRoute(const std::string& a, const std::string& b) {
  const nimbus::orch::RouterRoute ra = nimbus::orch::resolveRouterRoute(a);
  const nimbus::orch::RouterRoute rb = nimbus::orch::resolveRouterRoute(b);
  return ra.upstream == rb.upstream && ra.model == rb.model;
}

// A transport decorator that forwards every request and remembers the first
// router refusal of the current attempt (begin() starts one). Turn task / engine
// thread only, like every head turn. `routerHost` (not owned; it must outlive the
// tap) names the host whose answers count; null or "" counts every request.
//
// Both entry points are forwarded: the device transport stream-parses a JSON
// response (execJson) and never holds the body, so the tap widens the caller's
// filter by the router's reason and the error text (a refusal is a few hundred
// bytes) and reads them from the parsed document; exec() reads the whole body.
class RouterTap : public agent::HttpTransport {
 public:
  RouterTap(agent::HttpTransport* inner, const char* routerHost, ArduinoJson::Allocator* alloc = nullptr)
      : inner_(inner), host_(routerHost), alloc_(alloc) {}

  void setInner(agent::HttpTransport* inner) { inner_ = inner; }
  void begin() {
    requests_ = 0;
    refusal_ = RouterRefusal{};
  }
  const RouterRefusal& refusal() const { return refusal_; }

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    const bool ok = inner_->exec(req, out, err);
    if (!counts(req)) return ok;
    if (ok && out.status >= 400 && refusal_.status == 0) {
      JsonDocument d = newDoc();
      if (deserializeJson(d, out.body)) d.clear();
      note(req, out.status, d);
    }
    return ok;
  }

  int execJson(const agent::HttpRequest& req, JsonDocument& doc, const JsonDocument& filter,
               std::string& err) override {
    JsonDocument wide = newDoc();
    wide.set(filter);
    wide["reason"] = true;
    wide["error"] = true;     // a string reason, or the whole (small) error object
    wide["message"] = true;
    const int status = inner_->execJson(req, doc, wide, err);
    if (counts(req) && status >= 400 && refusal_.status == 0) note(req, status, doc);
    return status;
  }

 private:
  bool counts(const agent::HttpRequest& req) {
    if (host_ && host_[0] && req.host != host_) return false;
    ++requests_;
    return true;
  }
  JsonDocument newDoc() const { return alloc_ ? JsonDocument(alloc_) : JsonDocument(); }
  // The router names its reason in `reason`, and in `error` as a plain string; an
  // upstream's own refusal carries its text in error.message or message.
  void note(const agent::HttpRequest& req, int status, const JsonDocument& d) {
    refusal_.status = status;
    refusal_.firstRequest = requests_ == 1;
    refusal_.continuedThread = req.path.find("/conversations/") != std::string::npos;
    const char* why = d["reason"] | "";
    if (!why[0] && d["error"].is<const char*>()) why = d["error"].as<const char*>();
    const char* msg = d["error"]["message"] | "";
    if (!msg[0]) msg = d["message"] | "";
    refusal_.reason = std::string(why).substr(0, 48);
    refusal_.message = std::string(msg).substr(0, 160);
  }

  agent::HttpTransport* inner_;
  const char* host_;
  ArduinoJson::Allocator* alloc_;
  int requests_ = 0;
  RouterRefusal refusal_;
};

// ---- the refusal ladder (one turn) --------------------------------------------

// What one router head turn came to.
struct LadderResult {
  bool ok = false;
  std::string used;                           // the selector that answered
  RefusalKind kind = RefusalKind::None;       // why the PICK was refused (None: it answered)
  RefusalKind lastKind = RefusalKind::None;   // the last refusal seen (a failure's cause)
  std::string lastTried;                      // the selector that refusal was for
  bool fellBack() const { return ok && kind != RefusalKind::None; }
};

// The selector a refused pick falls back to first, or "" when none would help: a
// refused model goes back to the last selector that worked (else the router
// default, which the ladder also tries if that one is refused too); a paused
// upstream answers on the router default.
inline std::string fallbackFor(const std::string& picked, const std::string& lastGood,
                               const std::string& routerDefault, RefusalKind kind) {
  std::string fb;
  if (kind == RefusalKind::Selector)
    fb = (!lastGood.empty() && !sameRoute(lastGood, picked)) ? lastGood : routerDefault;
  else if (kind == RefusalKind::Upstream)
    fb = routerDefault;
  return (fb.empty() || sameRoute(fb, picked)) ? std::string() : fb;
}

// The router default is the last resort after the fallback `tried` was refused
// too: tried once, only when that refusal came before any tool ran and names the
// model or its upstream (an account-wide refusal would refuse the default too).
inline bool defaultIsNext(const RouterRefusal& rf, const std::string& picked, const std::string& tried,
                          const std::string& routerDefault) {
  const RefusalKind k = classifyRefusal(rf);
  return !sameRoute(tried, routerDefault) && !sameRoute(picked, routerDefault) && rf.firstRequest &&
         (k == RefusalKind::Selector || k == RefusalKind::Upstream);
}

// One Cumulo head turn with the CUM-539 refusal ladder. `attempt(selector, fresh)`
// runs one head attempt on a router selector; with `fresh` the caller first
// clears the conversation marker, the output and the error. Only a refusal of the
// attempt's FIRST request is answered again (no tool has run, so nothing repeats):
// a refused model on the last selector that worked, else the router default,
// then the router default once more if that one is refused too; a paused upstream
// on the router default. Never the same model twice.
template <class Attempt>
LadderResult runLadder(RouterTap& tap, const std::string& picked, const std::string& lastGood,
                       const std::string& routerDefault, Attempt&& attempt) {
  LadderResult r;
  tap.begin();
  if (attempt(picked, false)) {
    r.ok = true;
    r.used = picked;
    return r;
  }
  r.kind = r.lastKind = classifyRefusal(tap.refusal());
  r.lastTried = picked;
  std::string fb = fallbackFor(picked, lastGood, routerDefault, r.kind);
  if (!tap.refusal().firstRequest || fb.empty()) return r;
  agent::hlog::logf("router refused %s (%s) - answering on %s", picked.c_str(), tap.refusal().reason.c_str(),
                    fb.c_str());
  tap.begin();
  bool answered = attempt(fb, true);
  if (!answered && defaultIsNext(tap.refusal(), picked, fb, routerDefault)) {
    agent::hlog::logf("router refused %s too (%s) - answering on %s", fb.c_str(), tap.refusal().reason.c_str(),
                      routerDefault.c_str());
    fb = routerDefault;
    tap.begin();
    answered = attempt(fb, true);
  }
  if (answered) {
    r.ok = true;
    r.used = fb;
    return r;
  }
  r.lastKind = classifyRefusal(tap.refusal());   // the last model tried was refused too
  r.lastTried = fb;
  return r;
}

// A conversation turn a fallback answered: only a refused model moves the pick,
// to the model that answered ("" when that is the router default), and only while
// the pick is still the one this turn started with (the owner may have chosen
// another meanwhile); the notice opens the reply. A fold is not a conversation
// turn: its caller skips this.
template <class CurrentPick, class SetPick>
void settleFallback(const LadderResult& r, const std::string& picked, const std::string& routerDefault,
                    std::string& outJson, CurrentPick&& currentPick, SetPick&& setPick) {
  if (r.kind == RefusalKind::Selector && currentPick() == picked)
    setPick(sameRoute(r.used, routerDefault) ? std::string() : r.used);
  prefixReply(outJson, fallbackNotice(r.kind, picked, r.used));
}

}  // namespace router_refusal
}  // namespace agent
