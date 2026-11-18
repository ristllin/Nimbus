#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "hosted_effects.h"
#include "nimbus/harness/http.h"
#include "nimbus/orch/truth.h"
#include "posix_fs.h"

// hosted_truth.h - the truth check on a Virtual Nimbus (lane RB-G0, PRD section 8 rail 8). The
// portable check (nimbus/orch/truth.h) over the instance: the probe goes out on the instance's
// own HttpTransport with the head host's key (the Anthropic Messages wire, claude-haiku-5-5),
// the recent effects are the chat's ring (hosted_effects.h), and the modes and counters live in
// <data>/truth_state.json (written whole, by temp and rename, after every check). Phase 1 is the
// binding and its test; the engine hook after the head loop lands in phase 2 (engine.cpp), and
// calls check() there.
namespace nimbusd {

class HostedTruth {
 public:
  struct Deps {
    agent::HttpTransport* http = nullptr;
    // The key a request to the head's host carries ("" = none: no probe, receipt-only).
    std::function<std::string(const std::string& host)> key;
    // Milliseconds from a monotonic clock (tests drive it; default: steady_clock).
    std::function<uint64_t()> nowMs;
    // Automatic enforce/shadow moves (PRD 8.8). Off for v5.2.0: the rate is measured first.
    bool allowShadow = false;
  };

  // One reply to check, as the engine has it after the head loop.
  struct Input {
    std::string chatId;
    std::string host, model;               // the head that wrote the reply
    nimbus::orch::headqual::Line line = nimbus::orch::headqual::Line::E1;
    bool tainted = false;
    std::string owner;                      // the owner's message as text (photos as stub lines)
    std::string reply;
    std::vector<nimbus::orch::truth::TurnEffect> now;
    int64_t nowEpoch = 0;
    int32_t utcOffsetSec = 0;
  };

  HostedTruth(const std::string& dataDir, Deps deps, HostedEffects* effects)
      : path_(dataDir + "/truth_state.json"), deps_(std::move(deps)), effects_(effects) {
    std::string json;
    if (fsutil::readFile(path_, json)) nimbus::orch::truth::parseState(json, state_);   // damaged: start again
  }

  nimbus::orch::truth::Outcome check(const Input& in, const nimbus::orch::truth::RetryFn& retry) {
    nimbus::orch::truth::HeadState& h = head(in);
    const nimbus::orch::truth::Turn t = turn(in, h);
    const nimbus::orch::truth::Outcome o = nimbus::orch::truth::check(t, probeFor(in.host), retry, h.c);
    nimbus::orch::truth::nextMode(h, deps_.allowShadow);
    save();
    return o;
  }

  // The idle step's shadow probe (Outcome::shadowDue): counters and the window only.
  void shadow(const Input& in) {
    nimbus::orch::truth::HeadState& h = head(in);
    nimbus::orch::truth::shadowProbe(turn(in, h), probeFor(in.host), h);
    nimbus::orch::truth::nextMode(h, deps_.allowShadow);
    save();
  }

  // The probe over the instance's transport for a head on `host`; empty when there is no
  // transport or no key (the check then counts the turn and stays receipt-only for it).
  nimbus::orch::truth::ProbeFn probeFor(const std::string& host) {
    std::string key = deps_.key ? deps_.key(host) : std::string();
    if (!deps_.http || key.empty() || !nimbus::orch::truth::probeWire(host.c_str())) return {};
    return [this, key](const nimbus::orch::truth::ProbeRequest& r) { return send(r, key); };
  }

  const nimbus::orch::truth::State& state() const { return state_; }
  const std::string& statePath() const { return path_; }

 private:
  nimbus::orch::truth::HeadState& head(const Input& in) {
    using namespace nimbus::orch::truth;
    const Truth start = startMode(in.host.c_str(), in.model.c_str(), in.line);
    return state_.head(stateKey(in.host.c_str(), in.model.c_str(), in.line), start);
  }

  nimbus::orch::truth::Turn turn(const Input& in, const nimbus::orch::truth::HeadState& h) const {
    using namespace nimbus::orch::truth;
    Turn t;
    t.host = in.host.c_str();
    t.mode = turnMode(startMode(in.host.c_str(), in.model.c_str(), in.line), h, in.tainted);
    t.tainted = in.tainted;
    t.owner = in.owner;
    t.reply = in.reply;
    t.now = in.now;
    if (effects_) effects_->ring().recent(in.chatId, nimbus::orch::fx::kRingEntries, t.recent);
    t.nowEpoch = in.nowEpoch;
    t.utcOffsetSec = in.utcOffsetSec;
    return t;
  }

  uint64_t nowMs() const {
    if (deps_.nowMs) return deps_.nowMs();
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  }

  nimbus::orch::truth::ProbeResult send(const nimbus::orch::truth::ProbeRequest& r, const std::string& key) {
    using namespace nimbus::orch::truth;
    agent::HttpRequest req;
    req.method = "POST";
    req.host = r.host;
    req.path = r.path;
    req.timeoutMs = r.timeoutMs;
    req.headers = {{"x-api-key", key}, {"anthropic-version", "2023-06-01"}, {"content-type", "application/json"}};
    req.body = r.body;
    agent::HttpResponse resp;
    std::string err;
    const uint64_t t0 = nowMs();
    const bool sent = deps_.http->exec(req, resp, err);
    const uint64_t took = nowMs() - t0;
    ProbeResult out;
    out.ms = took > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)took;
    out.verdict = sent ? parseResponse(resp.status, resp.body) : Verdict::Error;   // past 8 s: the check's own bound
    return out;
  }

  void save() { fsutil::writeFileAtomic(path_, nimbus::orch::truth::renderState(state_)); }

  std::string path_;
  Deps deps_;
  HostedEffects* effects_;
  nimbus::orch::truth::State state_;
};

}  // namespace nimbusd
