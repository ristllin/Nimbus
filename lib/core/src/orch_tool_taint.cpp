#include "nimbus/orch/tool_taint.h"

namespace nimbus {
namespace orch {

namespace {

using T = Taint;
using E = ToolEffect;

// One row per tool the firmware registers by name. Taint is about the OUTPUT:
// a memory or file the device stores may hold text that once came from outside,
// so anything that returns stored documents, results or fetched pages is
// External; the device's own state, settings and docs are Internal.
const ToolTaintRow kRows[] = {
    // memory (lib/core/src/orch_memory_tools.cpp)
    {"memory.write", T::Internal, E::MemWrite},
    {"memory.search", T::Internal, E::Read},
    {"memory.update", T::Internal, E::MemUpdate},
    {"memory.pin", T::Internal, E::MemPin},
    {"memory.scratchpad", T::Internal, E::Scratch},
    {"memory.config", T::Internal, E::Config},
    {"memory.episodic", T::External, E::Read},   // past messages include tool outputs and media
    {"memory.archive", T::Internal, E::MemDelete},
    // the recent-results ring (lib/core/src/orch_result_store.cpp)
    {"results.get", T::External, E::Read},
    {"results.list", T::External, E::Read},
    // sub-agent sessions (lib/core/src/orch_session_tools.cpp)
    {"session.spawn", T::Internal, E::Spawn},
    {"session.tell", T::Internal, E::Spawn},
    {"session.poll", T::External, E::Read},
    {"session.terminate", T::Internal, E::Cancel},
    {"session.list", T::Internal, E::Read},
    // web (lib/harness/src/websearch.cpp)
    {"web.search", T::External, E::Read},
    // files (src/agent/files_subsystem.cpp)
    {"artifact.save", T::Internal, E::FileWrite},
    {"files.append", T::Internal, E::FileWrite},   // G1a: writes rows, echoes only the count
    {"files.fetch", T::External, E::FileWrite},
    {"files.list", T::Internal, E::Read},
    {"files.read", T::External, E::Read},
    {"files.search", T::External, E::Read},
    {"files.send", T::Internal, E::Send},
    {"files.share", T::Internal, E::Share},
    {"files.stat", T::Internal, E::Read},
    {"image.generate", T::External, E::FileWrite},
    // firmware docs, routines, replies, skills, tenants (src/agent/memory_subsystem.cpp)
    {"docs.list", T::Internal, E::Read},
    {"docs.read", T::Internal, E::Read},
    {"docs.search", T::Internal, E::Read},
    {"loop.create", T::Internal, E::Schedule},
    {"loop.cancel", T::Internal, E::Schedule},
    {"loop.list", T::Internal, E::Read},
    {"wakeup.set", T::Internal, E::Schedule},
    {"reply.telegram", T::Internal, E::Send},
    {"reply.speak", T::Internal, E::Speak},
    {"skill.get", T::Internal, E::Read},
    {"skill.list", T::Internal, E::Read},
    {"skill.save", T::Internal, E::SkillSave},
    {"skill.delete", T::Internal, E::SkillSave},
    {"system.health", T::Internal, E::Read},
    {"tenant.list", T::Internal, E::Read},
    {"tenant.set_role", T::Internal, E::Admin},
    {"tenant.set_quota", T::Internal, E::Admin},
    // device (src/main.cpp, src/agent/orchestrator.cpp)
    {"device.status", T::Internal, E::Read},
    {"device.selftest", T::Internal, E::Read},
    {"device.control", T::Internal, E::Config},
    {"ota.status", T::Internal, E::Read},
    // media (src/sfx/music.cpp)
    {"media.list", T::Internal, E::Read},
    {"media.play", T::Internal, E::Media},
    {"media.pause", T::Internal, E::Media},
    {"media.stop", T::Internal, E::Media},
};

}  // namespace

const ToolTaintRow* toolTaintRow(const std::string& name) {
  for (const ToolTaintRow& r : kRows)
    if (name == r.name) return &r;
  return nullptr;
}

Taint toolTaint(const std::string& name) {
  const ToolTaintRow* r = toolTaintRow(name);
  return r ? r->taint : Taint::External;
}

ToolEffect toolEffect(const std::string& name) {
  const ToolTaintRow* r = toolTaintRow(name);
  return r ? r->effect : ToolEffect::Read;
}

const ToolTaintRow* toolTaintTable(size_t& count) {
  count = sizeof(kRows) / sizeof(kRows[0]);
  return kRows;
}

}  // namespace orch
}  // namespace nimbus
