#pragma once
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/harness/fabric.h"

// FakeAdapter - a scripted ManagedAgentAdapter for host tests. dispatch()
// returns "<backend>:job-N"; poll() walks a scripted JobState sequence per
// job (repeating the last state once exhausted). Records every Directive.
namespace harness_test {

struct FakeAdapter : agent::ManagedAgentAdapter {
  std::string backend = "fake";
  agent::FabricErr dispatchErr = agent::FabricErr::Ok;
  agent::FabricErr pollErr = agent::FabricErr::Ok;

  struct SeenDirective {
    std::string category, instruction, chatId, tag, model, skill;
  };
  std::vector<SeenDirective> dispatched;

  // Script: states each successive poll() reports (applies to every job).
  std::vector<agent::JobState> pollScript{agent::JobState::Running,
                                          agent::JobState::Done};
  std::string doneReply = "fake result";
  // Optional scripted sub-session usage, reported on every poll (real adapters
  // fill it only when the provider returns a usage object - e.g. OpenAI).
  uint32_t usageIn = 0, usageOut = 0;
  int pollCount = 0;

  const char* backendId() const override { return backend.c_str(); }
  agent::Capabilities capabilities() const override { return {}; }

  agent::FabricErr dispatch(const agent::Directive& d, char outJobId[72]) override {
    if (dispatchErr != agent::FabricErr::Ok) return dispatchErr;
    SeenDirective s;
    s.category    = d.category    ? d.category    : "";
    s.instruction = d.instruction ? d.instruction : "";
    s.chatId      = d.chatId      ? d.chatId      : "";
    s.tag         = d.tag         ? d.tag         : "";
    s.model       = d.model       ? d.model       : "";
    s.skill       = d.skill       ? d.skill       : "";
    dispatched.push_back(s);
    snprintf(outJobId, 72, "%s:job-%d", backend.c_str(), (int)dispatched.size());
    return agent::FabricErr::Ok;
  }

  agent::FabricErr poll(const char* jobId, agent::ResultEnvelope& env) override {
    (void)jobId;
    if (pollErr != agent::FabricErr::Ok) return pollErr;
    size_t i = (size_t)pollCount < pollScript.size() ? pollCount
                                                     : pollScript.size() - 1;
    pollCount++;
    env.state = pollScript.empty() ? agent::JobState::Running : pollScript[i];
    env.promptTokens = usageIn;
    env.completionTokens = usageOut;
    if (env.state == agent::JobState::Done)
      strncpy(env.reply, doneReply.c_str(), sizeof(env.reply) - 1);
    return agent::FabricErr::Ok;
  }

  agent::FabricErr cancel(const char*) override { return agent::FabricErr::Ok; }
};

}  // namespace harness_test
