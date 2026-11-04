#pragma once
#include <string>
#include <vector>

#include "lab_env.h"
#include "lab_rig.h"

// Live + scripted legs for lane CONN: a Mistral-only client (only MISTRAL_API_KEY
// in the environment, head pinned to mistral-large-4-0, the model that refuses
// the built-in connectors) gets honest connector answers:
//
//   conn-gmail-none        Gmail is not configured: the head says so, names the
//                          two-step setup (Mistral's console, then the device
//                          page), invents no mailbox data, and no provider other
//                          than Mistral is ever asked.
//   conn-gmail-unauth      Gmail is configured as a Mistral Studio connector
//                          whose id no workspace can offer. The provider's real
//                          answer (recorded live) is a silent no-run: the
//                          conversation is accepted and the connector never
//                          executes, and the device's provenance check is what
//                          tells the owner. No mailbox content is invented.
//                          Live Mistral, no real mailbox is touched.
//   conn-notion-success    Notion via Mistral, the success path on a fake router:
//                          the sub's request lands on mistral-medium-latest (a
//                          model that accepts connectors) after the recorded
//                          builtin-connectors refusal, the connector's run is
//                          counted, and the reply says which model ran.
//
// Run with a dotenv; the legs drop the other providers' keys themselves so
// "no other provider asked" is a real assertion, not an artifact of the
// priority order:
//   nimbus-lab scenarios conn-gmail-none conn-gmail-unauth conn-notion-success
namespace lab {
namespace conn {

int run(const Env& env, const LabRig::Options& opt, const std::vector<std::string>& names,
        int& ranOut);

}  // namespace conn
}  // namespace lab
