#include "scenarios_monly.h"

#include <string>

#include "nimbus/harness/output_caps.h"

namespace lab {
namespace monly {
namespace {

// Every provider exchange of this run, one line each: host, status, size. The raw
// record the evidence keeps.
void printCalls(LabRig& rig, const Report& r) {
  for (const auto& c : rig.http().calls) {
    if (!isProviderHost(c.host) || c.path.find("/embeddings") != std::string::npos) continue;
    r.note(c.host + c.path + " -> " + std::to_string(c.status) +
           " body=" + std::to_string(c.reqBody.size()) + " B");
  }
}

}  // namespace

// A cut round on the mistral-large-4-0 head must step down same-host onto
// mistral-medium-latest (the one non-reasoning Mistral head) before a different
// provider is asked. On a Mistral-only client there is no different provider to
// ask, so this leg proves the whole path: the cut is forced for real
// (setTestCutRounds caps both attempts of the next round, so the provider's
// finish_reason is a real "length"), the step-down serves the round, the owner
// sees an answer with no switch notice, and every request on the wire went to
// Mistral.
void cutStepsDownSameHost(LabRig& rig, const Report& r) {
  const size_t before = rig.http().calls.size();
  agent::providers::setTestCutRounds(1, 48);
  auto t = rig.say("monly-stepdown",
                   "List the planets of the solar system with one short fact about each. "
                   "Then say how many there are.");
  agent::providers::setTestCutRounds(0, 0);
  printCalls(rig, r);
  r.note("reply: " + t.reply.substr(0, 400));

  bool stepped = false;
  for (const auto& l : rig.harnessLog())
    if (l.find("one same-host step on mistral-medium-latest") != std::string::npos) stepped = true;
  r.check(stepped, "the fabric logged the same-host step onto mistral-medium-latest");

  bool askedMedium = false;
  bool onlyMistral = true;
  for (size_t i = before; i < rig.http().calls.size(); ++i) {
    const auto& c = rig.http().calls[i];
    if (!isProviderHost(c.host) || c.path.find("/embeddings") != std::string::npos) continue;
    if (c.host != "api.mistral.ai") onlyMistral = false;
    if (c.reqBody.find("mistral-medium-latest") != std::string::npos) askedMedium = true;
  }
  r.check(onlyMistral, "every provider request went to api.mistral.ai (no other provider asked)");
  r.check(askedMedium, "a request on the wire carried mistral-medium-latest");

  r.check(!t.reply.empty() && t.reply.find("cut off") == std::string::npos,
          "an answer, not the cut marker");
  r.check(t.reply.find("switching to") == std::string::npos, "no owner notice: same provider");
}

}  // namespace monly
}  // namespace lab
