#pragma once
#include "nimbus/orch/tx/card_files.h"

// tx_store_device - the device binding of engine 2's transcript storage (lane E2-DEV, the RB-I3
// minimum): nimbus::orch::tx::CardTxFiles (tx/card_files.h) over the memory card at /mem/tx,
// under the card lock (memory::Lock), with the effect ring's write guards (a card, a task that may
// touch a file, no injected card fault) and the internal memory floor. With no card every call
// answers false and the chat's transcript runs in RAM. Engine 2 only (tools/engine/engine2_only.txt).
namespace agent::txdev {

nimbus::orch::tx::CardTxFiles& cardTxFiles();

}  // namespace agent::txdev
