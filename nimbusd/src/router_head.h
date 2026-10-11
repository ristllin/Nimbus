#pragma once
#include <ArduinoJson.h>

#include <string>

#include "nimbus/harness/http.h"

// router_head - what the hosted Cumulo head needs ON TOP of the native provider
// wire (CUM-539): knowing WHY the router refused a request, so a refusal becomes
// an honest reply and never a dead assistant.
//
// The native head turns (orchTurnAnthropic / OpenAI / Mistral, driven over the
// router by ProviderDeps.routerBase) only surface "messages HTTP 403" - the
// router's refusal body ({"error":"model_not_priced","reason":...,"message":...})
// carries its reason as a STRING `error`, which their `error.message` filter
// drops. The engine then maps any 403 to "the provider rejected this device's
// key", which is wrong for a refused MODEL (the key is fine). RouterTap sits
// between the head and the instance transport and records the router's own
// reason for the first refusal of an attempt, so the head can tell a refused
// model (switch back to a model that works) from an account-wide refusal (say
// what to do) without touching the shared provider code.
namespace nimbusd {

// The first refusal the router answered within one head attempt.
struct RouterRefusal {
  int status = 0;              // HTTP status; 0 = no refusal seen
  std::string reason;          // the router's machine reason ("model_not_priced", ...)
  bool firstRequest = false;   // it refused the attempt's FIRST request (no tool ran)
};

// How a refusal should be answered. Selector: the model the owner picked is not
// offered (switch back and answer). Upstream: that upstream is paused right now
// (answer on the default model, keep the pick). Account: no model would work
// (credits, spending limit). RateLimit/Auth/Other keep the engine's own reply.
enum class RefusalKind { None, Selector, Upstream, Account, Paused, RateLimit, Auth, Other };

inline RefusalKind classifyRefusal(const RouterRefusal& r) {
  if (r.status == 0) return RefusalKind::None;
  const std::string& why = r.reason;
  if (why == "model_not_priced" || why == "endpoint_not_allowed" ||
      why == "native_tool_not_metered" || r.status == 404)
    return RefusalKind::Selector;
  if (why == "funding_cap_reached" || why == "provider_disabled") return RefusalKind::Upstream;
  if (why == "router_disabled") return RefusalKind::Paused;
  if (r.status == 429) return RefusalKind::RateLimit;
  if (r.status == 402) return RefusalKind::Account;
  if (r.status == 401) return RefusalKind::Auth;
  return RefusalKind::Other;
}

// The engine renders a failed turn as "That didn't finish - <cause>. Nothing is
// still running; ask again to retry." and maps a cause containing "key",
// "network", "budget" or an "HTTP 4xx/5xx" token to its own generic wording, so
// these causes avoid those words and stay under its 100-character cut.
inline std::string refusalCause(RefusalKind k, const std::string& selector) {
  switch (k) {
    case RefusalKind::Selector:
      return "the Cumulo router does not offer " + selector + " right now. Choose another model under Models";
    case RefusalKind::Upstream:
      return "the Cumulo router has paused " + selector + " for now. Try again shortly";
    case RefusalKind::Account:
      return "the Cumulo router needs credits or a plan for this account. Add credits in the portal";
    case RefusalKind::Paused:
      return "the Cumulo router is paused right now. Try again in a few minutes";
    default:
      return std::string();
  }
}

// The line that opens a reply answered by a different model than the one picked.
inline std::string fallbackNotice(RefusalKind k, const std::string& picked, const std::string& used) {
  if (k == RefusalKind::Upstream)
    return "The Cumulo router has paused " + picked + " for now, so " + used +
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

// A transport decorator that forwards every request and remembers the first
// router refusal of the current attempt (begin() starts one). Engine thread only,
// like every head turn.
class RouterTap : public agent::HttpTransport {
 public:
  RouterTap(agent::HttpTransport* inner, std::string routerHost)
      : inner_(inner), host_(std::move(routerHost)) {}

  void setInner(agent::HttpTransport* inner) { inner_ = inner; }
  void begin() {
    requests_ = 0;
    refusal_ = RouterRefusal{};
  }
  const RouterRefusal& refusal() const { return refusal_; }

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    const bool ok = inner_->exec(req, out, err);
    if (req.host != host_) return ok;
    ++requests_;
    if (ok && out.status >= 400 && refusal_.status == 0) note(out);
    return ok;
  }

 private:
  void note(const agent::HttpResponse& out) {
    refusal_.status = out.status;
    refusal_.firstRequest = requests_ == 1;
    JsonDocument d;
    if (deserializeJson(d, out.body) || !d.is<JsonObject>()) return;
    // The router names its reason in `reason`, and in `error` as a plain string.
    const char* why = d["reason"].is<const char*>() ? d["reason"].as<const char*>()
                      : d["error"].is<const char*>() ? d["error"].as<const char*>()
                                                     : "";
    refusal_.reason = std::string(why).substr(0, 48);
  }

  agent::HttpTransport* inner_;
  std::string host_;
  int requests_ = 0;
  RouterRefusal refusal_;
};

}  // namespace nimbusd
