#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/tx/migrate.h"
#include "nimbus/orch/tx/old_engine.h"

// migrate_device - what an engine-2 device takes over from the earlier engine (lane E2-MIG), read
// only: the earlier engine's stores (the episodic day streams, /data/chatsum.txt, /data/prefs.txt)
// are never written, so a cable flash of the earlier image finds everything it wrote.
//   - fill(): plain::ChatBindingDeps::oldEngine. The chat's rows come from the episodic store's
//     forward query under the card lock (tx::EpisodicRowSource, pages of tx::kRowPage rows), its old
//     summary from the fold record; the old summary's export goes to the file store
//     (export-pre-v2/). The binding then runs tx::migrate before the engine opens the log, at
//     every boot of the chat: the first imports the history, a later one only what the earlier
//     engine wrote since. The fold record as the earlier engine left it is copied once to
//     /mem/tx-pre-e2/chatsum.txt. Engine 2 never writes /data/chatsum.txt (lane E2-FIX2B, P5); a
//     record an earlier engine-2 build replaced with its mirror is put back from that copy once.
//   - importRulesOnce(): the earlier engine's active standing rules into the owner's notes as
//     pinned rules (tx::importRules), once per boot; /data/notes/rules_import.txt records what was
//     copied, so a second run adds nothing and an edit made on engine 2 is kept.
// Engine 2 only (tools/engine/engine2_only.txt). tg_poll only.
namespace agent::migratedev {

bool fill(nimbus::orch::tx::MigrateDeps& m);

// Store calls the last fill's row source made (the bench reads it beside the outcome).
uint32_t lastQueries();

void importRulesOnce();

// May this chat boot now? False only before its first engine-2 start (no transcript yet) while the
// episodic index is still being built after the boot: the store's forward query is only
// trustworthy over the whole history once the index is complete, and a first start that read
// part of it would never read the rest (the next start is a re-entry). At most 10 minutes after
// the boot: an index that stopped at its cap never completes.
bool historyReady(const std::string& chatKey);

#ifdef NIMBUS_TEST
// Test builds (the TXASIDE console verb): /mem/tx/<chat> renamed to /mem/tx-aside/<chat>-<epoch>,
// nothing removed; the reply line names what happened. Refused while a turn runs: send it after a
// boot and before the chat's first message, so the chat is not resident.
std::string setAside(const std::string& chat);
#endif

}  // namespace agent::migratedev
