#pragma once
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/harness/http.h"
#include "request_view.h"
#include "script.h"
#include "wire_answers.h"

// ScriptedProvider - a scripted model at the HttpTransport seam, for engine-2 host tests
// (layer L1 of the battery) and anything else that talks to a provider through the
// transport: the old adapters, the plain engine's wires, nimbusd.
//
// It answers each request with the next Step of its script, rendered as the response
// body of the request's dialect (Anthropic Messages, OpenAI Responses, chat completions;
// decided from the path, or fixed at construction). Every request is recorded with its
// materialized body and read back into a RequestView, so a test asserts properties
// (prefix, pairing, tool choice, cache markers) on what was really sent.
//
// It stands in for the model the way the reference loop's scripted provider does
// (tests/evals/refloop/test_refloop_offline.py): a step is (tool calls, text), the loop
// runs until a step has no call. The same JSON script also drives the reference loop
// through scripted_provider.py, so one script can run both arms offline.
//
// Script exhaustion is a LOUD transport failure, never a lenient answer: a test that
// makes more requests than it scripted is wrong and goes red (as FakeHttpTransport).
//
//   harness_test::scripted::ScriptedProvider model;            // dialect from the path
//   model.push(scripted::call("read_file", R"({"path":"notes/a.md"})"));
//   model.push(scripted::say("It says hello."));
//   deps.transport = &model;  ...run the turn...
//   TEST_ASSERT_EQUAL(2, model.requests().size());
//   TEST_ASSERT_TRUE(model.allPrefixStable(why));
//   TEST_ASSERT_TRUE_MESSAGE(model.verify(why), why.c_str());   // last: nothing swallowed
namespace harness_test {
namespace scripted {

struct Recorded {
  agent::HttpRequest req;   // as sent, splices cleared
  std::string body;         // the materialized body (splices encoded in place)
  Dialect dialect = Dialect::Anthropic;
  RequestView view;
  size_t step = 0;          // the request's index: call ids are derived from it
  bool answerSchema = false;   // an Anthropic output_config.format: the answer comes back structured
};

class ScriptedProvider : public agent::HttpTransport {
 public:
  using Responder = std::function<Step(const Recorded& req, size_t index)>;
  using Expect = std::function<bool(const Recorded& req, std::string& why)>;

  ScriptedProvider() = default;
  explicit ScriptedProvider(Dialect fixed) : fixed_(true), dialect_(fixed) {}

  ScriptedProvider& push(Step s) {
    script_.push_back(std::move(s));
    return *this;
  }
  // Load a JSON script (script.h); a dialect it names fixes the dialect.
  bool load(const std::string& json, std::string& err) {
    std::vector<Step> steps;
    Dialect d = dialect_;
    bool named = false;
    if (!loadScript(json, d, steps, err, &named)) return false;
    if (named) {
      fixed_ = true;
      dialect_ = d;
    }
    for (Step& s : steps) script_.push_back(std::move(s));
    return true;
  }
  // Answers computed from the request, used once the pushed steps run out.
  void respondWith(Responder r) { responder_ = std::move(r); }
  // A check run on every request before it is answered; failures collect in failures().
  void expectEach(Expect e) { expect_ = std::move(e); }

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    Recorded rec = record(req);
    Dialect fromPath;
    if (!fixed_ && !dialectOfPath(req.path, fromPath))
      failures_.push_back("request " + std::to_string(seen_.size()) + ": no dialect answers " + req.path);
    if (!rec.view.parsed)
      failures_.push_back("request " + std::to_string(seen_.size()) + ": body is not JSON (" + rec.view.parseError + ")");
    if (expect_) {
      std::string why;
      if (!expect_(rec, why)) failures_.push_back("request " + std::to_string(seen_.size()) + ": " + why);
    }
    Step s;
    if (!nextStep(rec, s)) {
      seen_.push_back(std::move(rec));
      out.status = 0;
      err = "ScriptedProvider: script exhausted (unexpected request " + std::to_string(seen_.size()) + " to " +
            req.host + req.path + ")";
      failures_.push_back(err);
      return false;
    }
    seen_.push_back(std::move(rec));
    if (clock) *clock += s.takesMs;
    return answer(seen_.back(), s, out, err);
  }

  const std::vector<Recorded>& requests() const { return seen_; }
  const Recorded& last() const { return seen_.back(); }
  size_t remaining() const { return script_.size() - next_; }
  bool allUsed() const { return next_ == script_.size(); }
  const std::vector<std::string>& failures() const { return failures_; }
  // The end-of-test check: no failure recorded and every scripted step used. A run that
  // swallowed an exhausted script (an engine retry, an error path) still goes red here.
  bool verify(std::string& why) const {
    if (!failures_.empty()) {
      why = failures_.front();
      return false;
    }
    if (!allUsed()) {
      why = std::to_string(remaining()) + " scripted steps never requested";
      return false;
    }
    return true;
  }

  // Every request after the first keeps the one before it as a prefix (one turn's rounds).
  bool allPrefixStable(std::string& why) const {
    for (size_t i = 1; i < seen_.size(); ++i)
      if (!isPrefix(seen_[i - 1].view, seen_[i].view, why)) {
        why = "request " + std::to_string(i) + ": " + why;
        return false;
      }
    return true;
  }
  // Every request pairs its calls and results.
  bool allPaired(std::string& why) const {
    for (size_t i = 0; i < seen_.size(); ++i)
      if (!callsPaired(seen_[i].view, why)) {
        why = "request " + std::to_string(i) + ": " + why;
        return false;
      }
    return true;
  }

  uint32_t* clock = nullptr;   // advanced by each step's takesMs

 private:
  Recorded record(const agent::HttpRequest& req) const {
    Recorded rec;
    rec.req = req;
    rec.body = agent::materializeBody(req);
    rec.req.body = rec.body;
    rec.req.splices.clear();
    redactKeys(rec.req.headers);
    rec.dialect = dialect_;
    if (!fixed_) dialectOfPath(req.path, rec.dialect);
    rec.step = seen_.size();
    parseRequest(rec.dialect, rec.body, rec.view);
    rec.answerSchema = asksSchema(rec.dialect, rec.body);
    return rec;
  }

  // A recorded request never keeps a credential, so dumping one in a failure is safe.
  static void redactKeys(agent::HeaderList& headers) {
    for (auto& h : headers) {
      std::string n = h.first;
      for (char& c : n) c = (char)std::tolower((unsigned char)c);
      if (n == "authorization" || n == "x-api-key" || n == "api-key" || n == "x-goog-api-key") h.second = "[redacted]";
    }
  }

  bool nextStep(const Recorded& rec, Step& out) {
    if (next_ < script_.size()) {
      out = script_[next_++];
      return true;
    }
    if (!responder_) return false;
    out = responder_(rec, seen_.size());
    return true;
  }

  static bool answer(const Recorded& rec, const Step& s, agent::HttpResponse& out, std::string& err) {
    if (s.transportCut) {
      out.status = 0;
      err = s.transportErr;
      return false;
    }
    out.status = s.status;
    out.headers = s.headers;
    if (!s.rawBody.empty()) out.body = s.rawBody;
    else if (s.status < 200 || s.status >= 300) out.body = s.errorBody.empty() ? renderError(rec.dialect, s.status) : s.errorBody;
    else out.body = renderAnswer(rec.dialect, rec.answerSchema ? structured(s) : s, rec.step);
    return true;
  }

  // Lane E2-CMP: an Anthropic request with output_config.format (a json_schema) gets its answer the way
  // the provider's structured output returns it. A step scripted as change lines becomes {"changes":[...]}
  // (each non-empty line one item, "=" alone an empty list); a step already JSON, blank, cut or refused
  // is served as scripted.
  static Step structured(const Step& s) {
    if (finishOf(s) != Finish::End || s.text.empty() || s.text[0] == '{' ||
        s.text.find_first_not_of(" \t\r\n") == std::string::npos)
      return s;
    JsonDocument list;
    JsonArray items = list["changes"].to<JsonArray>();
    for (size_t at = 0; at <= s.text.size();) {
      size_t nl = s.text.find('\n', at);
      if (nl == std::string::npos) nl = s.text.size();
      const std::string line = s.text.substr(at, nl - at);
      if (!line.empty() && line != "=") items.add(line);
      at = nl + 1;
    }
    Step out = s;
    out.text.clear();
    serializeJson(list, out.text);
    return out;
  }
  static bool asksSchema(Dialect d, const std::string& body) {
    if (d != Dialect::Anthropic) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    return std::string(doc["output_config"]["format"]["type"] | "") == "json_schema";
  }

  bool fixed_ = false;
  Dialect dialect_ = Dialect::Anthropic;
  std::vector<Step> script_;
  size_t next_ = 0;
  Responder responder_;
  Expect expect_;
  std::vector<Recorded> seen_;
  std::vector<std::string> failures_;
};

}  // namespace scripted
}  // namespace harness_test
