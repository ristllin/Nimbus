#include "web_engine2.h"

#include <Arduino.h>

#include <memory>
#include <string>

#include "nimbus/harness/plain/surface.h"

namespace nimbus::net::e2web {
namespace {

namespace surf = agent::plain::surface;

// One response that waits for the turn task's answer to the staged ask. The headers go out at once;
// the filler answers "try again" until the body is there, then streams it from PSRAM-backed memory.
struct Waiter {
  uint32_t id = 0;
  uint32_t startMs = 0;
  bool json = false;
  bool done = false;
  std::string body;
  size_t sent = 0;
};

void serve(AsyncWebServerRequest* r, const surf::AskReq& q, bool json) {
  const uint32_t id = surf::askBox().stage(q, millis());
  if (!id) {
    r->send(503, "text/plain", "Another page is being read. Try again in a moment.\n");
    return;
  }
  std::shared_ptr<Waiter> w(new Waiter());
  w->id = id;
  w->startMs = millis();
  w->json = json;
  AsyncWebServerResponse* res = r->beginChunkedResponse(
      json ? "application/json" : "text/plain; charset=utf-8",
      [w](uint8_t* buf, size_t maxLen, size_t) -> size_t {
        if (!w->done) {
          surf::AskAnswer a;
          if (surf::askBox().take(w->id, a)) {
            w->body = std::move(a.body);
            w->done = true;
          } else if (millis() - w->startMs > surf::kAskWaitMs) {
            surf::askBox().abandon(w->id);
            w->body = w->json ? surf::kBusyFoldJson : surf::kBusyText;
            w->done = true;
          } else {
            return RESPONSE_TRY_AGAIN;   // the turn task answers between turns
          }
        }
        if (w->sent >= w->body.size()) return 0;
        size_t take = w->body.size() - w->sent;
        if (take > maxLen) take = maxLen;
        memcpy(buf, w->body.data() + w->sent, take);
        w->sent += take;
        return take;
      });
  res->addHeader("Cache-Control", "no-store");
  res->addHeader("X-Content-Type-Options", "nosniff");   // untrusted model and tool text
  r->send(res);
}

uint64_t rowOf(const String& id) {   // "m<hex>": the day-stream row a turn's message mirrors
  if (id.length() < 2 || id[0] != 'm') return 0;
  return (uint64_t)strtoull(id.c_str() + 1, nullptr, 16);
}

}  // namespace

void lastTurn(AsyncWebServerRequest* r) {
  surf::AskReq q;
  q.kind = surf::Ask::LastTurn;
  if (r->hasParam("round")) q.round = (size_t)r->getParam("round")->value().toInt();
  serve(r, q, false);
}

void trace(AsyncWebServerRequest* r) {
  surf::AskReq q;
  q.kind = surf::Ask::Trace;
  if (r->hasParam("turn")) q.arg = rowOf(r->getParam("turn")->value());
  serve(r, q, false);
}

void foldStatus(AsyncWebServerRequest* r) {
  surf::AskReq q;
  q.kind = surf::Ask::Fold;
  if (r->hasParam("chat")) q.chat = r->getParam("chat")->value().c_str();
  serve(r, q, true);
}

}  // namespace nimbus::net::e2web
