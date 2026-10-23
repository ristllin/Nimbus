#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/http.h"

// FakeHttpTransport - the wire-suite workhorse. A script is an ordered list of
// exchanges; every request is recorded verbatim so tests assert exact JSON
// bodies. Script exhaustion is a LOUD transport failure (never a lenient 200):
// a test that makes more calls than it scripted is wrong, and should fail red.
namespace harness_test {

struct Exchange {
  std::string expectHost;          // "" = don't care
  std::string expectPathContains;  // "" = don't care
  int status = 200;                // 0 => transport error (exec returns false)
  std::string body;                // canned provider JSON
  std::string err = "scripted transport error";
  agent::HeaderList headers;       // response headers (e.g. a 429's x-ratelimit-*)
  uint32_t takesMs = 0;            // time the exchange takes on the rig clock (see clock)
};

// A transport failure (status 0) with the transport's `err` text, taking `takesMs`
// on the rig clock.
inline Exchange transportCut(uint32_t takesMs, const char* err = "no response") {
  Exchange e;
  e.status = 0;
  e.err = err;
  e.takesMs = takesMs;
  return e;
}

// The usage read antPoll makes once a managed-agent session finished (lane DEF: GET
// /v1/sessions/<id>, the session's billed usage for the monthly spend ceiling).
inline Exchange antSessionUsage(const char* usage = "{\"input_tokens\":0,\"output_tokens\":0}") {
  Exchange e;
  e.expectPathContains = "/v1/sessions/";
  e.body = std::string("{\"usage\":") + usage + "}";
  return e;
}

struct FakeHttpTransport : agent::HttpTransport {
  std::vector<Exchange> script;
  size_t idx = 0;
  std::vector<agent::HttpRequest> seen;
  // The splices each request carried (R1 photos), in request order. `seen` records
  // the materialized body (what went on the wire) with its splices cleared.
  std::vector<agent::BodySplices> spliced;
  // When set, each scripted exchange advances this clock by its takesMs, so a
  // test can make a request outlive (or not) the caller's deadline.
  uint32_t* clock = nullptr;

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out,
            std::string& err) override {
    seen.push_back(req);
    spliced.push_back(req.splices);
    if (!req.splices.empty()) {
      seen.back().body = agent::materializeBody(req);
      seen.back().splices.clear();
    }
    if (idx >= script.size()) {
      out.status = 0;
      err = "FakeHttpTransport: script exhausted (unexpected extra request to " +
            req.host + req.path + ")";
      return false;
    }
    const Exchange& e = script[idx++];
    if (clock) *clock += e.takesMs;
    if (!e.expectHost.empty() && req.host != e.expectHost) {
      out.status = 0;
      err = "FakeHttpTransport: host mismatch, expected " + e.expectHost +
            " got " + req.host;
      return false;
    }
    if (!e.expectPathContains.empty() &&
        req.path.find(e.expectPathContains) == std::string::npos) {
      out.status = 0;
      err = "FakeHttpTransport: path mismatch, expected substr " +
            e.expectPathContains + " got " + req.path;
      return false;
    }
    if (e.status == 0) {
      out.status = 0;
      err = e.err;
      return false;
    }
    out.status = e.status;
    out.body = e.body;
    out.headers = e.headers;
    return true;
  }

  const std::string& lastBody() const {
    static const std::string empty;
    return seen.empty() ? empty : seen.back().body;
  }
  bool lastBodyContains(const char* needle) const {
    return !seen.empty() &&
           seen.back().body.find(needle) != std::string::npos;
  }
};

}  // namespace harness_test
