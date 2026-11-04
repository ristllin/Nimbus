#pragma once
#include <ArduinoJson.h>

#include <string>

#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/providers.h"

#if defined(NIMBUS_TEST) || defined(NIMBUS_NATIVE)

// TestAnswerTransport - the bench's scripted provider answer (PROVFAULT,
// providers.h setTestStatusOverride) AT THE TRANSPORT'S SEAT: a decorator over the
// real transport that answers a request to the armed host locally, exactly as the
// network would, and forwards everything else untouched.
//
// The wire's refusal machinery sits ABOVE the transport: the RouterTap a Cumulo
// head holds over it (router_refusal.h) and the resend loop in wire.h exchange
// (the clamp's capped resend, a short retry_later wait). A scripted answer that
// bypassed them proved nothing: the S10 bench row read the router's clamp 402 as an
// out-of-credit failover because the old override answered before either ran, so
// no build could ever show the capped resend (lane CLAMP). Answering here, a
// scripted refusal walks the production path: the tap notes its reason, the wire
// resends once capped, and the ladder answers with the refusal's own cause.
//
// The device test build wraps deviceTransport() in it (transport_tls.cpp); a host
// test wraps its own fake. Production images compile none of it.
namespace agent {

class TestAnswerTransport : public HttpTransport {
 public:
  explicit TestAnswerTransport(HttpTransport* inner) : inner_(inner) {}

  bool exec(const HttpRequest& req, HttpResponse& out, std::string& err) override {
    int status = 0;
    std::string body;
    if (!providers::testStatusOverride(req.host, status, body) || status <= 0)
      return inner_->exec(req, out, err);
    hlog::logf("provider: TEST override answered %s%s with HTTP %d", req.host.c_str(),
               req.path.c_str(), status);
    out.status = status;
    out.body = body;
    out.headers.clear();
    err.clear();
    ++answered_;
    return true;
  }

  // The scripted body is filter-parsed into `doc` exactly as the device transport
  // stream-parses a live answer off the socket: only the fields the caller's
  // filter keeps land in memory, so a fat body costs nothing here either.
  int execJson(const HttpRequest& req, JsonDocument& doc, const JsonDocument& filter,
               std::string& err) override {
    int status = 0;
    std::string body;
    if (!providers::testStatusOverride(req.host, status, body) || status <= 0)
      return inner_->execJson(req, doc, filter, err);
    hlog::logf("provider: TEST override answered %s%s with HTTP %d", req.host.c_str(),
               req.path.c_str(), status);
    doc.clear();
    err.clear();
    if (!body.empty()) {
      const DeserializationError derr =
          deserializeJson(doc, body, DeserializationOption::Filter(filter));
      if (derr) err = std::string("response parse failed: ") + derr.c_str();
    }
    ++answered_;
    return status;
  }

  // Requests this decorator answered locally (a resend counts: the wire's second
  // send of a clamp is a second answer).
  uint32_t answered() const { return answered_; }

 private:
  HttpTransport* inner_;
  uint32_t answered_ = 0;
};

}  // namespace agent

#endif  // NIMBUS_TEST || NIMBUS_NATIVE
