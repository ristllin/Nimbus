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
};

struct FakeHttpTransport : agent::HttpTransport {
  std::vector<Exchange> script;
  size_t idx = 0;
  std::vector<agent::HttpRequest> seen;

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out,
            std::string& err) override {
    seen.push_back(req);
    if (idx >= script.size()) {
      out.status = 0;
      err = "FakeHttpTransport: script exhausted (unexpected extra request to " +
            req.host + req.path + ")";
      return false;
    }
    const Exchange& e = script[idx++];
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
