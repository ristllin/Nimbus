#pragma once
#include "nimbus/harness/plain/compact.h"

// plain_fold_device - the device's binding of the engine-2 compaction driver (lane RB-C2,
// plain/compact.h): where its two small files live on the internal flash. Engine-2 device
// images only (tools/engine/engine2_only.txt); the engine-2 device wiring builds CompactDeps
// with these, the chat's log, arena and engine, and the rungs from the owner's keyed providers.
namespace agent {

// /data/compact_ctl.txt: the device-wide attempt count of the day and each chat's failures.
// Missing or damaged reads as fresh. A pre-v5.2 image never reads it.
plain::TextIO deviceCompactCtlIO();
// /data/chatsum.txt: the v5.1.1 summary store the swap mirrors into (rollback keeps a summary).
plain::TextIO deviceChatsumIO();

}  // namespace agent
