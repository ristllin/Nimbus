// Memory writes survive an embedding outage (lane P): the pending-embed queue,
// the park memo, the write seam's honest outcome codes and the drain.
//
// The class under test has two halves:
//   - a memory write never vanishes silently: every write ends stored, queued or
//     refused, and the words of a queued or refused one say "not stored";
//   - an embedding outage never costs a doomed call per turn: while parked, writes
//     and searches make no embed attempt at all.
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "nimbus/orch/billing_refusal.h"
#include "nimbus/orch/caps.h"
#include "nimbus/orch/memory_tools.h"
#include "nimbus/orch/pending_embed.h"

using namespace nimbus::orch;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;

void setUp() {}
void tearDown() {}

static bool has(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

// ---- a rig: engines, a fake embedder that counts its calls, a fake clock ------
struct Rig {
  VectorMemory vec;
  PendingEmbeds pending;
  EmbedPark park;
  ToolRegistry reg;
  MemoryContext ctx;
  uint32_t nowMs = 1000;
  uint32_t nowH = 500000;
  bool synced = true;
  std::string failWith;     // non-empty: the embedder fails with this raw token
  int embedCalls = 0;
  bool persistPendingOk = true;
  int persistPendingCalls = 0;
  std::string pendingBlob;  // what the "device storage" holds
  MemOpCounters ops;        // write outcome counters (lane M seam)
  static constexpr int kDims = 16;

  explicit Rig(bool bindQueue = true) {
    vec.configure(kDims);
    ctx.vec = &vec;
    ctx.embed = [this](const std::string& text, std::string& err) -> std::vector<int8_t> {
      embedCalls++;
      if (!failWith.empty()) { err = failWith; return {}; }
      err.clear();
      if (text.empty()) { err = "empty text"; return {}; }
      // One axis per topic, so recall geometry is controlled; anything else lands
      // on a neutral diagonal.
      // Distinct texts get near-orthogonal pseudo-random vectors (no accidental
      // near-duplicate merge), a topic word adds a strong axis of its own.
      std::vector<int8_t> v(kDims);
      std::mt19937 g((uint32_t)std::hash<std::string>()(text));
      for (auto& x : v) x = (int8_t)((int)(g() % 61) - 30);
      auto in = [&](const char* w) { return text.find(w) != std::string::npos; };
      if (in("locker")) v[0] = 127;
      if (in("coffee")) v[1] = 127;
      return v;
    };
    ctx.nowHours = [this] { return nowH; };
    ctx.ops = &ops;
    ctx.nowMs = [this] { return nowMs; };
    ctx.clockSynced = [this] { return synced; };
    ctx.embedProvider = [] { return std::string("openai"); };
    if (bindQueue) {
      ctx.pending = &pending;
      ctx.park = &park;
      ctx.persistPending = [this] {
        persistPendingCalls++;
        if (!persistPendingOk) return false;
        pendingBlob = pending.serialize();
        pending.markClean();
        return true;
      };
    }
    registerMemoryTools(reg, ctx);
  }

  ToolResult call(const char* tool, const std::string& json, const Principal& who) {
    JsonDocument d;
    deserializeJson(d, json);
    return reg.dispatch(tool, d.as<JsonObjectConst>(), who);
  }
  ToolResult write(const std::string& content, const Principal& who, const char* extra = "") {
    JsonDocument d;
    d["content"] = content;
    std::string j;
    serializeJson(d, j);
    if (extra[0]) j = j.substr(0, j.size() - 1) + "," + extra + "}";
    return call("memory.write", j, who);
  }
};

static Principal owner() { return principalForRole("1001", Role::Admin); }
static Principal member(const char* id = "2002") { return principalForRole(id, Role::User); }

// The drain deps a host test uses: principals by namespace from a map, the vector
// store "persisted" into a blob string.
struct DrainRig {
  std::map<std::string, Principal> principals;
  bool persistVecOk = true;
  std::string vecBlob;
  EmbedDrainDeps deps(Rig& r) {
    EmbedDrainDeps d;
    d.principalForNs = [this](const std::string& ns, Principal& who) {
      auto it = principals.find(ns);
      if (it == principals.end()) return false;
      who = it->second;
      return true;
    };
    d.persistVectors = [this, &r] {
      if (!persistVecOk) return false;
      vecBlob = r.vec.serialize();
      return true;
    };
    return d;
  }
};

static PendingEmbed entry(const std::string& content, const std::string& ns = kOwnerNs) {
  PendingEmbed e;
  e.content = content;
  unsigned long h = 5381;
  for (unsigned char c : content) h = ((h << 5) + h) + c;
  char buf[16];
  snprintf(buf, sizeof buf, "m%08lx", h & 0xffffffff);
  e.id = buf;
  e.ns = ns;
  e.createdAtHours = 400000;
  e.clockSynced = true;
  return e;
}

// ===== the queue engine ========================================================

static void test_queue_roundtrip_and_tolerant_deserialize() {
  std::mt19937 rng(18102026);
  for (int round = 0; round < 4; round++) {
    PendingEmbeds q;
    const int n = 10 + (int)(rng() % 54);
    for (int i = 0; i < n; i++) {
      PendingEmbed e = entry("fact " + std::to_string(round) + "-" + std::to_string(i) + " " +
                                 std::string(rng() % 300, 'a' + (char)(rng() % 26)),
                             (rng() % 3) ? kOwnerNs : ("chat:" + std::to_string(rng() % 3)));
      e.op = (rng() % 2) ? PendingOp::Write : PendingOp::Update;
      e.oldId = e.op == PendingOp::Update ? "mdeadbeef" : "";
      e.importance = (float)(rng() % 100) / 100.0f;
      e.permanent = rng() % 2;
      e.ttlName = (rng() % 2) ? "days" : "";
      e.attempts = (uint16_t)(rng() % 9);
      e.lastFail = (uint8_t)(rng() % 15);
      e.clockSynced = rng() % 2;
      q.enqueue(e);
    }
    const std::string blob = q.serialize();
    PendingEmbeds back;
    TEST_ASSERT_TRUE(back.deserialize(blob));
    TEST_ASSERT_EQUAL_STRING(blob.c_str(), back.serialize().c_str());   // byte-exact
    TEST_ASSERT_EQUAL_INT((int)q.size(), (int)back.size());
    // Every truncation prefix restores a clean prefix of the queue, never more,
    // never a mangled entry, never a crash.
    for (size_t cut = 0; cut < blob.size(); cut += 7) {
      PendingEmbeds t;
      const bool ok = t.deserialize(blob.substr(0, cut));
      TEST_ASSERT_FALSE(ok);
      TEST_ASSERT_TRUE(t.size() <= q.size());
      const auto all = q.getAll(), got = t.getAll();
      for (size_t i = 0; i < got.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(all[i].id.c_str(), got[i].id.c_str());
        TEST_ASSERT_EQUAL_STRING(all[i].content.c_str(), got[i].content.c_str());
      }
    }
  }
  // Garbage and an absent blob: an empty queue, false, no crash.
  PendingEmbeds g;
  TEST_ASSERT_FALSE(g.deserialize("not a queue at all"));
  TEST_ASSERT_EQUAL_INT(0, (int)g.size());
  TEST_ASSERT_FALSE(g.deserialize(std::string()));   // fresh device: no file
  TEST_ASSERT_EQUAL_INT(0, (int)g.size());
  TEST_ASSERT_FALSE(g.dirty());   // nothing to persist on a fresh device
  // A newer build's record with an extra trailing field still reads (the record
  // length prefix lets an older build skip what it does not know).
  PendingEmbeds one;
  one.enqueue(entry("my locker code is 4417"));
  std::string b = one.serialize();
  const size_t lenAt = 4 + 1 + 2;
  uint16_t len = (uint8_t)b[lenAt] | ((uint8_t)b[lenAt + 1] << 8);
  len += 3;
  b[lenAt] = (char)(len & 0xff);
  b[lenAt + 1] = (char)(len >> 8);
  b += "XYZ";
  PendingEmbeds newer;
  TEST_ASSERT_TRUE(newer.deserialize(b));
  TEST_ASSERT_EQUAL_STRING("my locker code is 4417", newer.front()->content.c_str());
}

static void test_queue_bounds_never_evict() {
  // 64 entries: the 65th is refused and the 64 are untouched.
  PendingEmbeds q;
  for (int i = 0; i < 64; i++)
    TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added,
                          (int)q.enqueue(entry("owner fact number " + std::to_string(i))));
  const std::string before = q.serialize();
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Full, (int)q.enqueue(entry("the 65th fact")));
  TEST_ASSERT_EQUAL_STRING(before.c_str(), q.serialize().c_str());

  // 48 KB: one byte under (exactly at) the cap is accepted, one over is refused.
  PendingEmbeds b;
  int i = 0;
  while (true) {
    PendingEmbed e = entry("long fact " + std::to_string(i++) + " " + std::string(1100, 'x'));
    if (b.bytes() + PendingEmbeds::entryBytes(e) > PendingEmbeds::kMaxBytes) break;
    TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added, (int)b.enqueue(e));
  }
  const size_t room = PendingEmbeds::kMaxBytes - b.bytes();
  PendingEmbed probe = entry("fit ");
  const size_t base = PendingEmbeds::entryBytes(probe);
  TEST_ASSERT_TRUE(room > base);
  PendingEmbed over = entry("fit " + std::string(room - base + 1, 'y'));
  PendingEmbed exact = entry("fit " + std::string(room - base, 'y'));
  TEST_ASSERT_EQUAL_INT((int)(PendingEmbeds::kMaxBytes + 1), (int)(b.bytes() + PendingEmbeds::entryBytes(over)));
  const std::string full = b.serialize();
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::BytesFull, (int)b.enqueue(over));
  TEST_ASSERT_EQUAL_STRING(full.c_str(), b.serialize().c_str());
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added, (int)b.enqueue(exact));
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::kMaxBytes, (int)b.bytes());

  // A member namespace stops at 16 while the owner is unaffected.
  PendingEmbeds m;
  for (int k = 0; k < 16; k++)
    TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added,
                          (int)m.enqueue(entry("member fact " + std::to_string(k), "chat:7")));
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::NsFull, (int)m.enqueue(entry("member 17", "chat:7")));
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added, (int)m.enqueue(entry("other member", "chat:8")));
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added, (int)m.enqueue(entry("owner fact")));
  // A fact longer than the model-memory cap is refused, never cut.
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::TooLong,
                        (int)m.enqueue(entry(std::string(kMemModelMax + 1, 'z'))));
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added,
                        (int)m.enqueue(entry(std::string(kMemModelMax, 'z'))));
}

static void test_coalesce_by_content_id() {
  PendingEmbeds q;
  PendingEmbed a = entry("my locker code is 4417");
  a.importance = 0.3f;
  PendingEmbed b = a;
  b.importance = 0.9f;
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Added, (int)q.enqueue(a));
  TEST_ASSERT_EQUAL_INT((int)PendingEmbeds::Add::Coalesced, (int)q.enqueue(b));
  TEST_ASSERT_EQUAL_INT(1, (int)q.size());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, q.front()->importance);
  a.importance = 0.1f;
  q.enqueue(a);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, q.front()->importance);   // the higher one wins
}

// ===== the park memo ===========================================================

static void test_park_policy_covers_every_embedfail_class() {
  // Walk the whole enum: a class with no rule falls to the default and is caught
  // here, because every class below is pinned to its intended rule.
  struct Row { EmbedFail k; ParkRule r; };
  const Row rows[] = {
      {EmbedFail::None, ParkRule::Clear},           {EmbedFail::NoKey, ParkRule::Long},
      {EmbedFail::KeyRejected, ParkRule::Long},     {EmbedFail::Busy, ParkRule::Retry},
      {EmbedFail::Unreachable, ParkRule::Backoff},  {EmbedFail::Timeout, ParkRule::Backoff},
      {EmbedFail::OutOfCredit, ParkRule::Long},     {EmbedFail::RateLimited, ParkRule::Backoff},
      {EmbedFail::RouteNotAllowed, ParkRule::Long}, {EmbedFail::ProviderError, ParkRule::Backoff},
      {EmbedFail::BadResponse, ParkRule::Backoff},  {EmbedFail::WidthMismatch, ParkRule::Long},
      {EmbedFail::NoModel, ParkRule::Long},         {EmbedFail::BadRequest, ParkRule::NoChange},
      {EmbedFail::Unknown, ParkRule::Backoff},
  };
  TEST_ASSERT_EQUAL_INT((int)EmbedFail::Unknown + 1, (int)(sizeof rows / sizeof rows[0]));
  for (int i = 0; i <= (int)EmbedFail::Unknown; i++) {
    TEST_ASSERT_EQUAL_INT(i, (int)rows[i].k);
    TEST_ASSERT_EQUAL_INT((int)rows[i].r, (int)parkRuleFor((EmbedFail)i));
    EmbedPark p;
    p.onResult((EmbedFail)i, 0);
    const ParkRule r = rows[i].r;
    if (r == ParkRule::Long) {
      TEST_ASSERT_TRUE(p.blocked(29u * 60u * 1000u));
      TEST_ASSERT_FALSE(p.blocked(30u * 60u * 1000u));   // then one probe
    } else if (r == ParkRule::Backoff) {
      TEST_ASSERT_TRUE(p.blocked(1000));
      TEST_ASSERT_FALSE(p.blocked(EmbedPark::kBackoffBaseMs));
    } else {
      TEST_ASSERT_FALSE(p.blocked(1));
    }
    p.onResult(EmbedFail::None, 2);
    TEST_ASSERT_FALSE(p.parked());   // any success clears it
  }
}

static void test_park_no_thrash() {
  // 24 simulated hours of 20 s drain ticks against a permanent credit refusal.
  EmbedPark p;
  int attempts = 0;
  for (uint32_t t = 0; t < 24u * 3600u * 1000u; t += 20000u) {
    if (p.blocked(t)) continue;
    attempts++;
    p.onResult(EmbedFail::OutOfCredit, t);
  }
  TEST_ASSERT_TRUE(attempts <= 48);
  TEST_ASSERT_TRUE(attempts >= 40);   // and it does keep probing
  // Transient errors back off but never stay parked for 30 minutes after a blip:
  // three rate limits then a success store within 4 simulated minutes.
  EmbedPark q;
  int fails = 0;
  uint32_t storedAt = 0;
  for (uint32_t t = 0; t < 10u * 60u * 1000u; t += 20000u) {
    if (q.blocked(t)) continue;
    if (fails < 3) { q.onResult(EmbedFail::RateLimited, t); fails++; continue; }
    q.onResult(EmbedFail::None, t);
    storedAt = t;
    break;
  }
  TEST_ASSERT_TRUE(storedAt > 0);
  TEST_ASSERT_TRUE(storedAt <= 4u * 60u * 1000u);
  // A permanent transient failure is capped at one try per 30 minutes too.
  EmbedPark u;
  int tries = 0;
  for (uint32_t t = 0; t < 24u * 3600u * 1000u; t += 20000u) {
    if (u.blocked(t)) continue;
    tries++;
    u.onResult(EmbedFail::Unreachable, t);
  }
  TEST_ASSERT_TRUE(tries <= 48 + 8);
  // The park episode starts once; repeats of the same refusal start nothing.
  EmbedPark e;
  TEST_ASSERT_TRUE(e.onResult(EmbedFail::OutOfCredit, 0));
  TEST_ASSERT_FALSE(e.onResult(EmbedFail::OutOfCredit, 30u * 60u * 1000u));
  e.onResult(EmbedFail::None, 1);
  TEST_ASSERT_TRUE(e.onResult(EmbedFail::OutOfCredit, 2));
}

// ===== classification: one billing table with the chat head ====================

static void test_billing_refusals_classify_as_out_of_credit() {
  // Every token in the shared table, inside the device seam's "HTTP <status> <code>".
  for (const char* tok : kBillingTokens) {
    for (int status : {402, 429, 400}) {
      std::string detail;
      const std::string raw = "HTTP " + std::to_string(status) + " " + tok;
      TEST_ASSERT_EQUAL_INT_MESSAGE((int)EmbedFail::OutOfCredit, (int)classifyEmbedFail(raw, detail),
                                    raw.c_str());
    }
  }
  struct Row { const char* raw; EmbedFail k; };
  const Row rows[] = {
      {"HTTP 429 credit_balance_exhausted", EmbedFail::OutOfCredit},   // Lumi, 13 Oct
      {"HTTP 429 insufficient_quota", EmbedFail::OutOfCredit},
      {"HTTP 402", EmbedFail::OutOfCredit},                           // every 402 is billing
      {"HTTP 402 something_new", EmbedFail::OutOfCredit},
      {"HTTP 429 ceiling_reached", EmbedFail::OutOfCredit},           // router monthly cap
      {"HTTP 402 funding_cap_reached", EmbedFail::OutOfCredit},
      // Counter-tests: a plain rate limit stays a rate limit; a 500 stays a 500.
      {"HTTP 429 rate_limited", EmbedFail::RateLimited},
      {"HTTP 429", EmbedFail::RateLimited},
      {"HTTP 500", EmbedFail::ProviderError},
      {"HTTP 403 endpoint_not_allowed", EmbedFail::RouteNotAllowed},
  };
  for (const auto& r : rows) {
    std::string detail;
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)r.k, (int)classifyEmbedFail(r.raw, detail), r.raw);
  }
  // The words name the provider (a known slug only) and the next step, and a
  // key-shaped string in the raw token never reaches them.
  Rig rig;
  rig.failWith = std::string("HTTP 429 credit_balance_exhausted Bearer s") + "k-LEAK12345678901234";
  ToolResult w = rig.write("my locker code is 4417", owner());
  TEST_ASSERT_TRUE(has(w.output, "out of embedding credit at openai"));
  TEST_ASSERT_TRUE(has(w.output, "billing page"));
  TEST_ASSERT_FALSE(has(w.output, "LEAK"));
  TEST_ASSERT_FALSE(has(w.output, "Bearer"));
  // A provider that is not a known slug is never named.
  Rig odd;
  odd.ctx.embedProvider = [] { return std::string("s") + "k-NOTASLUG"; };
  odd.reg = ToolRegistry();
  registerMemoryTools(odd.reg, odd.ctx);
  odd.failWith = "HTTP 402";
  ToolResult o = odd.write("my locker code is 4417", owner());
  TEST_ASSERT_TRUE(has(o.output, "out of embedding credit"));
  TEST_ASSERT_FALSE(has(o.output, "NOTASLUG"));
}

// ===== the write seam ==========================================================

static void test_write_during_outage_is_queued_for_every_failure_class() {
  const char* raws[] = {"no embeddings key for openai", "key rejected", "tls busy", "connect failed",
                        "timeout", "HTTP 429 credit_balance_exhausted", "HTTP 429 rate_limited",
                        "HTTP 403 endpoint_not_allowed", "HTTP 500", "parse: bad json",
                        "parse: dim mismatch: got 1024 expected 16", "no model", "brand new cause"};
  bool seen[(int)EmbedFail::Unknown + 1] = {};
  for (const char* raw : raws) {
    Rig r;
    r.failWith = raw;
    ToolResult w = r.write("my locker code is 4417", owner());
    std::string d;
    seen[(int)classifyEmbedFail(raw, d)] = true;
    TEST_ASSERT_TRUE_MESSAGE(w.success, raw);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(kMemOutcomeQueued, w.code.c_str(), raw);
    TEST_ASSERT_TRUE_MESSAGE(has(w.output, "not stored yet"), raw);
    TEST_ASSERT_TRUE_MESSAGE(has(w.output, "embeddings:"), raw);
    TEST_ASSERT_EQUAL_INT(0, r.vec.size());
    TEST_ASSERT_EQUAL_INT(1, (int)r.pending.size());
    TEST_ASSERT_TRUE(r.persistPendingCalls >= 1);   // durable before it answered
    // Recovery: the drain stores it under the id a direct write gives, and a
    // semantic search finds it.
    r.failWith.clear();
    r.park.clear();
    DrainRig dr;
    dr.principals[kOwnerNs] = owner();
    TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Stored, (int)drainOne(r.ctx, dr.deps(r)).step);
    TEST_ASSERT_EQUAL_INT(1, r.vec.size());
    TEST_ASSERT_EQUAL_INT(0, (int)r.pending.size());
    Rig direct;
    ToolResult dw = direct.write("my locker code is 4417", owner());
    TEST_ASSERT_EQUAL_STRING(direct.vec.getAll()[0].id.c_str(), r.vec.getAll()[0].id.c_str());
    TEST_ASSERT_EQUAL_STRING(kMemOutcomeStored, dw.code.c_str());
    TEST_ASSERT_TRUE(has(r.call("memory.search", R"({"query":"locker"})", owner()).output, "4417"));
  }
  // Every failure class except None and BadRequest was exercised above.
  for (int k = 1; k <= (int)EmbedFail::Unknown; k++)
    if (k != (int)EmbedFail::BadRequest) TEST_ASSERT_TRUE_MESSAGE(seen[k], std::to_string(k).c_str());
  // BadRequest (nothing to embed) is refused, not queued.
  Rig r;
  r.failWith = "empty text";
  ToolResult w = r.write("x", owner());
  TEST_ASSERT_FALSE(w.success);
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeRefused, w.code.c_str());
  TEST_ASSERT_TRUE(has(w.error, "not stored"));
}

// Lane M's dream row counts memories from these counters; a queued write (lane P)
// must land in ops.queued, not in added or failed, or the dream would claim a
// stored memory that is only parked in the queue.
static void test_counters_call_a_queued_write_queued() {
  Rig r;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  ToolResult w = r.write("my pool locker code is 4417", owner());
  TEST_ASSERT_TRUE(w.success);
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, w.code.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, r.ops.queued);
  TEST_ASSERT_EQUAL_UINT32(0, r.ops.added);
  TEST_ASSERT_EQUAL_UINT32(0, r.ops.updated);
  TEST_ASSERT_EQUAL_UINT32(0, r.ops.failed);
}

static void test_queue_full_or_storage_down_refuses_honestly() {
  Rig r;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  for (int i = 0; i < 64; i++)
    TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, r.write("fact " + std::to_string(i), owner()).code.c_str());
  ToolResult w = r.write("the 65th fact", owner());
  TEST_ASSERT_FALSE(w.success);
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeRefused, w.code.c_str());
  TEST_ASSERT_TRUE(has(w.error, "memory not stored: 64 memories are already waiting"));
  TEST_ASSERT_EQUAL_INT(64, (int)r.pending.size());
  // Never "queued" without a durable copy.
  Rig s;
  s.failWith = "HTTP 429 credit_balance_exhausted";
  s.persistPendingOk = false;
  ToolResult x = s.write("my locker code is 4417", owner());
  TEST_ASSERT_FALSE(x.success);
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeRefused, x.code.c_str());
  TEST_ASSERT_TRUE(has(x.error, "device storage unavailable"));
  TEST_ASSERT_EQUAL_INT(0, (int)s.pending.size());
  // Without the queue bound (an MCP-only context), today's refusal stands.
  Rig n(/*bindQueue=*/false);
  n.failWith = "HTTP 429 credit_balance_exhausted";
  ToolResult y = n.write("my locker code is 4417", owner());
  TEST_ASSERT_FALSE(y.success);
  TEST_ASSERT_TRUE(has(y.error, "memory not stored"));
}

static void test_parked_turn_spends_no_embed() {
  Rig r;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, r.write("first fact", owner()).code.c_str());
  TEST_ASSERT_EQUAL_INT(1, r.embedCalls);
  TEST_ASSERT_TRUE(r.park.parked());
  // N turns' worth of writes and searches while parked: zero embed attempts.
  for (int i = 0; i < 6; i++) {
    r.nowMs += 20000;
    TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, r.write("fact " + std::to_string(i), owner()).code.c_str());
    ToolResult s = r.call("memory.search", R"({"query":"fact"})", owner());
    TEST_ASSERT_TRUE(has(s.output, "keyword match, embeddings unavailable: out of embedding credit at openai"));
  }
  TEST_ASSERT_EQUAL_INT(1, r.embedCalls);
  TEST_ASSERT_EQUAL_INT(7, (int)r.pending.size());
  // The drain is parked too: no network.
  DrainRig dr;
  dr.principals[kOwnerNs] = owner();
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Parked, (int)drainOne(r.ctx, dr.deps(r)).step);
  TEST_ASSERT_EQUAL_INT(1, r.embedCalls);
  // After the park time one probe goes out; a success clears the park and the
  // queue drains one per step.
  r.nowMs += 31u * 60u * 1000u;
  r.failWith.clear();
  int steps = 0;
  while (!r.pending.empty() && steps < 20) {
    TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Stored, (int)drainOne(r.ctx, dr.deps(r)).step);
    steps++;
  }
  TEST_ASSERT_EQUAL_INT(7, steps);
  TEST_ASSERT_EQUAL_INT(7, r.vec.size());
  TEST_ASSERT_FALSE(r.park.parked());
}

static void test_billing_park_notices_once_per_episode() {
  Rig r;
  int notices = 0;
  r.ctx.onParked = [&](EmbedFail k, const std::string&) {
    if (k == EmbedFail::OutOfCredit && billingNoticeDue("openai")) notices++;
  };
  // Re-register so the tools capture the hook.
  r.reg = ToolRegistry();
  registerMemoryTools(r.reg, r.ctx);
  r.failWith = "HTTP 429 credit_balance_exhausted";
  for (int i = 0; i < 5; i++) {
    r.write("fact " + std::to_string(i), owner());
    r.nowMs += 31u * 60u * 1000u;   // every write is a probe: still one notice
  }
  TEST_ASSERT_EQUAL_INT(1, notices);
  // The chat head meeting the same refusal stays quiet: one shared latch.
  TEST_ASSERT_FALSE(billingNoticeDue("openai"));
  // A success ends the episode; the next refusal is told again.
  r.failWith.clear();
  r.write("recovered", owner());
  r.failWith = "HTTP 429 credit_balance_exhausted";
  r.write("again", owner());
  TEST_ASSERT_EQUAL_INT(2, notices);
  billingEpisodeEnd("openai");
}

// ===== the drain ===============================================================

static void test_drain_crash_safe_order() {
  Rig r;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  r.write("my locker code is 4417", owner());
  const std::string queuedBlob = r.pendingBlob;   // the durable queue before the drain
  r.failWith.clear();
  r.park.clear();
  DrainRig dr;
  dr.principals[kOwnerNs] = owner();
  // An injected vector-persist failure keeps the entry queued.
  dr.persistVecOk = false;
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::NotDurable, (int)drainOne(r.ctx, dr.deps(r)).step);
  TEST_ASSERT_EQUAL_INT(1, (int)r.pending.size());
  TEST_ASSERT_EQUAL_STRING(queuedBlob.c_str(), r.pendingBlob.c_str());
  // The next step (storage back) finds the id stored in RAM, persists, and pops.
  dr.persistVecOk = true;
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Duplicate, (int)drainOne(r.ctx, dr.deps(r)).step);
  TEST_ASSERT_EQUAL_INT(0, (int)r.pending.size());
  TEST_ASSERT_EQUAL_INT(1, r.vec.size());

  // A crash between the vector persist and the queue persist: on reboot the
  // vectors hold the fact AND the queue still holds it. The replay stores
  // nothing new and empties the queue.
  Rig c;
  c.failWith = "HTTP 429 credit_balance_exhausted";
  c.write("my locker code is 4417", owner());
  const std::string q0 = c.pendingBlob;
  c.failWith.clear();
  c.park.clear();
  DrainRig cd;
  cd.principals[kOwnerNs] = owner();
  c.persistPendingOk = false;   // the crash: the queue write never happens
  drainOne(c.ctx, cd.deps(c));
  const std::string v1 = cd.vecBlob;
  TEST_ASSERT_TRUE(v1.size() > 0);
  Rig boot;   // reboot: load both durable blobs
  TEST_ASSERT_TRUE(boot.vec.deserialize(v1));
  TEST_ASSERT_TRUE(boot.pending.deserialize(q0));
  TEST_ASSERT_EQUAL_INT(1, boot.vec.size());
  TEST_ASSERT_EQUAL_INT(1, (int)boot.pending.size());
  DrainRig bd;
  bd.principals[kOwnerNs] = owner();
  const int callsBefore = boot.embedCalls;
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Duplicate, (int)drainOne(boot.ctx, bd.deps(boot)).step);
  TEST_ASSERT_EQUAL_INT(callsBefore, boot.embedCalls);   // no embed for a replay
  TEST_ASSERT_EQUAL_INT(1, boot.vec.size());
  TEST_ASSERT_EQUAL_INT(0, (int)boot.pending.size());
}

static void test_drain_is_the_same_write_seam() {
  // Property over role x quota x permanent x ttl: enqueue-then-drain ends in the
  // same state as a direct write with embeddings up.
  const Role roles[] = {Role::Admin, Role::User, Role::Guest};
  const char* ttls[] = {"", "days", "months", "permanent"};
  for (Role role : roles)
    for (int atLimit = 0; atLimit < 2; atLimit++)
      for (int perm = 0; perm < 2; perm++)
        for (const char* ttl : ttls) {
          Quota q;
          q.maxVectors = 3;
          q.maxPins = 1;
          q.maxTtlHours = 48;
          const Principal who = principalForRole("3003", role, q);
          std::string extra = std::string("\"permanent\":") + (perm ? "true" : "false");
          if (ttl[0]) extra += std::string(",\"ttl\":\"") + ttl + "\"";
          auto seed = [&](Rig& r) {
            for (int i = 0; i < (atLimit ? 3 : 1); i++) r.write("seed fact " + std::to_string(i), who);
          };
          Rig direct;
          seed(direct);
          ToolResult dw = direct.write("the new fact", who, extra.c_str());
          Rig viaQueue;
          seed(viaQueue);
          viaQueue.failWith = "HTTP 429 credit_balance_exhausted";
          ToolResult qw = viaQueue.write("the new fact", who, extra.c_str());
          if (!dw.success) {
            // A refusal at the rails is the same refusal, before any queueing.
            TEST_ASSERT_FALSE(qw.success);
            TEST_ASSERT_EQUAL_STRING(dw.error.c_str(), qw.error.c_str());
            TEST_ASSERT_EQUAL_INT(0, (int)viaQueue.pending.size());
            continue;
          }
          TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, qw.code.c_str());
          viaQueue.failWith.clear();
          viaQueue.park.clear();
          DrainRig dr;
          dr.principals[who.ns] = who;
          drainOne(viaQueue.ctx, dr.deps(viaQueue));
          TEST_ASSERT_EQUAL_INT(direct.vec.size(), viaQueue.vec.size());
          const auto a = direct.vec.getAll(), b = viaQueue.vec.getAll();
          for (size_t i = 0; i < a.size(); i++) {
            const VecEntry* m = nullptr;
            for (const auto& e : b) if (e.id == a[i].id) m = &e;
            TEST_ASSERT_NOT_NULL(m);
            TEST_ASSERT_EQUAL_STRING(a[i].ns.c_str(), m->ns.c_str());
            TEST_ASSERT_EQUAL_INT(a[i].ttlHours, m->ttlHours);
            TEST_ASSERT_EQUAL_INT((int)a[i].permanentFlag, (int)m->permanentFlag);
          }
        }
  // A revoked principal is dropped, never stored.
  Rig r;
  const Principal m = member("4004");
  r.failWith = "HTTP 429 credit_balance_exhausted";
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, r.write("a member fact", m).code.c_str());
  r.failWith.clear();
  r.park.clear();
  DrainRig dr;
  Principal revoked = principalForRole("4004", Role::Unknown);
  dr.principals[m.ns] = revoked;
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Dropped, (int)drainOne(r.ctx, dr.deps(r)).step);
  TEST_ASSERT_EQUAL_INT(0, r.vec.size());
  TEST_ASSERT_EQUAL_INT(0, (int)r.pending.size());
  // ... and a namespace whose principal is gone entirely too.
  r.failWith = "HTTP 429 credit_balance_exhausted";
  r.write("another member fact", m);
  r.failWith.clear();
  r.park.clear();
  DrainRig none;
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Dropped, (int)drainOne(r.ctx, none.deps(r)).step);
  TEST_ASSERT_EQUAL_INT(0, r.vec.size());
}

static void test_queue_counts_against_the_quota() {
  // The queue is never a way around the memory ceiling.
  Quota q;
  q.maxVectors = 2;
  const Principal who = principalForRole("5005", Role::User, q);
  Rig r;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, r.write("one", who).code.c_str());
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, r.write("two", who).code.c_str());
  ToolResult third = r.write("three", who);
  TEST_ASSERT_FALSE(third.success);
  TEST_ASSERT_TRUE(has(third.error, "memory limit"));
  // Draining at the limit still stores what was accepted (the entry is not
  // counted twice).
  r.failWith.clear();
  r.park.clear();
  DrainRig dr;
  dr.principals[who.ns] = who;
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Stored, (int)drainOne(r.ctx, dr.deps(r)).step);
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Stored, (int)drainOne(r.ctx, dr.deps(r)).step);
  TEST_ASSERT_EQUAL_INT(2, r.vec.size());
}

static void test_presync_enqueue_restamped() {
  Rig r;
  r.synced = false;
  r.nowH = 3;   // boot-relative hours before SNTP
  r.failWith = "HTTP 429 credit_balance_exhausted";
  r.write("my locker code is 4417", owner(), "\"ttl\":\"days\"");
  TEST_ASSERT_FALSE(r.pending.front()->clockSynced);
  // The clock syncs, embeddings come back: the fact is stamped NOW, so it is not
  // instantly expired against a real-epoch clock.
  r.synced = true;
  r.nowH = 497000;
  r.failWith.clear();
  r.park.clear();
  DrainRig dr;
  dr.principals[kOwnerNs] = owner();
  drainOne(r.ctx, dr.deps(r));
  TEST_ASSERT_EQUAL_INT(1, r.vec.size());
  TEST_ASSERT_EQUAL_UINT32(497000u, r.vec.getAll()[0].createdAtHours);
  TEST_ASSERT_TRUE(has(r.call("memory.search", R"({"query":"locker"})", owner()).output, "4417"));
}

static void test_drain_notes_reach_only_their_writer() {
  Rig r;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  r.write("owner locker code is 4417", owner());
  r.write("member coffee is black", member("6006"));
  r.failWith.clear();
  r.park.clear();
  DrainRig dr;
  dr.principals[kOwnerNs] = owner();
  dr.principals[member("6006").ns] = member("6006");
  drainOne(r.ctx, dr.deps(r));
  drainOne(r.ctx, dr.deps(r));
  const std::string mine = r.pending.takeNotes(kOwnerNs);
  TEST_ASSERT_TRUE(has(mine, "4417"));
  TEST_ASSERT_TRUE(has(mine, "now stored"));
  TEST_ASSERT_FALSE(has(mine, "coffee"));
  const std::string theirs = r.pending.takeNotes(member("6006").ns);
  TEST_ASSERT_TRUE(has(theirs, "coffee"));
  TEST_ASSERT_TRUE(r.pending.takeNotes(kOwnerNs).empty());   // taken once
}

static void test_queued_fact_found_by_keyword_inside_boundary() {
  Rig r;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  const Principal a = member("7007"), b = member("8008");
  r.write("my locker code is 4417", a);
  ToolResult mine = r.call("memory.search", R"({"query":"locker code"})", a);
  TEST_ASSERT_TRUE(has(mine.output, "4417"));
  TEST_ASSERT_TRUE(has(mine.output, "pending, not stored yet"));
  // Property over principals: no other namespace ever sees it.
  for (const Principal& other : {b, owner(), principalForRole("9009", Role::Guest)}) {
    ToolResult s = r.call("memory.search", R"({"query":"locker code"})", other);
    TEST_ASSERT_FALSE(has(s.output, "4417"));
  }
}

static void test_update_during_outage_keeps_old_until_drain() {
  Rig r;
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeStored, r.write("I take my coffee black", owner()).code.c_str());
  const std::string oldId = r.vec.getAll()[0].id;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  ToolResult u = r.call("memory.update",
                        std::string(R"({"id":")") + oldId + R"(","content":"I take my coffee as a flat white"})",
                        owner());
  TEST_ASSERT_TRUE(u.success);
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, u.code.c_str());
  TEST_ASSERT_TRUE(has(u.output, "not changed yet"));
  TEST_ASSERT_EQUAL_INT(1, r.vec.size());
  TEST_ASSERT_EQUAL_STRING("I take my coffee black", r.vec.getAll()[0].content.c_str());
  r.failWith.clear();
  r.park.clear();
  DrainRig dr;
  dr.principals[kOwnerNs] = owner();
  TEST_ASSERT_EQUAL_INT((int)EmbedDrainStep::Stored, (int)drainOne(r.ctx, dr.deps(r)).step);
  TEST_ASSERT_EQUAL_INT(1, r.vec.size());
  TEST_ASSERT_EQUAL_STRING("I take my coffee as a flat white", r.vec.getAll()[0].content.c_str());
  // The description form resolves the old fact at drain time.
  r.failWith = "timeout";
  r.call("memory.update", R"({"old":"coffee","content":"coffee: oat flat white now"})", owner());
  r.failWith.clear();
  r.park.clear();
  drainOne(r.ctx, dr.deps(r));
  TEST_ASSERT_EQUAL_INT(1, r.vec.size());
  TEST_ASSERT_EQUAL_STRING("coffee: oat flat white now", r.vec.getAll()[0].content.c_str());
}

// The model-based class invariant: seeded random sequences of writes, outages,
// drain ticks and reboots. Once embeddings are up and enough ticks run, every
// accepted write is stored, and every refused write said "not stored".
// One random step of the model: a write, an outage on or off, a drain tick, or
// a reboot (the queue reloads from its durable blob, the vectors from their
// persisted state, the RAM-only park memo starts clear).
static void randomStep(Rig& r, DrainRig& dr, std::mt19937& rng, const std::string& fact,
                       std::vector<std::string>& accepted) {
  r.nowMs += 20000;
  const int op = (int)(rng() % 10);
  if (op < 4) {
    ToolResult w = r.write(fact, owner());
    TEST_ASSERT_TRUE(w.code == kMemOutcomeStored || w.code == kMemOutcomeQueued ||
                     w.code == kMemOutcomeRefused || w.code == kMemOutcomeDuplicate);
    if (w.code == kMemOutcomeRefused) TEST_ASSERT_TRUE(has(w.error, "not stored"));
    else accepted.push_back(fact);
    return;
  }
  if (op < 6) { r.failWith = (rng() % 2) ? "HTTP 429 credit_balance_exhausted" : "connect failed"; return; }
  if (op < 7) { r.failWith.clear(); return; }
  if (op < 9) { drainOne(r.ctx, dr.deps(r)); return; }
  const std::string vb = r.vec.serialize();
  r.vec = VectorMemory();
  r.vec.configure(Rig::kDims);
  r.vec.deserialize(vb);
  r.pending.deserialize(r.pendingBlob);
  r.park.clear();
}

static void test_mem_write_never_silently_lost() {
  for (uint32_t seed = 1; seed <= 25; seed++) {
    std::mt19937 rng(seed);
    Rig r;
    DrainRig dr;
    dr.principals[kOwnerNs] = owner();
    std::vector<std::string> accepted;
    for (int step = 0; step < 120; step++)
      randomStep(r, dr, rng, "fact " + std::to_string(seed) + "/" + std::to_string(step), accepted);
    r.failWith.clear();
    r.park.clear();
    for (int i = 0; i < 80 && !r.pending.empty(); i++) drainOne(r.ctx, dr.deps(r));
    TEST_ASSERT_EQUAL_INT(0, (int)r.pending.size());
    for (const auto& f : accepted) {
      bool found = false;
      for (const auto& e : r.vec.getAll()) if (e.content == f) found = true;
      TEST_ASSERT_TRUE_MESSAGE(found, f.c_str());
    }
  }
}

// F5: degraded automatic recall - the same read boundary and expiry rule as the
// semantic path, queued facts tagged, the cause carried, nothing boosted.
static void test_lexical_recall_bounds_and_tags() {
  Rig r;
  const Principal a = member("1201"), b = member("1202");
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeStored, r.write("my locker is on the second floor", a).code.c_str());
  r.write("the other locker belongs to b", b);
  r.write("an expiring locker note", a, "\"ttl\":\"session\"");
  r.failWith = "HTTP 429 credit_balance_exhausted";
  ToolResult q = r.write("my locker code is 4417", a);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kMemOutcomeQueued, q.code.c_str(), (q.output + q.error).c_str());
  const auto before = r.vec.getAll();
  r.nowH += 24;   // the session-ttl note (12 h) has expired; the weeks one has not
  RecallOut out = lexicalRecall(r.vec, &r.pending, "locker code", RecallScope{{a.ns}, r.nowH, 5}, "out of embedding credit");
  TEST_ASSERT_EQUAL_STRING("out of embedding credit", out.degraded.c_str());
  bool pendingSeen = false, floorSeen = false;
  for (const auto& h : out.hits) {
    TEST_ASSERT_FALSE(has(h, "belongs to b"));        // never another namespace
    TEST_ASSERT_FALSE(has(h, "expiring"));            // never an expired entry
    if (has(h, "[pending, not stored yet] my locker code is 4417")) pendingSeen = true;
    if (has(h, "second floor")) floorSeen = true;
  }
  TEST_ASSERT_TRUE(pendingSeen);
  TEST_ASSERT_TRUE(floorSeen);
  TEST_ASSERT_EQUAL_STRING(out.hits[0].c_str(), "[pending, not stored yet] my locker code is 4417");   // more words hit
  // Read-only: nothing boosted or re-stamped.
  const auto after = r.vec.getAll();
  for (size_t i = 0; i < before.size(); i++) {
    TEST_ASSERT_EQUAL_FLOAT(before[i].importance, after[i].importance);
    TEST_ASSERT_EQUAL_UINT32(before[i].createdAtHours, after[i].createdAtHours);
  }
  // Bounded by k; an unattributed caller recalls nothing.
  TEST_ASSERT_TRUE(lexicalRecall(r.vec, &r.pending, "locker", RecallScope{{a.ns}, r.nowH, 1}, "x").hits.size() == 1);
  TEST_ASSERT_TRUE(lexicalRecall(r.vec, &r.pending, "locker", RecallScope{{"\x01none"}, r.nowH, 5}, "x").hits.empty());
}

// Review findings: an update coalesced onto a queued write of the same text keeps
// the replace intent (and says so), and "delete" covers a queued fact.
static void test_coalesced_update_keeps_intent_and_delete_covers_queue() {
  Rig r;
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeStored, r.write("I take my coffee black", owner()).code.c_str());
  const std::string oldId = r.vec.getAll()[0].id;
  r.failWith = "HTTP 429 credit_balance_exhausted";
  r.write("I take my coffee as a flat white", owner());   // a plain write, queued
  ToolResult u = r.call("memory.update",
                        std::string(R"({"id":")") + oldId + R"(","content":"I take my coffee as a flat white"})",
                        owner());
  TEST_ASSERT_EQUAL_STRING(kMemOutcomeQueued, u.code.c_str());
  TEST_ASSERT_TRUE(has(u.output, "queued update"));
  TEST_ASSERT_EQUAL_INT(1, (int)r.pending.size());
  TEST_ASSERT_EQUAL_INT((int)PendingOp::Update, (int)r.pending.front()->op);
  TEST_ASSERT_EQUAL_STRING(oldId.c_str(), r.pending.front()->oldId.c_str());
  r.failWith.clear();
  r.park.clear();
  DrainRig dr;
  dr.principals[kOwnerNs] = owner();
  drainOne(r.ctx, dr.deps(r));
  TEST_ASSERT_EQUAL_INT(1, r.vec.size());   // the old fact was replaced, not duplicated
  TEST_ASSERT_EQUAL_STRING("I take my coffee as a flat white", r.vec.getAll()[0].content.c_str());

  // delete by content reaches a queued fact, only inside the caller's namespace.
  r.failWith = "HTTP 429 credit_balance_exhausted";
  r.write("my locker code is 4417", member("1301"));
  ToolResult other = r.call("memory.pin", R"({"action":"delete","content":"my locker code is 4417"})", owner());
  TEST_ASSERT_FALSE(other.success);
  TEST_ASSERT_EQUAL_INT(1, (int)r.pending.size());
  ToolResult mine = r.call("memory.pin", R"({"action":"delete","content":"my locker code is 4417"})", member("1301"));
  TEST_ASSERT_TRUE(mine.success);
  TEST_ASSERT_TRUE(has(mine.output, "never stored"));
  TEST_ASSERT_EQUAL_INT(0, (int)r.pending.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_queue_roundtrip_and_tolerant_deserialize);
  RUN_TEST(test_queue_bounds_never_evict);
  RUN_TEST(test_coalesce_by_content_id);
  RUN_TEST(test_park_policy_covers_every_embedfail_class);
  RUN_TEST(test_park_no_thrash);
  RUN_TEST(test_billing_refusals_classify_as_out_of_credit);
  RUN_TEST(test_write_during_outage_is_queued_for_every_failure_class);
  RUN_TEST(test_counters_call_a_queued_write_queued);
  RUN_TEST(test_queue_full_or_storage_down_refuses_honestly);
  RUN_TEST(test_parked_turn_spends_no_embed);
  RUN_TEST(test_billing_park_notices_once_per_episode);
  RUN_TEST(test_drain_crash_safe_order);
  RUN_TEST(test_drain_is_the_same_write_seam);
  RUN_TEST(test_queue_counts_against_the_quota);
  RUN_TEST(test_presync_enqueue_restamped);
  RUN_TEST(test_drain_notes_reach_only_their_writer);
  RUN_TEST(test_queued_fact_found_by_keyword_inside_boundary);
  RUN_TEST(test_update_during_outage_keeps_old_until_drain);
  RUN_TEST(test_mem_write_never_silently_lost);
  RUN_TEST(test_lexical_recall_bounds_and_tags);
  RUN_TEST(test_coalesced_update_keeps_intent_and_delete_covers_queue);
  return UNITY_END();
}
