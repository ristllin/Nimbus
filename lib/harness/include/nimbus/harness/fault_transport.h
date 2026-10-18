#pragma once
#include <ArduinoJson.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "nimbus/harness/http.h"

// FaultInjectingTransport - a TEST decorator over any agent::HttpTransport.
//
// It fails chosen requests on purpose so the retry, failover and error-reporting
// paths above the seam run on demand against otherwise real traffic: requests
// N..M (counted per host, 1-based) to one host come back as a transport failure
// (status 0, with the error text the device transport uses, so wire.h classifies
// it exactly as it would a real one) or as an HTTP status with a provider-shaped
// error body. Every other request passes straight through to the inner transport.
//
// Used by tools/harness-lab (--fault=<spec>, wrapping CurlHttpTransport). It is
// header-only and nothing in the firmware includes it, so a production image
// carries none of it; a test build may wrap its own transport with it.
//
// Spec grammar (parseFaultRule): host:range:status[:detail]
//   host    a host name, "*" for any host, or a provider alias
//           (anthropic, openai, mistral, zai, tavily)
//   range   N | N-M | N- (open-ended) | * (every request)
//   status  0 (transport failure) or an HTTP status 100..599
//   detail  status 0:  connect | unsent | noresp | lowmem | timeout
//           HTTP:      credit | overloaded | ratelimit | quota | badreq,
//                      or body=<raw response body>
// Examples: "anthropic:2:529:overloaded", "api.mistral.ai:1-3:0:noresp".
namespace agent {

struct FaultRule {
  std::string host;   // exact host, or "*" for any host
  uint32_t from = 1;  // first failing request (1-based, counted per host)
  uint32_t to = 1;    // last failing request, inclusive; 0 = every request from `from` on
  int status = 0;     // 0 = transport failure; otherwise the HTTP status returned
  std::string body;   // response body of an HTTP-status fault
  std::string err;    // transport error text of a status-0 fault
};

// One named detail: a status-0 error text, or an HTTP error body.
struct FaultPreset {
  const char* name;
  bool transport;     // true: `text` is a status-0 error; false: `text` is a body
  const char* text;
};

// Status-0 texts reuse the substrings wire.h classifies ("body truncated" = never
// sent, "no response" = sent but unanswered, "low memory"); "timeout:" is the
// prefix the engine reads as a timeout.
inline const std::vector<FaultPreset>& faultPresets() {
  static const std::vector<FaultPreset> k = {
      {"connect", true, "connect failed (injected fault)"},
      {"unsent", true, "request body truncated (injected fault)"},
      {"noresp", true, "no response (injected fault)"},
      {"lowmem", true, "low memory: send failed (injected fault)"},
      {"timeout", true, "timeout: no response (injected fault)"},
      {"credit", false,
       R"J({"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits."}})J"},
      {"overloaded", false, R"J({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})J"},
      {"ratelimit", false,
       R"J({"type":"error","error":{"type":"rate_limit_error","message":"Rate limit reached (injected fault)"}})J"},
      {"quota", false,
       R"J({"error":{"message":"You exceeded your current quota, please check your plan and billing details.","type":"insufficient_quota","code":"insufficient_quota"}})J"},
      {"badreq", false,
       R"J({"type":"error","error":{"type":"invalid_request_error","message":"invalid request (injected fault)"}})J"},
  };
  return k;
}

inline const FaultPreset* findFaultPreset(const std::string& name) {
  for (const FaultPreset& p : faultPresets())
    if (name == p.name) return &p;
  return nullptr;
}

// Provider alias -> its API host; anything else is returned unchanged. "zai" is
// the primary api.z.ai (the lab and nimbusd use only that host); name the device's
// fallback host open.bigmodel.cn explicitly to fault it.
inline std::string faultHostAlias(const std::string& h) {
  if (h == "anthropic") return "api.anthropic.com";
  if (h == "openai") return "api.openai.com";
  if (h == "mistral") return "api.mistral.ai";
  if (h == "zai") return "api.z.ai";
  if (h == "tavily") return "api.tavily.com";
  return h;
}

namespace fault_detail {

// Strict unsigned decimal: digits only, no sign, no overflow past 9 digits.
inline bool parseUint(const std::string& s, uint32_t& out) {
  if (s.empty() || s.size() > 9) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  out = (uint32_t)std::strtoul(s.c_str(), nullptr, 10);
  return true;
}

inline bool parseRange(const std::string& s, FaultRule& r) {
  if (s == "*") { r.from = 1; r.to = 0; return true; }
  const size_t dash = s.find('-');
  if (dash == std::string::npos) {
    if (!parseUint(s, r.from) || r.from == 0) return false;
    r.to = r.from;
    return true;
  }
  if (!parseUint(s.substr(0, dash), r.from) || r.from == 0) return false;
  const std::string hi = s.substr(dash + 1);
  if (hi.empty()) { r.to = 0; return true; }
  return parseUint(hi, r.to) && r.to >= r.from;
}

inline bool parseStatus(const std::string& s, int& status) {
  uint32_t v = 0;
  if (!parseUint(s, v)) return false;
  if (v != 0 && (v < 100 || v > 599)) return false;
  status = (int)v;
  return true;
}

// Fill the error text or body from the optional detail field.
inline bool applyDetail(const std::string& detail, FaultRule& r, std::string& why) {
  if (r.status == 0) {
    const FaultPreset* p = findFaultPreset(detail.empty() ? "connect" : detail);
    if (!p || !p->transport) { why = "unknown status-0 detail '" + detail + "'"; return false; }
    r.err = p->text;
    return true;
  }
  if (detail.rfind("body=", 0) == 0) { r.body = detail.substr(5); return true; }
  if (detail.empty()) {
    r.body = R"({"error":{"type":"injected_fault","message":"injected fault: HTTP )" +
             std::to_string(r.status) + R"("}})";
    return true;
  }
  const FaultPreset* p = findFaultPreset(detail);
  if (!p || p->transport) { why = "unknown HTTP detail '" + detail + "'"; return false; }
  r.body = p->text;
  return true;
}

}  // namespace fault_detail

// Parse one spec (grammar above). On failure returns false with `why` set and
// leaves `out` unspecified.
inline bool parseFaultRule(const std::string& spec, FaultRule& out, std::string& why) {
  const size_t a = spec.find(':');
  const size_t b = a == std::string::npos ? a : spec.find(':', a + 1);
  if (a == std::string::npos || b == std::string::npos) {
    why = "want host:range:status[:detail]";
    return false;
  }
  const size_t c = spec.find(':', b + 1);
  FaultRule r;
  r.host = faultHostAlias(spec.substr(0, a));
  if (r.host.empty()) { why = "empty host"; return false; }
  if (!fault_detail::parseRange(spec.substr(a + 1, b - a - 1), r)) {
    why = "bad range (want N, N-M, N- or *, from 1)";
    return false;
  }
  const std::string st = spec.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
  if (!fault_detail::parseStatus(st, r.status)) {
    why = "bad status (want 0 or 100..599)";
    return false;
  }
  const std::string detail = c == std::string::npos ? std::string() : spec.substr(c + 1);
  if (!fault_detail::applyDetail(detail, r, why)) return false;
  out = std::move(r);
  return true;
}

// The rules plus a per-host request counter. Single-threaded by design (the
// harness runs one request at a time); not safe to share across threads.
class FaultPlan {
 public:
  void add(const FaultRule& r) { rules_.push_back(r); }
  void clear() {
    rules_.clear();
    counts_.clear();
    injected_ = 0;
  }
  bool empty() const { return rules_.empty(); }
  const std::vector<FaultRule>& rules() const { return rules_; }

  // Count one request to `host` and return the rule that fails it, or nullptr.
  // The first matching rule wins.
  const FaultRule* onRequest(const std::string& host) {
    const uint32_t n = bump(host);
    for (const FaultRule& r : rules_) {
      if (r.host != "*" && r.host != host) continue;
      if (n < r.from || (r.to != 0 && n > r.to)) continue;
      injected_++;
      return &r;
    }
    return nullptr;
  }
  uint32_t requests(const std::string& host) const {
    for (const auto& c : counts_)
      if (c.first == host) return c.second;
    return 0;
  }
  uint32_t injected() const { return injected_; }

 private:
  uint32_t bump(const std::string& host) {
    for (auto& c : counts_)
      if (c.first == host) return ++c.second;
    counts_.push_back({host, 1});
    return 1;
  }
  std::vector<FaultRule> rules_;
  std::vector<std::pair<std::string, uint32_t>> counts_;
  uint32_t injected_ = 0;
};

class FaultInjectingTransport : public HttpTransport {
 public:
  FaultInjectingTransport(HttpTransport* inner, FaultPlan* plan) : inner_(inner), plan_(plan) {}

  bool exec(const HttpRequest& req, HttpResponse& out, std::string& err) override {
    const FaultRule* r = plan_ ? plan_->onRequest(req.host) : nullptr;
    if (!r) return inner_->exec(req, out, err);
    return synth(*r, out, err);
  }

  // Overridden so a pass-through keeps the inner transport's own (streaming)
  // execJson, and an injected failure is parsed through the caller's filter
  // exactly like the base class parses a real body.
  int execJson(const HttpRequest& req, JsonDocument& doc, const JsonDocument& filter,
               std::string& err) override {
    const FaultRule* r = plan_ ? plan_->onRequest(req.host) : nullptr;
    if (!r) return inner_->execJson(req, doc, filter, err);
    HttpResponse out;
    doc.clear();
    if (!synth(*r, out, err)) return 0;
    err.clear();
    if (!out.body.empty()) {
      DeserializationError derr =
          deserializeJson(doc, out.body, DeserializationOption::Filter(filter),
                          DeserializationOption::NestingLimit(kResponseNestingLimit));
      if (derr) err = std::string("response parse failed: ") + derr.c_str();
    }
    return out.status;
  }

 private:
  static bool synth(const FaultRule& r, HttpResponse& out, std::string& err) {
    out.headers.clear();
    if (r.status == 0) {
      out.status = 0;
      out.body.clear();
      err = r.err.empty() ? std::string("connect failed (injected fault)") : r.err;
      return false;
    }
    out.status = r.status;
    out.body = r.body;
    err.clear();
    return true;
  }

  HttpTransport* inner_;
  FaultPlan* plan_;
};

}  // namespace agent
