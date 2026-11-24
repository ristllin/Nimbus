#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cases.h"
#include "golden_io.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/platform.h"
#include "nimbus/harness/plain/exchange.h"
#include "nimbus/harness/plain/wire.h"
#include "render_case.h"

// parse_case.h - the parse goldens (router design section 11, layer L1p): for each recorded
// answer the wire's ParsedStep, then what the exchange makes of the same answer (its class,
// requests, retries, the output cap it sent, its status line and the step it hands back), as
// text. The exchange runs with a fixed transport that serves the answer verbatim and a scripted
// clock, around the text_only request; no provider is called.
namespace route_goldens {

inline constexpr const char* kAnswers = "test/support/fixtures/anthropic_wire_2026-11/";
inline constexpr const char* kMistralAnswers = "test/support/fixtures/mistral_wire_synthetic/";
inline constexpr const char* kZaiAnswers = "test/support/fixtures/zai_wire_synthetic/";

struct Answer {
  const char* file;   // under dir
  int status;         // the HTTP status it is served with
  const char* sig = kDirect;     // the row signature it is read on
  const char* dir = kAnswers;    // a synthetic fixture's header lines (source, status, expect) are not the body
};

// Every recorded Anthropic answer of the wire's parse suite, at the status it came with.
inline std::vector<Answer> answers() {
  return {{"text_end_turn.json", 200},           {"tool_use_one.json", 200},
          {"tool_use_two.json", 200},            {"thinking_then_call.json", 200},
          {"thinking_then_text.json", 200},      {"max_tokens_cut_call.json", 200},
          {"input_transformations_dropped.json", 200}, {"synthetic_refusal.json", 200},
          {"synthetic_interleaved.json", 200},   {"synthetic_empty_content.json", 200},
          {"thinking_near_the_line_call.json", 200},   {"thinking_over_the_line_cut.json", 200},
          {"error_not_found.json", 404},
          // Lane E2-R6: the chat rows' synthetic answers (written from the providers' documentation).
          {"tool_calls.txt", 200, kMistral, kMistralAnswers},
          {"thinking_chunks.txt", 200, kMistral, kMistralAnswers},
          {"length_cut_call.txt", 200, kMistral, kMistralAnswers},
          {"model_length.txt", 200, kMistral, kMistralAnswers},
          {"error_3051_effort_refused.txt", 400, kMistral, kMistralAnswers},
          {"error_invalid_model.txt", 400, kMistral, kMistralAnswers},
          {"reasoning_content_text.txt", 200, kZai, kZaiAnswers},
          {"tool_call_args_object.txt", 200, kZai, kZaiAnswers},
          {"tool_call_args_string.txt", 200, kZai, kZaiAnswers},
          {"length_cut.txt", 200, kZai, kZaiAnswers},
          {"sensitive.txt", 200, kZai, kZaiAnswers},
          {"model_context_window_exceeded.txt", 200, kZai, kZaiAnswers},
          {"error_1113_no_balance.txt", 429, kZai, kZaiAnswers},
          {"error_1301_unsafe_content.txt", 400, kZai, kZaiAnswers}};
}

// The answer as the transport serves it (the parse suite's own reading: trailing newlines off).
inline bool answerBody(const Answer& a, std::string& body) {
  if (!readFile(std::string(a.dir) + a.file, body)) return false;
  const size_t json = body.find("\n{");   // a synthetic fixture: the body follows its header lines
  if (a.dir != kAnswers && json != std::string::npos) body.erase(0, json + 1);
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
  return true;
}

inline std::string stem(const char* file) {
  std::string s = file;
  return s.substr(0, s.rfind('.'));
}
inline std::string parsePath(const Answer& a) { return std::string(kRoot) + a.sig + "/" + stem(a.file) + ".parse"; }

inline std::string usageText(const nimbus::orch::TokenUsage& u) {
  return "prompt=" + std::to_string(u.promptTokens) + " completion=" + std::to_string(u.completionTokens) +
         " cacheRead=" + std::to_string(u.cacheReadTokens) + " cacheWrite=" + std::to_string(u.cacheWriteTokens) +
         " cacheWrite1h=" + std::to_string(u.cacheWrite1hTokens) + " responses=" + std::to_string(u.responses) +
         " maxRequestIn=" + std::to_string(u.maxRequestIn) + " served=" + quoted(u.servedModel);
}

// A long opaque value: its size, its hash and its first 48 bytes.
inline std::string pinned(const std::string& s) {
  return std::to_string(s.size()) + " B fnv64=" + hex64(fnv64(s)) + " head=" + quoted(s.substr(0, 48));
}

inline std::string stepText(const plain::ParsedStep& s, const char* prefix) {
  const std::string p = prefix;
  std::string o = p + "stop " + plain::stopSlug(s.stop) + "\n" + p + "text " + pinned(s.text) + "\n";
  o += p + "refusal " + quoted(s.refusal) + "\n" + p + "calls " + std::to_string(s.calls.size()) + "\n";
  for (size_t i = 0; i < s.calls.size(); ++i) {
    const plain::ToolCall& c = s.calls[i];
    o += p + "  call " + std::to_string(i) + " id=" + quoted(c.id) + " name=" + quoted(c.name) +
         " complete=" + (c.complete ? "1" : "0") + " args=" + quoted(c.argsJson) + "\n";
  }
  o += p + "meta " + std::to_string(s.meta.size()) + "\n";
  for (size_t i = 0; i < s.meta.size(); ++i)
    o += p + "  meta " + std::to_string(i) + " beforeCall=" + std::to_string(s.meta[i].beforeCall) + " " +
         pinned(s.meta[i].bytes) + "\n";
  return o + p + "usage " + usageText(s.usage) + "\n";
}

// A transport that serves one answer to every request and keeps what it was sent.
struct Serve : agent::HttpTransport {
  int status = 200;
  std::string body;
  std::vector<std::string> sent;
  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string&) override {
    sent.push_back(agent::materializeBody(req));
    out.status = status;
    out.body = body;
    return true;
  }
};

inline uint32_t gClockMs = 0;

inline std::string exchangeText(const plain::ExchangeResult& r, const Serve& s) {
  std::string o = "exchange err=" + std::string(plain::exchangeErrSlug(r.err)) + " http=" + std::to_string(r.httpStatus) +
                  " requests=" + std::to_string(r.requests) + " retries=" + std::to_string(r.retries) +
                  " climbs=" + std::to_string(r.climbs) + " cutRetried=" + (r.cutRetried ? "1" : "0") +
                  " failOver=" + (r.failOver ? "1" : "0") + " parkHost=" + (r.parkHost ? "1" : "0") +
                  " maxTokens=" + std::to_string(r.maxTokens) + " requestBytes=" + std::to_string(r.requestBytes) +
                  " ms=" + std::to_string(r.ms) + " waitedMs=" + std::to_string(r.waitedMs) +
                  " statusLine=" + std::to_string((int)r.status) + "\n";
  o += "exchange line " + quoted(r.errorLine) + "\nexchange line stored " + quoted(r.errorLineStored) + "\n";
  for (size_t i = 0; i < s.sent.size(); ++i)
    o += "exchange request " + std::to_string(i) + " " + std::to_string(s.sent[i].size()) + " B fnv64=" +
         hex64(fnv64(s.sent[i])) + "\n";
  return o + stepText(r.step, "exchange step ");
}

// The exchange around the text_only request, answered by `a` on every request.
inline std::string runExchangeOn(const char* sig, int status, const std::string& body) {
  const Case c = make("text_only", firstMessage(), Facts(), sig);
  World w;
  tx::Status st = w.load(c);
  const plain::WireCodec* wire = nullptr;
  if (st.ok()) st = plain::wireFor(w.row.dialect, wire);
  if (!st.ok()) return refusedLine(st);
  Serve serve;
  serve.status = status;
  serve.body = body;
  agent::Platform platform;
  gClockMs = 1000;
  platform.nowMs = [] { return gClockMs; };
  platform.delayMs = [](uint32_t ms) { gClockMs += ms; };
  plain::ExchangeDeps deps;
  deps.transport = &serve;
  deps.platform = &platform;
  plain::ExchangeRequest req;
  req.wire = wire;
  req.render = &w.in;
  req.timeoutMs = 120000;
  req.deadlineMs = 240000;
  req.bodyCap = 200000;
  plain::ExchangeResult out;
  st = plain::runExchange(deps, req, out);
  if (!st.ok()) return "exchange refused " + std::string(tx::errSlug(st.err)) + " at " + st.where + "\n";
  return exchangeText(out, serve);
}

// The golden text of one answer: the parse, then the exchange.
inline std::string parseCase(const Answer& a) {
  std::string o = "# route golden parse " + std::string(a.sig) + "/" + a.file + " served at HTTP " +
                  std::to_string(a.status) + "\n";
  std::string body;
  if (!answerBody(a, body)) return o + "answer fixture missing\n";
  agent::HttpResponse resp;
  resp.status = a.status;
  resp.body = body;
  plain::ParsedStep step;
  const plain::WireCodec* wire = nullptr;
  if (!plain::wireFor(rowOf(a.sig, false).dialect, wire).ok() || !wire) return o + "no wire\n";
  const tx::Status ps = wire->parse(resp, step);
  o += std::string("parse ") + tx::errSlug(ps.err) + (ps.ok() ? "" : std::string(" at ") + ps.where) + "\n";
  o += stepText(step, "parse ");
  return o + runExchangeOn(a.sig, a.status, body);
}

}  // namespace route_goldens
