// Ported and adapted from Nuage-Solide src/agent/orchestrator.cpp (Head
// Orchestrator v2 - enqueueSpawn / runTurn parse path) + orch_schema.h.
//
// Portable turn-contract parser. No Arduino; ArduinoJson only (host-safe).

#include "nimbus/orch/turn.h"

#include "nimbus/orch/caps.h"
#include "nimbus/orch/mem_ttl.h"
#include "nimbus/mem_cap.h"

#include <ArduinoJson.h>

#include <cctype>

namespace nimbus {
namespace orch {

namespace {

// Lowercase ASCII in place (matches the device `String::toLowerCase()` on the
// provider field). Provider names are ASCII identifiers; leaving any non-ASCII
// bytes untouched is fine (they never name a valid provider).
std::string toLowerAscii(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// UTF-8-safe byte cap: keep the largest prefix <= maxBytes that ends on a complete
// character boundary. A multi-byte char straddling the cap is dropped whole - a
// split sequence would corrupt the next provider prompt (mem_cap.h rationale).
std::string capUtf8(const std::string& s, int maxBytes) {
  const int len = static_cast<int>(s.size());
  const int keep = utf8CapLen(s.c_str(), len, maxBytes);
  return keep < len ? s.substr(0, static_cast<size_t>(keep)) : s;
}

// Copy a required string field on a spawn item, applying an optional default when
// the wire value is empty, then a UTF-8-safe byte cap. Caller has already checked
// the field is a JSON string.
std::string spawnStr(JsonVariantConst v, const char* dflt, int maxBytes) {
  const char* raw = v.as<const char*>();
  std::string s = raw ? std::string(raw) : std::string();
  if (s.empty() && dflt) s = dflt;
  return capUtf8(s, maxBytes);
}

// Fail helper: clear the output and stamp the error.
bool fail(Turn& out, ParseError& err, ParseError::Code code, std::string detail) {
  out = Turn{};
  err.code = code;
  err.detail = std::move(detail);
  return false;
}

}  // namespace

bool parseTurn(const std::string& json, Turn& out, ParseError& err) {
  out = Turn{};
  err = ParseError{};

  // 1. Deserialize. Any parse failure is JsonError - the device fallback (deliver
  //    the raw string to the user) is CALLER policy, not the parser's job.
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, json);
  if (de) return fail(out, err, ParseError::Code::JsonError, de.c_str());
  if (!doc.is<JsonObjectConst>())
    return fail(out, err, ParseError::Code::NotObject, "top-level value is not an object");
  JsonObjectConst root = doc.as<JsonObjectConst>();

  // 2. reply/memory/ask are REQUIRED strings. Every array is OPTIONAL: absent =>
  //    empty, present-but-not-array => WrongType. This is backward-compatible (old
  //    6-field turns still validate) and forward-flexible: a new turn may omit
  //    spawn/await/device and use mem_write/mem_query/session_ops instead (Q3:
  //    spawning moved to the session tool). Missing string -> MissingField.
  static const char* kStrKeys[] = {"reply", "memory", "ask"};
  for (const char* k : kStrKeys) {
    JsonVariantConst v = root[k];
    if (v.isNull()) return fail(out, err, ParseError::Code::MissingField, k);
    if (!v.is<const char*>())
      return fail(out, err, ParseError::Code::WrongType, std::string(k) + " must be a string");
  }
  // Arrays are TOLERANT throughout (owner field bug 2026-07-16: on Anthropic the
  // wire schema is ADVISORY - its strict grammar budget rejects a contract this
  // size - so the parser is the ONLY enforcement layer, and a single slightly-off
  // array item was failing the whole turn, which leaked the raw JSON to the user
  // via the caller's fallback). Policy: a present-but-not-array field reads as
  // empty (as<JsonArrayConst>() on a non-array iterates zero times below), and a
  // malformed ITEM is dropped, never a whole-turn error. reply/memory/ask above
  // stay required - without those there is genuinely no usable turn.

  // 3. String fields. reply/ask are user-facing and uncapped here (the device
  //    budgets them elsewhere). memory is clamped to kMemModelMax UTF-8-safe so the
  //    parsed Turn is self-consistent with the memory module's cap even before
  //    OrchMemory::setModel runs; the definitive cap + truncation flag live there.
  out.reply  = root["reply"].as<const char*>();
  out.ask    = root["ask"].as<const char*>();
  out.memory = capUtf8(std::string(root["memory"].as<const char*>()), kMemModelMax);

  // 4. spawn[] - TOLERANT per item: a non-object element, or one whose `task` is
  //    missing/non-string/empty, is DROPPED (matches enqueueSpawn: `if (!task[0])
  //    return;`) - never a turn error. Every other field falls back to a default
  //    on copy (spawnStr), so a failover provider that omits `note` or sends a
  //    numeric `model` loses that item at worst, not the owner's reply. Defaults,
  //    provider-lowercasing, and per-field byte caps are applied on copy. The
  //    accepted vector is truncated to kAgentMaxJobs (6) - extra intents are
  //    unusable (the journal ceiling refuses them device-side) and bound the heap.
  for (JsonVariantConst s : root["spawn"].as<JsonArrayConst>()) {
    if (!s.is<JsonObjectConst>()) continue;                 // malformed item -> drop
    if (!s["task"].is<const char*>()) continue;             // no usable task -> drop
    // task is the only semantically-required field: empty -> drop this item. Cap at
    // kSpawnTaskMax-1 like every sibling field: the device buffer is task[420] but it
    // copies with strncpy(..., sizeof(task)-1)=419 bytes, so a full-420-byte task would
    // be silently re-truncated device-side by a non-UTF-8-safe copy - splitting a
    // multi-byte char at the boundary, the exact corruption capUtf8 exists to prevent.
    std::string task = capUtf8(std::string(s["task"].as<const char*>()), kSpawnTaskMax - 1);
    if (task.empty()) continue;
    if (static_cast<int>(out.spawn.size()) >= kAgentMaxJobs) break;  // cap the vector

    Spawn item;
    item.provider = toLowerAscii(spawnStr(s["provider"], nullptr,      kSpawnProviderMax - 1));
    item.model    = spawnStr(s["model"],    nullptr,     kSpawnModelMax    - 1);
    item.skill    = spawnStr(s["skill"],    nullptr,     kSpawnSkillMax    - 1);
    // name is LENIENT (schema requires it, parser defaults "") so an older wire /
    // failover provider that omits it can't fail the whole turn.
    item.name     = spawnStr(s["name"],     "",          kSpawnNameMax     - 1);
    item.task     = std::move(task);
    item.category = spawnStr(s["category"], "research",  kSpawnCategoryMax - 1);
    item.note     = spawnStr(s["note"],     "On it.",    kSpawnNoteMax     - 1);
    out.spawn.push_back(std::move(item));
  }

  // 5. await[] - TOLERANT: a non-string item is dropped; blank/whitespace-empty
  //    entries skipped. A whitespace-only tag can never match a real job tag (a
  //    "job0003"-style id), so trim first and drop it: keeping it would seed a
  //    dead poll request, not data.
  for (JsonVariantConst a : root["await"].as<JsonArrayConst>()) {
    if (!a.is<const char*>()) continue;   // malformed item -> drop
    const char* raw = a.as<const char*>();
    std::string tag = raw ? std::string(raw) : std::string();
    const size_t b = tag.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) continue;  // empty or whitespace-only -> skip
    const size_t e = tag.find_last_not_of(" \t\r\n");
    tag = tag.substr(b, e - b + 1);
    out.await_.push_back(std::move(tag));
  }

  // 6. device[] - TOLERANT: a non-object item is dropped; each valid item is carried
  //    as its raw JSON slice for the device-action validator (sibling module, which
  //    re-checks EVERY action incl. the protected-key refusal) + executor to act on.
  for (JsonVariantConst d : root["device"].as<JsonArrayConst>()) {
    if (!d.is<JsonObjectConst>()) continue;   // malformed item -> drop
    DeviceAction act;
    serializeJson(d, act.json);
    out.device.push_back(std::move(act));
  }

  // ---- live-integration fields. Unlike device/spawn (strict), these are TOLERANT:
  //      a malformed item is dropped, never a whole-turn error, so one slightly-off
  //      item can't lose a good reply. Each vector is capped to kAgentMaxJobs. ----

  // 7. mem_write[] - {content (required non-empty string), importance (number,
  //    clamped [0,1], default 0.5), permanent (bool, default false)}.
  for (JsonVariantConst m : root["mem_write"].as<JsonArrayConst>()) {
    if (!m.is<JsonObjectConst>()) continue;
    JsonVariantConst cv = m["content"];
    if (!cv.is<const char*>()) continue;
    std::string content = capUtf8(std::string(cv.as<const char*>()), kMemModelMax);
    if (content.empty()) continue;
    MemWrite w;
    w.content = std::move(content);
    JsonVariantConst iv = m["importance"];
    if (iv.is<int>() || iv.is<float>() || iv.is<double>()) w.importance = iv.as<double>();
    if (w.importance < 0.0) w.importance = 0.0;
    if (w.importance > 1.0) w.importance = 1.0;
    w.permanent = m["permanent"].is<bool>() ? m["permanent"].as<bool>() : false;
    // ttl: optional enum string (session|days|weeks|months|permanent); only
    // accepted when it names a known class, else left "" (default at apply).
    if (m["ttl"].is<const char*>()) {
      TtlClass tc;
      const char* ts = m["ttl"].as<const char*>();
      if (ttlClassFromName(ts, tc)) w.ttl = ts;
    }
    if (static_cast<int>(out.mem_write.size()) < kAgentMaxJobs) out.mem_write.push_back(std::move(w));
  }

  // 8. mem_query[] - search strings (trimmed; blank dropped).
  for (JsonVariantConst q : root["mem_query"].as<JsonArrayConst>()) {
    if (!q.is<const char*>()) continue;
    std::string s = q.as<const char*>();
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) continue;
    const size_t e = s.find_last_not_of(" \t\r\n");
    if (static_cast<int>(out.mem_query.size()) < kAgentMaxJobs)
      out.mem_query.push_back(capUtf8(s.substr(b, e - b + 1), kSpawnTaskMax - 1));
  }

  // 9. session_ops[] - {op (required, in the known set), id/message/task/provider/
  //    model per op}. Unknown op or non-object dropped.
  static const char* kOps[] = {"spawn", "tell", "poll", "terminate", "list"};
  for (JsonVariantConst o : root["session_ops"].as<JsonArrayConst>()) {
    if (!o.is<JsonObjectConst>()) continue;
    JsonVariantConst opv = o["op"];
    if (!opv.is<const char*>()) continue;
    std::string op = opv.as<const char*>();
    bool known = false;
    for (const char* k : kOps) if (op == k) { known = true; break; }
    if (!known) continue;
    SessionOp so;
    so.op       = std::move(op);
    so.id       = spawnStr(o["id"],       "", kSpawnCategoryMax - 1);
    so.message  = spawnStr(o["message"],  "", kSpawnTaskMax - 1);
    so.task     = spawnStr(o["task"],     "", kSpawnTaskMax - 1);
    so.provider = toLowerAscii(spawnStr(o["provider"], "", kSpawnProviderMax - 1));
    so.model    = spawnStr(o["model"],    "", kSpawnModelMax - 1);
    so.skill    = spawnStr(o["skill"],    "", 23);                       // capsule id cap
    so.name     = spawnStr(o["name"],     "", kSpawnNameMax - 1);
    so.project  = spawnStr(o["project"],  "", kSpawnProjectMax - 1);
    for (JsonVariantConst av : o["attach"].as<JsonArrayConst>()) {
      if (!av.is<const char*>()) continue;
      if ((int)so.attach.size() >= kSpawnAttachMax) break;
      std::string ref = spawnStr(av, "", 73);   // "<project 24>/<name 48>"
      if (!ref.empty()) so.attach.push_back(std::move(ref));
    }
    if (static_cast<int>(out.session_ops.size()) < kAgentMaxJobs) out.session_ops.push_back(std::move(so));
  }

  // 10. scratchpad - inline persistent working-memory update.
  //     TOLERANT: null/absent => no change; a non-null tier REPLACES that tier
  //     (capped kScratchTierItems, each item kScratchItemMax; blank items
  //     dropped). Applied device-side (apply.cpp -> memory::scratchpad()).
  {
    JsonVariantConst sp = root["scratchpad"];
    if (sp.is<JsonObjectConst>()) {
      out.scratchpad.present = true;
      JsonVariantConst av = sp["active"];
      if (av.is<const char*>()) {
        out.scratchpad.hasActive = true;
        out.scratchpad.active = capUtf8(std::string(av.as<const char*>()), kScratchActiveMax);
      }
      auto tier = [&](const char* key, bool& has, std::vector<std::string>& items) {
        JsonVariantConst a = sp[key];
        if (!a.is<JsonArrayConst>()) return;   // null / absent => leave unchanged
        has = true;
        for (JsonVariantConst it : a.as<JsonArrayConst>()) {
          if (!it.is<const char*>()) continue;
          std::string s = it.as<const char*>();
          size_t b = s.find_first_not_of(" \t\r\n");
          if (b == std::string::npos) continue;               // blank dropped
          size_t e = s.find_last_not_of(" \t\r\n");
          if ((int)items.size() >= kScratchTierItems) break;   // tier cap
          items.push_back(capUtf8(s.substr(b, e - b + 1), kScratchItemMax));
        }
      };
      tier("short", out.scratchpad.hasShort, out.scratchpad.shortItems);
      tier("mid",   out.scratchpad.hasMid,   out.scratchpad.midItems);
      tier("long",  out.scratchpad.hasLong,  out.scratchpad.longItems);
    }
  }

  err.code = ParseError::Code::Ok;
  return true;
}

}  // namespace orch
}  // namespace nimbus
