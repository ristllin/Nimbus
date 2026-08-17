#include "nimbus/orch/memory_tools.h"

#include <cmath>

#include "nimbus/mem_cap.h"
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

namespace {

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
// The counts come from the STORE, never from a number the caller passes in.
const char* applyWriteQuotas(const MemoryContext& ctx, const Principal& who,
                             VecEntry& e, std::string& note) {
  static std::string refusal;   // the message outlives this call
  const Quota q = effectiveQuota(who.role, who.quota);
  if (q.maxVectors && ctx.vec->countIn(who.ns) >= q.maxVectors) {
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

ToolResult doWrite(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  std::string content = strArg(a, "content");
  if (content.empty()) return ToolResult::fail("missing 'content'");
  if (!ctx.vec || !ctx.embed) return ToolResult::fail("memory engine unavailable");
  std::vector<int8_t> vec = ctx.embed(content);
  if (vec.empty()) return ToolResult::fail("embedding unavailable (provider offline?)");

  VecEntry e;
  e.id = contentId(content);
  e.content = content;
  double imp = 0.5;
  numArg(a, "importance", imp);
  e.importance = (float)imp;
  if (e.importance < 0) e.importance = 0;
  if (e.importance > 1) e.importance = 1;
  e.permanentFlag = a["permanent"].is<bool>() ? a["permanent"].as<bool>() : false;
  e.source = strArg(a, "source", "self");
  e.createdAtHours = ctx.nowHours();
  // ttl class chosen by the model (default weeks when absent/unknown).
  e.ttlHours = ttlHoursFromName(strArg(a, "ttl", "").c_str());
  e.vec = std::move(vec);

  // WRITE RAIL (v3.7.0): the namespace comes from the authenticated caller, not
  // from the model's arguments - a member's turn cannot store into the owner's
  // or another chat's memory by asking. Unattributed callers cannot write at all.
  if (!who.valid()) return ToolResult::fail("memory write needs an identified caller");
  if (!who.perms().writeOwn)
    return ToolResult::fail("this conversation isn't approved to store memories yet");
  e.ns = who.ns;
  std::string note;
  if (const char* refusal = applyWriteQuotas(ctx, who, e, note)) return ToolResult::fail(refusal);
  bool inserted = ctx.vec->add(e);
  return ToolResult::ok(inserted ? ("stored memory " + e.id + note)
                                 : ("duplicate of an existing memory (importance bumped)"));
}

// ---- memory.update (replace/supersede an existing fact) ---------------------
// The fix for "update my coffee to flat white" creating a SECOND vector next to the
// old one: find the memory to replace (by id, or by the closest semantic match to a
// short description of the OLD fact), remove it, and store the new content. If nothing
// close is found it just stores the new fact (never deletes an unrelated memory).
ToolResult doUpdate(const MemoryContext& ctx, JsonObjectConst a, const Principal& who) {
  if (!who.perms().writeOwn)
    return ToolResult::fail("this conversation isn't approved to change memories");
  std::string content = strArg(a, "content");
  if (content.empty()) return ToolResult::fail("missing 'content' (the new fact)");
  if (!ctx.vec || !ctx.embed) return ToolResult::fail("memory engine unavailable");

  // Identify the memory to replace: explicit id, else the nearest match to 'old'/'query'.
  std::string oldId = strArg(a, "id");
  if (oldId.empty()) {
    std::string q = strArg(a, "old");
    if (q.empty()) q = strArg(a, "query");
    if (!q.empty()) {
      std::vector<int8_t> qv = ctx.embed(q);
      if (!qv.empty()) {
        auto hits = ctx.vec->search(qv, 1, 0, readSetFor(who));
        // Only treat it as "the same fact" if it's genuinely close (cosine sim >= 0.55),
        // so a vague description can't delete an unrelated memory.
        if (!hits.empty() && (1.0f - hits[0].distance) >= 0.55f) oldId = hits[0].id;
      }
    }
  }
  // A caller may only replace a memory INSIDE its own boundary - an id from
  // another principal's namespace is refused rather than silently deleted.
  if (!oldId.empty() && !ctx.vec->idVisible(oldId, readSetFor(who)))
    return ToolResult::fail("no such memory");
  const bool removed = !oldId.empty() && ctx.vec->remove(oldId);

  std::vector<int8_t> vec = ctx.embed(content);
  if (vec.empty()) return ToolResult::fail("embedding unavailable (provider offline?)");
  VecEntry e;
  e.ns = who.ns;   // the replacement belongs to the caller, like every write
  e.id = contentId(content);
  e.content = content;
  double imp = 0.6;
  numArg(a, "importance", imp);
  e.importance = (float)(imp < 0 ? 0 : imp > 1 ? 1 : imp);
  e.permanentFlag = a["permanent"].is<bool>() ? a["permanent"].as<bool>() : false;
  e.source = strArg(a, "source", "self");
  e.createdAtHours = ctx.nowHours();
  // ttl class, SAME semantics + default as memory.write (audit 2026-07-24: update
  // silently dropped ttl, so an updated fact fell back to the struct default -
  // a DIFFERENT default than write's - and a permanent fact lost its permanence
  // unless re-flagged).
  e.ttlHours = ttlHoursFromName(strArg(a, "ttl", "").c_str());
  e.vec = std::move(vec);
  // The SAME rails as memory.write. This path also stores when nothing matched
  // ("stored as new" below), which makes it a full write seam - an unrailed one
  // was a way around every ceiling.
  std::string note;
  if (const char* refusal = applyWriteQuotas(ctx, who, e, note)) return ToolResult::fail(refusal);
  ctx.vec->add(e);
  return ToolResult::ok((removed ? ("updated: replaced the prior memory with \"" + content + "\"")
                                 : ("no close match to replace; stored \"" + content + "\" as new"))
                        + note);
}

// ---- memory.pin (pin / unpin / delete a stored memory) ----------------------
// Gives the model parity with the human web UI over its own long-term memory:
// pin (never decay/evict), UNPIN (let it decay/expire again), or delete. Memories
// are keyed by a content hash, so the model can target one by its exact `content`
// (we recompute the id) or by `id` if it kept one from a prior write.
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
  if (!ctx.vec->idVisible(id, readSetFor(who)))
    return ToolResult::fail("no memory with id " + id);
  bool ok;
  if (action == "pin") {
    // A pin outlives every quota - it is exempt from eviction AND prune - so it
    // is budgeted, not free. Guests get none.
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
  std::vector<int8_t> qv = ctx.embed(query);
  if (qv.empty()) return ToolResult::fail("embedding unavailable (provider offline?)");

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
  return ToolResult::ok("Episodic (" + std::to_string(rows.size()) + " of " +
                        std::to_string(ctx.episodic->messageCount()) + " indexed):\n" +
                        out + floorNote);
}

// ---- memory.scratchpad ------------------------------------------------------
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
    return ToolResult::ok("active task set");
  }
  if (action == "add") {
    Tier t = tierFromStr(strArg(a, "tier", "short"));
    if (!sp.add(t, strArg(a, "text")))
      return ToolResult::fail("could not add (tier full or empty item)");
    return ToolResult::ok("added to " + strArg(a, "tier", "short"));
  }
  if (action == "replace") {
    Tier t = tierFromStr(strArg(a, "tier", "short"));
    std::vector<std::string> items;
    JsonArrayConst arr = a["items"].as<JsonArrayConst>();
    for (JsonVariantConst v : arr)
      if (v.is<const char*>()) items.push_back(v.as<const char*>());
    int kept = sp.replace(t, items);
    return ToolResult::ok("replaced " + strArg(a, "tier", "short") + " with " +
                          std::to_string(kept) + " item(s)");
  }
  if (action == "clear") {
    std::string tier = strArg(a, "tier");
    if (tier.empty()) { sp.clearAll(); return ToolResult::ok("scratchpad cleared"); }
    sp.clear(tierFromStr(tier));
    return ToolResult::ok(tier + " cleared");
  }
  return ToolResult::fail("unknown action (use view|set_active|add|replace|clear)");
}

}  // namespace

void registerMemoryTools(ToolRegistry& reg, const MemoryContext& ctx) {
  reg.add("memory.write",
          "Store a fact in long-term associative memory so you can recall it later.",
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return doWrite(ctx, a, who); },
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
          [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return doUpdate(ctx, a, who); },
          R"({"type":"object","properties":{"content":{"type":"string"},"old":{"type":"string"},)"
          R"("id":{"type":"string"},"importance":{"type":"number"},"permanent":{"type":"boolean"},)"
          R"("ttl":{"type":"string","enum":["session","days","weeks","months","permanent"]}},"required":["content"]})");

  reg.add("memory.pin",
          "Pin a memory (never forgotten), unpin it (let it decay/expire again), or delete it. Identify it by exact content or id.",
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
            "SEARCH IT before saying you do not remember something. It answers one "
            "page at a time: when the result ends with a 'to continue' token, pass "
            "it back as `before` to read further into the past.",
            [ctx](ArduinoJson::JsonObjectConst a, const nimbus::orch::Principal& who) { return doEpisodic(ctx, a, who); },
            R"({"type":"object","properties":{)"
            R"("kind":{"type":"string","enum":["message","tool_output","llm_response","file","image","audio","transcript","log"]},)"
            R"("session":{"type":"string"},"text":{"type":"string"},"limit":{"type":"integer"},)"
            R"("since_hours":{"type":"integer"},"before_hours":{"type":"integer"},)"
            R"("before":{"type":"string"}}})");
  }
}

}  // namespace orch
}  // namespace nimbus
