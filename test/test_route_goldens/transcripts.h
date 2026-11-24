#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/connectors_wire.h"   // lane E2-P-CONN2
#include "nimbus/orch/tx/item.h"

// transcripts.h - the stored transcripts the route goldens render (lane E2-R2a, router design
// section 11 layer L1). Everything here is made-up data written for this suite alone: its own
// item builder, its own small system text and its own two tools, never the live prompt or the
// live tool descriptions, and never the battery's shared builders. So a prompt change, a tool
// description change or a change to another suite's support code moves no golden; only a change
// in what the wire writes for the same stored state does.
namespace route_goldens {

namespace tx = nimbus::orch::tx;
namespace plain = agent::plain;

// 2026-11-17 13:02:00 UTC, a Tuesday.
inline constexpr uint64_t kT0 = 1794920520ULL;

// One stored transcript, built item by item as the engine would store it.
struct Tx {
  std::vector<tx::Item> items;
  std::map<std::string, std::string> bulk;   // handle -> payload
  uint64_t q = 0;
  uint32_t turn = 0;
  uint64_t t = kT0;
  int16_t tz = 0;
  uint8_t round = 0;

  tx::Item& add(tx::ItemKind k) {
    tx::Item it;
    it.q = ++q;
    it.kind = k;
    it.turn = turn;
    it.t = t;
    items.push_back(it);
    return items.back();
  }
  // A t item; `photos` are the photos the turn shows (tx::visiblePhotos at turn open).
  void open(const std::vector<std::string>& photos = {}) {
    ++turn;
    round = 0;
    tx::Item& it = add(tx::ItemKind::TurnOpen);
    it.trigger = "owner";
    it.head = "api.anthropic.com/claude-sonnet-5-5";
    it.profile = "standard";
    it.photos = photos;
  }
  uint64_t owner(const std::string& text, const std::vector<std::string>& photos = {}, const std::string& note = "") {
    tx::Item& it = add(tx::ItemKind::User);
    it.x = text;
    it.ch = "tg";
    it.tzMinutes = tz;
    it.src = {9000 + q};
    for (const std::string& ref : photos) {
      tx::Part p{tx::PartKind::Image, ref, "image/jpeg", 41000};
      p.note = note;
      it.parts.push_back(p);
    }
    return it.q;
  }
  uint64_t say(const std::string& text, bool fin = true) {
    tx::Item& it = add(tx::ItemKind::Assistant);
    it.x = text;
    it.rd = round;
    it.fin = fin;
    return it.q;
  }
  uint64_t call(const std::string& id, const std::string& name, const std::string& args, bool effect = false) {
    tx::Item& it = add(tx::ItemKind::Call);
    it.id = id;
    it.n = name;
    it.args = args;
    it.eff = effect;
    return it.q;
  }
  uint64_t intent(const std::string& id, const std::string& name) {
    tx::Item& it = add(tx::ItemKind::Intent);
    it.id = id;
    it.n = name;
    return it.q;
  }
  static std::string stubOf(const std::string& name, uint64_t q, size_t bytes) {
    return "[stub " + name + " r" + std::to_string(q) + " " + std::to_string(bytes) + "B, view id=r" + std::to_string(q) + "]";
  }
  uint64_t receipt(const std::string& id, const std::string& name, const std::string& text, bool ok = true) {
    tx::Item& it = add(tx::ItemKind::Result);
    it.id = id;
    it.n = name;
    it.x = text;
    it.ok = ok;
    it.cls = tx::ResultClass::Receipt;
    it.bytes = (uint32_t)text.size();
    it.lines = 1;
    it.st = stubOf(name, it.q, text.size());
    return it.q;
  }
  uint64_t bulkResult(const std::string& id, const std::string& name, const std::string& payload,
                      tx::ResultClass cls = tx::ResultClass::Bulk) {
    tx::Item& it = add(tx::ItemKind::Result);
    it.id = id;
    it.n = name;
    it.cls = cls;
    it.handle = "b/" + std::to_string(turn) + ".txb@" + std::to_string(it.q);
    it.bytes = (uint32_t)payload.size();
    it.lines = 1;
    it.st = stubOf(name, it.q, payload.size());
    bulk[it.handle] = payload;
    return it.q;
  }
  uint64_t meta(const std::string& json) {
    tx::Item& it = add(tx::ItemKind::RoundMeta);
    it.x = json;
    it.rd = round;
    return it.q;
  }
  uint64_t note(const std::string& text, tx::NoteClass cls = tx::NoteClass::Notice) {
    tx::Item& it = add(tx::ItemKind::DeviceNote);
    it.x = text;
    it.note = cls;
    return it.q;
  }
  uint64_t move(uint64_t to, const char* why, const std::vector<uint64_t>& sup = {}) {
    tx::Item& it = add(tx::ItemKind::PointerMove);
    it.stub = to;
    it.why = why;
    it.sup = sup;
    return it.q;
  }
  uint64_t digest(const std::string& text, const std::string& day, uint64_t thru) {
    tx::Item& it = add(tx::ItemKind::Digest);
    it.x = text;
    it.day = day;
    it.thru = thru;
    return it.q;
  }
  void next() { ++round; }
  void advance(uint64_t seconds) { t += seconds; }
  void close() {
    tx::Item& d = add(tx::ItemKind::Delivery);
    d.msgIds = {(int64_t)(700 + turn)};
    tx::Item& e = add(tx::ItemKind::TurnEnd);
    e.end.cause = "reply";
    e.end.rounds = (uint16_t)(round + 1);
  }
  void chatTurn(const std::string& ask, const std::string& answer) {
    open();
    owner(ask);
    say(answer);
    close();
  }
};

// A payload of exactly `bytes` bytes that starts with `marker`.
inline std::string payload(const std::string& marker, size_t bytes) {
  std::string s = marker + "\n";
  for (int line = 1; s.size() < bytes; ++line) s += "line " + std::to_string(line) + " of " + marker + "\n";
  s.resize(bytes);
  return s;
}

// The facts around a transcript for one request: everything RenderInput takes besides the items.
enum class Sys : uint8_t { Chat, Summarizer };
enum class Photos : uint8_t { FileRef, Inline };
// Lane E2-P-CONN2: the fixture connectors on the head (HeadEndpoint::connectors), for whose turn.
enum class Conn : uint8_t { None, Owner, Member };

struct Facts {
  std::string effort = "medium";   // HeadEndpoint::effort ("" = none sent)
  uint32_t maxTokens = 4096;
  uint64_t pointer = 0;
  plain::LadderState ladder;
  bool toolChoiceNone = false;
  bool schema = false;             // the summarizer's structured form (RenderInput::answerSchema)
  bool longCache = false;
  bool routed = false;
  bool noTools = false;
  bool viewBound = false;
  Photos photos = Photos::FileRef;
  Sys system = Sys::Chat;
  uint16_t dayStartMin = 240;
  uint32_t openTurn = 0;           // 0: the transcript's last turn
  Conn conn = Conn::None;
};

// Lane E2-P-CONN2: the owner's connectors as engine 1's blob parse gives them, the Code sandbox on: one
// Anthropic server with a token and one without, a LAN server and a disabled one (never sent), an
// OpenAI server and a first-party connector with a token, a Mistral Studio connector.
inline std::vector<nimbus::orch::ConnectorInfo> fixtureConnectors() {
  auto one = [](const char* name, const char* prov, const char* kind, const char* url, bool on) {
    nimbus::orch::ConnectorInfo c;
    c.name = c.type = name;
    c.prov = prov;
    c.kind = kind;
    c.url = url;
    c.enabled = on;
    return c;
  };
  std::vector<nimbus::orch::ConnectorInfo> cs = {
      one("docs", "anthropic", "mcp", "https://mcp.docs.example.test/mcp", true),
      one("tickets", "anthropic", "mcp", "https://tickets.example.test/sse", true),
      one("nas", "anthropic", "mcp", "http://192.168.1.20:8080/mcp", true),
      one("old", "anthropic", "mcp", "https://old.example.test/mcp", false),
      one("github", "openai", "mcp", "https://mcp.github.example.test/", true),
      one("gmail", "openai", "connector", "", true),
      one("gcal", "mistral", "connector", "", true),
  };
  cs[5].connectorId = "connector_gmail";
  nimbus::orch::applyCodeSandbox(cs, true);
  return cs;
}
inline std::string fixtureBearer(const nimbus::orch::ConnectorInfo& c) {
  return c.name == "docs" || c.name == "gmail" ? "fixture-bearer-" + c.name : std::string();
}

struct Case {
  std::string name;
  std::string signature;           // the row signature directory (design section 11 L1)
  Tx tx;
  Facts facts;
};

inline constexpr const char* kDirect = "messages-anthropic-direct";
inline constexpr const char* kRouted = "messages-anthropic-routed";
// The chat rows (lane E2-R6): Mistral and Z.ai direct and routed, a custom endpoint (text mode).
inline constexpr const char* kMistral = "chat-mistral-direct";
inline constexpr const char* kMistralRouted = "chat-mistral-routed";
inline constexpr const char* kZai = "chat-zai-direct";
inline constexpr const char* kZaiRouted = "chat-zai-routed";
inline constexpr const char* kCustomChat = "chat-custom_chat-direct";
// The Responses rows (lane E2-R7): OpenAI direct and routed.
inline constexpr const char* kResponsesDirect = "responses-openai-direct";
inline constexpr const char* kResponsesRouted = "responses-openai-routed";

// The head a signature names: the dialect, the turn's host (a provider slug) and model, the
// endpoint host, and whether the render goes through the route and pre-flight (every chat row; the
// Messages goldens render as they did before the route, byte for byte).
struct RowFacts {
  plain::Dialect dialect = plain::Dialect::AnthropicMessages;
  const char* turnHost = "anthropic";
  const char* model = "claude-sonnet-5-5";
  const char* headHost = "api.anthropic.com";
  bool routed = false;
  bool route = false;
};

inline RowFacts rowOf(const std::string& sig, bool routedFact) {
  const plain::Dialect chat = plain::Dialect::ChatCompletions;
  if (sig == kMistral) return {chat, "mistral", "mistral-large-4-0", "api.mistral.ai", false, true};
  if (sig == kMistralRouted) return {chat, "cumulo", "mistral/mistral-large-4-0", "router.cumulo.test", true, true};
  if (sig == kZai) return {chat, "zai", "glm-5.3", "api.z.ai", false, true};
  if (sig == kZaiRouted) return {chat, "cumulo", "zai/glm-5.3", "router.cumulo.test", true, true};
  if (sig == kCustomChat) return {chat, "custom", "local-model", "llm.example.test", false, true};
  const plain::Dialect responses = plain::Dialect::OpenAIResponses;
  if (sig == kResponsesDirect) return {responses, "openai", "gpt-5.6", "api.openai.com", false, true};
  if (sig == kResponsesRouted) return {responses, "cumulo", "openai/gpt-5.6", "router.cumulo.test", true, true};
  RowFacts r;
  if (routedFact) {
    r.turnHost = "cumulo";
    r.headHost = "router.cumulo.test";
    r.routed = true;
  }
  return r;
}

// The fixed inputs around every transcript.
inline constexpr const char* kCore = "You are a test assistant for the route goldens. Answer briefly.";
inline constexpr const char* kNotes = "Pinned: the test garden has three beds.";
inline constexpr const char* kDigests = "Mon 16 Nov: planned the spring beds.";
inline constexpr const char* kSummaryCore = "Summary so far: the owner plans three garden beds.";
inline constexpr const char* kSummaryAsk = "Fold the slice below into the summary. Answer with the changes only.";
inline constexpr const char* kSchema =
    R"({"type":"object","properties":{"changes":{"type":"array","items":{"type":"string"}}},"required":["changes"],"additionalProperties":false})";
inline constexpr const char* kOmission = "[two earlier turns are left out of this request]";

inline std::vector<plain::ToolSpec> fixedTools() {
  return {{"read_note", "Read one note by its path.",
           R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"},
          {"view", "Show an earlier result or photo again by its id.",
           R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})"}};
}

}  // namespace route_goldens
