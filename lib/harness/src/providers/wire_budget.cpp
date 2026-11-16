#include "wire_budget.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The out-of-line half of wire_budget.h: one definition each, so the head rounds share one
// copy (a header's out-of-line helpers become one weak copy per file that uses them, and
// the debug tables of the copies the linker drops are what the stack gate's line lookups
// trip over).
namespace agent {
namespace providers {
namespace wire {

RoundBudget roundBudget(const ProviderDeps& pd, const std::string& host, const std::string& model, bool routed) {
  RoundBudget rb;
  rb.host = host;
  rb.model = model;
  rb.routed = routed;
  rb.budget = wireBudgetFor(host, model, routed);
  rb.fragmented = fragmentedNow(pd);
  return rb;
}

std::unique_ptr<const RoundBudget> newRoundBudget(const ProviderDeps& pd, const std::string& host,
                                                  const std::string& model, bool routed) {
  return std::unique_ptr<const RoundBudget>(new RoundBudget(roundBudget(pd, host, model, routed)));
}

size_t replayBytesFor(const RoundBudget& rb, size_t v511Bytes) {
  const size_t derived = nimbus::orch::headContextBudget(rb.host, rb.model, nimbus::orch::BudgetOverrides()).replayBytes;
  return std::max(v511Bytes, derived);
}

uint32_t textTokens(const RoundBudget& rb, size_t textBytes) {
  const uint32_t r = rb.budget.rNomX100 ? rb.budget.rNomX100 : 400;
  return (uint32_t)((uint64_t)textBytes * 100 / r);
}

RoundLoad roundLoad(const RoundBudget& rb, size_t textBytes, size_t imageBytes, uint32_t photoTokens) {
  RoundLoad l;
  l.bodyBytes = textBytes + imageBytes;
  l.imageBytes = imageBytes;
  l.promptTokens = textTokens(rb, textBytes) + photoTokens;
  l.routed = rb.routed;
  l.fragmented = rb.fragmented;
  return l;
}

size_t splicedBytes(const BodySplices& splices) {
  size_t n = 0;
  for (const BodySplice& s : splices) n += spliceLen(s);
  return n;
}

RoundRecord& lastRound() {
  static RoundRecord r;
  return r;
}

void recordRound(const RoundBudget& rb, RoundKind kind, uint32_t roundMs, const RoundLoad& load,
                 const OutputCap& cap) {
  RoundRecord& r = lastRound();
  r.host = rb.host;
  r.model = rb.model;
  r.kind = kind;
  r.roundMs = roundMs;
  r.load = load;
  r.cap = cap;
  r.textBudget = rb.budget.wireBodyBytes;
  r.photoTokens = 0;
}

bool replaceMaxTokens(std::string& body, BodySplices& splices, uint32_t from, uint32_t to) {
  if (from == to) return true;
  const std::string key = "\"max_tokens\":" + std::to_string(from);
  const size_t at = body.find("\"max_tokens\":");
  if (at == std::string::npos || body.compare(at, key.size(), key) != 0) return false;
  const size_t end = at + key.size();
  if (end >= body.size() || (body[end] != ',' && body[end] != '}')) return false;
  const std::string with = "\"max_tokens\":" + std::to_string(to);
  body.replace(at, key.size(), with);
  const long delta = (long)with.size() - (long)key.size();
  for (BodySplice& s : splices)
    if (s.at >= end) s.at = (size_t)((long)s.at + delta);
  return true;
}

bool fitCapToLoad(std::string& body, BodySplices& splices, OutputCap& cap, const RoundBudget& rb, RoundKind kind,
                  uint32_t roundMs, const RoundLoad& load) {
  const OutputCap fitted = outputCapFor(rb.host, rb.model, kind, roundMs, load);
  const bool sameShape = std::strcmp(fitted.effort, cap.effort) == 0 && (fitted.maxTokens == 0) == (cap.maxTokens == 0);
  const bool inPlace = sameShape && (cap.maxTokens == 0 || replaceMaxTokens(body, splices, cap.maxTokens, fitted.maxTokens));
  cap = fitted;
  recordRound(rb, kind, roundMs, load, fitted);
  return inPlace;
}

FitRates fitRates(const RoundBudget& rb, uint32_t promptTokens) {
  std::string m = rb.model;
  const size_t slash = m.find('/');
  if (slash != std::string::npos) m = m.substr(slash + 1);
  FitRates f{120000, 1000, 50, 3000};
  if (rb.host == "mistral") {
    f.windowMs = 60000;
    if (*sameHostNonReasoningModel("mistral", m)) {   // the forced reasoner (Large 4)
      f.prefill = 9000;
      f.decode = 50;
      f.minFinal = 1500;
    } else {
      f.prefill = 2500;
      f.decode = promptTokens < 32000 ? 130 : 70;
    }
  } else if (rb.host == "zai") {
    f.windowMs = 50000;
    f.prefill = 9000;
    f.decode = promptTokens < 32000 ? 55 : 48;
    f.minFinal = 1600;
  } else if (rb.host == "anthropic") {
    f.prefill = m.find("opus") != std::string::npos ? 30000 : 60000;
    f.decode = m.find("haiku") != std::string::npos ? 120 : m.find("opus") != std::string::npos ? 50 : 75;
  } else if (rb.host == "openai") {
    f.prefill = 25000;
  }
  if (rb.routed && f.windowMs > kRoutedRoundCeilingMs) f.windowMs = kRoutedRoundCeilingMs;
  return f;
}

uint32_t paceMsForBody(size_t bodyBytes) {
  const size_t slices = (bodyBytes + nimbus::heapwatch::kBodySliceBytes - 1) / nimbus::heapwatch::kBodySliceBytes;
  return kRequestPaceMaxMs + (uint32_t)slices * kSliceStallMs;
}

uint64_t overheadMs(const FitRates& f, const RoundLoad& load) {
  const uint64_t uploadMs = (uint64_t)load.bodyBytes * 1000 / 70000;
  const uint64_t paceMs = load.fragmented ? paceMsForBody(load.bodyBytes) : 0;
  const uint64_t prefillMs = (uint64_t)load.promptTokens * 1000 / (f.prefill ? f.prefill : 1);
  return 1500 + uploadMs + paceMs + prefillMs;
}

uint32_t roundFitTokens(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load) {
  const FitRates f = fitRates(rb, load.promptTokens);
  const uint64_t window = roundMs && roundMs < f.windowMs ? roundMs : f.windowMs;
  const uint64_t over = overheadMs(f, load);
  return over >= window ? 0 : (uint32_t)((window - over) * f.decode / 1000);
}

uint32_t minFinalTokens(const RoundBudget& rb) { return fitRates(rb, 0).minFinal; }

size_t photoFitShedBytes(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load) {
  const FitRates f = fitRates(rb, load.promptTokens);
  const uint32_t fit = roundFitTokens(rb, roundMs, load);
  const uint32_t want = minFinalTokens(rb);
  if (fit >= want) return 0;
  const uint64_t shortMs = (uint64_t)(want - fit) * 1000 / (f.decode ? f.decode : 1);
  // micro-seconds one text byte costs: 1/70 KB/s upload, 100 ms per 4 KB slice paced,
  // and its share of a token at the prefill rate
  const uint32_t r = rb.budget.rNomX100 ? rb.budget.rNomX100 : 400;
  const uint64_t perByteNs = 1000000000ull / 70000 + (load.fragmented ? 1000000000ull / 4096 / 10 : 0) +
                             1000000000ull * 100 / r / (f.prefill ? f.prefill : 1);
  return (size_t)(shortMs * 1000000ull / perByteNs) + 1024;
}

std::string photoFitError(const std::string& host, uint32_t fit, uint32_t minFinal, size_t overBytes) {
  char buf[160];
  std::snprintf(buf, sizeof buf, "photos do not fit the %s time window (fit %u of %u tokens): over by %u B",
                host.c_str(), (unsigned)fit, (unsigned)minFinal, (unsigned)overBytes);
  return buf;
}

bool parsePhotoFitError(const std::string& err, size_t& overBytes) {
  static const char kHead[] = "photos do not fit the ";
  static const char kOver[] = "): over by ";
  if (err.rfind(kHead, 0) != 0) return false;
  const size_t at = err.find(kOver);
  if (at == std::string::npos) return false;
  overBytes = (size_t)std::strtoul(err.c_str() + at + sizeof(kOver) - 1, nullptr, 10);
  return true;
}

bool photosOverFit(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load, uint32_t photoTokens,
                   std::string& err) {
  if (photoTokens == 0 && load.imageBytes == 0) return false;
  const uint32_t fit = roundFitTokens(rb, roundMs, load);
  const uint32_t want = minFinalTokens(rb);
  if (fit >= want) return false;
  err = photoFitError(rb.host, fit, want, photoFitShedBytes(rb, roundMs, load));
  hlog::logf("wire: %s", err.c_str());
  return true;
}

RoundFit fitRoundToLoad(const RoundFitIn& in, std::string& body, BodySplices& splices, OutputCap& cap,
                        std::string& err) {
  const RoundLoad load = roundLoad(in.rb, body.size(), splicedBytes(splices), in.photoTokens);
  RoundFit fit = RoundFit::Ready;
  if (!in.capped) recordRound(in.rb, in.kind, in.roundMs, load, OutputCap());
  else if (!fitCapToLoad(body, splices, cap, in.rb, in.kind, in.roundMs, load)) fit = RoundFit::Rebuild;
  lastRound().textBudget = in.textBudget;
  lastRound().photoTokens = in.photoTokens;
  if (photosOverFit(in.rb, in.roundMs, load, in.photoTokens, err)) return RoundFit::Refused;
  if (fit == RoundFit::Ready && !bodyWithinBudget(body, in.textBudget, err)) return RoundFit::Refused;   // a digit wider
  return fit;
}

ObservedRatio& lastObservedRatio() {
  static ObservedRatio o;
  return o;
}

void noteObservedRatio(const RoundBudget& rb, size_t textBytes, uint32_t requestTokens, uint32_t photoTokens) {
  if (!requestTokens || !textBytes) return;
  ObservedRatio& o = lastObservedRatio();
  o.host = rb.host;
  o.model = rb.model;
  o.textBytes = textBytes;
  o.promptTokens = requestTokens > photoTokens ? requestTokens - photoTokens : 0;
}

void addRoundUsage(ArduinoJson::JsonObjectConst usageJson, nimbus::orch::TokenUsage* usage, const RoundBudget& rb,
                   size_t textBytes, uint32_t photoTokens) {
  const nimbus::orch::TokenUsage round = nimbus::orch::tokenUsageFromJson(usageJson);
  if (usage) *usage += round;
  noteObservedRatio(rb, textBytes, round.maxRequestIn, photoTokens);
}

void addRoutedDeadline(HttpRequest& req, uint32_t timeoutMs) {
  if (req.path.rfind("/router/", 0) != 0 || timeoutMs <= kRoutedDeadlineSlackMs) return;
  req.headers.emplace_back(kRoutedDeadlineHeader, std::to_string(timeoutMs - kRoutedDeadlineSlackMs));
}

}  // namespace wire
}  // namespace providers
}  // namespace agent
