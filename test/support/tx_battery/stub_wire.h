#pragma once
#include <ArduinoJson.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../../lib/harness/src/providers/plain_body.h"
#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/tx/arena.h"
#include "nimbus/orch/tx/rules.h"

// tx_battery/stub_wire.h - a plain stand-in wire per dialect (lane RB-E1a), so the loop is
// proven before the real wires land (RB-W1a/W1b Anthropic, RB-W2 Responses, RB-W3 chat,
// RB-W4 flat). It renders the stored transcript the way PRD section 1 says each wire does
// (q order, the stub rule through tx::showResult, the ladder's state, device notes as user
// text, the send-time stamp on each owner message) and parses the scripted provider's
// answers. It does no caching marks, no photos beyond file ids, no thinking replay across
// turns: those are the real wires' properties and their own suites test them. The battery
// uses a real wire as soon as wireFor answers for the dialect.
namespace txb {

namespace plain = agent::plain;
namespace tx = nimbus::orch::tx;

class StubWire : public plain::WireCodec {
 public:
  explicit StubWire(plain::Dialect d) : d_(d) {}
  plain::Dialect dialect() const override { return d_; }

  plain::Status render(const plain::RenderInput& in, agent::HttpRequest& out) const override {
    if (!in.turn || !in.head || !in.system || !in.tools || !in.items)
      return plain::Status::fail(plain::Err::InvalidArgument, "txb::StubWire::render");
    out.method = "POST";
    out.host = in.head->host;
    out.path = d_ == plain::Dialect::AnthropicMessages ? "/v1/messages"
               : d_ == plain::Dialect::OpenAIResponses ? "/v1/responses"
                                                       : "/v1/chat/completions";
    out.headers.push_back({"content-type", "application/json"});
    std::vector<Piece> pieces;
    plain::Status st = collect(in, pieces);
    if (!st.ok()) return st;
    plain::body::BodyWriter w(out.body, &out.splices);
    w.beginObject().key("model").str(in.turn->model);
    if (in.maxTokens) w.key(d_ == plain::Dialect::OpenAIResponses ? "max_output_tokens" : "max_tokens").unum(in.maxTokens);
    system(w, *in.system);
    tools(w, *in.tools);
    toolChoice(w, in.toolChoiceNone);
    w.key(d_ == plain::Dialect::OpenAIResponses ? "input" : "messages").beginArray();
    if (d_ == plain::Dialect::ChatCompletions || d_ == plain::Dialect::Flat) {
      w.beginObject().key("role").str("system").key("content").str(joinedSystem(*in.system)).endObject();
    }
    messages(w, pieces);
    w.endArray().endObject();
    return w.status();
  }

  plain::Status parse(const agent::HttpResponse& in, plain::ParsedStep& out) const override {
    JsonDocument doc;
    if (deserializeJson(doc, in.body, DeserializationOption::NestingLimit(64)))
      return plain::Status::fail(plain::Err::InvalidArgument, "txb::StubWire::parse");
    if (d_ == plain::Dialect::AnthropicMessages) return parseAnthropic(doc, out);
    if (d_ == plain::Dialect::OpenAIResponses) return parseResponses(doc, out);
    return parseChat(doc, out);
  }

 private:
  enum class Role : uint8_t { User, Assistant, Tool };
  struct Piece {
    Role role;
    char kind;            // 'u' text, 'i' image, 'a' text, 'c' call, 'r' result, 'k' raw meta
    std::string text;     // text, result text, meta bytes
    std::string id, name, args;
    bool isError = false;
  };

  static bool jsonObject(const std::string& s) {
    JsonDocument d;
    return !deserializeJson(d, s, DeserializationOption::NestingLimit(64)) && d.is<JsonObjectConst>();
  }

  // Which items the request shows, and how, in q order.
  plain::Status collect(const plain::RenderInput& in, std::vector<Piece>& out) const {
    const tx::TxArena& arena = *in.items;
    std::vector<tx::Item> items(arena.count());
    for (size_t i = 0; i < items.size(); ++i) {
      plain::Status st = arena.get(i, items[i]);
      if (!st.ok()) return st;
    }
    std::vector<uint64_t> sup;
    for (const tx::Item& it : items)
      if (it.kind == tx::ItemKind::PointerMove) sup.insert(sup.end(), it.sup.begin(), it.sup.end());
    std::sort(sup.begin(), sup.end());
    const plain::LadderState none;
    const plain::LadderState& lad = in.ladder ? *in.ladder : none;
    tx::ShowFacts show;
    show.pointer = std::max(in.stubPointer, lad.closedThrough);
    show.openTurn = in.turn->turn;
    show.superseded = &sup;
    show.ladderStubThrough = lad.stubThrough;
    // Rung 3: the whole turns at or below omitThrough are left out.
    std::vector<uint32_t> omitted;
    for (const tx::Item& it : items)
      if (lad.omitThrough && it.q <= lad.omitThrough && it.turn &&
          std::find(omitted.begin(), omitted.end(), it.turn) == omitted.end())
        omitted.push_back(it.turn);
    std::vector<Piece> flat;
    if (!lad.omissionLine.empty()) flat.push_back(Piece{Role::User, 'u', lad.omissionLine, "", "", "", false});
    for (const tx::Item& it : items) {
      if (std::find(omitted.begin(), omitted.end(), it.turn) != omitted.end()) continue;
      plain::Status st = piece(in, it, show, flat);
      if (!st.ok()) return st;
    }
    pairResults(flat, out);
    return plain::Status::success();
  }

  // Each result right after the run of assistant pieces that holds its call, by call id: a
  // result stored late (the engine answers a call the card lost as "outcome unknown" at the
  // next turn open) still pairs with its call. A result with no call shown is left out.
  static void pairResults(const std::vector<Piece>& flat, std::vector<Piece>& out) {
    for (size_t i = 0; i < flat.size();) {
      if (flat[i].kind == 'r') {
        ++i;
        continue;
      }
      if (flat[i].role != Role::Assistant) {
        out.push_back(flat[i++]);
        continue;
      }
      size_t j = i;
      while (j < flat.size() && flat[j].role == Role::Assistant) out.push_back(flat[j++]);
      for (size_t c = i; c < j; ++c)
        if (flat[c].kind == 'c')
          for (const Piece& r : flat)
            if (r.kind == 'r' && r.id == flat[c].id) {
              out.push_back(r);
              break;
            }
      i = j;
    }
  }

  static void userPieces(const plain::RenderInput& in, const tx::Item& it, std::vector<Piece>& out) {
    char stamp[tx::kStampMaxBytes];
    std::string text = it.x;
    if (tx::messageStamp(it.t, it.tzMinutes, stamp, sizeof stamp).ok()) text = std::string(stamp) + " " + it.x;
    out.push_back(Piece{Role::User, 'u', text, "", "", "", false});
    for (const tx::Part& p : it.parts) {
      plain::PhotoBytes pb;
      if (p.kind == tx::PartKind::Image && in.photo && in.photo(p.ref, pb).ok() && !pb.fileId.empty())
        out.push_back(Piece{Role::User, 'i', pb.fileId, "", "", "", false});
      else
        out.push_back(Piece{Role::User, 'u', "[photo " + p.ref + " not shown: view id=" + p.ref + "]", "", "", "", false});
    }
  }

  static plain::Status resultPiece(const plain::RenderInput& in, const tx::Item& it, const tx::ShowFacts& show,
                                   std::vector<Piece>& out) {
    tx::ShowAs as = tx::ShowAs::Whole;
    plain::Status st = tx::showResult(it, show, as);
    if (!st.ok()) return st;
    std::string text = it.x;
    if (as == tx::ShowAs::Stub) text = it.st;
    else if (!it.handle.empty() && (!in.bulk || !in.bulk(it.handle, text).ok())) text = it.st;
    out.push_back(Piece{Role::Tool, 'r', text, it.id, it.n, "", !it.ok});
    return plain::Status::success();
  }

  plain::Status piece(const plain::RenderInput& in, const tx::Item& it, const tx::ShowFacts& show,
                      std::vector<Piece>& out) const {
    switch (it.kind) {
      case tx::ItemKind::User:
        userPieces(in, it, out);
        return plain::Status::success();
      case tx::ItemKind::Assistant:
        if (!it.x.empty()) out.push_back(Piece{Role::Assistant, 'a', it.x, "", "", "", false});
        return plain::Status::success();
      case tx::ItemKind::DeviceNote:
        out.push_back(Piece{Role::User, 'u', it.x, "", "", "", false});
        return plain::Status::success();
      case tx::ItemKind::RoundMeta:
        if (it.turn == in.turn->turn && jsonObject(it.x)) out.push_back(Piece{Role::Assistant, 'k', it.x, "", "", "", false});
        return plain::Status::success();
      case tx::ItemKind::Call:
        out.push_back(Piece{Role::Assistant, 'c', "", it.id, it.n, jsonObject(it.args) ? it.args : "{}", false});
        return plain::Status::success();
      case tx::ItemKind::Result:
        return resultPiece(in, it, show, out);
      default:
        return plain::Status::success();
    }
  }

  static std::string joinedSystem(const plain::SystemBlocks& s) {
    std::string out;
    for (const std::string* b : {&s.core, &s.notes, &s.digests})
      if (!b->empty()) out += (out.empty() ? "" : "\n\n") + *b;
    return out;
  }

  void system(plain::body::BodyWriter& w, const plain::SystemBlocks& s) const {
    if (d_ == plain::Dialect::OpenAIResponses) {
      w.key("instructions").str(joinedSystem(s));
      return;
    }
    if (d_ != plain::Dialect::AnthropicMessages) return;
    w.key("system").beginArray();
    for (const std::string* b : {&s.core, &s.notes, &s.digests})
      if (!b->empty()) w.beginObject().key("type").str("text").key("text").str(*b).endObject();
    w.endArray();
  }

  void tools(plain::body::BodyWriter& w, const std::vector<plain::ToolSpec>& ts) const {
    if (ts.empty()) return;
    w.key("tools").beginArray();
    for (const plain::ToolSpec& t : ts) {
      const std::string schema = t.schemaJson.empty() ? "{}" : t.schemaJson;
      w.beginObject();
      if (d_ == plain::Dialect::AnthropicMessages) {
        w.key("name").str(t.name).key("description").str(t.description).key("input_schema").raw(schema);
      } else if (d_ == plain::Dialect::OpenAIResponses) {
        w.key("type").str("function").key("name").str(t.name).key("description").str(t.description);
        w.key("parameters").raw(schema);
      } else {
        w.key("type").str("function").key("function").beginObject().key("name").str(t.name);
        w.key("description").str(t.description).key("parameters").raw(schema).endObject();
      }
      w.endObject();
    }
    w.endArray();
  }

  void toolChoice(plain::body::BodyWriter& w, bool none) const {
    if (d_ == plain::Dialect::AnthropicMessages) w.key("tool_choice").beginObject().key("type").str(none ? "none" : "auto").endObject();
    else w.key("tool_choice").str(none ? "none" : "auto");
  }

  void messages(plain::body::BodyWriter& w, const std::vector<Piece>& ps) const {
    if (d_ == plain::Dialect::OpenAIResponses) {
      for (const Piece& p : ps) responsesItem(w, p);
      return;
    }
    const bool chat = d_ != plain::Dialect::AnthropicMessages;
    // One message: consecutive pieces of one role (Anthropic results ride the user message;
    // chat gives each result its own tool message).
    auto roleOf = [chat](const Piece& p) { return p.role == Role::Tool && !chat ? Role::User : p.role; };
    for (size_t i = 0; i < ps.size();) {
      size_t j = i + 1;
      if (!(chat && ps[i].role == Role::Tool))
        while (j < ps.size() && !(chat && ps[j].role == Role::Tool) && roleOf(ps[j]) == roleOf(ps[i])) ++j;
      if (chat) chatMessage(w, ps, i, j);
      else anthropicMessage(w, roleOf(ps[i]), ps, i, j);
      i = j;
    }
  }

  void anthropicMessage(plain::body::BodyWriter& w, Role role, const std::vector<Piece>& ps, size_t i, size_t j) const {
    w.beginObject().key("role").str(role == Role::Assistant ? "assistant" : "user").key("content").beginArray();
    for (size_t n = i; n < j; ++n) {
      const Piece& p = ps[n];
      if (p.kind == 'k') {   // the provider's own block, as stored
        w.raw(p.text);
        continue;
      }
      w.beginObject();
      if (p.kind == 'u' || p.kind == 'a') w.key("type").str("text").key("text").str(p.text);
      else if (p.kind == 'i') w.key("type").str("image").key("source").beginObject().key("type").str("file").key("file_id").str(p.text).endObject();
      else if (p.kind == 'c') w.key("type").str("tool_use").key("id").str(p.id).key("name").str(p.name).key("input").raw(p.args);
      else if (p.kind == 'r') {
        w.key("type").str("tool_result").key("tool_use_id").str(p.id).key("content").str(p.text, plain::body::Over::Clip);
        if (p.isError) w.key("is_error").boolean(true);
      }
      w.endObject();
    }
    w.endArray().endObject();
  }

  void chatMessage(plain::body::BodyWriter& w, const std::vector<Piece>& ps, size_t i, size_t j) const {
    const Piece& first = ps[i];
    w.beginObject();
    if (first.role == Role::Tool) {
      w.key("role").str("tool").key("tool_call_id").str(first.id).key("content").str(first.text, plain::body::Over::Clip);
      w.endObject();
      return;
    }
    w.key("role").str(first.role == Role::Assistant ? "assistant" : "user");
    std::string text;
    std::vector<const Piece*> calls;
    for (size_t n = i; n < j; ++n) {
      if (ps[n].kind == 'c') calls.push_back(&ps[n]);
      else if (ps[n].kind == 'u' || ps[n].kind == 'a') text += (text.empty() ? "" : "\n") + ps[n].text;
    }
    if (text.empty() && !calls.empty()) w.key("content").null();
    else w.key("content").str(text);
    if (!calls.empty()) {
      w.key("tool_calls").beginArray();
      for (const Piece* c : calls) {
        w.beginObject().key("id").str(c->id).key("type").str("function").key("function").beginObject();
        w.key("name").str(c->name).key("arguments").str(c->args).endObject().endObject();
      }
      w.endArray();
    }
    w.endObject();
  }

  void responsesItem(plain::body::BodyWriter& w, const Piece& p) const {
    if (p.kind == 'k') {
      w.raw(p.text);
      return;
    }
    w.beginObject();
    if (p.kind == 'c') {
      w.key("type").str("function_call").key("call_id").str(p.id).key("name").str(p.name).key("arguments").str(p.args);
    } else if (p.kind == 'r') {
      w.key("type").str("function_call_output").key("call_id").str(p.id).key("output").str(p.text, plain::body::Over::Clip);
    } else {
      const bool a = p.role == Role::Assistant;
      w.key("role").str(a ? "assistant" : "user").key("content").beginArray().beginObject();
      if (p.kind == 'i') w.key("type").str("input_image").key("file_id").str(p.text);
      else w.key("type").str(a ? "output_text" : "input_text").key("text").str(p.text);
      w.endObject().endArray();
    }
    w.endObject();
  }

  static void usageOf(JsonVariantConst u, bool includesCache, plain::ParsedStep& out) {
    out.usage.cacheReadTokens = u["cache_read_input_tokens"] | (u["input_tokens_details"]["cached_tokens"] |
                                                                 (u["prompt_tokens_details"]["cached_tokens"] | 0u));
    out.usage.cacheWriteTokens = u["cache_creation_input_tokens"] | 0u;
    const uint32_t in = u["input_tokens"] | (u["prompt_tokens"] | 0u);
    out.usage.promptTokens = includesCache && in >= out.usage.cacheReadTokens ? in - out.usage.cacheReadTokens : in;
    out.usage.completionTokens = u["output_tokens"] | (u["completion_tokens"] | 0u);
    out.usage.responses = 1;
  }

  static plain::Status parseAnthropic(const JsonDocument& doc, plain::ParsedStep& out) {
    if (!doc["content"].is<JsonArrayConst>()) return plain::Status::fail(plain::Err::InvalidArgument, "txb::StubWire anthropic");
    for (JsonObjectConst b : doc["content"].as<JsonArrayConst>()) {
      const std::string type = b["type"] | "";
      if (type == "text") out.text += b["text"] | "";
      else if (type == "tool_use") {
        plain::ToolCall c;
        c.id = b["id"] | "";
        c.name = b["name"] | "";
        serializeJson(b["input"], c.argsJson);
        out.calls.push_back(c);
      } else if (type == "thinking") {
        plain::MetaPart m;
        m.beforeCall = (uint16_t)out.calls.size();
        serializeJson(b, m.bytes);
        out.meta.push_back(m);
      }
    }
    const std::string stop = doc["stop_reason"] | "";
    out.stop = stop == "max_tokens" ? plain::StopReason::MaxTokens
               : stop == "tool_use" ? plain::StopReason::ToolUse
               : stop == "refusal"  ? plain::StopReason::Refusal
                                    : plain::StopReason::EndTurn;
    usageOf(doc["usage"], false, out);
    return plain::Status::success();
  }

  static plain::Status parseResponses(const JsonDocument& doc, plain::ParsedStep& out) {
    if (!doc["output"].is<JsonArrayConst>()) return plain::Status::fail(plain::Err::InvalidArgument, "txb::StubWire responses");
    for (JsonObjectConst it : doc["output"].as<JsonArrayConst>()) {
      const std::string type = it["type"] | "";
      if (type == "message") {
        for (JsonObjectConst p : it["content"].as<JsonArrayConst>()) out.text += p["text"] | (p["refusal"] | "");
      } else if (type == "function_call") {
        plain::ToolCall c;
        c.id = it["call_id"] | "";
        c.name = it["name"] | "";
        c.argsJson = it["arguments"] | "{}";
        c.complete = jsonObject(c.argsJson);
        out.calls.push_back(c);
      } else if (type == "reasoning") {
        plain::MetaPart m;
        m.beforeCall = (uint16_t)out.calls.size();
        serializeJson(it, m.bytes);
        out.meta.push_back(m);
      }
    }
    const bool cut = std::string(doc["status"] | "") == "incomplete";
    out.stop = cut ? plain::StopReason::MaxTokens : out.calls.empty() ? plain::StopReason::EndTurn : plain::StopReason::ToolUse;
    usageOf(doc["usage"], true, out);
    return plain::Status::success();
  }

  static plain::Status parseChat(const JsonDocument& doc, plain::ParsedStep& out) {
    JsonObjectConst choice = doc["choices"][0];
    if (choice.isNull()) return plain::Status::fail(plain::Err::InvalidArgument, "txb::StubWire chat");
    JsonObjectConst msg = choice["message"];
    out.text = msg["content"] | "";
    for (JsonObjectConst tc : msg["tool_calls"].as<JsonArrayConst>()) {
      plain::ToolCall c;
      c.id = tc["id"] | "";
      c.name = tc["function"]["name"] | "";
      c.argsJson = tc["function"]["arguments"] | "{}";
      c.complete = jsonObject(c.argsJson);
      out.calls.push_back(c);
    }
    const std::string fin = choice["finish_reason"] | "";
    out.stop = fin == "length" ? plain::StopReason::MaxTokens : fin == "tool_calls" ? plain::StopReason::ToolUse : plain::StopReason::EndTurn;
    usageOf(doc["usage"], true, out);
    return plain::Status::success();
  }

  plain::Dialect d_;
};

// The wire the battery renders with: the dialect's real one once its lane has landed.
inline const plain::WireCodec* batteryWire(plain::Dialect d) {
  const plain::WireCodec* w = nullptr;
  if (plain::wireFor(d, w).ok() && w) return w;
  static const StubWire stubs[] = {StubWire(plain::Dialect::AnthropicMessages), StubWire(plain::Dialect::OpenAIResponses),
                                   StubWire(plain::Dialect::ChatCompletions), StubWire(plain::Dialect::Flat)};
  return &stubs[(size_t)d];
}

}  // namespace txb
