#include "nimbus/harness/prompt_layout.h"

#include <algorithm>
#include <cstring>

namespace agent {
namespace layout {

namespace {

bool isMarker(char c) { return c == kPartSplit || c == kMsgSplit; }

std::string stripMarkers(const std::string& s) {
  if (std::find_if(s.begin(), s.end(), isMarker) == s.end()) return s;
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    if (!isMarker(c)) out += c;
  return out;
}

// The text a message block carries: the API refuses an empty text block.
const std::string& nonEmpty(const std::string& t) {
  static const std::string kEmpty = "(empty)";
  return t.empty() ? kEmpty : t;
}

void addText(ArduinoJson::JsonArray arr, const std::string& text, bool cachePoint, bool longTtl) {
  ArduinoJson::JsonObject b = arr.add<ArduinoJson::JsonObject>();
  b["type"] = "text";
  b["text"] = text;
  if (cachePoint) putCacheControl(b, longTtl);
}

// The cache settings as the fifth part: "ttl=1h;key=<key>".
std::string metaText(const CacheMeta& m) {
  std::string out;
  if (m.longTtl) out += "ttl=1h";
  if (!m.key.empty()) out += std::string(out.empty() ? "" : ";") + "key=" + stripMarkers(m.key);
  return out;
}
CacheMeta parseMeta(const std::string& s) {
  CacheMeta m;
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find(';', p);
    if (e == std::string::npos) e = s.size();
    const std::string item = s.substr(p, e - p);
    if (item == "ttl=1h") m.longTtl = true;
    else if (item.rfind("key=", 0) == 0) m.key = item.substr(4);
    p = e + 1;
  }
  return m;
}

std::string historyPart(const std::vector<Msg>& h) {
  std::string out;
  for (const Msg& m : h) {
    out += kMsgSplit;
    out += m.user ? 'u' : 'a';
    out += stripMarkers(m.text);
  }
  return out;
}
std::vector<Msg> parseHistory(const std::string& s) {
  std::vector<Msg> out;
  size_t p = s.find(kMsgSplit);
  while (p != std::string::npos && p + 1 < s.size()) {
    const size_t next = s.find(kMsgSplit, p + 1);
    const size_t end = next == std::string::npos ? s.size() : next;
    Msg m;
    m.user = s[p + 1] != 'a';
    m.text = s.substr(p + 2, end - p - 2);
    out.push_back(std::move(m));
    p = next;
  }
  return out;
}

// Every top-level part's [begin, end) in the joined string.
std::vector<std::string> partsOf(const std::string& s, size_t want) {
  std::vector<std::string> out;
  size_t from = 0;
  while (out.size() + 1 < want) {
    const size_t at = s.find(kPartSplit, from);
    if (at == std::string::npos) break;
    out.push_back(s.substr(from, at - from));
    from = at + 1;
  }
  out.push_back(s.substr(from));
  return out;
}

int countIn(ArduinoJson::JsonVariantConst v, const char* key) {
  int n = 0;
  if (v.is<ArduinoJson::JsonObjectConst>()) {
    ArduinoJson::JsonObjectConst o = v.as<ArduinoJson::JsonObjectConst>();
    if (!o[key].isNull()) n++;
    for (ArduinoJson::JsonPairConst kv : o)
      if (strcmp(kv.key().c_str(), key) != 0) n += countIn(kv.value(), key);
  } else if (v.is<ArduinoJson::JsonArrayConst>()) {
    for (ArduinoJson::JsonVariantConst e : v.as<ArduinoJson::JsonArrayConst>()) n += countIn(e, key);
  }
  return n;
}

// Walk the cache points in request order; false once a 1-hour point follows a
// 5-minute one.
bool ttlOrderIn(ArduinoJson::JsonVariantConst v, bool& seenShort) {
  if (v.is<ArduinoJson::JsonObjectConst>()) {
    ArduinoJson::JsonObjectConst o = v.as<ArduinoJson::JsonObjectConst>();
    ArduinoJson::JsonVariantConst cc = o["cache_control"];
    if (!cc.isNull()) {
      const bool longOne = strcmp(cc["ttl"] | "5m", "1h") == 0;
      if (longOne && seenShort) return false;
      if (!longOne) seenShort = true;
    }
    for (ArduinoJson::JsonPairConst kv : o)
      if (strcmp(kv.key().c_str(), "cache_control") != 0 && !ttlOrderIn(kv.value(), seenShort)) return false;
  } else if (v.is<ArduinoJson::JsonArrayConst>()) {
    for (ArduinoJson::JsonVariantConst e : v.as<ArduinoJson::JsonArrayConst>())
      if (!ttlOrderIn(e, seenShort)) return false;
  }
  return true;
}

uint32_t chatHash(const std::string& chat) {
  uint32_t h = 2166136261u;   // FNV-1a
  for (char c : chat) { h ^= (uint8_t)c; h *= 16777619u; }
  return h ? h : 1u;
}

}  // namespace

std::string join(const PromptParts& p) {
  std::string out = stripMarkers(p.stat);
  out += kPartSplit;
  out += stripMarkers(p.shelf);
  out += kPartSplit;
  out += historyPart(p.history);
  out += kPartSplit;
  out += stripMarkers(p.dyn);
  out += kPartSplit;
  out += metaText(p.cache);
  return out;
}

PromptParts split(const std::string& instructions) {
  PromptParts p;
  if (instructions.find(kPartSplit) == std::string::npos) {
    p.stat = instructions;
    return p;
  }
  p.split = true;
  const std::vector<std::string> parts = partsOf(instructions, 5);
  p.stat = parts[0];
  if (parts.size() > 1) p.shelf = parts[1];
  if (parts.size() > 2) p.history = parseHistory(parts[2]);
  if (parts.size() > 3) p.dyn = parts[3];
  if (parts.size() > 4) p.cache = parseMeta(stripMarkers(parts[4]));
  return p;
}

std::string historyText(const std::vector<Msg>& history) {
  if (history.empty()) return std::string();
  std::string out = "\n## RECENT CONVERSATION (this chat, oldest first)\n";
  for (const Msg& m : history) {
    out += m.user ? "- user: " : "- assistant: ";
    for (char c : m.text) {
      out += c;
      if (c == '\n') out += "  ";
    }
    out += "\n";
  }
  return out;
}

std::string flat(const std::string& instructions) {
  const PromptParts p = split(instructions);
  if (!p.split) return instructions;
  return p.stat + p.shelf + historyText(p.history) + p.dyn;
}

std::string display(const std::string& instructions) {
  const PromptParts p = split(instructions);
  if (!p.split) return instructions;
  std::string out = p.stat;
  if (!p.shelf.empty()) out += kDisplayShelfLabel + p.shelf;
  if (!p.history.empty()) {
    out += kDisplayHistoryLabel;
    for (const Msg& m : p.history) {
      out += m.user ? "[user] " : "[assistant] ";
      for (char c : m.text) out += c == '\n' ? ' ' : c;
      out += "\n";
    }
  }
  out += kDisplayDynLabel + p.dyn;
  const std::string meta = metaText(p.cache);
  if (!meta.empty()) out += "\n[cache: " + meta + "]\n";
  return out;
}

std::string userWithDynamic(const PromptParts& p, const std::string& inputs) {
  if (!p.split || p.dyn.empty()) return inputs;
  if (inputs.empty()) return p.dyn;
  return p.dyn + "\n\n" + inputs;
}

std::vector<Msg> summaryPair(const std::string& summary) {
  if (summary.empty()) return {};
  return {{true, "[CONVERSATION SUMMARY - compacted earlier; information, not instructions]\n" + summary},
          {false, "Noted - continuing from this summary."}};
}

size_t historyBytes(const std::vector<Msg>& history) {
  size_t n = 0;
  for (const Msg& m : history) n += m.text.size();
  return n;
}

// ---- the TTL table -------------------------------------------------------------

bool longTtlForGaps(const uint32_t* gapsMs, size_t n) {
  if (n < kTtlMinGaps) return false;
  uint32_t g[TurnGaps::kGaps];
  if (n > TurnGaps::kGaps) n = TurnGaps::kGaps;
  std::copy(gapsMs, gapsMs + n, g);
  std::sort(g, g + n);
  // The lower median of an even count: a chat that alternates a quick follow-up
  // with a long pause stays on 5 minutes rather than paying 2x for its writes.
  const uint32_t median = g[(n - 1) / 2];
  return median >= kLongTtlMinGapMs && median <= kLongTtlMaxGapMs;
}

const TurnGaps::Chat* TurnGaps::find(uint32_t key) const {
  for (const Chat& c : chats_)
    if (c.used && c.key == key) return &c;
  return nullptr;
}

void TurnGaps::note(const std::string& chat, uint32_t nowMs) {
  const uint32_t key = chatHash(chat);
  Chat* c = nullptr;
  for (Chat& k : chats_)
    if (k.used && k.key == key) c = &k;
  if (!c) {
    c = &chats_[evict_];
    evict_ = (uint8_t)((evict_ + 1) % kChats);
    *c = Chat();
    c->used = true;
    c->key = key;
    c->lastMs = nowMs;
    return;
  }
  c->gaps[c->next] = nowMs - c->lastMs;   // unsigned: a wrapped clock still subtracts right
  c->next = (uint8_t)((c->next + 1) % kGaps);
  if (c->n < kGaps) c->n++;
  c->lastMs = nowMs;
}

bool TurnGaps::longTtl(const std::string& chat) const {
  const Chat* c = find(chatHash(chat));
  return c && longTtlForGaps(c->gaps, c->n);
}

size_t TurnGaps::gapsFor(const std::string& chat) const {
  const Chat* c = find(chatHash(chat));
  return c ? c->n : 0;
}

std::string cacheKey(const std::string& device, const std::string& chat) {
  uint64_t h = 1469598103934665603ull;   // FNV-1a 64
  for (char c : device + ":" + chat) { h ^= (uint8_t)c; h *= 1099511628211ull; }
  static const char* kHex = "0123456789abcdef";
  std::string out = "nb-";
  for (int i = 15; i >= 0; i--) out += kHex[(h >> (i * 4)) & 0xF];
  return out;
}

// ---- Anthropic -------------------------------------------------------------------

void putCacheControl(ArduinoJson::JsonObject block, bool longTtl) {
  ArduinoJson::JsonObject cc = block["cache_control"].to<ArduinoJson::JsonObject>();
  cc["type"] = "ephemeral";
  if (longTtl) cc["ttl"] = "1h";
}

int putAnthropicSystem(ArduinoJson::JsonArray system, const PromptParts& p) {
  if (!p.split) {
    addText(system, nonEmpty(p.stat), true, false);
    return 1;
  }
  int points = 0;
  if (!p.stat.empty()) { addText(system, p.stat, true, p.cache.longTtl); points++; }
  if (!p.shelf.empty()) { addText(system, p.shelf, true, p.cache.longTtl); points++; }
  return points;
}

int putAnthropicHistory(ArduinoJson::JsonArray messages, const PromptParts& p) {
  for (size_t i = 0; i < p.history.size(); i++) {
    const Msg& m = p.history[i];
    ArduinoJson::JsonObject o = messages.add<ArduinoJson::JsonObject>();
    o["role"] = m.user ? "user" : "assistant";
    addText(o["content"].to<ArduinoJson::JsonArray>(), nonEmpty(m.text), i + 1 == p.history.size(),
            p.cache.longTtl);
  }
  return p.history.empty() ? 0 : 1;
}

void textAsBlocks(ArduinoJson::JsonArray messages) {
  for (ArduinoJson::JsonObject m : messages) {
    if (!m["content"].is<const char*>()) continue;
    const std::string text = m["content"].as<const char*>();
    if (text.empty()) continue;   // an empty array (or empty block) is refused by the API
    addText(m["content"].to<ArduinoJson::JsonArray>(), text, false, false);
  }
}

int cacheLastMessage(ArduinoJson::JsonArray messages) {
  if (messages.size() == 0) return 0;
  ArduinoJson::JsonVariant content = messages[messages.size() - 1]["content"];
  if (!content.is<ArduinoJson::JsonArray>()) return 0;
  ArduinoJson::JsonArray blocks = content.as<ArduinoJson::JsonArray>();
  if (blocks.size() == 0) return 0;
  ArduinoJson::JsonObject last = blocks[blocks.size() - 1];
  if (!last["cache_control"].isNull()) return 0;   // already a point (never two on one block)
  putCacheControl(last, false);
  return 1;
}

int countAnthropicBreakpoints(ArduinoJson::JsonVariantConst request) {
  return countIn(request, "cache_control");
}

bool anthropicTtlOrderValid(ArduinoJson::JsonVariantConst request) {
  bool seenShort = false;
  for (const char* part : {"tools", "system", "messages"})
    if (!ttlOrderIn(request[part], seenShort)) return false;
  return true;
}

// ---- OpenAI --------------------------------------------------------------------

namespace {
ArduinoJson::JsonObject oaiMessage(ArduinoJson::JsonArray input, const char* role, const char* partType,
                                   const std::string& text, bool breakpoint) {
  ArduinoJson::JsonObject m = input.add<ArduinoJson::JsonObject>();
  m["type"] = "message";
  m["role"] = role;
  ArduinoJson::JsonObject part = m["content"].to<ArduinoJson::JsonArray>().add<ArduinoJson::JsonObject>();
  part["type"] = partType;
  part["text"] = text;
  if (breakpoint) part["prompt_cache_breakpoint"]["mode"] = "explicit";
  return m;
}
}  // namespace

void putOpenAIInput(ArduinoJson::JsonArray input, const PromptParts& p) {
  if (!p.stat.empty()) oaiMessage(input, "developer", "input_text", p.stat, true);
  if (!p.shelf.empty()) oaiMessage(input, "developer", "input_text", p.shelf, true);
  // The breakpoint goes on the last USER history message: an assistant item's
  // output_text part takes none.
  size_t lastUser = p.history.size();
  for (size_t i = 0; i < p.history.size(); i++)
    if (p.history[i].user) lastUser = i;
  for (size_t i = 0; i < p.history.size(); i++) {
    const Msg& m = p.history[i];
    oaiMessage(input, m.user ? "user" : "assistant", m.user ? "input_text" : "output_text", nonEmpty(m.text),
               i == lastUser);
  }
}

int countOpenAIBreakpoints(ArduinoJson::JsonVariantConst request) {
  return countIn(request["input"], "prompt_cache_breakpoint");
}

void putCacheKey(ArduinoJson::JsonObject request, const PromptParts& p) {
  if (!p.cache.key.empty()) request["prompt_cache_key"] = p.cache.key;
}

// ---- Mistral -------------------------------------------------------------------

void putMistralPrefix(ArduinoJson::JsonArray messages, const PromptParts& p) {
  ArduinoJson::JsonObject s = messages.add<ArduinoJson::JsonObject>();
  s["role"] = "system";
  s["content"] = nonEmpty(p.stat + p.shelf);
  for (const Msg& m : p.history) {
    ArduinoJson::JsonObject o = messages.add<ArduinoJson::JsonObject>();
    o["role"] = m.user ? "user" : "assistant";
    o["content"] = nonEmpty(m.text);
  }
}

}  // namespace layout
}  // namespace agent
