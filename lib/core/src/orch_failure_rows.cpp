#include "nimbus/orch/failure_rows.h"

#include <ArduinoJson.h>

#include "nimbus/mem_cap.h"

namespace nimbus {
namespace orch {

namespace {

// How far a turn-end row has been shrunk (each level keeps every earlier cut).
enum Shrink : int {
  kFull = 0,
  kViaNoModels,
  kErr120,
  kErr80,
  kErr56,
  kNoCache,
  kViaNoCache,
  kNoModel,
  kErrBrief,
  kNoVia,
  kBare,
  kLevels
};

std::string capUtf8(const std::string& s, size_t cap) {
  return s.substr(0, (size_t)utf8CapLen(s.c_str(), (int)s.size(), (int)cap));
}

std::string clean(const std::string& s, size_t cap) {
  return sanitizeFailText(s.data(), s.size(), cap);
}

// The cause at one shrink level: the chain rendered to a byte budget, or the last
// error text when there is no chain.
std::string causeAt(const TurnEndRowIn& r, int level) {
  const bool chain = r.chain && !r.chain->empty();
  if (level >= kErrBrief) {
    if (chain) return r.chain->brief(0);
    const std::string c = shortFailCause(clean(r.error, FailureChain::kTextMax));
    return c.empty() ? std::string("failed") : c;
  }
  const size_t cap = level >= kErr56 ? 56 : level >= kErr80 ? 80 : level >= kErr120 ? 120 : 160;
  if (chain) return r.chain->render(cap);
  const std::string e = clean(r.error, cap);
  return e.empty() ? std::string("failed") : e;
}

bool viaShown(const TurnEndRowIn& r) { return r.spend && r.spend->billedSlots() > 1; }

void addVia(JsonDocument& d, const TurnEndRowIn& r, int level) {
  JsonArray via = d["via"].to<JsonArray>();
  for (size_t i = 0; i < r.spend->n; ++i) {
    const SpendSlot& s = r.spend->slot[i];
    JsonArray e = via.add<JsonArray>();
    e.add(clean(s.host, 15));
    if (level < kViaNoModels) e.add(clean(s.model, 47));
    else if (level < kViaNoCache) e.add("");
    e.add(s.in);
    e.add(s.out);
    if (level < kViaNoCache) {
      e.add(s.cacheR);
      e.add(s.cacheW);
    }
  }
}

// Everything after host/ok/in/out (none of it on the bare row).
void addDetail(JsonDocument& d, const TurnEndRowIn& r, int level) {
  if (level < kNoCache && r.cacheR) d["cr"] = r.cacheR;
  if (level < kNoCache && r.cacheW) d["cw"] = r.cacheW;
  d["reply"] = (uint32_t)r.reply;
  if (r.blobFails) d["blobFails"] = r.blobFails;   // L19-24: the trace sidecar loss is on the row
  if (!r.ok) d["err"] = causeAt(r, level);
  if (level < kNoVia && viaShown(r)) addVia(d, r, level);
}

std::string rowAt(const TurnEndRowIn& r, int level) {
  JsonDocument d;
  d["host"] = clean(r.host, 15);
  if (level < kBare) {
    if (level < kNoModel && !r.model.empty()) d["model"] = clean(r.model, 64);
    if (r.fallback) d["fallback"] = true;
    if (!r.src.empty()) d["src"] = clean(r.src, 12);
    d["tools"] = r.tools;
  }
  d["ok"] = r.ok;
  d["in"] = r.in;
  d["out"] = r.out;
  if (level < kBare) addDetail(d, r, level);
  std::string js;
  serializeJson(d, js);
  return js;
}

std::string toolPhrase(int n) {
  if (n <= 0) return "no tool ran";
  if (n == 1) return "1 tool call ran";
  return std::to_string(n) + " tool calls ran";
}

}  // namespace

std::string turnEndRowJson(const TurnEndRowIn& r, size_t cap) {
  std::string js;
  for (int level = kFull; level < kLevels; ++level) {
    js = rowAt(r, level);
    if (js.size() <= cap) return js;
  }
  return js;   // the bare row: within kTurnEndRowMin by construction
}

std::string errorEventText(const ErrorEventIn& e, size_t cap) {
  const std::string from = clean(e.requestedHost, 15), to = clean(e.finalHost, 15);
  std::string pre = "Turn FAILED";
  if (!e.src.empty()) pre += " (" + clean(e.src, 12) + ")";
  if (!from.empty()) pre += " on " + from;
  if (!to.empty() && to != from) pre += (from.empty() ? " on " : " -> ") + to;
  pre += ": ";
  const std::string post = "; " + toolPhrase(e.toolCalls);
  const size_t fixed = pre.size() + post.size();
  const size_t budget = cap > fixed ? cap - fixed : 0;
  std::string cause;
  if (e.chain && !e.chain->empty()) cause = e.chain->render(budget);
  else cause = capUtf8(clean(e.error, FailureChain::kTextMax), budget);
  if (cause.empty()) cause = capUtf8("cause not recorded", budget);
  return capUtf8(pre + cause + post, cap);
}

std::string dreamRowText(bool ok, const std::string& cause, uint32_t spentTokens, int pruned,
                         int deduped, size_t cap) {
  const std::string tail = " - spent " + std::to_string(spentTokens) +
                           " tokens; maintenance pruned=" + std::to_string(pruned) +
                           " deduped=" + std::to_string(deduped);
  if (ok) return capUtf8("Nightly dream ok" + tail, cap);
  const std::string pre = "Nightly dream FAILED (";
  const size_t fixed = pre.size() + 1 + tail.size();
  std::string c = clean(cause, FailureChain::kTextMax);
  if (c.empty()) c = "cause not recorded";
  c = capUtf8(c, cap > fixed ? cap - fixed : 0);
  return capUtf8(pre + c + ")" + tail, cap);
}

}  // namespace orch
}  // namespace nimbus
