// test_web_guard - no web request can stall the device into a watchdog reset
// (nimbus/sys/web_guard.h).
//
// The owner's device reset through the task watchdog on 26 Oct 2026: a GET /api/state
// waited (portMAX_DELAY) for the shared card lock, agent::memory::lock, on the AsyncTCP
// task, which the watchdog watches, while a turn's image.look held that lock across
// two 60 s photo requests. These tests pin the class:
//   * the portable rules: the web task's slot wait and network budget over every ask,
//     one network allowance per web request however many exchanges it chains,
//     the worst web request inside the watchdog, the tools that wait on the network;
//   * at source level (web paths): no web file takes the card lock without a timeout,
//     and every web handler, filler or callback that reaches a function taking the
//     card lock (transitively, by name, through src/agent, src/sys and src/sfx) opens
//     with a WebLock first. A new web path that waits fails here;
//   * at source level (the state page): built whole and sent through the filler path,
//     so a short packet-buffer moment never cuts or repeats the page, and its flash
//     figures (a half-second LittleFS walk) are measured once a minute, not per poll,
//     and the spend and ledger locks (held across flash writes) are only tried;
//   * at source level (the turn's tool dispatch): every registered tool whose handler
//     makes a network call is one the dispatcher runs outside its whole-call card lock,
//     and the memory engines' embedding gives the lock up for its request.
// The gate reads call sites as text: a call through a function pointer or an object's
// method (agent::tgarchive::cardFs().exists) is outside its reach. The device keeps the
// runtime backstop for those: every blocking take on the web task and every network
// request started under the card lock is counted (WEBGUARD?) and named once per boot.
#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "nimbus/sys/web_guard.h"

using namespace nimbus::sys;

void setUp() {}
void tearDown() {}

// ---- the portable rules -------------------------------------------------------------

static void test_slot_wait_is_capped_on_the_web_task_only() {
  const uint32_t asks[] = {0u, 1u, kWebSlotWaitMs - 1, kWebSlotWaitMs, kWebSlotWaitMs + 1, 10000u, 35000u, UINT32_MAX};
  for (uint32_t ask : asks) {
    TEST_ASSERT_TRUE(slotWaitMs(true, ask) <= kWebSlotWaitMs);
    TEST_ASSERT_EQUAL_UINT32(ask < kWebSlotWaitMs ? ask : kWebSlotWaitMs, slotWaitMs(true, ask));
    TEST_ASSERT_EQUAL_UINT32(ask, slotWaitMs(false, ask));   // a turn waits what it asked for
  }
}

static void test_network_budget_is_capped_on_the_web_task_only() {
  const uint32_t asks[] = {0u, 1000u, kWebNetBudgetMs, kWebNetBudgetMs + 1, 20000u, UINT32_MAX};
  for (uint32_t ask : asks) {
    TEST_ASSERT_TRUE(netBudgetMs(true, ask) <= kWebNetBudgetMs);
    TEST_ASSERT_EQUAL_UINT32(ask, netBudgetMs(false, ask));
  }
}

// The worst web request: every card-lock try times out, the slot wait times out, the
// exchange runs its whole budget. A quarter of the watchdog stays for the handler.
// The watchdog arithmetic is static_assert-ed in web_guard.h; what the bench leg holds
// /api/state to is not: a degraded answer still comes within 1 s.
static void test_a_degraded_state_answer_fits_the_bench_bar() {
  TEST_ASSERT_TRUE(kWebLockWaitMs * kWebLockTriesPerRequest < 1000);
}

// One web request, one network allowance (review, 27 Oct): LAN /mcp memory.update by
// description embeds twice on the web task, and each exchange once had its own slot wait
// and 5 s budget, 10.5 s together, past the 8 s watchdog. Every exchange a request starts
// now draws on one allowance; the worst chain of exchanges is modelled here.
static void test_one_request_shares_one_network_allowance() {
  // The v4.6.0-era model, per exchange: two worst-case exchanges pass the watchdog.
  TEST_ASSERT_TRUE(kWebLockWaitMs + 2 * (kWebSlotWaitMs + kWebNetBudgetMs) > nimbus::cloud::kTaskWdtTimeoutMs);
  TEST_ASSERT_EQUAL_UINT32(kWebNetAllowanceMs, webNetLeftMs(0));
  TEST_ASSERT_EQUAL_UINT32(0, webNetLeftMs(kWebNetAllowanceMs));
  TEST_ASSERT_EQUAL_UINT32(0, webNetLeftMs(UINT32_MAX));
  for (uint32_t left = 0; left <= kWebNetAllowanceMs + 1000; left += 10) {
    const WebNetShare sh = webNetShare(left);
    const uint32_t cap = left < kWebNetBudgetMs ? left : kWebNetBudgetMs;
    TEST_ASSERT_EQUAL(cap >= kWebNetMinExchangeMs, sh.go);
    if (!sh.go) continue;
    TEST_ASSERT_TRUE(sh.handshakeS >= 1 && sh.connectMs > 0);
    TEST_ASSERT_TRUE(sh.connectMs + sh.handshakeS * 1000 <= sh.replyMs);
    TEST_ASSERT_TRUE(sh.replyMs <= cap);
    const uint32_t slot = slotWaitMs(true, 12000, left);
    TEST_ASSERT_TRUE(slot <= kWebSlotWaitMs && left - slot >= kWebNetMinExchangeMs);
  }
  TEST_ASSERT_EQUAL_UINT32(0, slotWaitMs(true, 12000, kWebNetMinExchangeMs - 1));
  TEST_ASSERT_EQUAL_UINT32(12000, slotWaitMs(false, 12000, 0));   // a turn: what it asked
  // The worst chain: each exchange waits its whole slot share and runs to its reply
  // deadline, as many as the allowance lets start. The second is refused.
  uint32_t elapsed = 0, exchanges = 0;
  for (int ask = 0; ask < 5; ++ask) {
    const uint32_t left = webNetLeftMs(elapsed);
    if (left < kWebNetMinExchangeMs) break;
    elapsed += slotWaitMs(true, 12000, left);
    const WebNetShare sh = webNetShare(webNetLeftMs(elapsed));
    if (!sh.go) break;
    elapsed += sh.replyMs;
    ++exchanges;
  }
  TEST_ASSERT_EQUAL_UINT32(1, exchanges);
  TEST_ASSERT_TRUE(elapsed <= kWebNetAllowanceMs);
  TEST_ASSERT_TRUE(kWebLockWaitMs * kWebLockTriesPerRequest + elapsed <= nimbus::cloud::kTaskWdtTimeoutMs * 3 / 4);
  // Two quick exchanges (the usual memory.update by description) both run.
  elapsed = 0;
  for (int i = 0; i < 2; ++i) {
    const WebNetShare sh = webNetShare(webNetLeftMs(elapsed));
    TEST_ASSERT_TRUE(sh.go);
    elapsed += 800;
  }
}

static void test_the_tools_that_wait_on_the_network() {
  for (const char* t : {"image.generate", "image.look", "reply.telegram", "reply.speak", "web.search",
                        "device.control", "mcp.notion.search", "mcp.x.y"})
    TEST_ASSERT_TRUE_MESSAGE(toolWaitsOnNetwork(t), t);
  for (const char* t : {"memory.write", "memory.search", "files.read", "device.status", "", "image.lookx", "image",
                        "mcp.", "mcp", "xmcp.a.b"})
    TEST_ASSERT_FALSE_MESSAGE(toolWaitsOnNetwork(t), t);
}

static void test_busy_answers_are_json() {
  JsonDocument d;
  TEST_ASSERT_TRUE(deserializeJson(d, cardBusyJson()) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("busy", d["error"] | "");
  JsonDocument e;
  TEST_ASSERT_TRUE(deserializeJson(e, turnOnlyToolRpcError("image.look", "7")) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("2.0", e["jsonrpc"] | "");
  TEST_ASSERT_EQUAL_INT(7, e["id"] | 0);   // the request's id echoed
  TEST_ASSERT_EQUAL_INT(-32003, e["error"]["code"] | 0);
  TEST_ASSERT_TRUE(std::string(e["error"]["message"] | "").find("image.look") != std::string::npos);
  // A hostile name (an "mcp." prefix passes the tool check) stays inside its string.
  JsonDocument h;
  const std::string hostile = "mcp.x\",\"id\":99,\"x\":\"";
  TEST_ASSERT_TRUE(deserializeJson(h, turnOnlyToolRpcError(hostile, "null")) == DeserializationError::Ok);
  TEST_ASSERT_TRUE(h["id"].isNull());
  TEST_ASSERT_FALSE(h["x"].is<const char*>());
  TEST_ASSERT_TRUE(std::string(h["error"]["message"] | "").rfind(hostile, 0) == 0);
  JsonDocument s;   // a string id survives; garbage reads as null
  TEST_ASSERT_TRUE(deserializeJson(s, turnOnlyToolRpcError("web.search", "\"abc\"")) == DeserializationError::Ok);
  TEST_ASSERT_EQUAL_STRING("abc", s["id"] | "");
  TEST_ASSERT_TRUE(deserializeJson(s, turnOnlyToolRpcError("web.search", "{bad")) == DeserializationError::Ok);
  TEST_ASSERT_TRUE(s["id"].isNull());
}

// ---- source reading -------------------------------------------------------------------

static std::string readSource(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// The code without comments; string and char literals become "" (keepStrings: kept
// whole, for the tool names). Each skip returns the index of the last char consumed.
static size_t skipQuoted(const std::string& s, size_t i, std::string& out, bool keep) {
  const char q = s[i];
  size_t j = i + 1;
  while (j < s.size() && s[j] != q) j += (s[j] == '\\') ? 2 : 1;
  if (keep) out.append(s, i, j - i + 1);
  else out.append(2, q);
  return j;
}
static size_t skipRawString(const std::string& s, size_t i, std::string& out, bool keep) {
  const size_t open = s.find('(', i + 2);
  const std::string close = ")" + s.substr(i + 2, open - (i + 2)) + "\"";
  const size_t end = s.find(close, open) + close.size() - 1;
  if (keep) out.append(s, i, end - i + 1);
  else out += "\"\"";
  return end;
}
// A digit separator (1'000) or an identifier ending in R is not a literal's start.
static bool prevIsIdent(const std::string& s, size_t i) { return i > 0 && isalnum((unsigned char)s[i - 1]); }

static std::string stripped(const std::string& s, bool keepStrings) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i], n = i + 1 < s.size() ? s[i + 1] : '\0';
    if (c == '/' && n == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) break;
      out += '\n';
    } else if (c == '/' && n == '*') {
      const size_t e = s.find("*/", i + 2);
      i = e == std::string::npos ? s.size() : e + 1;
    } else if (c == 'R' && n == '"' && !prevIsIdent(s, i)) {
      i = skipRawString(s, i, out, keepStrings);
    } else if (c == '"' || (c == '\'' && !prevIsIdent(s, i))) {
      i = skipQuoted(s, i, out, keepStrings);
    } else {
      out += c;
    }
  }
  return out;
}
static std::string codeOnly(const std::string& s) { return stripped(s, false); }

static bool identChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// The identifier that ends right before `end` (spaces skipped), or "".
static std::string identBefore(const std::string& s, size_t end) {
  size_t e = end;
  while (e > 0 && isspace((unsigned char)s[e - 1])) --e;
  size_t b = e;
  while (b > 0 && identChar(s[b - 1])) --b;
  return s.substr(b, e - b);
}

// Index of the bracket matching the closing one at `close` (')' or ']'), or npos.
static size_t openOf(const std::string& code, size_t close) {
  const char cl = code[close], op = cl == ')' ? '(' : '[';
  int depth = 0;
  for (size_t r = close + 1; r-- > 0;) {
    if (code[r] == cl) ++depth;
    if (code[r] == op && --depth == 0) return r;
  }
  return std::string::npos;
}

// Index one past the '}' matching the '{' at `brace`.
static size_t closeOf(const std::string& code, size_t brace) {
  int d = 0;
  for (size_t j = brace; j < code.size(); ++j) {
    if (code[j] == '{') ++d;
    if (code[j] == '}' && --d == 0) return j + 1;
  }
  return code.size();
}

// ---- callables: functions and lambdas -------------------------------------------------

struct Callable {
  std::string name;   // a function's name, or "lambda@<line>"
  size_t open;        // index of the body's '{'
  size_t close;       // one past its '}'
};

// The ')' a body's '{' follows: skipping a "-> type" and the const/override/noexcept/
// mutable qualifiers. npos when the brace opens something else.
static size_t parenBeforeBrace(const std::string& code, size_t brace) {
  static const std::regex kTail(R"(\)\s*(const|override|noexcept|mutable)?\s*(->\s*[\w:<>,\s\*&]+)?\s*$)");
  const size_t from = brace > 120 ? brace - 120 : 0;
  std::smatch m;
  const std::string head = code.substr(from, brace - from);
  if (!std::regex_search(head, m, kTail)) return std::string::npos;
  return from + (size_t)m.position(0);
}

static size_t lineOf(const std::string& code, size_t at) {
  size_t n = 1;
  for (size_t i = 0; i < at; ++i) n += code[i] == '\n';
  return n;
}

// Every function body and every lambda body in `code`.
static std::vector<Callable> callables(const std::string& code) {
  static const std::set<std::string> kKeywords = {"if", "for", "while", "switch", "catch", "return", "sizeof"};
  std::vector<Callable> out;
  for (size_t i = 0; i < code.size(); ++i) {
    if (code[i] != '{') continue;
    const size_t paren = parenBeforeBrace(code, i);
    if (paren == std::string::npos) continue;
    const size_t open = openOf(code, paren);
    if (open == std::string::npos) continue;
    size_t p = open;
    while (p > 0 && isspace((unsigned char)code[p - 1])) --p;
    if (p > 0 && code[p - 1] == ']') {
      out.push_back({"lambda@" + std::to_string(lineOf(code, i)), i, closeOf(code, i)});
      continue;
    }
    const std::string name = identBefore(code, open);
    if (name.empty() || kKeywords.count(name)) continue;
    out.push_back({name, i, closeOf(code, i)});
  }
  return out;
}

// A callable's own text: its body with every nested callable's body blanked (a filler
// or a callback created inside runs later, on its own terms). Offsets are kept.
static std::string ownText(const std::string& code, const Callable& c, const std::vector<Callable>& all) {
  std::string t = code.substr(c.open + 1, c.close - c.open - 2);
  for (const Callable& o : all) {
    if (o.open <= c.open || o.close > c.close) continue;
    for (size_t k = o.open + 1; k + 1 < o.close; ++k) t[k - c.open - 1] = ' ';
  }
  return t;
}

// ---- the card-lock universe -----------------------------------------------------------

// One source file: its functions' bodies by name (overloads joined) and the namespaces
// it declares (the last segment of each).
struct Unit {
  std::string path;
  std::string code;
  std::map<std::string, std::string> fns;
  std::set<std::string> namespaces;
};

static Unit unitFrom(const std::string& path, const std::string& raw) {
  Unit u;
  u.path = path;
  u.code = codeOnly(raw);
  const std::vector<Callable> cs = callables(u.code);
  for (const Callable& c : cs)
    if (c.name.rfind("lambda@", 0) != 0) u.fns[c.name] += u.code.substr(c.open + 1, c.close - c.open - 2);
  static const std::regex kNs(R"(namespace\s+([\w:]+)\s*\{)");
  for (std::sregex_iterator it(u.code.begin(), u.code.end(), kNs), end; it != end; ++it) {
    const std::string full = (*it)[1];
    const size_t c = full.rfind("::");
    u.namespaces.insert(c == std::string::npos ? full : full.substr(c + 2));
  }
  return u;
}

static Unit loadUnit(const std::string& path) { return unitFrom(path, readSource(path)); }

// (path, source text) of every unit; the gate's own test passes synthetic sources.
using Sources = std::vector<std::pair<std::string, std::string>>;

static Sources readAll(const std::vector<std::string>& paths) {
  Sources out;
  for (const std::string& p : paths) out.push_back({p, readSource(p)});
  return out;
}

// Position of the first blocking card-lock take in `text` (npos when none). Outside the
// memory subsystem a bare `Lock` may be another mutex (music, loops), so only the
// qualified names count there.
static size_t firstBlockingTake(const std::string& text, bool memoryUnit) {
  size_t best = std::string::npos;
  auto take = [&](size_t at) { best = std::min(best, at); };
  for (const char* p : {"memory::Lock ", "memory::Lock{", "mem::Lock ", "memory::lock()", "mem::lock()"}) {
    const size_t at = text.find(p);
    if (at != std::string::npos) take(at);
  }
  if (!memoryUnit) return best;
  static const std::regex kBare(R"((^|[^\w:])Lock\s+\w+\s*[;({])");
  static const std::regex kCall(R"((^|[^\w:.>])lock\(\))");
  std::smatch m;
  if (std::regex_search(text, m, kBare)) take((size_t)m.position(0));
  if (std::regex_search(text, m, kCall)) take((size_t)m.position(0));
  return best;
}

static size_t firstBoundedTake(const std::string& text) {
  size_t best = std::string::npos;
  for (const char* p : {"WebLock", "LoopLock", "tryLock("}) {
    const size_t at = text.find(p);
    if (at != std::string::npos) best = std::min(best, at);
  }
  return best;
}

struct CallSite {
  std::string ns;     // the last qualifier segment, "" when unqualified
  std::string name;
  size_t at;
};

static std::vector<CallSite> callSites(const std::string& text) {
  static const std::regex kCall(R"(((?:\w+::)*)(\w+)\s*\()");
  std::vector<CallSite> out;
  for (std::sregex_iterator it(text.begin(), text.end(), kCall), end; it != end; ++it) {
    const size_t at = (size_t)it->position(0);
    if (at > 0 && (text[at - 1] == '.' || (at > 1 && text[at - 1] == '>' && text[at - 2] == '-'))) continue;
    std::string q = (*it)[1];
    if (!q.empty()) q.resize(q.size() - 2);
    const size_t c = q.rfind("::");
    out.push_back({c == std::string::npos ? q : q.substr(c + 2), (*it)[2], at});
  }
  return out;
}

class Universe {
 public:
  explicit Universe(const Sources& sources) {
    for (const auto& src : sources) units_.push_back(unitFrom(src.first, src.second));
    for (size_t i = 0; i < units_.size(); ++i)
      for (const std::string& ns : units_[i].namespaces) byNs_[ns].push_back(i);
    byNs_["mem"] = byNs_["memory"];   // the web files' alias for agent::memory
    solve();
  }
  const Unit& unit(size_t i) const { return units_[i]; }
  size_t unitOf(const std::string& path) const {
    for (size_t i = 0; i < units_.size(); ++i)
      if (units_[i].path == path) return i;
    return units_.size();
  }
  bool takesCard(size_t unit, const std::string& fn) const { return taking_.count({unit, fn}) != 0; }

  // Where `text` (in unit `u`) first takes the card lock without a timeout: a blocking
  // take or a call of a function that does. npos when it never does; `what` names it.
  size_t firstCardTake(size_t u, const std::string& text, std::string& what) const {
    const Unit& un = units_[u];
    return firstTake(u, firstBlockingTake(text, memoryUnit(un)), callSites(text), what);
  }

 private:
  static bool memoryUnit(const Unit& u) { return u.path.find("memory_subsystem.cpp") != std::string::npos; }

  // One function's facts, read once: its first blocking take, its first bounded take
  // and its call sites.
  struct FnInfo {
    size_t direct;
    size_t bounded;
    std::vector<CallSite> calls;
  };

  size_t firstTake(size_t u, size_t direct, const std::vector<CallSite>& calls, std::string& what) const {
    size_t at = direct;
    if (at != std::string::npos) what = "a blocking card-lock take";
    for (const CallSite& c : calls) {
      if (c.at >= at) continue;
      for (size_t t : targets(u, c))
        if (takesCard(t, c.name)) {
          at = c.at;
          what = c.name + " (" + units_[t].path + ")";
        }
    }
    return at;
  }

  std::vector<size_t> targets(size_t from, const CallSite& c) const {
    std::vector<size_t> out;
    if (c.ns.empty()) {
      if (units_[from].fns.count(c.name)) out.push_back(from);
      return out;
    }
    auto it = byNs_.find(c.ns);
    if (it == byNs_.end()) return out;
    for (size_t t : it->second)
      if (units_[t].fns.count(c.name)) out.push_back(t);
    return out;
  }

  // A function takes the card lock when a blocking take or a call of such a function
  // comes before its first bounded take (a WebLock, LoopLock or tryLock opening it).
  void solve() {
    std::map<std::pair<size_t, std::string>, FnInfo> info;
    for (size_t u = 0; u < units_.size(); ++u)
      for (const auto& kv : units_[u].fns)
        info[{u, kv.first}] = FnInfo{firstBlockingTake(kv.second, memoryUnit(units_[u])),
                                     firstBoundedTake(kv.second), callSites(kv.second)};
    for (bool changed = true; changed;) {
      changed = false;
      for (const auto& kv : info) {
        if (taking_.count(kv.first)) continue;
        std::string what;
        const size_t at = firstTake(kv.first.first, kv.second.direct, kv.second.calls, what);
        if (at == std::string::npos || kv.second.bounded < at) continue;
        taking_.insert(kv.first);
        changed = true;
      }
    }
  }

  std::vector<Unit> units_;
  std::map<std::string, std::vector<size_t>> byNs_;
  std::set<std::pair<size_t, std::string>> taking_;
};

// ---- the web gate -----------------------------------------------------------------------

// Every source the web files can reach by name.
static std::vector<std::string> universePaths() {
  std::vector<std::string> out = {
      "src/agent/memory_subsystem.cpp", "src/agent/files_subsystem.cpp", "src/agent/skills.cpp",
      "src/agent/prefs_subsystem.cpp", "src/agent/safety_activity_store.cpp", "src/agent/tg_archive.cpp",
      "src/agent/loops_subsystem.cpp", "src/agent/orchestrator.cpp", "src/agent/telegram.cpp",
      "src/agent/store.cpp", "src/agent/health.cpp", "src/agent/dream_subsystem.cpp",
      "src/agent/connectors.cpp", "src/agent/spend_ceiling_store.cpp", "src/agent/provider_verify.cpp",
      "src/agent/model_refresh.cpp", "src/agent/device_log_tool.cpp", "src/agent/adapters/embeddings.cpp",
      "src/agent/adapters/image_vision.cpp", "src/agent/adapters/tts_voices.cpp", "src/sys/errlog_fs.cpp",
      "src/sys/crash_report.cpp", "src/sys/ota_update.cpp", "src/sfx/music.cpp", "src/sfx/music_store.cpp",
      "src/net/web_admission.cpp"};
  for (const std::string& w : std::vector<std::string>{"webui", "web_memory", "web_files", "web_prefs", "web_skills",
                                                       "web_tg_archive", "errlog_routes", "safety_routes"})
    out.push_back("src/net/" + w + ".cpp");
  return out;
}

static bool isWebFile(const std::string& path) {
  return path.rfind("src/net/", 0) == 0;
}

// Web-file functions that do not run on the web task: setup (beginWeb registers the
// routes and starts the memory subsystem) and the main loop's drain (loopWeb, whose
// card work the loop guard covers).
static const std::set<std::string> kNotOnWebTask = {"beginWeb", "loopWeb"};

// Each web callable that reaches the card lock, and whether it opens with a WebLock first.
struct WebViolation {
  std::string where;
  std::string what;
};

static std::vector<WebViolation> webViolations(const Universe& uni, size_t u) {
  std::vector<WebViolation> out;
  const Unit& un = uni.unit(u);
  const std::vector<Callable> cs = callables(un.code);
  for (const Callable& c : cs) {
    if (kNotOnWebTask.count(c.name)) continue;
    const std::string own = ownText(un.code, c, cs);
    std::string what;
    const size_t at = uni.firstCardTake(u, own, what);
    if (at == std::string::npos) continue;
    const size_t web = own.find("WebLock");
    if (web != std::string::npos && web < at) continue;
    out.push_back({un.path + ":" + c.name + " (line " + std::to_string(lineOf(un.code, c.open)) + ")", what});
  }
  return out;
}

static void test_no_web_file_takes_the_card_lock_without_a_timeout() {
  for (const std::string& p : universePaths()) {
    if (!isWebFile(p)) continue;
    const std::string code = codeOnly(readSource(p));
    TEST_ASSERT_FALSE_MESSAGE(code.empty(), p.c_str());
    for (const char* bad : {"memory::Lock ", "mem::Lock ", "memory::lock()", "portMAX_DELAY", "LoopLock"})
      TEST_ASSERT_TRUE_MESSAGE(code.find(bad) == std::string::npos,
                               (p + ": " + bad + " on the web task; use agent::memory::WebLock").c_str());
  }
}

static void test_every_web_path_to_the_card_lock_opens_with_a_web_lock() {
  const Universe uni(readAll(universePaths()));
  // The analysis really sees the lock: the accessors /api/state and the crash path use.
  const size_t files = uni.unitOf("src/agent/files_subsystem.cpp");
  const size_t mem = uni.unitOf("src/agent/memory_subsystem.cpp");
  TEST_ASSERT_TRUE(uni.takesCard(files, "storageTruth"));
  TEST_ASSERT_TRUE(uni.takesCard(mem, "handleMcp"));
  TEST_ASSERT_TRUE(uni.takesCard(uni.unitOf("src/sys/errlog_fs.cpp"), "readRecent"));
  TEST_ASSERT_FALSE(uni.takesCard(mem, "stats"));   // opens with a LoopLock: bounded on the web task
  size_t checked = 0;
  std::string report;
  for (const std::string& p : universePaths()) {
    if (!isWebFile(p)) continue;
    ++checked;
    for (const WebViolation& v : webViolations(uni, uni.unitOf(p)))
      report += "\n  " + v.where + " reaches the card lock through " + v.what + " without a WebLock first";
  }
  TEST_ASSERT_TRUE(checked >= 8);
  TEST_ASSERT_TRUE_MESSAGE(report.empty(), ("web paths that wait (nimbus/sys/web_guard.h):" + report).c_str());
  TEST_ASSERT_FALSE(uni.takesCard(files, "abortWriteGen"));   // a WebLock, the abort owed when busy
}

// The gate itself: it fails the 26 Oct shape (a polled handler reaching a blocking
// accessor), sees through lambdas, fillers and helpers, and passes the fixed shape.
static void test_gate_rejects_the_26_oct_shape() {
  const std::string agent =
      "namespace agent::files {\nStorageTruth storageTruth() {\n  memory::Lock g;\n  return t;\n}\n"
      "bool safeRead() {\n  memory::WebLock g;\n  if (!g) return false;\n  return storageTruth().present;\n}\n}\n";
  const std::string bad =
      "namespace nimbus::net {\nstatic void buildState(String& out) {\n  auto t = agent::files::storageTruth();\n}\n"
      "void beginWeb() {\n  s.on(\"/api/state\", HTTP_GET, [](AsyncWebServerRequest* r) {\n"
      "    String o; buildState(o);\n  });\n"
      "  s.on(\"/x\", HTTP_GET, [](AsyncWebServerRequest* r) {\n    memory::WebLock card;\n"
      "    if (!card) return;\n    auto fill = [](uint8_t* b, size_t n, size_t) -> size_t {\n"
      "      return agent::files::storageTruth().files;\n    };\n  });\n}\n}\n";
  const Universe uni({{"src/agent/fake.cpp", agent}, {"src/net/fake.cpp", bad}});
  TEST_ASSERT_TRUE(uni.takesCard(0, "storageTruth"));
  TEST_ASSERT_FALSE(uni.takesCard(0, "safeRead"));
  const std::vector<WebViolation> v = webViolations(uni, 1);
  std::set<std::string> where;
  for (const WebViolation& x : v) where.insert(x.where.substr(x.where.find(':') + 1, x.where.find(' ') - x.where.find(':') - 1));
  TEST_ASSERT_TRUE(where.count("buildState"));     // the 26 Oct accessor call
  TEST_ASSERT_TRUE(where.count("lambda@6"));       // the polled handler, through buildState
  TEST_ASSERT_TRUE(where.count("lambda@12"));      // the filler: it runs without the handler's lock
  TEST_ASSERT_FALSE(where.count("lambda@9"));      // the handler that opens with a WebLock
  TEST_ASSERT_FALSE(where.count("beginWeb"));      // setup, not the web task
}

// ---- the tool dispatch gate --------------------------------------------------------------

// Calls that wait on the network from inside a tool handler.
static const char* const kNetworkSeams[] = {
    "vision::look(", "vision::read(", "describeImage(", "generateToBuffer(", "deps.search(",
    "tavily::search(", "sendToChat(", "speakOnDevice(", "embeddings::embed(", "embeddings::embedWith(",
    "deviceTransport(", "acquireWork(", "voicereply::deliver(", "applyDeviceElement("};

// Every `.add("name", ...)` registration in `text` (comments stripped, strings kept):
// the tool name and its argument text.
static std::vector<std::pair<std::string, std::string>> registrations(const std::string& text) {
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t at = text.find(".add("); at != std::string::npos; at = text.find(".add(", at + 1)) {
    size_t q = at + 5;
    while (q < text.size() && isspace((unsigned char)text[q])) ++q;
    if (q >= text.size() || text[q] != '"') continue;   // not a literal tool name
    ++q;
    const size_t qe = text.find('"', q);
    int depth = 0;
    size_t j = at + 4;
    for (; j < text.size(); ++j) {
      if (text[j] == '(') ++depth;
      if (text[j] == ')' && --depth == 0) break;
    }
    out.push_back({text.substr(q, qe - q), text.substr(at, j - at)});
  }
  return out;
}

static std::string networkSeamIn(const std::string& text) {
  for (const char* s : kNetworkSeams)
    if (text.find(s) != std::string::npos) return s;
  return std::string();
}

static void test_every_network_tool_runs_outside_the_dispatch_lock() {
  const char* const kToolFiles[] = {"src/agent/memory_subsystem.cpp", "src/agent/files_subsystem.cpp",
                                    "src/agent/pdf_tool.cpp",         "src/agent/orchestrator.cpp",
                                    "src/agent/loops_subsystem.cpp",  "src/agent/skills.cpp",
                                    "src/agent/model_refresh.cpp",    "src/agent/device_log_tool.cpp",
                                    "src/main.cpp",                   "src/sfx/music.cpp",
                                    "lib/harness/src/websearch.cpp"};
  size_t tools = 0, network = 0;
  for (const char* p : kToolFiles) {
    const std::string text = stripped(readSource(p), true);
    TEST_ASSERT_FALSE_MESSAGE(text.empty(), p);
    for (const auto& reg : registrations(text)) {
      ++tools;
      const std::string seam = networkSeamIn(reg.second);
      if (seam.empty()) continue;
      ++network;
      TEST_ASSERT_TRUE_MESSAGE(toolWaitsOnNetwork(reg.first),
                               (reg.first + " (" + p + ") calls " + seam +
                                " under the dispatcher's card lock: list it in toolWaitsOnNetwork")
                                   .c_str());
    }
  }
  TEST_ASSERT_TRUE(tools >= 30);    // the scan really saw the registry
  TEST_ASSERT_TRUE(network >= 6);   // image.look, image.generate, both reply tools, web.search, device.control

  // The dispatcher runs exactly those outside its lock, and refuses them on the web task.
  const std::map<std::string, std::string> mem = loadUnit("src/agent/memory_subsystem.cpp").fns;
  TEST_ASSERT_TRUE(mem.count("locksItself") && mem.at("locksItself").find("toolWaitsOnNetwork(") != std::string::npos);
  const std::string& mcp = mem.at("handleMcp");
  const size_t unlocked = mcp.find("dispatchUnlocked(");
  TEST_ASSERT_TRUE(unlocked != std::string::npos && unlocked < mcp.find("Lock g;"));
  TEST_ASSERT_TRUE(mem.count("dispatchUnlocked"));
  const std::string& du = mem.at("dispatchUnlocked");
  TEST_ASSERT_TRUE(du.find("onWebTask()") != std::string::npos && du.find("turnOnlyToolRpcError(") != std::string::npos);
  // The memory tools' embedding (inside the dispatch) gives the card lock up for its request.
  const std::string& raw = mem.at("embedRaw");
  TEST_ASSERT_TRUE(raw.find("CardRelease") != std::string::npos);
  TEST_ASSERT_TRUE(raw.find("CardRelease") < raw.find("embeddings::embed("));
}

// True when a block holding `text[lockAt]` is still open at `pos` (brace depth relative
// to the lock never drops below its own between the two).
static bool blockOpenAt(const std::string& text, size_t lockAt, size_t pos) {
  int depth = 0;
  for (size_t i = lockAt; i < pos && i < text.size(); ++i) {
    if (text[i] == '{') ++depth;
    if (text[i] == '}' && --depth < 0) return false;
  }
  return true;
}

// A connected tool server's tools (registered at run time as "mcp.<slug>.<tool>", so
// the literal scan above cannot see them) run outside the dispatcher's lock, and their
// handler holds the card lock only around its slot, never across the request.
static void test_tool_server_calls_never_hold_the_card_lock() {
  TEST_ASSERT_TRUE(toolWaitsOnNetwork("mcp.server.tool"));
  const Unit con = loadUnit("src/agent/connectors.cpp");
  TEST_ASSERT_TRUE(con.fns.count("callTool"));
  const std::string& body = con.fns.at("callTool");
  const size_t call = body.find("exchange(");
  TEST_ASSERT_TRUE(call != std::string::npos);
  for (size_t at = body.find("memory::Lock"); at != std::string::npos && at < call; at = body.find("memory::Lock", at + 1))
    TEST_ASSERT_FALSE_MESSAGE(blockOpenAt(body, at, call), "callTool holds the card lock across exchange()");
  // The counter-test: the v4.6.0 shape (the lock taken at the top of the handler) fails.
  const std::string old = "{\n  agent::memory::Lock lk;\n  exchange(q, resp);\n}";
  TEST_ASSERT_TRUE(blockOpenAt(old, old.find("memory::Lock"), old.find("exchange(")));
}

// Every work TLS session passes the network-under-lock check and the web slot cap.
static void test_the_tls_arbiter_checks_every_request() {
  const Unit arb = loadUnit("src/sys/tls_arbiter.cpp");
  TEST_ASSERT_TRUE(arb.fns.count("acquireWork"));
  const std::string& a = arb.fns.at("acquireWork");
  TEST_ASSERT_TRUE(a.find("noteNetworkStart(") != std::string::npos);
  TEST_ASSERT_TRUE(a.find("slotWaitMs(") != std::string::npos);
  TEST_ASSERT_TRUE(a.find("noteNetworkStart(") < a.find("xSemaphoreTake("));
}

// The web task's exchanges draw on the request's allowance: the server opens a clock
// around every handler, the arbiter's slot wait and the embedding's timers come out of
// what is left, and a request with too little left is refused before any socket opens.
static void test_every_web_exchange_draws_on_the_request_allowance() {
  const Unit arb = loadUnit("src/sys/tls_arbiter.cpp");
  const std::string& a = arb.fns.at("acquireWork");
  TEST_ASSERT_TRUE(a.find("webNetLeftNow()") != std::string::npos);
  TEST_ASSERT_TRUE(a.find("kWebNetMinExchangeMs") < a.find("xSemaphoreTake("));
  TEST_ASSERT_TRUE(a.find("slotWaitMs(web, timeoutMs, left)") != std::string::npos);
  const Unit emb = loadUnit("src/agent/adapters/embeddings.cpp");
  TEST_ASSERT_TRUE(emb.fns.count("embedWith"));
  const std::string& e = emb.fns.at("embedWith");
  const size_t share = e.find("webNetShare(");
  TEST_ASSERT_TRUE_MESSAGE(share != std::string::npos, "the embedding takes its own per-exchange budget");
  TEST_ASSERT_TRUE(e.find("webNetLeftNow()") != std::string::npos && share < e.find("client.connect("));
  TEST_ASSERT_TRUE(e.find("share.connectMs") != std::string::npos && e.find("share.handshakeS") != std::string::npos);
  TEST_ASSERT_TRUE(e.find("startedMs + share.replyMs") != std::string::npos);
  TEST_ASSERT_TRUE(e.find("kWebNetHandshakeS") == std::string::npos && e.find("kWebNetConnectMs") == std::string::npos);
  const Unit ui = loadUnit("src/net/webui.cpp");
  const std::string& begin = ui.fns.at("beginWeb");
  const size_t mw = begin.find("addMiddleware(");
  TEST_ASSERT_TRUE_MESSAGE(mw != std::string::npos, "no network clock around the request handlers");
  TEST_ASSERT_TRUE(begin.find("openWebRequest()", mw) < begin.find("next()", mw));
  TEST_ASSERT_TRUE(begin.find("next()", mw) < begin.find("closeWebRequest()", mw));
  TEST_ASSERT_TRUE(mw < begin.find("s_server.begin()"));
  const std::string task = codeOnly(readSource("src/sys/web_task.h"));
  TEST_ASSERT_TRUE(task.find("webNetLeftMs(") != std::string::npos && task.find("onWebTask()") != std::string::npos);
}

// The async_tcp task's watchdog subscription is stated in the build, not left to a
// library default (DoD: confirm what the build intends; never disable it).
static void test_the_web_task_watchdog_is_stated_in_the_build() {
  const std::string ini = readSource("platformio.ini");
  TEST_ASSERT_TRUE(ini.find("-DCONFIG_ASYNC_TCP_USE_WDT=1") != std::string::npos);
  TEST_ASSERT_TRUE(ini.find("CONFIG_ASYNC_TCP_USE_WDT=0") == std::string::npos);
  const std::string web = codeOnly(readSource("src/net/webui.cpp"));
  TEST_ASSERT_TRUE(web.find("setWebTask(") != std::string::npos);
}

// The polled state page answers whole and quickly while the packet buffers run short:
// its body is built in one allocation (or not at all) and sent through the filler path,
// which counts what lwIP queued; the library's basic response re-queues bytes after a
// failed output (bench, 26 Oct: the page's start repeated past 5,608 B in a photo turn).
static void test_the_state_page_never_goes_out_cut_or_repeated() {
  const std::string web = stripped(readSource("src/net/webui.cpp"), true);
  for (const char* path : {"\"/api/state\", HTTP_GET", "\"/api/orch\", HTTP_GET"}) {
    const size_t reg = web.find(path);
    TEST_ASSERT_TRUE_MESSAGE(reg != std::string::npos, path);
    const size_t open = web.find('{', reg);
    const std::string route = web.substr(open, closeOf(web, open) - open);
    TEST_ASSERT_TRUE_MESSAGE(route.find("admission::sendJson(") != std::string::npos, path);
    TEST_ASSERT_TRUE_MESSAGE(route.find("beginResponse(") == std::string::npos, path);
  }
  const Unit ui = unitFrom("src/net/webui.cpp", readSource("src/net/webui.cpp"));
  TEST_ASSERT_TRUE(ui.fns.count("buildState") && ui.fns.count("buildOrchState") && ui.fns.count("serializeWhole"));
  const std::string& build = ui.fns.at("buildState");
  TEST_ASSERT_TRUE(build.find("serializeWhole(d, out)") != std::string::npos);
  TEST_ASSERT_TRUE(build.find("serializeJson(d, out)") == std::string::npos);
  TEST_ASSERT_TRUE(ui.fns.at("buildOrchState").find("serializeWhole(d, out)") != std::string::npos);
  // Whole or empty: ArduinoJson's String writer empties the string first (a reserve is
  // lost), so the length is checked against the measured size afterwards.
  const std::string& whole = ui.fns.at("serializeWhole");
  TEST_ASSERT_TRUE(whole.find("measureJson(") != std::string::npos && whole.find("out.length() != n") != std::string::npos);
  // The flash figures: LittleFS walks its whole file area to count them (half a second,
  // under its own lock), so the page measures them at most once a minute, after a quiet
  // spell with no turn.
  TEST_ASSERT_TRUE_MESSAGE(build.find("LittleFS.usedBytes(") == std::string::npos, "buildState walks LittleFS");
  TEST_ASSERT_TRUE(build.find("readFlashFacts(") != std::string::npos);
  TEST_ASSERT_TRUE(ui.fns.count("readFlashFacts"));
  const std::string& flash = ui.fns.at("readFlashFacts");
  TEST_ASSERT_TRUE(flash.find("kWebFlashFactsMaxAgeMs") != std::string::npos);
  TEST_ASSERT_TRUE(flash.find("kWebFlashFactsQuietMs") != std::string::npos);
  TEST_ASSERT_TRUE(flash.find("turnInFlight()") != std::string::npos);
  TEST_ASSERT_TRUE(flash.find("turnCount()") != std::string::npos);
  TEST_ASSERT_TRUE(flash.find("turnInFlight()") < flash.find("LittleFS.usedBytes("));
  TEST_ASSERT_TRUE(kWebFlashFactsMaxAgeMs >= 30000 && kWebFlashFactsQuietMs >= 5000);
  const Unit adm = loadUnit("src/net/web_admission.cpp");
  TEST_ASSERT_TRUE(adm.fns.count("sendJson"));
  const std::string& send = adm.fns.at("sendJson");
  TEST_ASSERT_TRUE(send.find("isEmpty()") != std::string::npos && send.find("sendBusy(") != std::string::npos);
  // The filler overload: a content length and a callback, no content string.
  TEST_ASSERT_TRUE(std::regex_search(send, std::regex(R"(beginResponse\(\s*(""|\w+)\s*,\s*len\s*,\s*\[)")));
}

// The state page's other locks: the spend limits and the usage ledger are written to
// flash under their own locks (1.1 s at a turn's end on the bench, 27 Oct), so the
// accessors the page reads take them with webTake (tried on the web task, the last
// answer served), and the page keeps those last answers.
static void test_the_state_page_never_waits_out_a_flash_write() {
  const std::string task = codeOnly(readSource("src/sys/web_task.h"));
  TEST_ASSERT_TRUE(task.find("kWebFillerWaitMs") != std::string::npos && task.find("try_lock()") != std::string::npos);
  const Unit spend = loadUnit("src/agent/spend_ceiling_store.cpp");
  for (const char* fn : {"stateJson", "setupText"}) {
    TEST_ASSERT_TRUE_MESSAGE(spend.fns.count(fn), fn);
    const std::string& b = spend.fns.at(fn);
    TEST_ASSERT_TRUE_MESSAGE(b.find("webTake(") != std::string::npos, fn);
    TEST_ASSERT_TRUE_MESSAGE(b.find("lock_guard") == std::string::npos, fn);
  }
  const Unit store = loadUnit("src/agent/store.cpp");
  TEST_ASSERT_TRUE(store.fns.count("usageLedgerSaved"));
  const std::string& saved = store.fns.at("usageLedgerSaved");
  TEST_ASSERT_TRUE(saved.find("webTake(") != std::string::npos && saved.find("lock_guard") == std::string::npos);
  const Unit ui = unitFrom("src/net/webui.cpp", readSource("src/net/webui.cpp"));
  TEST_ASSERT_TRUE(ui.fns.at("buildState").find("readSpendFacts()") != std::string::npos);
  TEST_ASSERT_TRUE(ui.fns.at("buildOrchState").find("readSpendFacts()") != std::string::npos);
  TEST_ASSERT_TRUE(ui.fns.at("buildOrchState").find("spend::stateJson()") == std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_slot_wait_is_capped_on_the_web_task_only);
  RUN_TEST(test_network_budget_is_capped_on_the_web_task_only);
  RUN_TEST(test_a_degraded_state_answer_fits_the_bench_bar);
  RUN_TEST(test_one_request_shares_one_network_allowance);
  RUN_TEST(test_the_tools_that_wait_on_the_network);
  RUN_TEST(test_busy_answers_are_json);
  RUN_TEST(test_no_web_file_takes_the_card_lock_without_a_timeout);
  RUN_TEST(test_every_web_path_to_the_card_lock_opens_with_a_web_lock);
  RUN_TEST(test_gate_rejects_the_26_oct_shape);
  RUN_TEST(test_every_network_tool_runs_outside_the_dispatch_lock);
  RUN_TEST(test_tool_server_calls_never_hold_the_card_lock);
  RUN_TEST(test_the_tls_arbiter_checks_every_request);
  RUN_TEST(test_every_web_exchange_draws_on_the_request_allowance);
  RUN_TEST(test_the_web_task_watchdog_is_stated_in_the_build);
  RUN_TEST(test_the_state_page_never_goes_out_cut_or_repeated);
  RUN_TEST(test_the_state_page_never_waits_out_a_flash_write);
  return UNITY_END();
}
