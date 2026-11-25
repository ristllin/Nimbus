#pragma once
#include <string>

#include "nimbus/orch/notes.h"

// notes_device - the device binding of engine 2's notes (lane RB-N1; PRD section 4 "Notes"): the
// portable Notebook (nimbus/orch/notes.h) over the device's two stores.
//   - The owner: internal flash (LittleFS), /data/notes/owner.md (pinned), owner.more.md
//     (unpinned), owner.pending.md (kept for approval), each written whole with
//     agent::writeFileAtomic (tmp then rename); independent of the card and of any network call.
//     notes::flashBound(kOwnerCaps) is at most 80 KB with the atomic copy (test/test_notes).
//   - A member: the card, /mem/notes/<chat id>.md, .more.md, .pending.md, under the card lock;
//     with no card every call is refused in one line.
// Engine 2 only (tools/engine/engine2_only.txt). Every call runs under the PSRAM-first scope;
// a refused allocation is an error result, never an abort.
namespace agent::notesdev {

// Register `note` (the manifest's description and schema, plain::toolDecl) on the device's shared
// registry. `origin` reads the running turn (RB-I2's engine binding); null: every call is kept for
// approval (fail closed) until the engine binds it.
void registerNote(nimbus::orch::ToolRegistry& reg, nimbus::orch::notes::OriginFn origin);

// RequestFacts::pinnedNotes for this person ("" with none, or with no storage).
std::string pinnedNotesFor(const nimbus::orch::Principal& who);

// The library object (unpinned for search scope=notes, pending, approve, reject). One turn task
// on the device (no on-device concurrency, AGENTS.md section 4): no lock of its own.
nimbus::orch::notes::Notebook& notebook();

}  // namespace agent::notesdev
