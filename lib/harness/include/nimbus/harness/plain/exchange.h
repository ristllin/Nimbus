#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/http.h"
#include "nimbus/harness/plain/wire.h"

namespace agent {
struct Platform;
}  // namespace agent

// plain/exchange.h - one exchange (PRD section 7): render, post through the kept
// transport, read, the output cap, the retry policy for a direct head (same-host retry
// after 2 s then 8 s on transport, 5xx and 429, at most two per round), cut handling
// (one retry at the largest cap the window allows; cut tool arguments are never
// dispatched) and the checked body writer over providers/wire.h.
//
// Seam declared by lane RB-B0a; the body (plain_exchange.cpp) is lane RB-EX's.
namespace agent {
namespace plain {

// The error class of an exchange. Slugs ride the e item: frozen.
enum class ExchangeErr : uint8_t {
  None = 0,
  Transport = 1,       // connect, TLS, timeout
  Server = 2,          // 5xx
  RateLimited = 3,     // 429
  ContextRefused = 4,  // the provider refused the size: ladder rung 3 and one retry
  KeyRefused = 5,      // billing or key: park the host
  Cut = 6,             // the answer was cut after its retry
  Malformed = 7,       // the answer did not parse
  Other = 8,           // anything else: one plain line
  Blocked = 9,         // a moderation block (the router's): the moderation status line
};
inline const char* exchangeErrSlug(ExchangeErr e) {
  switch (e) {
    case ExchangeErr::None: return "ok";
    case ExchangeErr::Transport: return "transport";
    case ExchangeErr::Server: return "server";
    case ExchangeErr::RateLimited: return "rate-limited";
    case ExchangeErr::ContextRefused: return "context";
    case ExchangeErr::KeyRefused: return "key";
    case ExchangeErr::Cut: return "cut";
    case ExchangeErr::Malformed: return "malformed";
    case ExchangeErr::Other: return "other";
    case ExchangeErr::Blocked: return "blocked";
  }
  return "other";
}

struct ExchangeDeps {
  HttpTransport* transport = nullptr;
  const Platform* platform = nullptr;
};

struct ExchangeRequest {
  const WireCodec* wire = nullptr;
  const RenderInput* render = nullptr;
  uint32_t timeoutMs = 0;      // the host's read window (Anthropic 120 s, ...)
  uint32_t deadlineMs = 0;     // the turn's remaining soft wall clock
};

struct ExchangeResult {
  ParsedStep step;
  ExchangeErr err = ExchangeErr::None;
  int httpStatus = 0;
  uint8_t retries = 0;
  uint32_t requestBytes = 0;
  uint32_t ms = 0;
  std::string errorLine;       // the one plain line for the owner when err != None
};

Status runExchange(const ExchangeDeps& deps, const ExchangeRequest& req, ExchangeResult& out);

}  // namespace plain
}  // namespace agent
