// Ported and adapted from Nuage-Solide src/agent/orchestrator.cpp (Head
// Orchestrator v2 - enqueueSpawn / runTurn parse path) + orch_schema.h.
//
// Portable turn-contract parser. No Arduino; ArduinoJson only (host-safe).

#include "nimbus/orch/turn.h"

#include "nimbus/orch/caps.h"
#include "nimbus/orch/head_loop.h"   // classifyStepError - the typed cut rule (isCutError)
#include "nimbus/orch/mem_ttl.h"
#include "nimbus/orch/reply_markup.h"
#include "nimbus/mem_cap.h"

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

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

// ---- the field table (single source; pinned to ORCH_SCHEMA_BODY by a test) ----
struct FieldRow {
  const char* name;
  FieldKind   kind;
};
const FieldRow kFieldRows[] = {
    {"reply", FieldKind::String},
    {"memory", FieldKind::String},
    {"ask", FieldKind::String},
    {"device", FieldKind::ArrayOfObjects},
    {"mem_write", FieldKind::ArrayOfObjects},
    {"mem_query", FieldKind::ArrayOfStrings},
    {"session_ops", FieldKind::ArrayOfObjects},
    {"scratchpad", FieldKind::Object},
    {"spawn", FieldKind::ArrayOfObjects},
    {"await", FieldKind::ArrayOfStrings},
};
static_assert(sizeof(kFieldRows) / sizeof(kFieldRows[0]) == (size_t)TurnField::Count,
              "every TurnField needs a row");

// ---- the parse-boundary normalizer (subagents F1, formatting F2, parsing P4) ----
// A JSON string bigger than this is never re-parsed (bounded work on the turn
// task); the field is reported instead. Six full spawn tasks fit with room.
constexpr size_t kCoerceMaxBytes = (size_t)kAgentMaxJobs * kSpawnTaskMax + 4096;
// Nesting accepted inside a field that arrived as a JSON string.
constexpr uint8_t kCoerceNesting = 8;
// Bytes of a dropped spawn's task kept for its report line.
constexpr int kDropTaskMax = 200;

const char kWs[] = " \t\r\n";

std::string trimCopy(const std::string& s) {
  const size_t b = s.find_first_not_of(kWs);
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(kWs);
  return s.substr(b, e - b + 1);
}

// Trailing whitespace off; the start is kept, so a salvaged prefix stays a
// prefix of the original reply.
std::string rtrimCopy(const std::string& s) {
  const size_t e = s.find_last_not_of(kWs);
  return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

bool isSpawnList(TurnField f) { return f == TurnField::SessionOps || f == TurnField::Spawn; }

void addDrop(Turn& t, const std::string& name, const std::string& task, std::string reason) {
  SpawnDrop d;
  d.name = capUtf8(name, kSpawnNameMax - 1);
  d.task = capUtf8(task, kDropTaskMax);
  d.reason = std::move(reason);
  t.spawnDrops.push_back(std::move(d));
}

const char* jsonTypeName(JsonVariantConst v) {
  if (v.isNull()) return "null";
  if (v.is<bool>()) return "a true/false value";
  if (v.is<long long>() || v.is<double>()) return "a number";
  if (v.is<const char*>()) return "text";
  if (v.is<JsonArrayConst>()) return "a list";
  if (v.is<JsonObjectConst>()) return "an object";
  return "an unknown value";
}

bool kindOk(FieldKind k, JsonVariantConst v) {
  switch (k) {
    case FieldKind::String:         return v.is<const char*>();
    case FieldKind::ArrayOfObjects:
    case FieldKind::ArrayOfStrings: return v.is<JsonArrayConst>();
    case FieldKind::Object:         return v.is<JsonObjectConst>();
  }
  return false;
}

// Replace root[key] with a copy of `v` (v may live in another document).
void setField(JsonObject root, const char* key, JsonVariantConst v) {
  JsonDocument copy;
  copy.set(v);
  root[key] = copy.as<JsonVariantConst>();
}

// Wrap a value as a one-item list in root[key].
void wrapField(JsonObject root, const char* key, JsonVariantConst v) {
  JsonDocument copy;
  copy.set(v);
  JsonArray a = root[key].to<JsonArray>();
  a.add(copy.as<JsonVariantConst>());
}

// Report a container field the normalizer could not use. A spawn list that is
// lost this way is a spawn intent that did not start: it is reported as one.
void unusable(Turn& rep, TurnField f, const std::string& why) {
  const std::string name = turnFieldName(f);
  rep.parseNotes.push_back(name + " " + why + "; not used");
  if (isSpawnList(f))
    addDrop(rep, "", "", "the " + name + " field " + why + ", so no sub-agent in it could be read");
}

// A repair recorded once: the note, the count.
bool repaired(Turn& rep, const std::string& name, const char* how) {
  rep.parseNotes.push_back(name + " " + how + "; repaired");
  rep.coercedFields++;
  return true;
}

// A container field that arrived as JSON text: parsed and used when it holds the
// expected type (or one object where a list of objects belongs).
bool coerceJsonText(JsonObject root, TurnField f, const std::string& s, Turn& rep) {
  const char* key = turnFieldName(f);
  const FieldKind kind = turnFieldKind(f);
  if (s.size() > kCoerceMaxBytes) {
    unusable(rep, f, "arrived as JSON text too large to read");
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, s, DeserializationOption::NestingLimit(kCoerceNesting))) {
    unusable(rep, f, "arrived as JSON text that does not parse");
    return false;
  }
  if (kindOk(kind, d.as<JsonVariantConst>())) {
    setField(root, key, d.as<JsonVariantConst>());
    return repaired(rep, key, "arrived as a JSON string");
  }
  if (kind == FieldKind::ArrayOfObjects && d.is<JsonObjectConst>()) {
    wrapField(root, key, d.as<JsonVariantConst>());
    return repaired(rep, key, "arrived as a JSON string holding one object");
  }
  unusable(rep, f, std::string("arrived as JSON text holding ") + jsonTypeName(d.as<JsonVariantConst>()));
  return false;
}

// A container field that arrived as a string.
bool coerceString(JsonObject root, TurnField f, const std::string& s, Turn& rep) {
  if (s.empty() || s == "null") return false;   // carries no item: nothing was meant
  if (s[0] == '[' || s[0] == '{') return coerceJsonText(root, f, s, rep);
  if (turnFieldKind(f) == FieldKind::ArrayOfStrings) {   // one search string: a list of one
    JsonDocument d;
    d.set(s);
    wrapField(root, turnFieldName(f), d.as<JsonVariantConst>());
    return repaired(rep, turnFieldName(f), "arrived as one string");
  }
  unusable(rep, f, "arrived as plain text, not a list");
  return false;
}

// One container field brought to its contract shape. Returns true on a repair.
bool coerceContainer(JsonObject root, TurnField f, Turn& rep) {
  const char* key = turnFieldName(f);
  const FieldKind kind = turnFieldKind(f);
  JsonVariant v = root[key];
  if (v.isNull() || kindOk(kind, v)) return false;
  if (v.is<const char*>()) return coerceString(root, f, trimCopy(v.as<const char*>()), rep);
  if (kind == FieldKind::ArrayOfObjects && v.is<JsonObjectConst>()) {
    wrapField(root, key, v);
    return repaired(rep, key, "arrived as a single object");
  }
  unusable(rep, f, std::string("arrived as ") + jsonTypeName(v));
  return false;
}

// Names the head uses for "start a sub-agent" besides spawn.
bool spawnSynonym(const std::string& op) {
  static const char* kSyn[] = {"start", "run", "launch", "create", "new", "spawn_agent",
                               "session_spawn", "spawn_session", "start_agent", "subagent",
                               "sub_agent", "agent", "delegate"};
  for (const char* k : kSyn)
    if (op == k) return true;
  return false;
}

// Other names the head gives a spawn's brief (task, then message, are the wire's).
const char* const kTaskAliases[] = {"prompt", "instructions", "instruction", "brief",
                                    "description", "query", "request", "goal"};

bool nonEmptyStr(JsonVariantConst v) {
  return v.is<const char*>() && !trimCopy(v.as<const char*>()).empty();
}

// One session_ops item made usable: op spliced/defaulted, brief under its name.
// Appends the result(s) to `out`. Returns true when the item changed.
bool fixOpItem(JsonVariantConst item, JsonArray out, Turn& rep, int depth);

// A JSON string holding session_ops items (a list or one object): parsed and
// each item made usable into `out`, with `note` recorded. False when `s` holds
// no such JSON (left for the caller).
bool spliceEmbedded(const std::string& raw, JsonArray out, Turn& rep, int depth, const char* note) {
  const std::string s = trimCopy(raw);
  if (s.empty() || (s[0] != '[' && s[0] != '{') || s.size() > kCoerceMaxBytes) return false;
  JsonDocument d;
  if (deserializeJson(d, s, DeserializationOption::NestingLimit(kCoerceNesting))) return false;
  rep.parseNotes.push_back(note);
  if (d.is<JsonArrayConst>()) {
    for (JsonVariantConst e : d.as<JsonArrayConst>()) fixOpItem(e, out, rep, depth + 1);
  } else {
    fixOpItem(d.as<JsonVariantConst>(), out, rep, depth + 1);
  }
  return true;
}

// The op slot holding the list itself ({"op":"[{\"op\":\"spawn\"...}]"}).
bool spliceOpList(JsonVariantConst opv, JsonArray out, Turn& rep, int depth) {
  if (!opv.is<const char*>() || depth != 0) return false;
  return spliceEmbedded(opv.as<const char*>(), out, rep, depth,
                        "session_ops item carried its list inside op; repaired");
}

// A brief under another name becomes the task. True when one was taken.
bool aliasTask(JsonObjectConst o, JsonDocument& fixed, Turn& rep) {
  for (const char* alias : kTaskAliases) {
    if (!nonEmptyStr(o[alias])) continue;
    fixed["task"] = o[alias];
    rep.parseNotes.push_back(std::string("session_ops spawn brief arrived as ") + alias +
                             "; read as task");
    return true;
  }
  return false;
}

// A missing op on an item with a task, a mis-cased op, or a synonym of spawn.
bool fixOpName(JsonVariantConst opv, bool hasTask, JsonDocument& fixed, Turn& rep) {
  if (opv.isNull()) {
    if (!hasTask) return false;
    fixed["op"] = "spawn";
    rep.parseNotes.push_back("session_ops item had no op; read as spawn");
    return true;
  }
  if (!opv.is<const char*>()) return false;
  const std::string op = toLowerAscii(trimCopy(opv.as<const char*>()));
  if (op != opv.as<const char*>() && (op == "spawn" || op == "terminate")) {
    fixed["op"] = op;
    rep.parseNotes.push_back("session_ops op '" + capUtf8(opv.as<const char*>(), 32) +
                             "' read as " + op);
    return true;
  }
  if (!spawnSynonym(op)) return false;
  fixed["op"] = "spawn";
  rep.parseNotes.push_back("session_ops op '" + op + "' read as spawn");
  return true;
}

bool fixOpObject(JsonObjectConst o, JsonArray out, Turn& rep, int depth) {
  JsonVariantConst opv = o["op"];
  if (spliceOpList(opv, out, rep, depth)) return true;
  JsonDocument fixed;
  fixed.set(o);
  bool hasTask = nonEmptyStr(o["task"]) || nonEmptyStr(o["message"]);
  bool changed = false;
  if (!hasTask && aliasTask(o, fixed, rep)) hasTask = changed = true;
  if (fixOpName(opv, hasTask, fixed, rep)) changed = true;
  out.add(fixed.as<JsonVariantConst>());
  return changed;
}

bool fixOpItem(JsonVariantConst item, JsonArray out, Turn& rep, int depth) {
  if (item.is<JsonObjectConst>()) return fixOpObject(item.as<JsonObjectConst>(), out, rep, depth);
  if (item.is<const char*>() && depth < 2 &&
      spliceEmbedded(item.as<const char*>(), out, rep, depth,
                     "session_ops item arrived as a JSON string; repaired"))
    return true;
  out.add(item);   // left for parseTurn to report
  return false;
}

// Every session_ops item made usable where it can be. The list is rebuilt only
// when something changed, so a well-formed list is untouched.
bool normalizeSessionOps(JsonObject root, Turn& rep) {
  JsonArrayConst ops = root["session_ops"].as<JsonArrayConst>();
  if (ops.isNull()) return false;
  JsonDocument rebuilt;
  JsonArray out = rebuilt.to<JsonArray>();
  bool changed = false;
  for (JsonVariantConst item : ops)
    if (fixOpItem(item, out, rep, 0)) changed = true;
  if (changed) setField(root, "session_ops", rebuilt.as<JsonVariantConst>());
  return changed;
}

}  // namespace

const char* turnFieldName(TurnField f) {
  const size_t i = (size_t)f;
  return i < (size_t)TurnField::Count ? kFieldRows[i].name : "";
}

FieldKind turnFieldKind(TurnField f) {
  const size_t i = (size_t)f;
  return i < (size_t)TurnField::Count ? kFieldRows[i].kind : FieldKind::String;
}

bool turnFieldFromName(const char* s, size_t n, TurnField& out) {
  if (!s) return false;
  for (size_t i = 0; i < (size_t)TurnField::Count; i++) {
    if (std::strlen(kFieldRows[i].name) == n && std::memcmp(kFieldRows[i].name, s, n) == 0) {
      out = (TurnField)i;
      return true;
    }
  }
  return false;
}

bool normalizeTurnDoc(JsonObject root, Turn& rep) {
  bool changed = false;
  repairLeakedTop(root, rep.repair);
  if (rep.repair.repaired) {
    changed = true;
    rep.parseNotes.push_back("reply carried leaked tool markup; cut");
    if (rep.repair.droppedBlocks)
      rep.parseNotes.push_back(std::to_string((unsigned)rep.repair.droppedBlocks) +
                               " leaked field value(s) did not parse; not used");
  }
  for (size_t i = (size_t)TurnField::Device; i < (size_t)TurnField::Count; i++)
    if (coerceContainer(root, (TurnField)i, rep)) changed = true;
  if (normalizeSessionOps(root, rep)) changed = true;
  const uint8_t cutsBefore = rep.repair.nestedCuts;
  repairLeakedNested(root, rep.repair);
  if (rep.repair.nestedCuts != cutsBefore) {
    changed = true;
    rep.parseNotes.push_back("a nested field carried leaked tool markup; cut");
  }
  return changed;
}

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

  // 2. `reply` is the ONE required field: absent -> MissingField, present but not
  //    a string -> WrongType (without it there is genuinely no usable turn).
  //    `memory` and `ask` are OPTIONAL strings (CUM-483): absent, null, or a
  //    non-string read as "" - which already means "memory unchanged" / "no
  //    question" downstream (apply.cpp). On Anthropic the wire schema is
  //    ADVISORY (its strict-grammar budget rejects a contract this size), so a
  //    model can and does leave `memory` out; failing the whole turn for that
  //    threw away its scratchpad, mem_write, device and session_ops side effects
  //    (bench B, 2026-10-10). Every array is OPTIONAL too: absent => empty.
  {
    JsonVariantConst v = root["reply"];
    if (v.isNull()) return fail(out, err, ParseError::Code::MissingField, "reply");
    if (!v.is<const char*>())
      return fail(out, err, ParseError::Code::WrongType, "reply must be a string");
  }
  // Arrays are TOLERANT throughout (owner field bug 2026-07-16: on Anthropic the
  // wire schema is ADVISORY - its strict grammar budget rejects a contract this
  // size - so the parser is the ONLY enforcement layer, and a single slightly-off
  // array item was failing the whole turn, which leaked the raw JSON to the user
  // via the caller's fallback). Policy: REPAIR OR REPORT. The normalizer first
  // cuts a leaked tool-markup tail and recovers the fields it carried, then
  // brings every container to its contract shape (a JSON string, a bare object,
  // a list hidden in an item's op); what it cannot repair it names in
  // out.parseNotes, and every spawn intent that cannot start lands in
  // out.spawnDrops with its reason. A malformed ITEM is still never a whole-turn
  // error, and only `reply` can fail a turn.
  normalizeTurnDoc(doc.as<JsonObject>(), out);

  // 3. String fields. reply/ask are user-facing and uncapped here (the device
  //    budgets them elsewhere). memory is clamped to kMemModelMax UTF-8-safe so the
  //    parsed Turn is self-consistent with the memory module's cap even before
  //    OrchMemory::setModel runs; the definitive cap + truncation flag live there.
  auto optStr = [&](const char* k) -> std::string {
    JsonVariantConst v = root[k];
    return v.is<const char*>() ? std::string(v.as<const char*>()) : std::string();
  };
  out.reply  = root["reply"].as<const char*>();
  out.ask    = optStr("ask");
  out.memory = capUtf8(optStr("memory"), kMemModelMax);

  // 4. spawn[] - TOLERANT per item: a non-object element, or one whose `task` is
  //    missing/non-string/empty, is DROPPED (matches enqueueSpawn: `if (!task[0])
  //    return;`) - never a turn error. Every other field falls back to a default
  //    on copy (spawnStr), so a failover provider that omits `note` or sends a
  //    numeric `model` loses that item at worst, not the owner's reply. Defaults,
  //    provider-lowercasing, and per-field byte caps are applied on copy. The
  //    accepted vector is truncated to kAgentMaxJobs (6) - extra intents are
  //    unusable (the journal ceiling refuses them device-side) and bound the heap.
  // Every drop below is REPORTED (out.spawnDrops): the item was a spawn intent.
  const std::string kNoTask = "it was sent without a task";
  const std::string kOverCap = spawnOverCapReason();
  for (JsonVariantConst s : root["spawn"].as<JsonArrayConst>()) {
    const std::string sname = s["name"].is<const char*>() ? s["name"].as<const char*>() : "";
    if (!s.is<JsonObjectConst>()) {                         // malformed item -> drop
      addDrop(out, "", "", "a spawn item was not an object");
      continue;
    }
    if (!s["task"].is<const char*>()) {                     // no usable task -> drop
      addDrop(out, sname, "", kNoTask);
      continue;
    }
    // task is the only semantically-required field: empty -> drop this item. Cap at
    // kSpawnTaskMax-1 like every sibling field: the device buffer is task[420] but it
    // copies with strncpy(..., sizeof(task)-1)=419 bytes, so a full-420-byte task would
    // be silently re-truncated device-side by a non-UTF-8-safe copy - splitting a
    // multi-byte char at the boundary, the exact corruption capUtf8 exists to prevent.
    std::string task = capUtf8(std::string(s["task"].as<const char*>()), kSpawnTaskMax - 1);
    if (trimCopy(task).empty()) {
      addDrop(out, sname, "", kNoTask);
      continue;
    }
    if (static_cast<int>(out.spawn.size()) >= kAgentMaxJobs) {   // cap the vector
      addDrop(out, sname, task, kOverCap);
      continue;
    }

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
  int spawnsKept = 0;
  for (JsonVariantConst o : root["session_ops"].as<JsonArrayConst>()) {
    if (!o.is<JsonObjectConst>()) {
      addDrop(out, "", "", std::string("a session_ops item arrived as ") + jsonTypeName(o) +
                               ", not an object");
      continue;
    }
    // The task the item carries, whatever its op - an item with a brief was meant
    // to start something, so losing it is reported as a spawn that did not start.
    const std::string itemTask =
        nonEmptyStr(o["task"]) ? o["task"].as<const char*>()
                               : (nonEmptyStr(o["message"]) ? o["message"].as<const char*>() : "");
    const std::string itemName = o["name"].is<const char*>() ? o["name"].as<const char*>() : "";
    JsonVariantConst opv = o["op"];
    if (!opv.is<const char*>()) {
      if (!itemTask.empty()) addDrop(out, itemName, itemTask, "it had no op");
      else out.parseNotes.push_back("a session_ops item had no op; not used");
      continue;
    }
    std::string op = opv.as<const char*>();
    bool known = false;
    for (const char* k : kOps) if (op == k) { known = true; break; }
    if (!known) {
      if (!itemTask.empty()) addDrop(out, itemName, itemTask, "its op '" + op + "' is unknown");
      else out.parseNotes.push_back("a session_ops item had the unknown op '" + op + "'; not used");
      continue;
    }
    if (op == "spawn") {
      if (itemTask.empty()) {
        addDrop(out, itemName, "", kNoTask);
        continue;
      }
      if (spawnsKept >= kAgentMaxJobs ||
          static_cast<int>(out.session_ops.size()) >= kAgentMaxJobs) {
        addDrop(out, itemName, itemTask, kOverCap);
        continue;
      }
      spawnsKept++;
    }
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
    if (static_cast<int>(out.session_ops.size()) < kAgentMaxJobs)
      out.session_ops.push_back(std::move(so));
    else
      out.parseNotes.push_back("a session_ops '" + so.op + "' item was over the per-turn limit; not used");
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

namespace {
const char kSpace[] = " \t\r\n";

// Index of the bracket that closes the JSON object or array opening at s[b] (a
// '{' or '['), skipping string contents and escapes; npos when it never closes
// (a truncated reply). Only the opening kind is counted - enough for JSON, where
// the other kind always nests balanced inside.
size_t objectEnd(const std::string& s, size_t b) {
  const char open = s[b], close = open == '{' ? '}' : ']';
  int depth = 0;
  bool inString = false;
  for (size_t i = b; i < s.size(); ++i) {
    const char c = s[i];
    if (inString) {
      if (c == '\\') ++i;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') inString = true;
    else if (c == open) ++depth;
    else if (c == close && --depth == 0) return i;
  }
  return std::string::npos;
}

// Where a reply's leading JSON object would start: the first non-space byte, or
// for a reply that opens with a ``` fence, the first non-space byte of its second
// line. `fenced` reports which. npos when the reply is empty or a one-line fence.
size_t leadingObjectStart(const std::string& text, bool& fenced) {
  const size_t b = text.find_first_not_of(kSpace);
  fenced = b != std::string::npos && text.compare(b, 3, "```") == 0;
  if (!fenced) return b;
  const size_t nl = text.find('\n', b);
  return nl == std::string::npos ? nl : text.find_first_not_of(kSpace, nl);
}

// Whether the object [ob, oe] is the WHOLE reply: nothing but whitespace after it
// (bare), or nothing but whitespace and the one closing fence (fenced).
bool spansWholeReply(const std::string& text, size_t oe, bool fenced) {
  const size_t next = text.find_first_not_of(kSpace, oe + 1);
  if (!fenced) return next == std::string::npos;
  return next != std::string::npos && text.compare(next, 3, "```") == 0 &&
         text.find_first_not_of(kSpace, next + 3) == std::string::npos;
}

// A JSON object with a string "reply": the shape of a turn attempt.
bool turnShaped(const std::string& json) {
  JsonDocument d;
  return !deserializeJson(d, json) && d["reply"].is<const char*>();
}

ReplyShape prose(const std::string& text, Turn& out, ParseError& err) {
  const size_t b = text.find_first_not_of(kSpace);
  const size_t e = text.find_last_not_of(kSpace);
  out = Turn{};
  out.reply = text.substr(b, e - b + 1);
  err = ParseError{};
  return ReplyShape::Prose;
}

// Whether the WHOLE reply is one JSON array (bare or one whole fence): wire data
// that is not a turn - never shown to a person as prose (the base apologized for
// it too). A reply that merely starts with "[" (a citation, a Markdown link) is
// prose: the array must close exactly at the end and parse.
bool wholeJsonArray(const std::string& text) {
  bool fenced = false;
  const size_t b = leadingObjectStart(text, fenced);
  if (b == std::string::npos || text[b] != '[') return false;
  const size_t e = objectEnd(text, b);
  if (e == std::string::npos || !spansWholeReply(text, e, fenced)) return false;
  JsonDocument d;
  return !deserializeJson(d, text.substr(b, e - b + 1)) && d.is<JsonArrayConst>();
}

// Whether the reply opens a JSON array of objects ("[{" or a nested "[[{", bare
// or fenced) that never closes: a wire reply cut by the provider's output cap.
// Never prose - the raw partial array would reach the owner (gate W2 #19). Prose
// that merely starts with "[" ("[1] ...", "[the docs](...)") is not "[{"-shaped
// and stays prose.
bool truncatedJsonArray(const std::string& text) {
  bool fenced = false;
  const size_t b = leadingObjectStart(text, fenced);
  if (b == std::string::npos || text[b] != '[') return false;
  const size_t first = text.find_first_not_of(std::string(kSpace) + "[", b + 1);
  return first != std::string::npos && text[first] == '{' &&
         objectEnd(text, b) == std::string::npos;
}

ReplyShape unusable(Turn& out, ParseError& err, const char* why) {
  out = Turn{};
  if (err.ok()) {
    err.code = ParseError::Code::JsonError;
    err.detail = why;
  }
  return ReplyShape::Unusable;
}
}  // namespace

std::string leadingJsonObject(const std::string& text, bool* wholeReply) {
  if (wholeReply) *wholeReply = false;
  bool fenced = false;
  const size_t ob = leadingObjectStart(text, fenced);
  if (ob == std::string::npos || text[ob] != '{') return std::string();
  const size_t oe = objectEnd(text, ob);
  if (oe == std::string::npos) return std::string();
  if (wholeReply) *wholeReply = spansWholeReply(text, oe, fenced);
  return text.substr(ob, oe - ob + 1);
}

namespace {
ReplyShape parseHeadReplyText(const std::string& text, Turn& out, ParseError& err) {
  if (text.find_first_not_of(kSpace) == std::string::npos)
    return unusable(out, err, "empty reply");
  bool fenced = false;
  const size_t ob = leadingObjectStart(text, fenced);
  if (ob == std::string::npos || text[ob] != '{') {
    if (wholeJsonArray(text)) return unusable(out, err, "a JSON array, not a turn");
    if (truncatedJsonArray(text)) return unusable(out, err, "truncated JSON array");
    // A reply cut right after its opening fence ("```json" and nothing after
    // it) is wire output that never arrived, not prose to show the owner.
    if (fenced && ob == std::string::npos && text.find("```", text.find("```") + 3) == std::string::npos)
      return unusable(out, err, "truncated JSON fence");
    if (trimCopy(text).find_first_not_of('`') == std::string::npos)   // "`" or "``": the same
      return unusable(out, err, "truncated JSON fence");
    return prose(text, out, err);
  }
  bool whole = false;
  const std::string obj = leadingJsonObject(text, &whole);
  if (obj.empty()) return unusable(out, err, "truncated JSON object");
  if (whole)
    return parseTurn(obj, out, err) ? ReplyShape::Turn : unusable(out, err, "not a turn");
  // An object followed by more text: a turn attempt with trailing chatter is
  // salvaged (reply/ask only); a JSON example that opens a prose answer is prose.
  if (turnShaped(obj)) return unusable(out, err, "text after the turn object");
  return prose(text, out, err);
}
}  // namespace

ReplyShape parseHeadReply(const std::string& rawText, Turn& out, ParseError& err) {
  err = ParseError{};
  // Formatting F3: a leading reasoning block and a whole-reply wrapper are not
  // the answer. "<think>...</think>{json}" is a turn (its side effects apply),
  // never prose that shows the owner the reasoning AND the raw JSON.
  size_t stripped = 0;
  const std::string text = unwrapWholeReply(stripReasoningPreamble(rawText, &stripped));
  const ReplyShape shape = parseHeadReplyText(text, out, err);
  if (stripped)
    out.parseNotes.push_back("reasoning preamble stripped (" + std::to_string(stripped) + " B)");
  if (text.size() + stripped != rawText.size() && shape != ReplyShape::Unusable)
    out.parseNotes.push_back("whole-reply wrapper removed");
  return shape;
}

// ---- pseudo-tool spawns (subagents F2) -------------------------------------------

int spawnsFromToolArgs(const std::string& argsJson, Turn& out) {
  out = Turn{};
  JsonDocument d;
  if (deserializeJson(d, argsJson, DeserializationOption::NestingLimit(kCoerceNesting))) {
    addDrop(out, "", "", "the call's arguments were not valid JSON");
    return 0;
  }
  // Where the spawn list sits in this call's arguments.
  JsonDocument root;
  root["reply"] = "";
  JsonVariantConst list;
  if (d.is<JsonObjectConst>()) {
    JsonObjectConst o = d.as<JsonObjectConst>();
    if (!o["session_ops"].isNull()) list = o["session_ops"];
    else if (!o["spawn"].isNull()) list = o["spawn"];
    else if (!o["op"].isNull() || nonEmptyStr(o["task"]) || nonEmptyStr(o["message"])) list = o;
    else for (const char* alias : kTaskAliases)
      if (nonEmptyStr(o[alias])) { list = o; break; }
  } else if (!d.isNull()) {
    list = d.as<JsonVariantConst>();
  }
  if (list.isNull()) {
    addDrop(out, "", "", "the call carried no spawn item");
    return 0;
  }
  root["session_ops"] = list;
  std::string json;
  serializeJson(root, json);
  Turn parsed;
  ParseError err;
  if (!parseTurn(json, parsed, err)) {
    addDrop(out, "", "", "the call's spawn list could not be read");
    return 0;
  }
  out.parseNotes = std::move(parsed.parseNotes);
  out.spawnDrops = std::move(parsed.spawnDrops);
  for (SessionOp& op : parsed.session_ops) {
    if (op.op == "spawn") out.session_ops.push_back(std::move(op));
    else out.parseNotes.push_back("a '" + op.op + "' op sent as a tool call was not run");
  }
  return (int)out.session_ops.size();
}

// ---- salvage of a cut or unreadable reply (parsing P3) ---------------------------

const char kSalvageCutMarker[] =
    "(My answer was cut off before I finished, and nothing else in it was carried out. "
    "Ask me to continue.)";

namespace {

// Append the UTF-8 encoding of code point cp.
void putUtf8(std::string& o, uint32_t cp) {
  if (cp < 0x80) {
    o += (char)cp;
  } else if (cp < 0x800) {
    o += (char)(0xC0 | (cp >> 6));
    o += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    o += (char)(0xE0 | (cp >> 12));
    o += (char)(0x80 | ((cp >> 6) & 0x3F));
    o += (char)(0x80 | (cp & 0x3F));
  } else {
    o += (char)(0xF0 | (cp >> 18));
    o += (char)(0x80 | ((cp >> 12) & 0x3F));
    o += (char)(0x80 | ((cp >> 6) & 0x3F));
    o += (char)(0x80 | (cp & 0x3F));
  }
}

bool hex4(const std::string& s, size_t i, uint32_t& v) {
  if (i + 4 > s.size()) return false;
  v = 0;
  for (size_t k = i; k < i + 4; k++) {
    const char c = s[k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return false;
  }
  return true;
}

constexpr uint32_t kReplacement = 0xFFFD;

// The low half after a high surrogate `hi`, at s[i]. Advances i. Returns false
// when the pair is cut between its halves - a cut, never a replacement
// character the original did not have.
bool decodeLowHalf(const std::string& s, size_t& i, uint32_t hi, std::string& out) {
  if (i >= s.size()) return false;
  if (s[i] == '\\' && (i + 1 >= s.size() || (s[i + 1] == 'u' && i + 6 > s.size()))) return false;
  uint32_t lo = 0;
  const bool pair = i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u' && hex4(s, i + 2, lo) &&
                    lo >= 0xDC00 && lo <= 0xDFFF;
  if (!pair) {
    putUtf8(out, kReplacement);   // a lone high surrogate
    return true;
  }
  putUtf8(out, 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
  i += 6;
  return true;
}

// One \uXXXX escape at s[i] ('\\' at i), surrogate pairs combined. Advances i.
// Returns false when the text ends inside the escape (a cut).
bool decodeUnicode(const std::string& s, size_t& i, std::string& out) {
  uint32_t hi = 0;
  if (!hex4(s, i + 2, hi)) {
    if (i + 6 > s.size()) return false;   // cut inside the escape
    putUtf8(out, kReplacement);
    i += 2;
    return true;
  }
  i += 6;
  if (hi >= 0xD800 && hi <= 0xDBFF) return decodeLowHalf(s, i, hi, out);
  putUtf8(out, (hi >= 0xDC00 && hi <= 0xDFFF) ? kReplacement : hi);   // a lone low half: U+FFFD
  return true;
}

// The byte one simple escape (\n, \", ...) stands for.
char simpleEscape(char e) {
  switch (e) {
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    default:  return e;   // \" \\ \/ and anything else: the byte itself
  }
}

// Decode the JSON string whose opening quote is at s[q]. Returns the index
// after the closing quote, or npos when the text ends first (cut).
size_t decodeJsonString(const std::string& s, size_t q, std::string& out) {
  size_t i = q + 1;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') return i + 1;
    if (c != '\\') {
      out += c;
      i++;
      continue;
    }
    if (i + 1 >= s.size()) return std::string::npos;
    if (s[i + 1] == 'u') {
      if (!decodeUnicode(s, i, out)) return std::string::npos;
      continue;
    }
    out += simpleEscape(s[i + 1]);
    i += 2;
  }
  return std::string::npos;
}

// Drop a UTF-8 sequence the cut split (its lead byte without all its
// continuation bytes).
void dropSplitSequence(std::string& s) {
  const size_t n = s.size();
  size_t k = n;
  while (k > 0 && ((unsigned char)s[k - 1] & 0xC0) == 0x80) k--;
  if (k == 0) return;
  const unsigned char lead = (unsigned char)s[k - 1];
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (need > 1 && n - (k - 1) < need) s.resize(k - 1);
}

bool sentenceEnd(const std::string& s, size_t i) {   // s[i-1] ends a sentence
  const char c = s[i - 1];
  const bool stop = c == '.' || c == '!' || c == '?' || c == '\n';
  return stop && (i == s.size() || s[i] == ' ' || s[i] == '\n');
}

// A cut string's decoded prefix, made presentable: no split sequence at the
// end, then back to the last sentence end (or, failing that, the last word
// break) when one sits in the second half.
std::string trimCutPrefix(std::string s) {
  dropSplitSequence(s);
  const size_t n = s.size();
  for (size_t i = n; i > n / 2; i--)
    if (sentenceEnd(s, i)) return rtrimCopy(s.substr(0, i));
  for (size_t i = n; i > n / 2; i--)
    if (s[i - 1] == ' ' || s[i - 1] == '\n') return rtrimCopy(s.substr(0, i - 1));
  return rtrimCopy(s);
}

// The single pass over the leading object: depth, string state, and whether
// the next string at depth 1 is a key or a value.
struct SalvageLexer {
  SalvageLexer(const std::string& t, Salvage& o) : text(t), out(o) {}
  const std::string& text;
  Salvage& out;
  int depth = 1;
  bool expectKey = true;
  bool inValue = false;
  std::string key;

  // One string at text[i]; returns the index after it, npos when it was cut.
  size_t string(size_t i) {
    std::string str;
    const size_t next = decodeJsonString(text, i, str);
    const bool complete = next != std::string::npos;
    if (depth == 1 && expectKey) {
      key = str;
      expectKey = false;
    } else if (depth == 1 && inValue) {
      inValue = false;
      take(str, complete);
    }
    return next;
  }

  void take(const std::string& str, bool complete) {
    if (key != "reply" && key != "ask") return;
    std::string& dst = key == "reply" ? out.reply : out.ask;
    bool& done = key == "reply" ? out.replyComplete : out.askComplete;
    dst = complete ? str : trimCutPrefix(str);
    done = complete;
  }

  void punct(char c) {
    if (depth == 1 && c == ':') inValue = true;
    else if (depth == 1 && c == ',') { expectKey = true; inValue = false; }
    else if (c == '{' || c == '[') { depth++; inValue = false; }
    else if (c == '}' || c == ']') depth--;
    else if (depth == 1) inValue = false;   // a number / true / false / null value
  }
};

}  // namespace

bool salvageTurnText(const std::string& text, Salvage& out) {
  out = Salvage{};
  bool fenced = false;
  const size_t ob = leadingObjectStart(text, fenced);
  if (ob == std::string::npos || text[ob] != '{') return false;
  SalvageLexer lex(text, out);
  size_t i = ob + 1;
  while (i < text.size() && lex.depth > 0) {
    const char c = text[i];
    if (c == '"') {
      i = lex.string(i);
      if (i == std::string::npos) break;
      continue;
    }
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') lex.punct(c);
    i++;
  }
  return out.found();
}

std::string spawnOverCapReason() {
  return "it was over the limit of " + std::to_string(kAgentMaxJobs) +
         " sub-agents one turn can start; start it in the next wave";
}

bool isCutError(const std::string& err) {
  // Lane K's typed rule decides wherever it knows the line; the spellings below
  // only read lines it cannot type (it calls those Transport).
  const StepErr kind = classifyStepError(err);
  if (kind == StepErr::Cut) return true;
  if (kind != StepErr::Transport) return false;
  std::string e = toLowerAscii(err);
  return e.rfind("cut", 0) == 0 || e.find("max_tokens") != std::string::npos ||
         e.find("max_output_tokens") != std::string::npos ||
         e.find("output limit") != std::string::npos ||
         e.find("finish_reason=length") != std::string::npos ||
         e.find("finish_reason length") != std::string::npos;
}

}  // namespace orch
}  // namespace nimbus
