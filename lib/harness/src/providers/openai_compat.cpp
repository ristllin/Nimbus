#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "nimbus/harness/providers.h"
#include "wire.h"

// openai_compat - a PORTABLE synchronous sub-session over the OpenAI
// chat-completions dialect for ANY compatible endpoint, parameterized by
// host/basePath/key/model (not the pd.custom* closures), so several such
// providers run at once. Backs the Z.ai (GLM) and Cumulo router device adapters.
// A generic proxy exposes a synchronous completion, not an async agent harness,
// so dispatch runs inline and caches; poll returns immediately.
namespace agent {
namespace providers {

using wire::exchange;
using wire::serializeBody;

namespace {
struct CompatSlot {
  char jobId[96];
  std::string reply;
  bool used;
};
// One slot per possible in-flight sub-agent (kAgentMaxJobs = 6) plus headroom, so
// a full fan-out of zai/cumulo subs whose replies are not yet polled never evicts
// an un-polled result. Proxy jobs are short-lived; dispatch prefers a free slot
// and only round-robin-evicts when every slot still holds an unpolled reply.
static CompatSlot s_slots[8];
static int s_next = 0;

CompatSlot* findSlot(const char* jobId) {
  for (CompatSlot& s : s_slots)
    if (!s.used && s.jobId[0] && strcmp(s.jobId, jobId) == 0) return &s;
  return nullptr;
}

// Pick a slot to write: a free one (never used, or already drained) if any, else
// the round-robin oldest (last resort, only when all slots hold unpolled replies).
CompatSlot& claimSlot() {
  const int n = (int)(sizeof(s_slots) / sizeof(s_slots[0]));
  for (int i = 0; i < n; ++i)
    if (s_slots[i].used || s_slots[i].jobId[0] == 0) return s_slots[i];
  CompatSlot& s = s_slots[s_next];
  s_next = (s_next + 1) % n;
  return s;
}

// Build the request for the endpoint's wire: sets `path` + response `filter`, and
// returns the serialized body. OpenAI/Mistral use chat-completions; Anthropic uses
// Messages (system is top-level, max_tokens required, reply at content[0].text).
std::string buildCompatRequest(const CompatEndpoint& ep, const Directive& d,
                               const std::string& sysText, std::string& path,
                               JsonDocument& filter) {
  JsonDocument doc;
  doc["model"] = ep.model;
  const char* instr = d.instruction ? d.instruction : "";
  if (ep.wire == CompatWire::AnthropicMessages) {
    path = ep.basePath + "/messages";
    doc["max_tokens"] = 1024;
    doc["system"] = sysText;
    JsonObject u = doc["messages"].to<JsonArray>().add<JsonObject>();
    u["role"] = "user";
    u["content"] = instr;
    filter["content"][0]["text"] = true;
  } else {
    path = ep.basePath + "/chat/completions";
    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = sysText;
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user";
    u["content"] = instr;
    filter["choices"][0]["message"]["content"] = true;
  }
  filter["error"]["message"] = true;
  return serializeBody(doc);
}
}  // namespace

FabricErr openaiCompatDispatch(const ProviderDeps& pd, const CompatEndpoint& ep,
                               const Directive& d, char outJobId[72]) {
  if (!ep.host || !*ep.host || ep.model.empty()) return FabricErr::BadRequest;
  const char* backendTag = ep.backendTag ? ep.backendTag : "compat";
  const bool anthropic = (ep.wire == CompatWire::AnthropicMessages);
  const std::string sysText = std::string("You are an autonomous ") +
                              (d.category ? d.category : "ops") +
                              " agent. Complete the task and reply with the final result only.";

  std::string path;
  JsonDocument filter;
  std::string body = buildCompatRequest(ep, d, sysText, path, filter);

  std::vector<std::pair<std::string, std::string>> headers;
  if (!ep.key.empty()) headers.push_back({"Authorization", "Bearer " + ep.key});
  headers.push_back({"Content-Type", "application/json"});
  // The Anthropic Messages wire requires a version header. A router that proxies
  // on the native wire adds it, but sending it is harmless there and correct if
  // the endpoint is Anthropic directly.
  if (anthropic) headers.push_back({"anthropic-version", "2023-06-01"});

  JsonDocument out;
  int code = exchange(pd, ep.host, ep.port, ep.tls, "POST", path, std::move(headers),
                      std::move(body), 30000, out, filter);
  if (code == 401 || code == 403) return FabricErr::Auth;
  if (code <= 0) return FabricErr::Network;
  if (code != 200) {
    hlog::logf("%s: HTTP %d", backendTag, code);
    return FabricErr::RemoteFail;
  }
  std::string reply = anthropic ? std::string((const char*)(out["content"][0]["text"] | ""))
                                : std::string((const char*)(out["choices"][0]["message"]["content"] | ""));
  if (reply.empty()) return FabricErr::ParseFail;

  char id[24];
  snprintf(id, sizeof(id), "%08lx", (unsigned long)(pd.nowMs ? pd.nowMs() : 0));
  snprintf(outJobId, 72, "%s:%s", backendTag, id);
  CompatSlot& slot = claimSlot();
  strncpy(slot.jobId, outJobId, sizeof(slot.jobId) - 1);
  slot.jobId[sizeof(slot.jobId) - 1] = 0;
  slot.reply = reply;
  slot.used = false;
  hlog::logf("%s: completed %s (%u chars)", backendTag, id, (unsigned)reply.length());
  return FabricErr::Ok;
}

FabricErr openaiCompatPoll(const ProviderDeps& pd, const char* backendTag, const char* jobId,
                           ResultEnvelope& env) {
  (void)pd;
  strncpy(env.jobId, jobId, sizeof(env.jobId) - 1);
  env.jobId[sizeof(env.jobId) - 1] = 0;
  strncpy(env.backend, backendTag, sizeof(env.backend) - 1);
  env.backend[sizeof(env.backend) - 1] = 0;
  CompatSlot* slot = findSlot(jobId);
  if (!slot) return FabricErr::NotFound;
  env.state = JobState::Done;
  strncpy(env.reply, slot->reply.c_str(), sizeof(env.reply) - 1);
  env.reply[sizeof(env.reply) - 1] = 0;
  slot->used = true;
  slot->reply.clear();
  slot->jobId[0] = 0;
  return FabricErr::Ok;
}

FabricErr openaiCompatCancel(const ProviderDeps& pd, const char* jobId) {
  (void)pd;
  CompatSlot* slot = findSlot(jobId);
  if (slot) {
    slot->used = true;
    slot->reply.clear();
    slot->jobId[0] = 0;
  }
  return FabricErr::Ok;
}

}  // namespace providers
}  // namespace agent
