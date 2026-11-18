// truth_device.cpp - the truth check's device binding (lane RB-G0). See truth_device.h.
#include "truth_device.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <ctime>
#include <string>

#include "fs_atomic.h"       // agent::writeFileAtomic, flashMounted
#include "store.h"           // the Anthropic key
#include "transport_tls.h"   // agent::deviceTransport(): the one TLS slot
#include "../sys/agent_log.h"

namespace agent {
namespace truthdev {

namespace {

namespace truth = nimbus::orch::truth;
namespace fx = nimbus::orch::fx;

constexpr const char* kStatePath = "/data/truth_state.json";
constexpr size_t kStateMax = 8192;   // 8 heads of about 400 B: anything longer is not ours

RecentFn g_recent = nullptr;

// The owner's offset from UTC now (the device's POSIX TZ), in seconds.
int32_t utcOffsetNow(time_t now) {
  struct tm local, utc;
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
  local.tm_isdst = 0;
  utc.tm_isdst = 0;
  return (int32_t)difftime(mktime(&local), mktime(&utc));
}

std::string readState() {
  if (!flashMounted()) return std::string();
  File f = LittleFS.open(kStatePath, FILE_READ);
  if (!f) return std::string();
  std::string out;
  char buf[256];
  while (out.size() <= kStateMax) {
    const int n = f.read((uint8_t*)buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  f.close();
  return out.size() > kStateMax ? std::string() : out;
}

truth::ProbeResult send(const truth::ProbeRequest& r) {
  HttpRequest req;
  req.method = "POST";
  req.host = r.host;
  req.path = r.path;
  req.timeoutMs = r.timeoutMs;
  req.headers = {{"x-api-key", store::anthropicKey().c_str()},
                 {"anthropic-version", "2023-06-01"},
                 {"content-type", "application/json"}};
  req.body = r.body;
  HttpResponse resp;
  std::string err;
  const uint32_t t0 = millis();
  const bool sent = deviceTransport().exec(req, resp, err);
  truth::ProbeResult out;
  out.ms = millis() - t0;   // the check bounds it at 8 s
  out.verdict = sent ? truth::parseResponse(resp.status, resp.body) : truth::Verdict::Error;
  if (!sent) alogf("truth: probe not answered (%.80s)", err.c_str());
  return out;
}

class DeviceTruth : public truth::Binding {
 public:
  truth::Counters* open(const std::string& chatId, const std::string& host, const std::string& model, bool tainted,
                        truth::Turn& t, truth::ProbeFn& probe) override {
    if (!loaded_) {
      loaded_ = true;
      truth::parseState(readState(), state_);   // damaged or absent: every head starts from the table
    }
    const nimbus::orch::headqual::Line line = nimbus::orch::headqual::Line::E1;
    const truth::Truth start = truth::startMode(host.c_str(), model.c_str(), line);
    truth::HeadState& h = state_.head(truth::stateKey(host.c_str(), model.c_str(), line), start);
    key_ = h.key;
    t.mode = truth::turnMode(start, h, tainted);
    const time_t now = time(nullptr);
    t.nowEpoch = now > 1000000000 ? (int64_t)now : 0;
    t.utcOffsetSec = t.nowEpoch ? utcOffsetNow(now) : 0;
    t.recent.clear();
    t.ringKnown = false;
    if (g_recent) t.ringKnown = g_recent(chatId, t.recent);
    if (truth::probeWire(host.c_str()) && store::hasAnthropicKey()) probe = send;
    return &h.c;
  }

  void close() override {
    for (auto& h : state_.heads)
      if (h.key == key_) truth::nextMode(h, /*allowShadow=*/false);   // v5.2.0: no automatic move
    key_.clear();
    const std::string json = truth::renderState(state_);
    if (json != saved_ && writeFileAtomic(kStatePath, json, "truth", /*logFailure=*/false)) saved_ = json;
  }

 private:
  truth::State state_;
  std::string key_, saved_;
  bool loaded_ = false;
};

DeviceTruth* g_binding = nullptr;

}  // namespace

truth::Binding* binding() {
  if (!g_binding) g_binding = new DeviceTruth();
  return g_binding;
}

void useRecent(RecentFn fn) { g_recent = fn; }

}  // namespace truthdev
}  // namespace agent
