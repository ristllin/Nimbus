#pragma once
#include <ArduinoJson.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <regex>
#include <string>

#include "nimbus/harness/http.h"

// Lab-only transport decorators. They sit between the shipping provider adapters
// and libcurl, so nothing in lib/ changes:
//
//   adapters -> WireOverrideTransport -> FaultInjectingTransport -> RecordingTransport -> curl
//
// WireOverrideTransport applies the run's pins (NIMBUS_LAB_EFFORT /
// NIMBUS_LAB_MAXTOK / NIMBUS_LAB_CEILING_MS) to each outgoing request. The device
// sends no effort and caps Anthropic output at 3000 tokens, and adaptive thinking
// counts toward that cap, so a live leg that drives the device adapter pins a low
// effort until the adapters carry their own cap table.
//
// RecordingTransport (--record=<dir>) appends every provider usage object and
// error body to <dir>/usage.jsonl and <dir>/errors.jsonl: spend is priced from
// these provider-reported numbers (tools/harness-lab/spend_ledger.py), never from
// a device ledger. It sits next to curl, so an injected fault never reaches it.
namespace lab {

struct WirePins {
  std::string effort;        // Anthropic output_config.effort; "" = send what the adapter sends
  uint32_t    maxTokens = 0; // 0 = the adapter's own cap
  uint32_t    ceilingMs = 0; // 0 = the adapter's own per-request timeout
};

inline bool isProviderHost(const std::string& h) {
  return h == "api.anthropic.com" || h == "api.openai.com" || h == "api.mistral.ai" ||
         h == "api.z.ai" || h == "open.bigmodel.cn";
}

class WireOverrideTransport : public agent::HttpTransport {
 public:
  WireOverrideTransport(agent::HttpTransport* inner, const WirePins* pins)
      : inner_(inner), pins_(pins) {}

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    agent::HttpRequest r = req;
    apply(r);
    return inner_->exec(r, out, err);
  }
  int execJson(const agent::HttpRequest& req, JsonDocument& doc, const JsonDocument& filter,
               std::string& err) override {
    agent::HttpRequest r = req;
    apply(r);
    return inner_->execJson(r, doc, filter, err);
  }

  // The rewrite, exposed for tests.
  void apply(agent::HttpRequest& r) const {
    if (pins_->ceilingMs && r.timeoutMs > pins_->ceilingMs) r.timeoutMs = pins_->ceilingMs;
    if (r.method != "POST" || !isProviderHost(r.host) || r.body.empty()) return;
    if (pins_->effort.empty() && !pins_->maxTokens) return;
    JsonDocument d;
    if (deserializeJson(d, r.body, DeserializationOption::NestingLimit(agent::kResponseNestingLimit)) ||
        !d.is<JsonObject>())
      return;
    const bool changed = r.host == "api.anthropic.com" ? pinAnthropic(r.path, d) : pinCapOnly(r.path, d);
    if (!changed) return;
    r.body.clear();
    serializeJson(d, r.body);
  }

 private:
  // Messages API: effort (a model without effort support answers 400; run it with
  // --effort=adapter) and the output cap.
  bool pinAnthropic(const std::string& path, JsonDocument& d) const {
    if (path.find("/messages") == std::string::npos) return false;
    if (!pins_->effort.empty()) d["output_config"]["effort"] = pins_->effort;
    if (pins_->maxTokens) d["max_tokens"] = pins_->maxTokens;
    return !pins_->effort.empty() || pins_->maxTokens;
  }
  // OpenAI Responses and chat completions (Mistral, Z.ai): the output cap only.
  bool pinCapOnly(const std::string& path, JsonDocument& d) const {
    if (!pins_->maxTokens) return false;
    if (path.find("/responses") != std::string::npos) {
      d["max_output_tokens"] = pins_->maxTokens;
      return true;
    }
    if (path.find("/chat/completions") == std::string::npos) return false;
    d["max_tokens"] = pins_->maxTokens;
    return true;
  }

  agent::HttpTransport* inner_;
  const WirePins* pins_;
};

// Secret-shaped substrings an error body can echo (a partly masked key in a 401).
inline std::string scrubSecrets(std::string s) {
  static const std::regex kPatterns[] = {
      std::regex(R"(sk-[A-Za-z0-9_\-\*]{6,})"),
      std::regex(R"((Bearer|bearer)\s+[A-Za-z0-9._\-]{8,})"),
      std::regex(R"(([?&](key|token|api_key)=)[^&\s"]+)"),
  };
  s = std::regex_replace(s, kPatterns[0], "sk-[redacted]");
  s = std::regex_replace(s, kPatterns[1], "$1 [redacted]");
  s = std::regex_replace(s, kPatterns[2], "$1[redacted]");
  return s;
}

class RecordingTransport : public agent::HttpTransport {
 public:
  RecordingTransport(agent::HttpTransport* inner, std::string dir)
      : inner_(inner), dir_(std::move(dir)) {}

  bool enabled() const { return !dir_.empty(); }
  uint32_t usageRows() const { return usageRows_; }
  uint32_t errorRows() const { return errorRows_; }

  bool exec(const agent::HttpRequest& req, agent::HttpResponse& out, std::string& err) override {
    const bool ok = inner_->exec(req, out, err);
    if (enabled() && ok && isProviderHost(req.host)) record(req, out);
    return ok;
  }

 private:
  static std::string utcNow() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
  }
  void append(const char* file, const JsonDocument& row) {
    const std::string path = dir_ + "/" + file;
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) { std::fprintf(stderr, "[record] cannot open %s\n", path.c_str()); return; }
    std::string line;
    serializeJson(row, line);
    std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
  }
  void record(const agent::HttpRequest& req, const agent::HttpResponse& out) {
    JsonDocument row;
    row["ts"] = utcNow();
    row["host"] = req.host;
    row["path"] = req.path;
    row["status"] = out.status;
    row["bodyBytes"] = req.body.size();   // the request body as sent (the prompt-size measure)
    JsonDocument rq;
    if (!req.body.empty() &&
        !deserializeJson(rq, req.body, DeserializationOption::NestingLimit(agent::kResponseNestingLimit))) {
      row["requested"] = rq["model"] | "";
      // What the run pinned, so a recorded row proves the pin reached the wire.
      if (!rq["output_config"]["effort"].isNull()) row["effort"] = rq["output_config"]["effort"];
      if (!rq["max_tokens"].isNull()) row["max_tokens"] = rq["max_tokens"];
      if (!rq["max_output_tokens"].isNull()) row["max_tokens"] = rq["max_output_tokens"];
    }
    if (out.status >= 400) {
      row["body"] = scrubSecrets(out.body.substr(0, 4096));
      append("errors.jsonl", row);
      errorRows_++;
      return;
    }
    JsonDocument rs;
    if (deserializeJson(rs, out.body, DeserializationOption::NestingLimit(agent::kResponseNestingLimit)) ||
        rs["usage"].isNull())
      return;   // no usage: nothing to price
    row["model"] = rs["model"] | "";
    row["usage"] = rs["usage"];
    append("usage.jsonl", row);
    usageRows_++;
  }

  agent::HttpTransport* inner_;
  std::string dir_;
  uint32_t usageRows_ = 0, errorRows_ = 0;
};

}  // namespace lab
