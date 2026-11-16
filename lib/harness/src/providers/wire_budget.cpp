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
  const size_t derived = nimbus::orch::deriveRequestContext(rb.budget, nimbus::orch::BudgetOverrides()).replayBytes;
  return std::max(v511Bytes, derived);
}

uint32_t textTokens(const RoundBudget& rb, size_t textBytes) {
  const uint32_t r = rNomOf(rb);
  return (uint32_t)((uint64_t)textBytes * 100 / r);
}

RoundLoad roundLoad(const RoundBudget& rb, size_t textBytes, size_t imageBytes, uint32_t photoTokens) {
  RoundLoad l;
  l.bodyBytes = textBytes + imageBytes;
  l.imageBytes = imageBytes;
  l.promptTokens = textTokens(rb, textBytes) + photoTokens;
  l.routed = rb.routed;
  l.fragmented = rb.fragmented;
  // RULINGS-v3 W(3): a low-balance Cumulo wallet answers 402 with what fits and the wire
  // sends the body again, capped. Every routed round counts that second upload in its fit
  // (at the 85,000 B routed floor about 1.2 s).
  l.uploads = rb.routed ? 2 : 1;
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

OutputCap fittedCap(const RoundBudget& rb, RoundKind kind, uint32_t roundMs, const RoundLoad& load) {
  OutputCap c = outputCapFor(rb.host, rb.model, kind, roundMs, load);
  const uint32_t want = minFinalTokens(rb);
  if (!c.maxTokens || c.maxTokens >= want || roundFitTokens(rb, roundMs, load) >= want) return c;
  c.maxTokens = want;
  const bool lowerEffort = std::strcmp(c.retryEffort, c.effort) != 0;
  if (c.retryTokens && !lowerEffort && c.retryTokens < want + kRetryGainTokens) c.retryTokens = 0;
  else if (c.retryTokens && c.retryTokens < want) c.retryTokens = want;
  return c;
}

uint32_t roundFitTokens(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load) {
  return providers::roundFitTokens(rb.host, rb.model, roundMs, load);
}

uint32_t minFinalTokens(const RoundBudget& rb) { return nimbus::orch::minFinalTokensFor(rb.host, rb.model); }

size_t photoFitShedBytes(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load) {
  const uint32_t fit = roundFitTokens(rb, roundMs, load);
  const uint32_t want = minFinalTokens(rb);
  if (fit >= want) return 0;
  const uint32_t decode = nimbus::orch::decodeFloorTokPerS(rb.host, rb.model, load.promptTokens);
  const uint32_t prefill = nimbus::orch::prefillFloorTokPerS(rb.host, rb.model);
  const uint64_t shortMs = (uint64_t)(want - fit) * 1000 / (decode ? decode : 1);
  // What one text byte costs, in ns: its upload (every time the body goes up), its share
  // of a paced slice on a fragmented heap, and its share of a token at the prefill rate.
  const uint32_t r = rNomOf(rb);
  const uint64_t perByteNs =
      (uint64_t)load.uploads * 1000000000ull / nimbus::orch::kBudgetUploadBytesPerS +
      (load.fragmented ? 1000000ull * nimbus::orch::kBudgetPaceSliceMs / nimbus::orch::kBudgetPaceSliceBytes : 0) +
      1000000000ull * 100 / r / (prefill ? prefill : 1);
  return (size_t)(shortMs * 1000000ull / (perByteNs ? perByteNs : 1)) + 1024;
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

bool textOverFit(const RoundBudget& rb, const RoundLoad& load, std::string& err) {
  // A photo round's own rule decides (photosOverFit); a body inside v5.1.1's budget is sent
  // as v5.1.1 sent it (no head below its v5.1.1 budget), with the cap its fit allows.
  if (load.imageBytes || load.bodyBytes <= kWireBodyMax) return false;
  const uint32_t fit = roundFitTokens(rb, 0, load);
  const uint32_t want = minFinalTokens(rb);
  if (fit >= want) return false;
  const size_t text = load.bodyBytes;
  const size_t shed = photoFitShedBytes(rb, 0, load);
  const size_t budget = std::max(kWireBodyMax, text > shed ? (text - shed) / 1000 * 1000 : 0);
  err = nimbus::orch::bodyBudgetError(budget, text);
  hlog::logf("wire: %s/%s round over its time fit (fit %u of %u tokens in the %s window) - %s", rb.host.c_str(),
             rb.model.c_str(), (unsigned)fit, (unsigned)want, rb.routed ? "routed" : "host's", err.c_str());
  return true;
}

bool fitRoundToLoad(const RoundFitIn& in, std::string& body, BodySplices& splices, OutputCap& cap, std::string& err) {
  const RoundLoad load = roundLoad(in.rb, body.size(), splicedBytes(splices), in.photoTokens);
  const OutputCap sent = in.capped ? fittedCap(in.rb, in.kind, in.roundMs, load) : OutputCap();
  recordRound(in.rb, in.kind, in.roundMs, load, sent);
  lastRound().textBudget = in.textBudget;
  lastRound().photoTokens = in.photoTokens;
  // Both fits are the host's whole read window: what the request carries decides them, and
  // the turn's own deadline is the loop's to handle (it forces the final round).
  if (photosOverFit(in.rb, 0, load, in.photoTokens, err) || textOverFit(in.rb, load, err)) return false;
  if (in.capped) {
    const bool sameShape = std::strcmp(sent.effort, cap.effort) == 0 && (sent.maxTokens == 0) == (cap.maxTokens == 0);
    if (!sameShape || (cap.maxTokens && !replaceMaxTokens(body, splices, cap.maxTokens, sent.maxTokens))) {
      err = "the round's output cap could not be set in its request";
      hlog::logf("wire: %s/%s %s", in.rb.host.c_str(), in.rb.model.c_str(), err.c_str());
      return false;
    }
    cap = sent;
  }
  return bodyWithinBudget(body, in.textBudget, err);   // the patched cap is a digit wider at most
}

bool seedWithinBudget(const std::string& seed, std::string& err) {
  if (seed.size() <= flatPromptBudget()) return true;
  err = "the newest message is over the " + std::to_string(flatPromptBudget() / 1000) +
        " KB message budget: " + std::to_string(seed.size()) + " B";
  return false;
}

bool parseSeedBudgetError(const std::string& err, size_t& overBytes) {
  static const char kHead[] = "the newest message is over the ";
  static const char kMid[] = " KB message budget: ";
  if (err.rfind(kHead, 0) != 0) return false;
  const size_t at = err.find(kMid);
  if (at == std::string::npos) return false;
  const size_t size = (size_t)std::strtoul(err.c_str() + at + sizeof(kMid) - 1, nullptr, 10);
  overBytes = size > flatPromptBudget() ? size - flatPromptBudget() : 0;
  return true;
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
  nimbus::orch::noteObservedRatio(rb.host, rb.model, textBytes, o.promptTokens);
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
