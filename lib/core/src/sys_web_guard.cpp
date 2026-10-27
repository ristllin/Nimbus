#include "nimbus/sys/web_guard.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace nimbus::sys {

uint32_t slotWaitMs(bool webTask, uint32_t requestedMs) {
  return webTask ? std::min(requestedMs, kWebSlotWaitMs) : requestedMs;
}

uint32_t slotWaitMs(bool webTask, uint32_t requestedMs, uint32_t leftMs) {
  if (!webTask) return requestedMs;
  if (leftMs < kWebNetMinExchangeMs) return 0;
  return std::min(slotWaitMs(true, requestedMs), leftMs - kWebNetMinExchangeMs);
}

uint32_t netBudgetMs(bool webTask, uint32_t requestedMs) {
  return webTask ? std::min(requestedMs, kWebNetBudgetMs) : requestedMs;
}

uint32_t webNetLeftMs(uint32_t elapsedMs) {
  return elapsedMs >= kWebNetAllowanceMs ? 0 : kWebNetAllowanceMs - elapsedMs;
}

WebNetShare webNetShare(uint32_t leftMs) {
  WebNetShare s;
  const uint32_t cap = std::min(leftMs, kWebNetBudgetMs);
  if (cap < kWebNetMinExchangeMs) return s;
  s.go = true;
  s.connectMs = std::min(kWebNetConnectMs, cap * 2 / 5);
  s.handshakeS = std::min(kWebNetHandshakeS, (cap - s.connectMs) / 1000);
  s.replyMs = cap;
  return s;
}

bool toolWaitsOnNetwork(const std::string& name) {
  static const char* const kTools[] = {
      "image.generate",   // a 90-120 s image download
      "image.look",       // a photo caption request, up to two providers
      "reply.telegram",   // a Telegram upload (a voice reply: speech synthesis first)
      "reply.speak",      // speech synthesis and the playback wait
      "web.search",       // a search provider request
      "device.control",   // a tts action: speech synthesis and the speaker's start wait
  };
  for (const char* t : kTools)
    if (name == t) return true;
  // A tool a connected tool server (MCP) offers: a call to that server.
  return name.size() > 4 && name.compare(0, 4, "mcp.") == 0;
}

std::string turnOnlyToolRpcError(const std::string& name, const std::string& idJson) {
  ArduinoJson::JsonDocument id;   // the request's id echoed back; anything unreadable is null
  if (deserializeJson(id, idJson) != ArduinoJson::DeserializationError::Ok) id.clear();
  ArduinoJson::JsonDocument d;
  d["jsonrpc"] = "2.0";
  d["id"] = id.as<ArduinoJson::JsonVariantConst>();
  d["error"]["code"] = -32003;
  d["error"]["message"] = name + " waits on the network, so it runs only in the device's own turns, "
                                 "not over the LAN endpoint";
  std::string out;
  serializeJson(d, out);
  return out;
}

const char* cardBusyJson() {
  return "{\"error\":\"busy\",\"message\":\"The device is busy right now. Try again in a few seconds.\"}";
}

}  // namespace nimbus::sys
