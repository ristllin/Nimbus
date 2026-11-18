// orch_truth.cpp - the truth check (lane RB-G0). The contract and the decision table are in
// nimbus/orch/truth.h. Nothing here reads the reply for words: the probe's answer, this
// turn's effects and the mode decide.
#include "nimbus/orch/truth.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nimbus {
namespace orch {
namespace truth {

namespace {

// tests/evals/measured/probe.json, models.<id>.ruling9.qualifies (test_truth compares).
const MeasuredProbe kMeasured[] = {
    {"claude-haiku-5-5", true},
    {"gpt-5.6-luna", false},
    {"glm-5.3-flash", false},
    {"mistral-small-latest", false},
};

bool same(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

bool blank(const std::string& s) {
  for (unsigned char ch : s)
    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return false;
  return true;
}

// The longest prefix of s of at most cap bytes that ends on a UTF-8 boundary.
std::string clip(const std::string& s, size_t cap) {
  if (s.size() <= cap) return s;
  size_t n = cap;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

std::string oneLine(const std::string& s, size_t cap) { return fx::cleanText(s, cap); }

std::string nowLine(const TurnEffect& e) {
  return oneLine(e.tool, fx::kToolMax) + (e.ok ? " ok: " : " FAILED: ") + oneLine(e.receipt, kNowReceiptMax);
}

// A reply the probe can read whole up to kReplyMax; a longer one keeps its start and its end.
std::string clipReply(const std::string& s) {
  if (s.size() <= kReplyMax) return s;
  size_t from = s.size() - kReplyMax / 2;
  while (from < s.size() && ((unsigned char)s[from] & 0xC0) == 0x80) ++from;
  return clip(s, kReplyMax / 2) + "\n[...]\n" + s.substr(from);
}

std::string recentLine(const RecentLine& r) { return r.at + " " + r.tool + ": " + r.receipt; }

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
void civil(int64_t days, int& y, unsigned& m, unsigned& d) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = (unsigned)(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

const char* const kTags[] = {"owner_message", "reply", "effects_this_turn", "recent_effects"};

bool sameNoCase(const std::string& s, size_t at, const char* t, size_t n) {
  if (at + n > s.size()) return false;
  for (size_t k = 0; k < n; ++k)
    if (std::tolower((unsigned char)s[at + k]) != t[k]) return false;
  return true;
}

// The length of a section tag ("<tag>" or "</tag>", any case, spaces or tabs before the '>') at
// s[i], 0 when there is none.
size_t tagAt(const std::string& s, size_t i) {
  const size_t j = i + 1 + (i + 1 < s.size() && s[i + 1] == '/');
  for (const char* t : kTags) {
    const size_t n = std::strlen(t);
    if (!sameNoCase(s, j, t, n)) continue;
    size_t e = j + n;
    while (e < s.size() && (s[e] == ' ' || s[e] == '\t')) ++e;
    if (e < s.size() && s[e] == '>') return e + 1 - i;
  }
  return 0;
}

// The first {"claims_unbacked_action": true|false} at or after `from`: its verdict and span.
bool answerAt(const std::string& s, size_t at, Verdict& v, size_t& end) {
  static const char kKey[] = "\"claims_unbacked_action\"";
  auto ws = [&s](size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i;
  };
  size_t i = ws(at + 1);
  if (s.compare(i, sizeof(kKey) - 1, kKey) != 0) return false;
  i = ws(i + sizeof(kKey) - 1);
  if (i >= s.size() || s[i] != ':') return false;
  i = ws(i + 1);
  if (s.compare(i, 4, "true") == 0) {
    v = Verdict::Yes;
    i += 4;
  } else if (s.compare(i, 5, "false") == 0) {
    v = Verdict::No;
    i += 5;
  } else {
    return false;
  }
  i = ws(i);
  if (i >= s.size() || s[i] != '}') return false;
  end = i + 1;
  return true;
}

// The answer text as run_probe.strict_answer sees it: trimmed, a code fence taken off.
std::string strictForm(const std::string& text) {
  size_t a = 0, b = text.size();
  while (a < b && std::strchr(" \t\r\n", text[a])) ++a;
  while (b > a && std::strchr(" \t\r\n", text[b - 1])) --b;
  std::string t = text.substr(a, b - a);
  if (t.compare(0, 3, "```") != 0) return t;
  a = t.find_first_not_of('`');
  b = t.find_last_not_of('`');
  t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
  if (t.compare(0, 4, "json") == 0) t.erase(0, 4);
  return strictForm(t);
}

Verdict timed(const ProbeResult& r, uint32_t bound) { return r.ms > bound ? Verdict::Timeout : r.verdict; }

void countFail(Verdict v, Counters& c) {
  if (v == Verdict::Timeout) ++c.failTimeout;
  if (v == Verdict::Error) ++c.failError;
  if (v == Verdict::Malformed) ++c.failMalformed;
}

bool answered(Verdict v) { return v == Verdict::Yes || v == Verdict::No; }

// One probe, bounded; a probe that throws is an error.
Verdict send(const ProbeFn& probe, const ProbeRequest& req) {
  try {
    return timed(probe(req), req.timeoutMs);
  } catch (...) {
    return Verdict::Error;
  }
}

// One probe of `reply` against `now`: builds the request, sends it, bounds it, counts it.
Verdict ask(const Turn& t, const std::vector<TurnEffect>& now, const std::string& reply, const ProbeFn& probe,
            Counters& c, Outcome& o) {
  ProbeRequest req;
  const std::string user = renderUser(t.owner, reply, now, recentLines(t.recent, t.nowEpoch, t.utcOffsetSec));
  Verdict v = Verdict::Error;
  if (probe && buildRequest(t.host, user, req)) {
    ++c.probes;
    ++o.probes;
    v = send(probe, req);
  }
  if (o.probes <= 1 && o.first == Verdict::Error) o.first = v;
  if (v == Verdict::Yes) ++c.hits;
  countFail(v, c);
  return v;
}

// The receipt line after a probe: on yes, or deterministically when there was no answer.
void receiptAfter(Verdict v, const std::vector<TurnEffect>& now, Counters& c, Outcome& o) {
  if (v == Verdict::No) return;
  o.status = receiptLine(now);
  ++(v == Verdict::Yes ? c.receiptsProbed : c.receiptsFixed);
}

bool runRound(const RetryFn& retry, const std::string& item, std::string& r, std::vector<TurnEffect>& more) {
  try {
    return retry && retry(item, r, more);
  } catch (...) {
    return false;
  }
}

}  // namespace

// ---- the measured probe ------------------------------------------------------------------

const MeasuredProbe* measuredProbes(size_t& count) {
  count = sizeof(kMeasured) / sizeof(kMeasured[0]);
  return kMeasured;
}

bool probePasses(const char* model) {
  for (const auto& m : kMeasured)
    if (same(m.model, model)) return m.passes;
  return false;
}

const char* probeModelFor(const char* host) {
  if (same(host, "anthropic")) return kAnthropicProbeModel;
  if (same(host, "openai")) return "gpt-5.6-luna";
  if (same(host, "zai")) return "glm-5.3-flash";
  return "";
}

bool probeWire(const char* host) { return same(host, "anthropic"); }

Truth startModeFrom(Truth rowMode, bool probeWireExists, bool probeModelPasses) {
  return probeWireExists && probeModelPasses ? rowMode : Truth::ReceiptOnly;
}

Truth startMode(const char* host, const char* model, headqual::Line line) {
  if (!host) return Truth::ReceiptOnly;
  return startModeFrom(headqual::lookup(host, model, line).truth, probeWire(host), probePasses(probeModelFor(host)));
}

// ---- effects ---------------------------------------------------------------------------

const char* kindNoun(Kind k) {
  switch (k) {
    case Kind::Read:    return "";
    case Kind::Ledger:  return "the ledger change";
    case Kind::File:    return "the file change";
    case Kind::Memory:  return "the memory change";
    case Kind::Routine: return "the reminder change";
    case Kind::Setting: return "the setting change";
    case Kind::Send:    return "the message";
    case Kind::Other:   return "the change";
  }
  return "the change";
}

namespace {
constexpr size_t kKinds = (size_t)Kind::Other + 1;
// For each write kind, the index of its last effect when that effect failed, else -1.
void lastFailures(const std::vector<TurnEffect>& now, int (&last)[kKinds]) {
  for (int& v : last) v = -1;
  for (size_t i = 0; i < now.size(); ++i) {
    const size_t k = (size_t)now[i].kind;
    if (now[i].kind == Kind::Read || k >= kKinds) continue;
    last[k] = now[i].ok ? -1 : (int)i;
  }
}
}  // namespace

bool writeFailed(const std::vector<TurnEffect>& now) {
  int last[kKinds];
  lastFailures(now, last);
  for (int v : last)
    if (v >= 0) return true;
  return false;
}

std::string receiptLine(const std::vector<TurnEffect>& now) {
  int last[kKinds];
  lastFailures(now, last);
  std::vector<size_t> order;   // the failed effects, in the order they happened
  for (size_t i = 0; i < now.size(); ++i)
    for (int v : last)
      if (v == (int)i) order.push_back(i);
  if (order.empty()) return "";
  std::string nouns, errors;
  for (size_t j = 0; j < order.size(); ++j) {
    const TurnEffect& e = now[order[j]];
    nouns += j == 0 ? "" : (j + 1 == order.size() ? " and " : ", ");
    nouns += kindNoun(e.kind);
    const std::string err = oneLine(e.receipt, kErrorMax);
    if (!err.empty()) errors += (errors.empty() ? "" : "; ") + err;
  }
  std::string line = "Note: " + nouns + " in this reply did not go through";
  if (!errors.empty()) line += " (" + errors + ")";
  return line;
}

// ---- the probe request -------------------------------------------------------------------

std::string inert(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const size_t n = text[i] == '<' ? tagAt(text, i) : 0;
    if (!n) {
      out += text[i];
      continue;
    }
    out += "&lt;";
    out.append(text, i + 1, n - 1);
    i += n - 1;
  }
  return out;
}

std::string formatAt(int64_t epoch, int64_t now, int32_t utcOffsetSec) {
  if (epoch == 0) return "--:--";
  const bool clockSet = now >= kClockSetEpoch;
  static const char* const kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  const int64_t local = epoch + utcOffsetSec;
  const int64_t day = floorDiv(local, 86400), today = floorDiv(now + utcOffsetSec, 86400);
  const int64_t sec = local - day * 86400;
  char buf[32];
  const int hh = (int)(sec / 3600), mm = (int)(sec % 3600 / 60);
  if (clockSet && day >= today) {
    snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
  } else if (clockSet && today - day <= 6) {
    snprintf(buf, sizeof(buf), "%s %02d:%02d", kDays[(int)(((day % 7) + 7) % 7)], hh, mm);
  } else {
    int y;
    unsigned m, d;
    civil(day, y, m, d);
    snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d", y, m, d, hh, mm);
  }
  return buf;
}

std::vector<RecentLine> recentLines(const std::vector<fx::RingEntry>& ring, int64_t now, int32_t utcOffsetSec) {
  std::vector<RecentLine> out;
  out.reserve(ring.size());
  for (const auto& e : ring) out.push_back({formatAt(e.epoch, now, utcOffsetSec), e.tool, e.receipt});
  return out;
}

std::string renderUser(const std::string& owner, const std::string& reply, const std::vector<TurnEffect>& now,
                       const std::vector<RecentLine>& recent) {
  std::string nowText, recentText;
  for (const auto& e : now) nowText += (nowText.empty() ? "- " : "\n- ") + inert(nowLine(e));
  for (const auto& r : recent) recentText += (recentText.empty() ? "- " : "\n- ") + inert(recentLine(r));
  std::string u;
  u.reserve(owner.size() + reply.size() + nowText.size() + recentText.size() + 160);
  u += "<owner_message>\n" + inert(clip(owner, kOwnerMax)) + "\n</owner_message>\n";
  u += "<reply>\n" + inert(clipReply(reply)) + "\n</reply>\n";
  u += "<effects_this_turn>\n" + (nowText.empty() ? std::string("(none)") : nowText) + "\n</effects_this_turn>\n";
  u += "<recent_effects>\n" + (recentText.empty() ? std::string("(none)") : recentText) + "\n</recent_effects>";
  return u;
}

bool buildRequest(const char* host, const std::string& user, ProbeRequest& out) {
  if (!probeWire(host)) return false;
  JsonDocument d;
  d["model"] = kAnthropicProbeModel;
  d["max_tokens"] = kProbeMaxTokens;
  d["system"] = kProbePrompt;
  d["output_config"]["effort"] = "low";
  JsonObject m = d["messages"].add<JsonObject>();
  m["role"] = "user";
  m["content"] = user;
  out.body.clear();
  serializeJson(d, out.body);
  out.model = kAnthropicProbeModel;
  out.host = "api.anthropic.com";
  out.path = "/v1/messages";
  out.timeoutMs = kProbeTimeoutMs;
  return true;
}

const char* verdictSlug(Verdict v) {
  switch (v) {
    case Verdict::No:        return "no";
    case Verdict::Yes:       return "yes";
    case Verdict::Malformed: return "malformed";
    case Verdict::Error:     return "error";
    case Verdict::Timeout:   return "timeout";
  }
  return "error";
}

Verdict parseAnswer(const std::string& text, bool* lenient) {
  for (size_t at = text.find('{'); at != std::string::npos; at = text.find('{', at + 1)) {
    Verdict v;
    size_t end;
    if (!answerAt(text, at, v, end)) continue;
    if (lenient) *lenient = strictForm(text) != text.substr(at, end - at);
    return v;
  }
  return Verdict::Malformed;
}

Verdict parseResponse(int status, const std::string& body) {
  if (status != 200) return Verdict::Error;
  JsonDocument filter, d;
  filter["content"][0]["type"] = true;
  filter["content"][0]["text"] = true;
  if (deserializeJson(d, body, DeserializationOption::Filter(filter))) return Verdict::Error;
  std::string text;
  for (JsonVariant b : d["content"].as<JsonArray>())
    if (same(b["type"] | "", "text")) text += b["text"] | "";
  return parseAnswer(text);
}

// ---- counters and modes ------------------------------------------------------------------

uint32_t HeadState::windowHits() const {
  uint32_t n = 0;
  for (uint8_t b : window)
    for (int k = 0; k < 8; ++k) n += (b >> k) & 1u;
  return n;
}

void HeadState::pushShadow(bool hit) {
  const uint8_t mask = (uint8_t)(1u << (winPos % 8));
  window[winPos / 8] = hit ? (uint8_t)(window[winPos / 8] | mask) : (uint8_t)(window[winPos / 8] & ~mask);
  winPos = (uint16_t)((winPos + 1) % kWindow);
  if (winLen < kWindow) ++winLen;
}

std::string stateKey(const char* host, const char* model, headqual::Line line) {
  return std::string(host ? host : "") + "/" + (model ? model : "") + "/" + headqual::lineSlug(line);
}

namespace {
// P(X <= k) for X ~ Binomial(n, p), each term carried as a logarithm so a large n cannot
// underflow the terms that matter (log, log1p and exp only: no lgamma in the image).
double binomCdf(uint32_t k, uint32_t n, double p) {
  const double step = std::log(p) - std::log1p(-p);
  double lt = n * std::log1p(-p), sum = 0;   // log P(X = 0)
  for (uint32_t i = 0; i <= k && i <= n; ++i) {
    sum += std::exp(lt);
    lt += std::log((double)(n - i) / (double)(i + 1)) + step;
  }
  return sum;
}
}  // namespace

bool nextMode(HeadState& h, bool allowShadow) {
  if (!allowShadow) return false;
  if (h.mode == Truth::Enforce) {
    // The one-sided 95% upper bound is at most 1% exactly when P(X <= k | p = 1%) <= 5%.
    const uint32_t k = h.c.effectHits, n = h.c.effectTurns;
    if (n < kShadowMinTurns || k >= n || binomCdf(k, n, 0.01) > 0.05) return false;
    h.mode = Truth::Shadow;
    return true;
  }
  if (h.mode != Truth::Shadow || h.windowHits() < kShadowBackHits) return false;
  h.mode = Truth::Enforce;
  h.c.effectTurns = h.c.effectHits = 0;
  std::memset(h.window, 0, sizeof(h.window));
  h.winLen = h.winPos = 0;
  return true;
}

HeadState& State::head(const std::string& key, Truth start) {
  size_t i = 0;
  while (i < heads.size() && heads[i].key != key) ++i;
  if (i == heads.size()) {
    if (heads.size() >= kMaxHeads) heads.erase(heads.begin());
    HeadState h;
    h.key = key;
    h.mode = start;
    heads.push_back(h);
  } else if (i + 1 != heads.size()) {
    HeadState h = heads[i];   // the most recently used goes last
    heads.erase(heads.begin() + (long)i);
    heads.push_back(h);
  }
  HeadState& h = heads.back();
  if (start == Truth::ReceiptOnly || h.mode == Truth::ReceiptOnly) h.mode = start;
  return h;
}

Truth turnMode(Truth start, const HeadState& h, bool tainted) {
  if (tainted || start == Truth::ReceiptOnly || h.mode == Truth::ReceiptOnly) return Truth::ReceiptOnly;
  return h.mode;
}

namespace {
constexpr size_t kCounters = sizeof(Counters) / sizeof(uint32_t);
static_assert(sizeof(Counters) == kCounters * sizeof(uint32_t), "Counters holds uint32_t only");

bool truthFromSlug(const char* s, Truth& out) {
  for (Truth t : {Truth::ReceiptOnly, Truth::Enforce, Truth::Shadow})
    if (same(s, headqual::truthSlug(t))) {
      out = t;
      return true;
    }
  return false;
}

bool parseHead(JsonObject o, HeadState& h) {
  const char* key = o["k"] | "";
  const char* w = o["w"] | "";
  JsonArray c = o["c"].as<JsonArray>();
  if (!*key || !truthFromSlug(o["m"] | "", h.mode) || c.size() != kCounters || std::strlen(w) != 2 * sizeof(h.window))
    return false;
  h.key = key;
  uint32_t* dst = reinterpret_cast<uint32_t*>(&h.c);
  for (size_t i = 0; i < kCounters; ++i) {
    if (!c[i].is<uint32_t>()) return false;
    dst[i] = c[i].as<uint32_t>();
  }
  for (size_t i = 0; i < sizeof(h.window); ++i) {
    unsigned v;
    if (!std::isxdigit((unsigned char)w[2 * i]) || !std::isxdigit((unsigned char)w[2 * i + 1]) ||
        std::sscanf(w + 2 * i, "%2x", &v) != 1)
      return false;
    h.window[i] = (uint8_t)v;
  }
  const uint32_t n = o["n"] | 0xFFFFu, p = o["p"] | 0xFFFFu;
  if (n > kWindow || p >= kWindow) return false;
  h.winLen = (uint16_t)n;
  h.winPos = (uint16_t)p;
  return true;
}
}  // namespace

std::string renderState(const State& s) {
  JsonDocument d;
  d["v"] = 1;
  JsonArray heads = d["heads"].to<JsonArray>();
  for (const HeadState& h : s.heads) {
    JsonObject o = heads.add<JsonObject>();
    o["k"] = h.key;
    o["m"] = headqual::truthSlug(h.mode);
    JsonArray c = o["c"].to<JsonArray>();
    const uint32_t* src = reinterpret_cast<const uint32_t*>(&h.c);
    for (size_t i = 0; i < kCounters; ++i) c.add(src[i]);
    char hex[2 * sizeof(h.window) + 1];
    for (size_t i = 0; i < sizeof(h.window); ++i) snprintf(hex + 2 * i, 3, "%02x", h.window[i]);
    o["w"] = hex;
    o["n"] = h.winLen;
    o["p"] = h.winPos;
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

bool parseState(const std::string& json, State& out) {
  out.heads.clear();
  JsonDocument d;
  if (deserializeJson(d, json) || !d.is<JsonObject>() || (d["v"] | 0) != 1 || !d["heads"].is<JsonArray>()) return false;
  State s;
  for (JsonVariant v : d["heads"].as<JsonArray>()) {
    HeadState h;
    if (!v.is<JsonObject>() || !parseHead(v.as<JsonObject>(), h) || s.heads.size() >= kMaxHeads) return false;
    s.heads.push_back(h);
  }
  out = s;
  return true;
}

// ---- the check ---------------------------------------------------------------------------

std::string factsItem(const std::vector<TurnEffect>& now, const std::vector<RecentLine>& recent) {
  std::string item = "[device] effects this turn: ";
  for (size_t i = 0; i < now.size(); ++i) item += (i ? "; " : "") + nowLine(now[i]);
  if (now.empty()) item += "none";
  item += ". Recent: ";
  // The newest recent effects that fit, shown oldest first.
  std::vector<std::string> kept;
  // Room for the entries, keeping 5 B for "none." so the item stays inside kItemMax.
  size_t room = item.size() + 5 < kItemMax ? kItemMax - item.size() - 5 : 0;
  for (size_t i = recent.size(); i > 0 && kept.size() < kItemRecentMax; --i) {
    std::string l = oneLine(recentLine(recent[i - 1]), kItemMax);
    const size_t need = l.size() + (kept.empty() ? 0 : 2);
    if (need > room) break;
    room -= need;
    kept.push_back(std::move(l));
  }
  for (size_t i = kept.size(); i > 0; --i) item += kept[i - 1] + (i > 1 ? "; " : "");
  if (kept.empty()) item += "none";
  return item + ".";
}

// ---- one turn's check across a head loop ------------------------------------------------

namespace {
bool ringModeOk(const Turn& t) { return t.ringKnown && !t.tainted; }
}  // namespace

Truth TurnCheck::mode() const { return ringModeOk(t_) ? t_.mode : Truth::ReceiptOnly; }

void TurnCheck::reset(Binding* binding, const std::string& chatId, const std::string& owner) {
  *this = TurnCheck();
  binding_ = binding;
  chatId_ = chatId;
  t_.owner = owner;
}

void TurnCheck::start(const Turn& t, ProbeFn probe, Counters* c) {
  std::vector<TurnEffect> pending = std::move(t_.now);
  t_ = t;
  t_.now.insert(t_.now.end(), pending.begin(), pending.end());
  host_ = t.host ? t.host : "";
  t_.host = host_.c_str();
  probe_ = std::move(probe);
  c_ = c;
}

bool TurnCheck::open(const std::string& host, const std::string& model, bool tainted) {
  if (c_) return true;
  if (!binding_) return false;
  Turn t;
  ProbeFn probe;
  t.owner = t_.owner;
  host_ = host;
  t.host = host_.c_str();
  t.tainted = tainted;
  Counters* c = binding_->open(chatId_, host, model, tainted, t, probe);
  if (!c) return false;
  t.host = host_.c_str();   // whatever the binding did with it, the head is this one
  t.tainted = tainted;
  start(t, std::move(probe), c);
  return true;
}

void TurnCheck::note(const TurnEffect& e) { t_.now.push_back(e); }

std::string TurnCheck::atFinal(const std::string& reply, bool canHold) {
  if (!c_ || asked_) return "";
  asked_ = true;
  firstReply_ = reply;
  ++c_->turns;
  if (t_.tainted) ++c_->tainted;
  if (mode() != Truth::Enforce || blank(reply)) return "";
  first_ = ask(t_, t_.now, reply, probe_, *c_, o_);
  if (answered(first_) && (!t_.now.empty() || first_ == Verdict::Yes)) {
    ++c_->effectTurns;
    c_->effectHits += first_ == Verdict::Yes;
  }
  if (first_ != Verdict::Yes || !canHold) return "";
  ++c_->holds;
  o_.held = true;
  return factsItem(t_.now, recentLines(t_.recent, t_.nowEpoch, t_.utcOffsetSec));
}

Outcome TurnCheck::finish(const std::string& reply, bool roundReplied) {
  if (c_ && !asked_) atFinal(reply, false);   // the loop never asked (a single shot, a used round)
  Outcome o = o_;
  o.reply = reply;
  if (!c_) return o;
  const bool failed = writeFailed(t_.now);
  const Truth m = mode();
  if (m == Truth::ReceiptOnly || blank(firstReply_)) {
    if (failed) receiptAfter(Verdict::Error, t_.now, *c_, o);
  } else if (m == Truth::Shadow) {
    if (failed) receiptAfter(ask(t_, t_.now, reply, probe_, *c_, o), t_.now, *c_, o);
    else o.shadowDue = true;
  } else if (o.held) {
    o.retried = roundReplied && !blank(reply);
    ++(o.retried ? c_->retryReplies : c_->retryFailed);
    if (!o.retried) o.reply = firstReply_;
    // The receipt question about the reply that is delivered: the round's, else the first
    // (which the probe already said reports success).
    if (failed) receiptAfter(o.retried ? ask(t_, t_.now, o.reply, probe_, *c_, o) : Verdict::Yes, t_.now, *c_, o);
  } else if (failed) {
    receiptAfter(first_, t_.now, *c_, o);
  }
  if (binding_) binding_->close();
  c_ = nullptr;
  return o;
}

Outcome check(const Turn& t, const ProbeFn& probe, const RetryFn& retry, Counters& c) {
  TurnCheck k;
  k.start(t, probe, &c);
  const std::string item = k.atFinal(t.reply, (bool)retry);
  if (item.empty()) return k.finish(t.reply);
  std::string r;
  std::vector<TurnEffect> more;
  const bool ran = runRound(retry, item, r, more);
  for (const TurnEffect& e : more) k.note(e);
  return k.finish(ran ? r : t.reply, ran);
}

void shadowProbe(const Turn& t, const ProbeFn& probe, HeadState& h) {
  if (!ringModeOk(t) || blank(t.reply)) return;
  ProbeRequest req;
  const std::string user = renderUser(t.owner, t.reply, t.now, recentLines(t.recent, t.nowEpoch, t.utcOffsetSec));
  if (!probe || !buildRequest(t.host, user, req)) {
    ++h.c.failError;
    return;
  }
  ++h.c.probes;
  const Verdict v = send(probe, req);
  if (!answered(v)) {
    countFail(v, h.c);
    return;
  }
  ++h.c.shadowProbes;
  h.c.shadowHits += v == Verdict::Yes;
  h.pushShadow(v == Verdict::Yes);
}

}  // namespace truth
}  // namespace orch
}  // namespace nimbus
