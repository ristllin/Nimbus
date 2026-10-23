#pragma once
// model_selectable - the ONE "is this model allowed" check (lane F, R5).
//
// The recurring bug: the head's orch_model and spawn checks read the short NVS list,
// the web picker also accepted the fuller catalog file, and the prompt offered a
// third view, so "No Sonnet 5.5 in what I can access" was true of the device and
// wrong about the world, and a model the picker offered could be refused by the
// head. orch_model (apply.cpp through Deps::modelIsValid), the spawn validator
// (jobs.cpp, the same seam) and the web pick (webui.cpp through model_pick.h) now all
// ask modelSelectable() over the same sources: the provider's model store, else its
// compiled cold-start list, plus the compiled default. A model is offered if and
// only if this accepts it (test/test_model_selectable, the parity class).
//
// Portable (no Arduino): the device passes its cached store text; tests pass a
// fixture's.

#include <cstdint>
#include <string>

namespace nimbus {
namespace orch {

enum class PickRole : uint8_t {
  Any = 0,   // a chat model for either role (the head's orch_model and spawn seam)
  Head,      // the orchestrator (assistant head)
  Sub,       // a sub-agent
};

struct SelectableSources {
  const std::string* storeText = nullptr;   // the provider's model store file ("" or null = none)
  std::string coldStart;                    // the compiled cold-start list (CSV)
  std::string defaultModel;                 // the provider's head default (always allowed)
  std::string subDefaultModel;              // its sub-agent default (allowed for Sub and Any)
  uint32_t now = 0;                         // unix seconds; 0 = clock unset (no retire date applies)
};

// Whether `id` may run as `role` on `provider`:
//   - never an empty id, and for the router (cumulo) never a selector no upstream
//     runs; router selectors compare by canonical "<upstream>/<model>" and across
//     the dot and dash spellings of one version (sameModelKey);
//   - the provider's role default always (the head default for every role, the sub
//     default for Sub and Any: lane DEF), so it can be re-picked by name;
//   - with a store holding any offered model for the role: a row with that id that
//     rowOffered() accepts (usable, carries the role, not a non-chat family, not past
//     its retire date). A listed but unusable or retired model is refused;
//   - with none: membership of the cold-start list.
bool modelSelectable(const std::string& provider, const std::string& id, PickRole role,
                     const SelectableSources& src);

}  // namespace orch
}  // namespace nimbus
