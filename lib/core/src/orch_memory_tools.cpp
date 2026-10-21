#include "nimbus/orch/memory_tools.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include "nimbus/mem_cap.h"
#include "nimbus/orch/billing_refusal.h"   // isBillingRefusal / billingEpisodeEnd - one table with the chat head
#include "nimbus/orch/pending_embed.h"
#include "nimbus/orch/caps.h"   // scratchpad tier/item caps -> capacity line (W11)
#include "nimbus/orch/episodic_log.h"   // civilDate for the honest history floor
#include "nimbus/orch/mem_ttl.h"

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;

namespace nimbus {
namespace orch {

// v3.7.0 read set: your own namespace plus the shared one. The OWNER's chats
// all map to kOwnerNs (one human, many channels), so an owner sees everything
// they ever stored; a member sees only their own conversation's memories plus
// device-level shared facts. An unattributed principal (empty ns) gets ONLY
// shared - the most restrictive reading, never a wildcard.
// A member's namespace is "chat:<id>"; its episodic session id is the bare id.
static std::string sessionForNs(const std::string& ns) {
  const std::string pfx = "chat:";
  return ns.rfind(pfx, 0) == 0 ? ns.substr(pfx.size()) : ns;
}

// v3.7.0 read set: STRICTLY the caller's own namespace. There is deliberately
// NO shared vector namespace (owner's call, 2026-07-27) - a shared namespace
// any tenant could write would be a persistent injection path INTO THE ADMIN'S
// CONTEXT: whatever a guest stored there would be recalled into the admin's
// turns and read as the device's own memory. Device-level facts therefore live
// in the ADMIN's namespace, which admins already read.
//
// Sharing exists only for FILES, where it is an explicit, per-artifact,
// READ-ONLY grant by that file's owner - a document someone chooses to hand
// over, not a fact that silently enters someone else's prompt.
static std::vector<std::string> readSetFor(const Principal& who) {
  std::vector<std::string> allow;
  if (who.valid()) allow.push_back(who.ns);
  return allow;
}

// ---- honest effect reports (the claim check reads them) ---------------------
const char kAlreadyPinnedPrefix[] = "already pinned (";
const char kAlreadyPinnedTail[] =
    "), no change. Pinning only stops decay; it does not put a memory into every prompt.";
const char kScratchRamOnlyNote[] = "; kept until restart only (saving failed)";

// ---- honest embedding-failure reasons (CUM-435) -----------------------------
// Map the device seam's raw err token (agent::embeddings::embedWith) to a class +
// honest words. Secret-safe by construction: `detail` is only ever a KNOWN
// provider slug, a numeric HTTP status, or two numeric widths, never arbitrary text
// from `raw`, so a key-shaped string in `raw` cannot reach the tool result. Keep every token here
// in step with what embeddings.cpp emits and the test that iterates the classes.
namespace {
bool rawStartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The status portion of an "HTTP ..." token. A billing refusal (the ONE table
// shared with the chat head, billing_refusal.h) is OutOfCredit; a 429 without a
// billing token is RateLimited; a known router code maps to its own class;
// anything else is ProviderError carrying ONLY the numeric status.
EmbedFail classifyHttpToken(const std::string& raw, std::string& detail) {
  std::string code;
  for (size_t i = 5; i < raw.size(); i++) {   // digits after "HTTP " are secret-safe
    const char c = raw[i];
    if (c < '0' || c > '9') break;
    code += c;
  }
  const int status = code.empty() ? 0 : std::atoi(code.c_str());
  // The router's monthly cap (HTTP 429 ceiling_reached) is a billing refusal too;
  // it is checked beside the shared table, which does not carry it yet.
  if (isBillingRefusal(status, raw) || raw.find("ceiling_reached") != std::string::npos)
    return EmbedFail::OutOfCredit;
  if (raw.find("endpoint_not_allowed") != std::string::npos) return EmbedFail::RouteNotAllowed;
  if (status == 429 || raw.find("rate_limited") != std::string::npos) return EmbedFail::RateLimited;
  detail = code;
  return EmbedFail::ProviderError;
}

// The digits right after `marker` in `raw` ("" if none). Digits only, so the
// result is secret-safe whatever else `raw` carries.
std::string digitsAfter(const std::string& raw, const char* marker) {
  const size_t at = raw.find(marker);
  std::string n;
  if (at == std::string::npos) return n;
  for (size_t i = at + std::strlen(marker); i < raw.size() && n.size() < 6; i++) {
    if (raw[i] < '0' || raw[i] > '9') break;
    n += raw[i];
  }
  return n;
}
}  // namespace

EmbedFail classifyEmbedFail(const std::string& raw, std::string& detail) {
  detail.clear();
  if (raw.empty()) return EmbedFail::None;
  // Exact-match tokens the device seam emits.
  static const struct { const char* tok; EmbedFail kind; } kExact[] = {
    {"empty text",     EmbedFail::BadRequest},
    {"no model",       EmbedFail::NoModel},
    {"tls busy",       EmbedFail::Busy},
    {"connect failed", EmbedFail::Unreachable},
    {"timeout",        EmbedFail::Timeout},
    {"key rejected",   EmbedFail::KeyRejected},
  };
  for (const auto& e : kExact) if (raw == e.tok) return e.kind;
  if (rawStartsWith(raw, "no embeddings key for ")) {
    const std::string p = raw.substr(sizeof("no embeddings key for ") - 1);
    // Name the provider ONLY when it is a known slug - never echo raw text.
    static const char* kProviders[] = {"openai", "mistral", "cumulo", "anthropic", "zai"};
    for (const char* known : kProviders) if (p == known) { detail = p; break; }
    return EmbedFail::NoKey;
  }
  // The width contract (nimbus/orch/embedding.h): a vector of the wrong width is a
  // config mismatch, not a malformed reply, so it gets its own words + fix (CUM-469).
  if (rawStartsWith(raw, "parse: dim mismatch")) {
    const std::string got = digitsAfter(raw, "got "), want = digitsAfter(raw, "expected ");
    if (!got.empty() && !want.empty()) detail = got + "/" + want;
    return EmbedFail::WidthMismatch;
  }
  if (rawStartsWith(raw, "parse:")) return EmbedFail::BadResponse;
  if (rawStartsWith(raw, "HTTP "))  return classifyHttpToken(raw, detail);
  return EmbedFail::Unknown;
}

std::string embedFailWords(EmbedFail kind, const std::string& detail) {
  // The two detail-bearing classes are explicit; the rest are a flat table.
  if (kind == EmbedFail::NoKey)
    return detail.empty() ? "no embeddings key for the configured provider"
                          : ("no embeddings key for " + detail);
  if (kind == EmbedFail::ProviderError)
    return detail.empty() ? "provider error" : ("provider error HTTP " + detail);
  if (kind == EmbedFail::OutOfCredit)
    return detail.empty() ? "out of embedding credit" : ("out of embedding credit at " + detail);
  if (kind == EmbedFail::WidthMismatch) {
    const size_t slash = detail.find('/');
    if (slash == std::string::npos)
      return "the embedding size doesn't match the memory setting";
    return "the model returns " + detail.substr(0, slash) +
           "-wide embeddings but memory is set to " + detail.substr(slash + 1);
  }
  static const struct { EmbedFail k; const char* w; } kW[] = {
    {EmbedFail::None,            ""},
    {EmbedFail::KeyRejected,     "the embeddings key was rejected"},
    {EmbedFail::Busy,            "connection busy, retry"},
    {EmbedFail::Unreachable,     "provider unreachable"},
    {EmbedFail::Timeout,         "provider timed out"},
    {EmbedFail::RateLimited,     "provider busy (rate limited)"},
    {EmbedFail::RouteNotAllowed, "embeddings route not allowed for this provider"},
    {EmbedFail::BadResponse,     "provider sent a malformed response"},
    {EmbedFail::NoModel,         "no embeddings model configured"},
    {EmbedFail::BadRequest,      "no text to embed"},
    {EmbedFail::Unknown,         "unavailable"},
  };
  for (const auto& e : kW) if (e.k == kind) return e.w;
  return "unavailable";
}

std::string embedFailReasonFor(EmbedFail k, const std::string& detail) {
  std::string w = embedFailWords(k, detail);
  if (w.empty()) w = "unavailable";
  std::string out = "embeddings: " + w;
  if (k == EmbedFail::NoKey) out += " (add the embedding provider's key under Assistant > Models on the device web page)";
  if (k == EmbedFail::WidthMismatch)
    out += " (set Dimensions to the model's size in Memory > Embedding model)";
  if (k == EmbedFail::OutOfCredit) out += " (add credit on the provider's billing page)";
  return out;
}

std::string embedFailReason(const std::string& raw) {
  std::string detail;
  const EmbedFail k = classifyEmbedFail(raw, detail);
  return embedFailReasonFor(k, detail);
}

namespace {

// Reason phrase for a search fallback label, given the seam's raw err.
std::string embedFailLabelReason(const std::string& raw) {
  std::string detail;
  std::string w = embedFailWords(classifyEmbedFail(raw, detail), detail);
  return w.empty() ? std::string("unavailable") : w;
}

// Embed `text` for a store `width` wide. A vector of any other width can never be
// stored or compared - VectorMemory::add rejects it and search matches nothing - so
// it is refused HERE with the same token the response parser emits, and every tool
// reports the mismatch honestly (CUM-469). Without this a mismatch read as
// "duplicate of an existing memory" on write, as an empty result on search, and an
// update deleted the old fact before its replacement failed to store. It happens
// whenever the stored dims disagree with what the model returns (dims=0 on a
// 256-wide store, or a seam that does not check the width, like nimbusd's).
std::vector<int8_t> embedForStore(const MemoryContext& ctx, int width, const std::string& text,
                                  std::string& err) {
  std::vector<int8_t> v = ctx.embed(text, err);
  if (!v.empty() && (int)v.size() != width) {
    err = "parse: dim mismatch: got " + std::to_string(v.size()) + " expected " +
          std::to_string(width);
    v.clear();
  }
  return v;
}

// Mirror of VectorMemory nsVisible (orch_vector_memory.cpp): an empty allow-list
// is unscoped; a legacy empty entry ns belongs to the owner. Kept in lockstep so
// the lexical fallback enforces EXACTLY the semantic read boundary and can never
// widen access.
// Compared in place (no copy of the namespace per entry: a member namespace such as
// "chat:-1001234567890" is longer than a short-string buffer, and the keyword scan
// asks this once per entry).
bool nsAllowsEntry(const std::string& entryNs, const std::vector<std::string>& allow) {
  if (allow.empty()) return true;
  const std::string_view eff =
      entryNs.empty() ? std::string_view(kOwnerNs) : std::string_view(entryNs);
  for (const auto& a : allow) if (std::string_view(a) == eff) return true;
  return false;
}

// Mirror of VectorMemory::isExpiredRaw (orch_vector_memory.cpp) over an entry's
// view, so the keyword fallback hides the SAME entries the semantic path drops at
// query time (a fact the owner set to expire must not resurface through the
// fallback). nowHours == 0 (clockless) disables the age check, exactly like the
// engine.
bool entryExpired(const VectorMemory::EntryView& e, uint32_t nowHours) {
  if (e.permanentFlag || e.creatorFlag) return false;
  if (e.importance < VectorMemory::kMinImportance) return true;
  if (e.ttlHours <= 0) return false;
  if (nowHours == 0) return false;
  const uint32_t age = nowHours >= e.createdAtHours ? nowHours - e.createdAtHours : 0;
  return (int64_t)age > (int64_t)e.ttlHours;
}

// Appended to a keyword fallback's empty answer when the query held only
// common words (lexical.h ignores them), so the model searches again with a
// distinctive word instead of concluding the memory is gone.
std::string commonWordsHint(const LexQuery& lq) {
  return lq.onlyCommonWords()
             ? std::string(" Every word in that query is a common word that keyword "
                           "search ignores - search again with a distinctive word.")
             : std::string();
}

// Deterministic id from content (djb2). On-device a content hash is stable,
// needs no RNG, and dedup handles near-collisions anyway.
std::string contentId(const std::string& s) {
  unsigned long h = 5381;
  for (unsigned char c : s) h = ((h << 5) + h) + c;
  char buf[16];
  snprintf(buf, sizeof(buf), "m%08lx", h & 0xffffffff);
  return std::string(buf);
}

std::string strArg(JsonObjectConst a, const char* k, const char* def = "") {
  return a[k].is<const char*>() ? std::string(a[k].as<const char*>()) : std::string(def);
}
Tier tierFromStr(const std::string& s) {
  if (s == "mid") return Tier::Mid;
  if (s == "long") return Tier::Long;
  return Tier::Short;
}

// Read a numeric arg tolerantly. JSON draws no int/float line, so a provider may
// encode an integer field as "20" OR "20.0" - accept either. Writes `out` and
// returns true only when the key holds a number; leaves `out` untouched
// otherwise, so callers can pre-seed a default and ignore the result.
bool numArg(JsonObjectConst a, const char* k, double& out) {
  JsonVariantConst v = a[k];
  if (v.is<int>() || v.is<long long>() || v.is<float>() || v.is<double>()) {
    out = v.as<double>();
    return true;
  }
  return false;
}

// ---- memory.write -----------------------------------------------------------
// The quota contract for EVERY path that adds a vector. It lives in one function
// because it did not: memory.update was a second write seam that skipped all
// three rails, so a tenant at its ceiling could keep writing - permanently, with
// no expiry - just by using the other tool. Stamps `e` in place, appends any
// user-facing note, and returns a refusal string (or nullptr to proceed).
//
// The counts come from the STORE, never from a number the caller passes in. A
// fact waiting in the pending-embed queue counts too, so the queue is never a
// way around the ceiling. `freed` is how many of those counted slots this write
// gives back: the queued entry being stored itself (the drain), and the memory an
// update replaces.
const char* applyWriteQuotas(const MemoryContext& ctx, const Principal& who,
                             VecEntry& e, std::string& note, int freed = 0) {
  static std::string refusal;   // the message outlives this call
  const Quota q = effectiveQuota(who.role, who.quota);
  const int64_t used = (int64_t)ctx.vec->countIn(who.ns) +
                       (ctx.pending ? ctx.pending->countIn(who.ns) : 0) - freed;
  if (q.maxVectors && used >= (int64_t)q.maxVectors) {
    refusal = "this conversation has reached its memory limit (" +
              std::to_string(q.maxVectors) +
              ") - ask the device's admin to raise it, or remove an old memory first";
    return refusal.c_str();
  }
  bool ttlClamped = false;
  e.ttlHours = clampTtl(who.role, who.quota, e.ttlHours, ttlClamped);
  if (ttlClamped) note += " (kept for " + std::to_string(e.ttlHours) + "h - this "
                          "conversation's limit)";
  // `permanent` exempts an entry from eviction AND prune, so it is budgeted
  // rather than refused: the fact still lands, it just does not become
  // unreclaimable storage.
  if (e.permanentFlag && !pinAllowed(who.role, who.quota, ctx.vec->pinsIn(who.ns))) {
    e.permanentFlag = false;
    note += " (not pinned - pinning isn't available on this conversation)";
  }
  return nullptr;
}

ToolResult withCode(ToolResult r, const char* code, const MemoryContext& ctx) {
  r.code = code;
  if (ctx.onWriteOutcome) ctx.onWriteOutcome(code);
  return r;
}

// memory.write did not insert: a duplicate (saying what it did about a pin,
// so a "pinned" reply can be checked - only for the writer's own memory: another
// namespace's pin state is not theirs to learn), or an entry of the wrong width.
// Every duplicate answer extends one shared base sentence, so the reply and the
// counters (which classify by the outcome code, falling back to this prefix)
// can never drift apart.
const char kDuplicateWrite[] = "duplicate of an existing memory (importance bumped";
ToolResult notInsertedResult(const VectorMemory& vec, bool askedPermanent) {
  if (!vec.lastAddWasDuplicate())
    return ToolResult::fail("the embedding has the wrong width for this store (memory not stored)");
  std::string s = kDuplicateWrite;
  if (vec.lastDuplicateSameNs() && vec.lastDuplicatePinned())
    s += "; it is already pinned)";
  else if (vec.lastDuplicateSameNs() && askedPermanent)
    s += "; it is NOT pinned - use memory.pin to pin it)";
  else
    s += ")";
  return ToolResult::ok(s);
}

// One embed attempt that consults and feeds the park memo. While parked it makes
// NO network call and reports the parked cause; a success ends the park (and the
// billing episode at that provider, so its next refusal is told again).
struct EmbedTry {
  std::vector<int8_t> vec;
  EmbedFail kind = EmbedFail::None;
  std::string detail;   // secret-safe (a known slug or digits)
  bool parked = false;  // skipped: the park said the attempt was doomed
};

bool knownProviderSlug(const std::string& p) {
  static const char* kProviders[] = {"openai", "mistral", "cumulo", "anthropic", "zai"};
  for (const char* known : kProviders) if (p == known) return true;
  return false;
}

void underParkLock(const MemoryContext& ctx, const std::function<void()>& fn) {
  if (ctx.parkLock) ctx.parkLock(fn);
  else fn();
}

// Record an embed attempt's outcome in the park memo (under its lock).
void noteEmbedResult(const MemoryContext& ctx, EmbedTry& t, const std::string& err, uint32_t now) {
  const std::string prov = ctx.embedProvider ? ctx.embedProvider() : std::string();
  if (t.vec.empty()) {
    t.kind = classifyEmbedFail(err, t.detail);
    if (t.kind == EmbedFail::None) t.kind = EmbedFail::Unknown;   // empty vector, empty err
    if (t.kind == EmbedFail::OutOfCredit && knownProviderSlug(prov)) t.detail = prov;
  }
  if (!ctx.park) return;
  bool started = false;
  underParkLock(ctx, [&] {
    if (!t.vec.empty() && ctx.park->reason() == EmbedFail::OutOfCredit && knownProviderSlug(prov))
      billingEpisodeEnd(prov.c_str());
    started = ctx.park->onResult(t.vec.empty() ? t.kind : EmbedFail::None, now, t.detail);
  });
  if (started && t.vec.empty() && ctx.onParked) ctx.onParked(t.kind, err);
}

EmbedTry tryEmbed(const MemoryContext& ctx, int width, const std::string& text) {
  EmbedTry t;
  const uint32_t now = ctx.nowMs ? ctx.nowMs() : 0;
  if (ctx.park)
    underParkLock(ctx, [&] {
      if (!ctx.park->blocked(now)) return;
      t.parked = true;
      t.kind = ctx.park->reason();
      t.detail = ctx.park->detail();
    });
  if (t.parked) return t;
  std::string err;
  t.vec = embedForStore(ctx, width, text, err);   // the network call: never under the lock
  noteEmbedResult(ctx, t, err, now);
  return t;
}

// Can this failure wait in the queue? Everything except "nothing to embed".
bool queueable(const MemoryContext& ctx, EmbedFail k) {
  return ctx.pending && k != EmbedFail::BadRequest && k != EmbedFail::None;
}

// The fields of one write, from the tool's args or from a queued entry.
struct WriteSpec {
  std::string content;
  float importance = 0.5f;
  bool permanent = false;
  std::string ttlName;
  std::string source = "self";
  uint32_t createdAtHours = 0;
};

WriteSpec specFromArgs(const MemoryContext& ctx, JsonObjectConst a, double defImportance) {
  WriteSpec w;
  w.content = strArg(a, "content");
  double imp = defImportance;
  numArg(a, "importance", imp);
  w.importance = (float)(imp < 0 ? 0 : imp > 1 ? 1 : imp);
  w.permanent = a["permanent"].is<bool>() ? a["permanent"].as<bool>() : false;
  w.source = strArg(a, "source", "self");
  w.ttlName = strArg(a, "ttl", "");
  w.createdAtHours = ctx.nowHours();
  return w;
}

WriteSpec specFromPending(const MemoryContext& ctx, const PendingEmbed& p) {
  WriteSpec w;
  w.content = p.content;
  w.importance = p.importance;
  w.permanent = p.permanent;
  w.ttlName = p.ttlName;
  w.source = p.source;
  // A fact queued before the clock was set is stamped now, when it is stored, so
  // its age is never read against a boot-relative hour (the pre-sync trap).
  w.createdAtHours = p.clockSynced ? p.createdAtHours : ctx.nowHours();
  return w;
}

// Build the entry a write stores (no vector yet) and run the quota rails.
// Returns the refusal, or nullptr. The caller has already checked who.valid()
// and writeOwn.
const char* prepareWrite(const MemoryContext& ctx, const Principal& who, const WriteSpec& w,
                         VecEntry& e, std::string& note, int freed) {
  e = VecEntry();
  e.id = contentId(w.content);
  e.content = w.content;
  e.importance = w.importance;
  e.permanentFlag = w.permanent;
  e.source = w.source;
  e.createdAtHours = w.createdAtHours;
  // ttl class chosen by the model (default weeks when absent/unknown).
  e.ttlHours = ttlHoursFromName(w.ttlName.c_str());
  // WRITE RAIL (v3.7.0): the namespace comes from the authenticated caller, not
  // from the model's arguments.
  e.ns = who.ns;
  return applyWriteQuotas(ctx, who, e, note, freed);
}

PendingEmbed pendingFrom(const MemoryContext& ctx, const VecEntry& e, const WriteSpec& w,
                         EmbedFail why) {
  PendingEmbed p;
  p.id = e.id;
  p.ns = e.ns;
  p.content = w.content;
  p.importance = w.importance;
  p.permanent = w.permanent;
  p.ttlName = w.ttlName;
  p.source = w.source;
  p.createdAtHours = w.createdAtHours;
  p.clockSynced = ctx.clockSynced ? ctx.clockSynced() : true;
  p.lastFail = (uint8_t)why;
  return p;
}

// Put a write the embedder cannot serve now into the durable queue. "queued" is
// claimed only after the queue reached storage; a full queue or a failed persist
// refuses honestly and nothing queued is evicted.
ToolResult enqueueOrRefuse(const MemoryContext& ctx, PendingEmbed p, const EmbedTry& t,
                           const std::string& note, const char* notStored) {
  const std::string cause = embedFailReasonFor(t.kind, t.detail);
  auto refuse = [&](const std::string& why) {
    return withCode(ToolResult::fail(cause + " (" + notStored + ": " + why + ")"),
                    kMemOutcomeRefused, ctx);
  };
  switch (ctx.pending->enqueue(p)) {
    case PendingEmbeds::Add::Full:
      return refuse(std::to_string(PendingEmbeds::kMaxEntries) + " memories are already waiting");
    case PendingEmbeds::Add::BytesFull:
      return refuse("the waiting list on the device is full");
    case PendingEmbeds::Add::NsFull:
      return refuse("this conversation already has " +
                    std::to_string(PendingEmbeds::kMaxPerMember) + " memories waiting");
    case PendingEmbeds::Add::TooLong:
      return refuse("too long to keep while embeddings are down; save a shorter version");
    case PendingEmbeds::Add::Empty:
      return refuse("nothing to store");
    case PendingEmbeds::Add::Added:
      if (ctx.persistPending && !ctx.persistPending()) {
        ctx.pending->pop(p.id);
        return refuse("device storage unavailable");
      }
      break;
    case PendingEmbeds::Add::Coalesced:
      // Already durably queued; a failed rewrite only loses the importance bump.
      if (ctx.persistPending) ctx.persistPending();
      break;
  }
  // Report what is queued now (a coalesced entry keeps the stronger intent).
  const PendingEmbed* queued = ctx.pending->find(p.id);
  const bool update = (queued ? queued->op : p.op) == PendingOp::Update;
  return withCode(
      ToolResult::ok(std::string(update ? "queued update for " : "queued memory ") + p.id +
                     (update ? ", not changed yet: " : ", not stored yet: ") + cause +
                     ". It is kept on the device and " +
                     (update ? "applied" : "stored") +
                     " automatically when embeddings work again; keyword search finds it "
                     "meanwhile. Until then, do not say it is saved." + note),
      kMemOutcomeQueued, ctx);
}

// Store an embedded entry: the end of every successful write. A duplicate keeps
// the shared pin-state sentence (notInsertedResult), so the reply and the
// counters can never drift apart.
ToolResult storeEmbedded(const MemoryContext& ctx, const VecEntry& e, const std::string& note) {
  const bool inserted = ctx.vec->add(e);
  if (inserted) return withCode(ToolResult::ok("stored memory " + e.id + note), kMemOutcomeStored, ctx);
  ToolResult r = notInsertedResult(*ctx.vec, e.permanentFlag);
  return withCode(std::move(r), r.success ? kMemOutcomeDuplicate : kMemOutcomeRefused, ctx);
}

ToolResult doWrite(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  const WriteSpec w = specFromArgs(ctx, a, 0.5);
  if (w.content.empty()) return withCode(ToolResult::fail("missing 'content'"), kMemOutcomeRefused, ctx);
  if (!ctx.vec || !ctx.embed)
    return withCode(ToolResult::fail("memory engine unavailable"), kMemOutcomeRefused, ctx);
  // The rails run BEFORE the embed, so a caller that may not write never spends
  // an embedding call. Unattributed callers cannot write at all.
  if (!who.valid())
    return withCode(ToolResult::fail("memory write needs an identified caller"), kMemOutcomeRefused, ctx);
  if (!who.perms().writeOwn)
    return withCode(ToolResult::fail("this conversation isn't approved to store memories yet"),
                    kMemOutcomeRefused, ctx);
  VecEntry e;
  std::string note;
  if (const char* refusal = prepareWrite(ctx, who, w, e, note, 0))
    return withCode(ToolResult::fail(refusal), kMemOutcomeRefused, ctx);

  EmbedTry t = tryEmbed(ctx, ctx.vec->dims(), w.content);
  if (!t.vec.empty()) {
    e.vec = std::move(t.vec);
    return storeEmbedded(ctx, e, note);
  }
  // No embedding: never store the text as if it were embedded. With the queue
  // bound the fact waits for the embedder; without it (or with nothing to embed)
  // the write is refused with the real cause.
  if (!queueable(ctx, t.kind))
    return withCode(ToolResult::fail(embedFailReasonFor(t.kind, t.detail) + " (memory not stored)"),
                    kMemOutcomeRefused, ctx);
  // The exact same fact is already stored: nothing to wait for.
  if (ctx.vec->idVisible(e.id, readSetFor(who)))
    return withCode(ToolResult::ok("duplicate of an existing memory"), kMemOutcomeDuplicate, ctx);
  return enqueueOrRefuse(ctx, pendingFrom(ctx, e, w, t.kind), t, note, "memory not stored");
}

// ---- memory.update (replace/supersede an existing fact) ---------------------
// The fix for "update my coffee to flat white" creating a SECOND vector next to the
// old one: find the memory to replace (by id, or by the closest semantic match to a
// short description of the OLD fact), remove it, and store the new content. If nothing
// close is found it just stores the new fact (never deletes an unrelated memory).

// The memory an update replaces: the explicit id, else the nearest match to the
// description, only when it is genuinely close (cosine sim >= 0.55), so a vague
// description can't delete an unrelated memory. Empty = nothing to replace.
std::string resolveUpdateTarget(const MemoryContext& ctx, const Principal& who,
                                const std::string& id, const std::vector<int8_t>& descVec) {
  if (!id.empty()) return id;
  if (descVec.empty()) return std::string();
  auto hits = ctx.vec->search(descVec, 1, 0, readSetFor(who));
  if (!hits.empty() && (1.0f - hits[0].distance) >= 0.55f) return hits[0].id;
  return std::string();
}

ToolResult doUpdate(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  if (!who.perms().writeOwn)
    return withCode(ToolResult::fail("this conversation isn't approved to change memories"),
                    kMemOutcomeRefused, ctx);
  const WriteSpec w = specFromArgs(ctx, a, 0.6);
  if (w.content.empty())
    return withCode(ToolResult::fail("missing 'content' (the new fact)"), kMemOutcomeRefused, ctx);
  if (!ctx.vec || !ctx.embed)
    return withCode(ToolResult::fail("memory engine unavailable"), kMemOutcomeRefused, ctx);
  const std::string oldIdArg = strArg(a, "id");
  std::string desc = strArg(a, "old");
  if (desc.empty()) desc = strArg(a, "query");
  // A caller may only replace a memory INSIDE its own boundary - an id from
  // another principal's namespace is refused rather than silently deleted.
  if (!oldIdArg.empty() && !ctx.vec->idVisible(oldIdArg, readSetFor(who)))
    return withCode(ToolResult::fail("no such memory"), kMemOutcomeRefused, ctx);

  // Embed the NEW content FIRST: if embeddings are unavailable nothing is removed,
  // so a failed update can never delete the old fact and leave nothing in its
  // place (CUM-435). With the queue bound the update waits, and the old fact stays
  // until the new one is stored.
  EmbedTry t = tryEmbed(ctx, ctx.vec->dims(), w.content);
  if (t.vec.empty()) {
    if (!queueable(ctx, t.kind) || !who.valid())
      return withCode(ToolResult::fail(embedFailReasonFor(t.kind, t.detail) + " (memory not changed)"),
                      kMemOutcomeRefused, ctx);
    VecEntry e;
    std::string note;
    if (const char* refusal = prepareWrite(ctx, who, w, e, note, oldIdArg.empty() ? 0 : 1))
      return withCode(ToolResult::fail(refusal), kMemOutcomeRefused, ctx);
    PendingEmbed p = pendingFrom(ctx, e, w, t.kind);
    p.op = PendingOp::Update;
    p.oldId = oldIdArg;
    p.oldDesc = oldIdArg.empty() ? desc : std::string();
    return enqueueOrRefuse(ctx, p, t, note, "memory not changed");
  }
  std::vector<int8_t> descVec;
  if (oldIdArg.empty() && !desc.empty()) descVec = tryEmbed(ctx, ctx.vec->dims(), desc).vec;
  const std::string oldId = resolveUpdateTarget(ctx, who, oldIdArg, descVec);

  VecEntry e;
  std::string note;
  // The SAME rails as memory.write. This path also stores when nothing matched
  // ("stored as new" below), which makes it a full write seam - an unrailed one
  // was a way around every ceiling. The replacement belongs to the caller.
  const int freed = (!oldId.empty() && ctx.vec->idVisible(oldId, readSetFor(who))) ? 1 : 0;
  if (const char* refusal = prepareWrite(ctx, who, w, e, note, freed))
    return withCode(ToolResult::fail(refusal), kMemOutcomeRefused, ctx);
  e.vec = std::move(t.vec);
  const bool removed = !oldId.empty() && ctx.vec->remove(oldId);
  ctx.vec->add(e);
  return withCode(
      ToolResult::ok((removed ? ("updated: replaced the prior memory with \"" + w.content + "\"")
                              : ("no close match to replace; stored \"" + w.content + "\" as new")) +
                     note),
      kMemOutcomeStored, ctx);
}

// ---- memory.pin (pin / unpin / delete a stored memory) ----------------------
// Gives the model parity with the human web UI over its own long-term memory:
// pin (never decay/evict), UNPIN (let it decay/expire again), or delete. Memories
// are keyed by a content hash, so the model can target one by its exact `content`
// (we recompute the id) or by `id` if it kept one from a prior write.
// A fact still waiting for an embedding is a memory too: "delete" removes it from
// the queue (inside the caller's namespace), so it is never stored later.
bool deleteQueued(const MemoryContext& ctx, const std::string& id, const Principal& who) {
  if (!ctx.pending) return false;
  const PendingEmbed* q = ctx.pending->find(id);
  if (!q || !nsAllowsEntry(q->ns, readSetFor(who))) return false;
  ctx.pending->pop(id);
  if (ctx.persistPending) ctx.persistPending();
  if (ctx.vec->idVisible(id, readSetFor(who))) ctx.vec->remove(id);
  return true;
}

ToolResult doPin(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  if (!ctx.vec) return ToolResult::fail("memory engine unavailable");
  if (!who.perms().writeOwn)
    return ToolResult::fail("this conversation isn't approved to use memory");
  std::string action = strArg(a, "action");
  std::string id = strArg(a, "id");
  if (id.empty()) {
    std::string content = strArg(a, "content");
    if (!content.empty()) id = contentId(content);
  }
  if (id.empty()) return ToolResult::fail("identify the memory by 'id' or exact 'content'");
  // ⚠ This tool addresses a memory by RAW ID - or by a hash of its exact content,
  // which is the same thing for anyone who can guess the wording. Without a
  // visibility check, "delete the fact that the alarm code is 4417" from any
  // chat would remove the admin's entry. The refusal is deliberately identical
  // to "no such memory": confirming that an id exists is itself a disclosure.
  if (action == "delete" && deleteQueued(ctx, id, who))
    return ToolResult::ok("delete ok (" + id + ", it was waiting for embeddings and is never stored)");
  if (!ctx.vec->idVisible(id, readSetFor(who)))
    return ToolResult::fail("no memory with id " + id);
  bool ok;
  if (action == "pin") {
    // A pin outlives every quota - it is exempt from eviction AND prune - so it
    // is budgeted, not free. Guests get none.
    // An already pinned memory changes nothing: say so, so the reply cannot
    // claim a fix that did not happen (the claim check reads this result).
    if (ctx.vec->isPermanent(id)) return ToolResult::ok(kAlreadyPinnedPrefix + id + kAlreadyPinnedTail);
    if (!pinAllowed(who.role, who.quota, ctx.vec->pinsIn(who.ns)))
      return ToolResult::fail("you've used all your permanent memories - unpin one first");
    ok = ctx.vec->markPermanent(id);
  }
  else if (action == "unpin")  ok = ctx.vec->markTemporary(id, 720, ctx.nowHours());
  else if (action == "delete") ok = ctx.vec->remove(id);
  else return ToolResult::fail("unknown action (use pin|unpin|delete)");
  if (!ok) return ToolResult::fail("no memory with id " + id);
  return ToolResult::ok(action + " ok (" + id + ")");
}

// Degraded recall when no embedding can be produced (CUM-435): a bounded,
// case-insensitive keyword scan over the LIVE vector store, honoring the SAME
// namespace read boundary the semantic path applies (nsAllowsEntry mirrors the
// engine's nsVisible). Clearly labelled so the model never mistakes it for
// semantic recall. Returns ok (degraded), never a bare failure, so a transient
// embed outage no longer reads as "empty memory".
// The keyword scan both degraded paths share (the search tool's fallback and
// automatic recall): live entries inside the read boundary and not expired, plus
// the facts waiting in the pending queue, ranked by distinct keyword hits; the
// best k come back as "[NN%] content" or the pending tag.
//
// It copies nothing from the store (it runs while embeddings are down, often on a
// device already short of memory): one forEachEntry pass keeps the kLexScanMax most
// important entries as 8-byte (importance, index) slots, each is scored in place,
// the queue is read by reference, and only the k returned hits get a text.

// One slot of the scan order: an entry's importance and its storage index.
struct LexSlot {
  float    importance;
  uint32_t index;
};
// Scan order: importance desc, storage order within a tie (getAll()'s order).
bool lexSlotBefore(const LexSlot& a, const LexSlot& b) {
  if (a.importance != b.importance) return a.importance > b.importance;
  return a.index < b.index;
}
using LexSlots = std::vector<LexSlot, WorkingAllocator<LexSlot>>;

// The kLexScanMax most important entries, in scan order, through a bounded heap whose
// front is the last slot in scan order (the one a better entry replaces).
LexSlots lexScanOrder(const VectorMemory& vec) {
  LexSlots slots;
  slots.reserve(std::min((size_t)vec.size(), kLexScanMax));
  vec.forEachEntry([&slots](size_t i, const VectorMemory::EntryView& e) {
    // A NaN importance (a corrupt stored blob) ranks as 0, so the order stays strict.
    const LexSlot s{e.importance == e.importance ? e.importance : 0.0f, (uint32_t)i};
    if (slots.size() < kLexScanMax) {
      slots.push_back(s);
      std::push_heap(slots.begin(), slots.end(), lexSlotBefore);
    } else if (lexSlotBefore(s, slots.front())) {
      std::pop_heap(slots.begin(), slots.end(), lexSlotBefore);
      slots.back() = s;
      std::push_heap(slots.begin(), slots.end(), lexSlotBefore);
    }
  });
  std::sort_heap(slots.begin(), slots.end(), lexSlotBefore);
  return slots;
}

// One hit before its text is built: keyword hits, its position in the scan (live
// entries in scan order, then the queue oldest first) and its content, by address
// (valid while the caller holds the memory lock). pct < 0 marks a queued fact.
struct LexHit {
  int                score;
  uint32_t           seq;
  const std::string* content;
  int                pct;
};
// Rank: more keyword hits first, scan position within a tie (the stable order).
bool lexHitBefore(const LexHit& a, const LexHit& b) {
  if (a.score != b.score) return a.score > b.score;
  return a.seq < b.seq;
}
using LexHits = std::vector<LexHit, WorkingAllocator<LexHit>>;

// Keep the k best hits in rank order; `best` has room for k + 1.
void keepBest(LexHits& best, size_t k, const LexHit& h) {
  if (best.size() == k && !lexHitBefore(h, best.back())) return;
  best.insert(std::upper_bound(best.begin(), best.end(), h, lexHitBefore), h);
  if (best.size() > k) best.pop_back();
}

std::string lexHitText(const LexHit& h) {
  char tag[32];
  const int n = h.pct < 0 ? snprintf(tag, sizeof(tag), "[pending, not stored yet] ")
                          : snprintf(tag, sizeof(tag), "[%d%%] ", h.pct);
  std::string t;
  t.reserve((size_t)n + h.content->size());
  t.append(tag, (size_t)n);
  t.append(*h.content);
  return t;
}

std::vector<std::string> lexicalHits(const VectorMemory& vec, const PendingEmbeds* pending,
                                     const LexQuery& lq, const std::vector<std::string>& allow,
                                     uint32_t nowH, size_t k) {
  std::vector<std::string> out;
  if (k == 0) return out;
  LexHits best;
  best.reserve(std::min(k, kLexScanMax + (pending ? pending->size() : 0)) + 1);
  uint32_t seq = 0;
  for (const LexSlot& s : lexScanOrder(vec)) {
    const VectorMemory::EntryView e = vec.entryAt(s.index);
    ++seq;
    if (!nsAllowsEntry(e.ns, allow)) continue;   // never widen the read boundary
    if (entryExpired(e, nowH)) continue;         // same query-time TTL as semantic search
    const int sc = lexCoverage(e.content, lq);   // distinct words matched, in place
    if (sc > 0) keepBest(best, k, LexHit{sc, seq, &e.content, (int)(s.importance * 100)});
  }
  // Facts written during the outage wait in the queue; the same read boundary
  // applies, and each says it is not stored yet.
  if (pending)
    pending->forEachEntry([&](const PendingEmbed& p) {
      ++seq;
      if (!nsAllowsEntry(p.ns, allow)) return;
      const int sc = lexCoverage(p.content, lq);
      if (sc > 0) keepBest(best, k, LexHit{sc, seq, &p.content, -1});
    });
  out.reserve(best.size());
  for (const LexHit& h : best) out.push_back(lexHitText(h));
  return out;
}

ToolResult lexicalSearchFallback(const MemoryContext& ctx, const std::string& query,
                                 int k, const Principal& who, const std::string& reasonWords) {
  const std::string label = "keyword match, embeddings unavailable: " + reasonWords;
  const LexQuery lq = lexParse(query);   // the shared word rule (CUM-515)
  const auto found = lexicalHits(*ctx.vec, ctx.pending, lq, readSetFor(who),
                                 ctx.nowHours ? ctx.nowHours() : 0, (size_t)k);
  if (found.empty()) return ToolResult::ok("No memories matched (" + label + ")." + commonWordsHint(lq));
  std::string out;
  for (const auto& f : found) out += "- " + f + "\n";
  return ToolResult::ok("Found " + std::to_string(found.size()) + " memories (" + label + "):\n" + out);
}

// ---- memory.search ----------------------------------------------------------
ToolResult doSearch(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  // A revoked (Unknown) principal keeps its namespace - the data is retained
  // for the admin - but it may no longer READ it. Gating only writes would let
  // a removed tenant keep pulling its history out of the device.
  if (!who.perms().readOwn)
    return ToolResult::fail("this conversation isn't approved to use memory");
  std::string query = strArg(a, "query");
  if (query.empty()) return ToolResult::fail("missing 'query'");
  if (!ctx.vec || !ctx.embed) return ToolResult::fail("memory engine unavailable");
  double kNum;
  int k = numArg(a, "n_results", kNum) ? (int)std::lround(kNum)
        : (ctx.cfg ? ctx.cfg->retrievalCount : 5);
  if (k < 1) k = 1;
  EmbedTry t = tryEmbed(ctx, ctx.vec->dims(), query);
  // Degrade instead of hard-failing: a keyword scan over the same rows + the same
  // read boundary, clearly labelled, so recall is not simply "empty" (CUM-435).
  // While embeddings are parked it goes straight here, with no network call.
  if (t.vec.empty()) {
    std::string w = embedFailWords(t.kind, t.detail);
    return lexicalSearchFallback(ctx, query, k, who, w.empty() ? std::string("unavailable") : w);
  }
  const std::vector<int8_t>& qv = t.vec;

  auto hits = ctx.vec->search(qv, k, ctx.nowHours(), readSetFor(who));   // TTL + v3.7.0 read boundary
  // Apply the user/model relevance threshold (similarity = 1 - distance).
  float thr = ctx.cfg ? ctx.cfg->relevanceThreshold : 0.0f;
  std::string out;
  int n = 0;
  for (const auto& h : hits) {
    if ((1.0f - h.distance) < thr) continue;
    out += "- [" + std::to_string((int)(h.importance * 100)) + "%] " + h.content + "\n";
    n++;
  }
  if (n == 0) return ToolResult::ok("No relevant memories found.");
  return ToolResult::ok("Found " + std::to_string(n) + " memories:\n" + out);
}

// ---- memory.archive (search/restore/list the SD cold store) -----------------
// TTL-expired memories are MOVED to the archive (embedding preserved) instead of
// being deleted, but ONLY on a device with an SD card - so this tool is registered
// only when an archive is bound, and every action re-checks that the card is still
// present (a card pulled mid-run refuses cleanly). Archived entries are invisible to
// normal recall/search; this is the one surface that reaches them. Everything stays
// inside the caller's namespace boundary (readSetFor) - a member searches/restores
// only its own archived memories.
// Format a list of archived entries as "- [NN%] content (id ...)" bullets.
static std::string archiveBullet(const std::string& content, float importance,
                                 const std::string& id) {
  return "- [" + std::to_string((int)(importance * 100)) + "%] " + content +
         " (id " + id + ")\n";
}

// Keyword fallback for the archive (CUM-435): same contract as the live-store
// fallback (bounded, clearly labelled). getAll already applies the caller's read
// set, so the namespace boundary is preserved.
ToolResult lexicalArchiveFallback(const MemoryContext& ctx, const std::string& query,
                                  int k, const Principal& who, const std::string& eerr) {
  const std::string label = "keyword match, embeddings unavailable: " +
                            embedFailLabelReason(eerr);
  const LexQuery lq = lexParse(query);   // the shared word rule (CUM-515)
  std::vector<std::pair<int, std::string>> found;
  size_t scanned = 0;
  for (const auto& e : ctx.archive->getAll(readSetFor(who))) {   // already ns-scoped
    if (++scanned > kLexScanMax) break;
    const int sc = lexCoverage(e.content, lq);
    if (sc <= 0) continue;
    found.push_back({sc, archiveBullet(e.content, e.importance, e.id)});
  }
  std::stable_sort(found.begin(), found.end(),
                   [](const std::pair<int, std::string>& x,
                      const std::pair<int, std::string>& y) { return x.first > y.first; });
  std::string out;
  int n = 0;
  for (const auto& f : found) { if (n >= k) break; out += f.second; n++; }
  if (n == 0)
    return ToolResult::ok("No matching archived memories (" + label + ")." + commonWordsHint(lq));
  return ToolResult::ok("Found " + std::to_string(n) + " archived memories (" + label +
                        "; restore one with action=restore, id=...):\n" + out);
}

ToolResult doArchiveList(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  double num;
  int limit = numArg(a, "limit", num) ? (int)std::lround(num) : 10;
  if (limit < 1) limit = 1;
  auto all = ctx.archive->getAll(readSetFor(who));   // FIFO: oldest first, newest last
  if (all.empty()) return ToolResult::ok("The archive holds no memories for you yet.");
  std::string out;
  int shown = 0;
  for (auto it = all.rbegin(); it != all.rend() && shown < limit; ++it, ++shown)   // newest first
    out += archiveBullet(it->content, it->importance, it->id);
  return ToolResult::ok("Archived memories (" + std::to_string(shown) + " of " +
                        std::to_string((int)all.size()) + "):\n" + out);
}

ToolResult doArchiveSearch(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  if (!ctx.embed) return ToolResult::fail("memory engine unavailable");
  std::string query = strArg(a, "query");
  if (query.empty()) return ToolResult::fail("missing 'query'");
  double num;
  int k = numArg(a, "n_results", num) ? (int)std::lround(num)
        : (ctx.cfg ? ctx.cfg->retrievalCount : 5);
  if (k < 1) k = 1;
  std::string eerr;
  std::vector<int8_t> qv = embedForStore(ctx, ctx.archive->dims(), query, eerr);
  if (qv.empty()) return lexicalArchiveFallback(ctx, query, k, who, eerr);
  auto hits = ctx.archive->search(qv, k, readSetFor(who));
  float thr = ctx.cfg ? ctx.cfg->relevanceThreshold : 0.0f;
  std::string out;
  int n = 0;
  for (const auto& h : hits) {
    if ((1.0f - h.distance) < thr) continue;
    out += archiveBullet(h.content, h.importance, h.id);
    n++;
  }
  if (n == 0) return ToolResult::ok("No matching archived memories.");
  return ToolResult::ok("Found " + std::to_string(n) + " archived memories "
                        "(restore one with action=restore, id=...):\n" + out);
}

// Resolve the archived entry to restore: explicit id, else the nearest match to a
// query (only if genuinely close, so a vague query can't grab an unrelated memory).
static std::string resolveRestoreId(const MemoryContext& ctx, JsonObjectConst a,
                                    const Principal& who) {
  std::string id = strArg(a, "id");
  if (!id.empty()) return id;
  std::string q = strArg(a, "query");
  if (q.empty() || !ctx.embed) return std::string();
  std::string qerr;
  std::vector<int8_t> qv = embedForStore(ctx, ctx.archive->dims(), q, qerr);
  if (qv.empty()) return std::string();   // restore-by-id still works; refuses cleanly
  auto hits = ctx.archive->search(qv, 1, readSetFor(who));
  if (!hits.empty() && (1.0f - hits[0].distance) >= 0.55f) return hits[0].id;
  return std::string();
}

ToolResult doArchiveRestore(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  if (!who.perms().writeOwn)
    return ToolResult::fail("this conversation isn't approved to store memories");
  std::string id = resolveRestoreId(ctx, a, who);
  if (id.empty()) return ToolResult::fail("identify the memory to restore by 'id' or a 'query'");
  // Visibility BEFORE anything else - the refusal is identical to "not found" so a
  // probe can't confirm another principal's archived id exists.
  if (!ctx.archive->idVisible(id, readSetFor(who)))
    return ToolResult::fail("no archived memory with id " + id);
  // Quota gate BEFORE taking the entry out, so a refusal never strands it out of both
  // stores. countIn is the live store's count for this namespace.
  const Quota qta = effectiveQuota(who.role, who.quota);
  if (qta.maxVectors && ctx.vec->countIn(who.ns) >= qta.maxVectors)
    return ToolResult::fail("this conversation has reached its memory limit (" +
                            std::to_string(qta.maxVectors) +
                            ") - remove a live memory before restoring one");
  VecEntry e;
  if (!ctx.archive->take(id, e, readSetFor(who)))
    return ToolResult::fail("no archived memory with id " + id);
  // Bring it back to LIFE: reset the TTL clock to now and lift importance above the
  // prune floor so the freshly-restored fact isn't re-expired on the next dream.
  // Pinned/creator entries never expire, so nothing pinned is ever in the archive;
  // a restored entry starts as an ordinary temporary memory.
  e.createdAtHours = ctx.nowHours();
  e.permanentFlag = false;
  if (e.importance < 0.5f) e.importance = 0.5f;
  e.ttlHours = ttlHoursFromName(strArg(a, "ttl", "").c_str());
  e.ns = who.ns;   // stays inside the caller's boundary (id was already visible to it)
  std::string note;
  // Reuse the write rails (ttl clamp + pin budget). maxVectors was checked above,
  // before the take, so this only clamps ttl here.
  if (const char* refusal = applyWriteQuotas(ctx, who, e, note)) {
    ctx.archive->archive(e, ctx.nowHours());   // never lose the entry: put it back
    return ToolResult::fail(refusal);
  }
  ctx.vec->add(e, /*dedup=*/false);   // it is a known-unique restored fact
  return ToolResult::ok("restored \"" + e.content + "\" to live memory" + note);
}

ToolResult doArchive(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  if (!ctx.archive) return ToolResult::fail("memory archive unavailable");
  if (!ctx.archiveAvailable())
    return ToolResult::fail("the memory archive lives on the SD card; no card is present");
  if (!who.perms().readOwn)
    return ToolResult::fail("this conversation isn't approved to use memory");
  std::string action = strArg(a, "action", "search");
  if (action == "list")    return doArchiveList(ctx, a, who);
  if (action == "search")  return doArchiveSearch(ctx, a, who);
  if (action == "restore") return doArchiveRestore(ctx, a, who);
  return ToolResult::fail("unknown action (use search|restore|list)");
}

// ---- memory.config (the memory-config tool) ---------------------------------
ToolResult doConfig(const MemoryContext& ctx, JsonObjectConst a) {
  if (!ctx.cfg) return ToolResult::fail("config unavailable");
  MemConfig& c = *ctx.cfg;
  std::string action = strArg(a, "action", "view");
  if (action == "view") {
    char buf[448];
    // W11: max_vectors is the CONFIGURED knob; the EFFECTIVE cap is tier-clamped
    // (flash-degraded devices cap far lower) - reporting only the knob misled.
    snprintf(buf, sizeof(buf),
             "retrieval_count=%d relevance_threshold=%.2f decay_factor=%.2f "
             "max_context_bytes=%d max_vectors=%d (configured; the effective "
             "cap depends on storage - device.status memory.maxVectors is the "
             "live number) recency_half_life_hours=%d mmr_lambda=%.2f",
             c.retrievalCount, c.relevanceThreshold, c.decayFactor, c.maxContextBytes,
             c.maxVectors, c.recencyHalfLifeHours, c.mmrLambda);
    return ToolResult::ok(buf);
  }
  if (action == "update") {
    int applied = 0;
    double num;
    // Accept int- OR float-encoded numbers for every field (JSON has no int/
    // float distinction; a provider may send "20" or "20.0").
    if (numArg(a, "retrieval_count", num))     { c.setRetrievalCount((int)std::lround(num)); applied++; }
    if (numArg(a, "max_context_bytes", num))   { c.setMaxContextBytes((int)std::lround(num)); applied++; }
    if (numArg(a, "max_vectors", num))         { c.setMaxVectors((int)std::lround(num)); applied++; }
    if (numArg(a, "relevance_threshold", num)) { c.setRelevanceThreshold((float)num); applied++; }
    if (numArg(a, "decay_factor", num))        { c.setDecayFactor((float)num); applied++; }
    if (numArg(a, "recency_half_life_hours", num)) { c.setRecencyHalfLifeHours((int)std::lround(num)); applied++; }
    if (numArg(a, "mmr_lambda", num))          { c.setMmrLambda((float)num); applied++; }
    if (!applied) return ToolResult::fail("no valid config field to update");
    return ToolResult::ok("updated " + std::to_string(applied) + " field(s) (clamped to range)");
  }
  return ToolResult::fail("unknown action (use view|update)");
}

// ---- memory.episodic (read_recent / filtered blob reads) --------------------
// A ranked page keeps only its best rows: say when more matched, so a short
// page is not read as everything the history holds. Adding words widens an
// any-word search, so the advice is the narrowing moves: one distinctive word
// (which pages newest-first through every match) or a time window.
std::string unshownNote(int matched, int shown) {
  if (matched <= shown) return std::string();
  return "[" + std::to_string(matched - shown) +
         " more matching rows in this range are not shown - raise limit, search one "
         "distinctive word, or narrow with since_hours/before_hours]\n";
}

// Read-only recall over the "everything that happened" store: past user/assistant
// messages, tool outputs, media artifacts. Filter by kind/session/text; newest
// first, capped. Complements memory.search (semantic) with literal/temporal recall.
ToolResult doEpisodic(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  if (!who.perms().readOwn)
    return ToolResult::fail("this conversation isn't approved to use memory");
  if (!ctx.episodic) return ToolResult::fail("episodic store unavailable");
  MsgQuery q;
  double num;
  if (numArg(a, "limit", num)) q.limit = (int)std::lround(num);
  if (q.limit < 1) q.limit = 1;
  if (q.limit > 100) q.limit = 100;
  std::string s = strArg(a, "session");
  if (!s.empty()) q.sessionId = s;
  // v3.7.0: the OWNER reads every session (their device, their history); any
  // other principal is confined to its own conversation plus the device's
  // "system" timeline (boots/OTA/mode - device facts, not anyone's words).
  // A `session` argument can only NARROW inside that set, never escape it.
  if (!who.owner) {
    q.sessionAllow.push_back(who.ns.empty() ? std::string("\x01none") : sessionForNs(who.ns));
    q.sessionAllow.push_back("system");
  }
  std::string t = strArg(a, "text");
  if (!t.empty()) q.textContains = t;
  // The store applies the shared word rule (lexical.h, CUM-515); its shape here
  // lets the answer say WHY nothing matched and whether the page is ranked.
  const LexShape shape = lexShape(t);
  if (shape == LexShape::CommonOnly)
    return ToolResult::ok(
        "No matching episodic messages: every word in that search is a common word "
        "(like \"the\", \"my\" or \"what\") that search ignores. Search again with a "
        "distinctive word from the conversation.");
  const bool ranked = shape == LexShape::Ranked;
  std::string kindStr = strArg(a, "kind");
  MsgKind k;
  if (!kindStr.empty() && kindFromName(kindStr.c_str(), k)) { q.haveKind = true; q.kind = k; }
  // Time window, RELATIVE hours-ago (owner ask 2026-07-24: "what happened between
  // T1 and T2" was unanswerable - the store's absolute-tsHours bounds were
  // implemented + tested underneath but never exposed here). since_hours=24 =>
  // rows from the last day; before_hours=2 excludes the last two hours.
  const uint32_t nowH = ctx.nowHours ? ctx.nowHours() : 0;
  if (numArg(a, "since_hours", num) && num > 0 && nowH > 0)
    q.sinceHours = nowH > (uint32_t)num ? nowH - (uint32_t)num : 1;
  if (numArg(a, "before_hours", num) && num > 0 && nowH > 0)
    q.beforeHours = nowH > (uint32_t)num ? nowH - (uint32_t)num : 1;

  // v4.0.0: the model-facing search reaches PAST the boot-scan index (the rest
  // of the history is on the card, and "I don't remember" was wrong, not honest).
  // The per-call budget lives in the store; this side just pages.
  q.coldScan = true;
  q.before = strArg(a, "before");
  EpiQueryInfo qi;
  auto rows = ctx.episodic->query(q, &qi);
  // The floor is part of the answer even when nothing matched.
  std::string floorNote;
  if (qi.olderExists) {
    floorNote = "\n[searched back to " +
                AppendLogEpisodicStore::civilDate(qi.searchedToDay) +
                "; older history exists";
    if (!qi.nextBefore.empty())
      floorNote += " - to continue, call this again with before=\"" + qi.nextBefore + "\"";
    floorNote += "]";
  }
  if (rows.empty())
    return ToolResult::ok("No matching episodic messages." + floorNote);
  std::string out;
  for (const auto& m : rows) {
    // 600-char excerpt (was 200 - the owner's "even short messages are trimmed":
    // the STORE is full-text, but every row the MODEL read back was cut at 200,
    // so it 'forgot' the tail of anything it was told).
    // The SESSION rides every row (B2 field find): the tool searches ALL chats
    // by design, and without the source chat the model mis-attributed another
    // channel's words to "this chat" (live 2026-07-27).
    out += "- [" + std::string(kindName(m.kind)) + "/" + m.role + "@" + m.sessionId + "] " +
           (m.text.size() > 600 ? m.text.substr(0, 600) + "..." : m.text);
    if (!m.blobPath.empty()) out += " (" + m.blobPath + ")";
    out += "\n";
  }
  if (ranked) out += unshownNote(qi.matched, (int)rows.size());
  // While the index is still catching up with the card after a restart, the
  // count is a floor, not the size of the history: say so rather than present
  // the boot window as everything that was ever said.
  const bool complete = ctx.episodic->indexComplete();
  return ToolResult::ok("Episodic (" + std::to_string(rows.size()) + " of " +
                        std::to_string(ctx.episodic->messageCount()) + " indexed" +
                        (ranked ? ", best match first" : "") +
                        (complete ? "" : "; older history on the card is not in this count") +
                        "):\n" + out + floorNote);
}

// ---- memory.scratchpad ------------------------------------------------------
// The persist hook's answer rides the change result: "kept until restart only"
// when saving failed, so the model never tells the owner a RAM-only note
// survives a restart.
std::string scratchSaved(const MemoryContext& ctx, std::string out) {
  if (ctx.persistScratch && !ctx.persistScratch()) out += kScratchRamOnlyNote;
  return out;
}

ToolResult doScratch(const MemoryContext& ctx, JsonObjectConst a) {
  if (!ctx.scratch) return ToolResult::fail("scratchpad unavailable");
  Scratchpad& sp = *ctx.scratch;
  std::string action = strArg(a, "action", "view");

  if (action == "view") {
    std::string out;
    sp.appendPromptBlock(out);
    // W11: fill vs caps as numbers - the caps were only discoverable by
    // FAILING an add.
    out += "\n[capacity: short " + std::to_string(sp.count(Tier::Short)) + "/" +
           std::to_string(kScratchTierItems) + ", mid " +
           std::to_string(sp.count(Tier::Mid)) + "/" +
           std::to_string(kScratchTierItems) + ", long " +
           std::to_string(sp.count(Tier::Long)) + "/" +
           std::to_string(kScratchTierItems) + "; item cap " +
           std::to_string(kScratchItemMax) + " chars]";
    return ToolResult::ok(out);
  }
  if (action == "set_active") {
    sp.setActiveTask(strArg(a, "text"));
    return ToolResult::ok(scratchSaved(ctx, "active task set"));
  }
  if (action == "add") {
    Tier t = tierFromStr(strArg(a, "tier", "short"));
    if (!sp.add(t, strArg(a, "text")))
      return ToolResult::fail("could not add (tier full or empty item)");
    return ToolResult::ok(scratchSaved(ctx, "added to " + strArg(a, "tier", "short")));
  }
  if (action == "replace") {
    Tier t = tierFromStr(strArg(a, "tier", "short"));
    std::vector<std::string> items;
    JsonArrayConst arr = a["items"].as<JsonArrayConst>();
    for (JsonVariantConst v : arr)
      if (v.is<const char*>()) items.push_back(v.as<const char*>());
    int kept = sp.replace(t, items);
    return ToolResult::ok(scratchSaved(ctx, "replaced " + strArg(a, "tier", "short") + " with " +
                                                std::to_string(kept) + " item(s)"));
  }
  if (action == "clear") {
    std::string tier = strArg(a, "tier");
    if (tier.empty()) { sp.clearAll(); return ToolResult::ok(scratchSaved(ctx, "scratchpad cleared")); }
    sp.clear(tierFromStr(tier));
    return ToolResult::ok(scratchSaved(ctx, tier + " cleared"));
  }
  return ToolResult::fail("unknown action (use view|set_active|add|replace|clear)");
}

}  // namespace

// Count what a write or update did (MemoryContext::ops; no-op when unbound). The
// result's outcome code says which way it went (every doWrite/doUpdate return is
// withCode'd); the duplicate-prefix text match stays as the fallback for a result
// that carries no code, so the reply and the counter can never drift apart. A
// QUEUED write counts as queued, never as stored: "ok" must not report a fact that
// only waits for its embedding as landed (lane M's honest dream row + lane P's queue).
static ToolResult countWrite(MemOpCounters* ops, ToolResult r) {
  if (!ops) return r;
  if (!r.code.empty()) {
    if (!r.success) ops->failed++;
    else if (r.code == kMemOutcomeQueued) ops->queued++;
    else if (r.code == kMemOutcomeDuplicate) ops->dupSkipped++;
    else ops->added++;
    return r;
  }
  if (!r.success) ops->failed++;
  else if (r.output.rfind(kDuplicateWrite, 0) == 0) ops->dupSkipped++;
  else ops->added++;
  return r;
}
static ToolResult countUpdate(MemOpCounters* ops, ToolResult r) {
  if (!ops) return r;
  if (r.code == kMemOutcomeQueued && r.success) ops->queued++;
  else (r.success ? ops->updated : ops->failed)++;
  return r;
}

static void registerOptionalMemoryTools(ToolRegistry& reg, const MemoryContext& ctx);

void registerMemoryTools(ToolRegistry& reg, const MemoryContext& ctx) {
  reg.add("memory.write",
          "Store a fact in long-term associative memory so you can recall it later. If "
          "embeddings are down the fact is queued on the device and stored later; the result "
          "says which.",
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
            return countWrite(ctx.ops, doWrite(ctx, a, who));
          },
          R"({"type":"object","properties":{"content":{"type":"string"},)"
          R"("importance":{"type":"number"},"permanent":{"type":"boolean"},)"
          R"("ttl":{"type":"string","enum":["session","days","weeks","months","permanent"]},)"
          R"("source":{"type":"string"}},"required":["content"]})");

  reg.add("memory.search",
          "Search your long-term memory for facts relevant to a query.",
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return doSearch(ctx, a, who); },
          R"({"type":"object","properties":{"query":{"type":"string"},)"
          R"("n_results":{"type":"integer"}},"required":["query"]})");

  reg.add("memory.update",
          "Update a fact that CHANGED: give the new 'content' plus a short description of the 'old' fact (or its id). The closest matching memory is removed and the new one stored, so you don't leave a stale duplicate. Prefer this over memory.write when a preference/detail changes.",
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) {
            return countUpdate(ctx.ops, doUpdate(ctx, a, who));
          },
          R"({"type":"object","properties":{"content":{"type":"string"},"old":{"type":"string"},)"
          R"("id":{"type":"string"},"importance":{"type":"number"},"permanent":{"type":"boolean"},)"
          R"("ttl":{"type":"string","enum":["session","days","weeks","months","permanent"]}},"required":["content"]})");

  reg.add("memory.pin",
          "Pin a memory (never forgotten), unpin it (let it decay/expire again), or delete it. Identify it by exact content or id. Pinning only stops decay; it does not put a memory into every prompt.",
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return doPin(ctx, a, who); },
          R"({"type":"object","properties":{"action":{"type":"string","enum":["pin","unpin","delete"]},)"
          R"("content":{"type":"string"},"id":{"type":"string"}},"required":["action"]})");

  reg.add("memory.config",
          "View or tune how memory is retrieved (retrieval_count, relevance_threshold, decay_factor, max_context_bytes, max_vectors, recency_half_life_hours, mmr_lambda).",
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
            // These knobs are DEVICE-GLOBAL, not per-person: max_vectors drives
            // eviction across every namespace, and relevance_threshold decides
            // what anyone recalls. A member setting max_vectors=1 would evict
            // the admin's memory. Viewing is harmless; changing is not.
            if (strArg(a, "action") == "update" && !who.perms().manageTenants)
              return nimbus::orch::ToolResult::fail(
                  "only an admin can change how this device's memory works");
            if (!who.perms().readOwn)
              return nimbus::orch::ToolResult::fail(
                  "this conversation isn't approved to use memory");
            return doConfig(ctx, a);
          },
          R"({"type":"object","properties":{"action":{"type":"string","enum":["view","update"]},)"
          R"("retrieval_count":{"type":"integer"},"relevance_threshold":{"type":"number"},)"
          R"("decay_factor":{"type":"number"},"max_context_bytes":{"type":"integer"},)"
          R"("max_vectors":{"type":"integer"},"recency_half_life_hours":{"type":"integer"},"mmr_lambda":{"type":"number"}},"required":["action"]})");

  reg.add("memory.scratchpad",
          "Read or edit your working scratchpad (active task + short/mid/long-term goal tiers).",
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) -> nimbus::orch::ToolResult {
            // There is ONE scratchpad on the device and it is injected into
            // EVERY turn's prompt. A member reading it sees the owner's goals;
            // a member writing it plants text in the owner's next prompt, which
            // is a prompt-injection channel with an audience of one - the owner.
            // Until it is per-principal, it is admin-only.
            if (!who.perms().manageTenants)
              return nimbus::orch::ToolResult::fail(
                  "the scratchpad belongs to this device's admin");
            return doScratch(ctx, a);
          },
          R"({"type":"object","properties":{"action":{"type":"string",)"
          R"("enum":["view","set_active","add","replace","clear"]},)"
          R"("tier":{"type":"string","enum":["short","mid","long"]},)"
          R"("text":{"type":"string"},"items":{"type":"array","items":{"type":"string"}}},)"
          R"("required":["action"]})");
  // W14: wholly admin-only -> not ADVERTISED to a member/guest turn (the
  // handler above still refuses; this only stops the prompt from offering it).
  reg.setAdminOnly("memory.scratchpad");

  registerOptionalMemoryTools(reg, ctx);
}

// Tools that exist only when their store is bound.
static void registerOptionalMemoryTools(ToolRegistry& reg, const MemoryContext& ctx) {
  // memory.episodic is registered only when an episodic store is bound (device).
  if (ctx.episodic) {
    reg.add("memory.episodic",
            "Search your episodic history (past messages, tool outputs, media) by "
            "kind, session, text, or a time window (since_hours/before_hours = "
            "hours ago). Session 'system' holds the DEVICE EVENT TIMELINE - boots "
            "(with reset reason), firmware updates (with release notes), mode "
            "switches, storage changes, nightly-dream outcomes - so questions like "
            "'did you restart?' or 'what happened today?' are answerable from data. "
            "The search reaches months back, beyond what is loaded at startup, so "
            "SEARCH IT before saying you do not remember something. `text` finds rows "
            "containing ANY of its words (common words like 'the' or 'my' are ignored, "
            "'boats' also finds 'boat') and ranks rows holding more of them first, so "
            "one search can carry a few likely words. It answers one "
            "page at a time: when the result ends with a 'to continue' token, pass "
            "it back as `before` to read further into the past.",
            [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return doEpisodic(ctx, a, who); },
            R"({"type":"object","properties":{)"
            R"("kind":{"type":"string","enum":["message","tool_output","llm_response","file","image","audio","transcript","log"]},)"
            R"("session":{"type":"string"},"text":{"type":"string"},"limit":{"type":"integer"},)"
            R"("since_hours":{"type":"integer"},"before_hours":{"type":"integer"},)"
            R"("before":{"type":"string"}}})");
  }

  // memory.archive is registered only when a cold store is bound - which the device
  // does ONLY when an SD card is present (CUM-225). It reaches memories that reached
  // their TTL and were moved to the archive instead of deleted, so they can be found
  // or restored later without paying to re-embed them.
  if (ctx.archive) {
    reg.add("memory.archive",
            "Search or restore memories that expired (reached their time-to-live) and "
            "were moved to the archive on the SD card instead of being deleted. "
            "action=search finds archived facts by meaning; action=list shows the most "
            "recently archived; action=restore brings one back into live memory (by id, "
            "or the closest match to a query). Restored facts count against your normal "
            "memory limit. Use this before saying you have forgotten something the owner "
            "told you a long time ago.",
            [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return doArchive(ctx, a, who); },
            R"({"type":"object","properties":{)"
            R"("action":{"type":"string","enum":["search","restore","list"]},)"
            R"("query":{"type":"string"},"id":{"type":"string"},"n_results":{"type":"integer"},)"
            R"("limit":{"type":"integer"},)"
            R"("ttl":{"type":"string","enum":["session","days","weeks","months","permanent"]}},)"
            R"("required":["action"]})");
  }
}

// ---- degraded automatic recall ------------------------------------------------
const char kRecallLowMemoryTail[] = "; keyword recall skipped, the device is low on memory";

RecallOut lexicalRecall(const VectorMemory& vec, const PendingEmbeds* pending, const std::string& query,
                        const RecallScope& scope, const std::string& cause) {
  RecallOut r;
  if (cause.empty()) r.degraded = "unavailable";
  else r.degraded = cause;   // one copy (a ternary made a const temporary, then a second)
  // Below the recall floors nothing is scanned: the prompt line says recall is
  // keyword-only and that this turn's keyword recall was skipped.
  if (scope.lowMemory) {
    r.degraded += kRecallLowMemoryTail;
    return r;
  }
  const int k = scope.k > 0 ? scope.k : 5;
  r.hits = lexicalHits(vec, pending, lexParse(query), scope.nsAllow, scope.nowHours, (size_t)k);
  return r;
}

// ---- the pending-embed drain ---------------------------------------------------
const char* embedDrainStepName(EmbedDrainStep s) {
  switch (s) {
    case EmbedDrainStep::Idle:       return "idle";
    case EmbedDrainStep::Parked:     return "parked";
    case EmbedDrainStep::Stored:     return "stored";
    case EmbedDrainStep::Duplicate:  return "duplicate";
    case EmbedDrainStep::Dropped:    return "dropped";
    case EmbedDrainStep::Refused:    return "refused";
    case EmbedDrainStep::Failed:     return "failed";
    case EmbedDrainStep::NotDurable: return "not-durable";
  }
  return "?";
}

namespace {
std::string excerpt80(const std::string& s) {
  if (s.size() <= 80) return s;
  return s.substr(0, (size_t)utf8CapLen(s.c_str(), (int)s.size(), 80)) + "...";
}

void runLocked(const EmbedDrainDeps& d, const std::function<void()>& fn) {
  if (d.withLock) d.withLock(fn);
  else fn();
}

// Pop `id` once the vector store is durable (the crash-safety invariant), then
// persist the queue. Returns false when the vector persist failed: the entry stays.
bool commitDrained(const MemoryContext& ctx, const EmbedDrainDeps& d, const std::string& id) {
  if (d.persistVectors && !d.persistVectors()) return false;
  ctx.pending->pop(id);
  if (ctx.persistPending) ctx.persistPending();
  return true;
}

// The namespace's writer NOW: a member revoked since the write never gets the
// fact stored (their queued text is dropped with them).
bool writerOf(const EmbedDrainDeps& d, const PendingEmbed& p, Principal& who) {
  return d.principalForNs && d.principalForNs(p.ns, who) && who.valid() && who.ns == p.ns &&
         who.perms().writeOwn;
}

void popAndPersist(const MemoryContext& ctx, const std::string& id) {
  ctx.pending->pop(id);
  if (ctx.persistPending) ctx.persistPending();
}

// Embed the queued content (and, for an update by description, the description),
// OUTSIDE the lock: a TLS call, like recall. An empty vec means retry later.
EmbedTry embedQueued(const MemoryContext& ctx, const PendingEmbed& p, std::vector<int8_t>& descVec) {
  EmbedTry t = tryEmbed(ctx, ctx.vec->dims(), p.content);
  if (t.vec.empty() || p.op != PendingOp::Update || !p.oldId.empty() || p.oldDesc.empty()) return t;
  EmbedTry td = tryEmbed(ctx, ctx.vec->dims(), p.oldDesc);
  if (td.vec.empty()) return td;   // keep the old fact: retry the whole update later
  descVec = std::move(td.vec);
  return t;
}

// Under the lock: rebuild the entry with the CURRENT principal's rails, store it,
// and pop it only once the vectors are durable.
EmbedDrainStep storeQueued(const MemoryContext& ctx, const EmbedDrainDeps& d, const PendingEmbed& p,
                      const Principal& who, std::vector<int8_t> vec, const std::vector<int8_t>& descVec) {
  if (!ctx.pending->find(p.id)) return EmbedDrainStep::Idle;   // erased meanwhile
  std::string oldId;
  if (p.op == PendingOp::Update) {
    oldId = resolveUpdateTarget(ctx, who, p.oldId, descVec);
    if (!oldId.empty() && !ctx.vec->idVisible(oldId, readSetFor(who))) oldId.clear();
  }
  VecEntry e;
  std::string note;
  const WriteSpec w = specFromPending(ctx, p);
  if (const char* refusal = prepareWrite(ctx, who, w, e, note, 1 + (oldId.empty() ? 0 : 1))) {
    popAndPersist(ctx, p.id);
    ctx.pending->addNote(p.ns, "[MEMORY] queued mem_write \"" + excerpt80(p.content) +
                                   "\" -> NOT stored (" + refusal + ")");
    if (ctx.onWriteOutcome) ctx.onWriteOutcome(kMemOutcomeRefused);
    return EmbedDrainStep::Refused;
  }
  e.vec = std::move(vec);
  if (!oldId.empty()) ctx.vec->remove(oldId);
  const bool inserted = ctx.vec->add(e);
  // A failed persist keeps the entry; the vector stays in RAM and the next step
  // finds its id stored and only retries the persist.
  if (!commitDrained(ctx, d, p.id)) return EmbedDrainStep::NotDurable;
  if (ctx.onWriteOutcome) ctx.onWriteOutcome(inserted ? kMemOutcomeStored : kMemOutcomeDuplicate);
  ctx.pending->addNote(p.ns, "[MEMORY] queued mem_write \"" + excerpt80(p.content) + "\" -> now " +
                                 (p.op == PendingOp::Update ? "applied" : "stored") + " (" + p.id +
                                 (inserted ? "" : ", it matched an existing memory") + ")");
  return inserted ? EmbedDrainStep::Stored : EmbedDrainStep::Duplicate;
}

// A crash after the vector persist and before the queue persist leaves the fact
// both stored and queued: the replay only pops it, with no embed. True when it
// handled the entry (`step` says how).
bool replayIfStored(const MemoryContext& ctx, const EmbedDrainDeps& d, const PendingEmbed& p,
                    const Principal& who, EmbedDrainStep& step) {
  bool done = false;
  runLocked(d, [&] {
    if (p.op != PendingOp::Write || !ctx.vec->idVisible(p.id, readSetFor(who))) return;
    done = true;
    step = commitDrained(ctx, d, p.id) ? EmbedDrainStep::Duplicate : EmbedDrainStep::NotDurable;
  });
  return done;
}
}  // namespace

EmbedDrainResult drainOne(const MemoryContext& ctx, const EmbedDrainDeps& d) {
  EmbedDrainResult r;
  if (!ctx.pending || !ctx.vec || !ctx.embed) return r;
  PendingEmbed p;
  bool have = false;
  runLocked(d, [&] {
    if (const PendingEmbed* f = ctx.pending->front()) { p = *f; have = true; }
  });
  if (!have) return r;
  r.id = p.id;
  bool parked = false;
  if (ctx.park) underParkLock(ctx, [&] { parked = ctx.park->blocked(ctx.nowMs ? ctx.nowMs() : 0); });
  if (parked) {
    r.step = EmbedDrainStep::Parked;
    return r;
  }
  Principal who;
  if (!writerOf(d, p, who)) {
    runLocked(d, [&] { popAndPersist(ctx, p.id); });
    r.step = EmbedDrainStep::Dropped;
    return r;
  }
  if (replayIfStored(ctx, d, p, who, r.step)) return r;
  std::vector<int8_t> descVec;
  EmbedTry t = embedQueued(ctx, p, descVec);
  if (t.vec.empty()) {
    runLocked(d, [&] {
      ctx.pending->noteFailed(p.id, t.kind);
      if (ctx.persistPending) ctx.persistPending();
    });
    r.step = t.parked ? EmbedDrainStep::Parked : EmbedDrainStep::Failed;
    return r;
  }
  runLocked(d, [&] { r.step = storeQueued(ctx, d, p, who, std::move(t.vec), descVec); });
  return r;
}
}  // namespace orch
}  // namespace nimbus
