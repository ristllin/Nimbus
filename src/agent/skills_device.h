#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/plain/chat_skills.h"

// skills_device - the owner's skills for engine 2 on the device (lane E2-CHAT): the SKILL.md files
// the old engine's skill store keeps (/mem/skills/<id>/SKILL.md with a card, /data/skills/<id>/
// without), read with the old engine's reader (skills.cpp DeviceSkillFs read and listDirs: the card
// lock, the read staged in PSRAM) and nothing else of its machinery. Engine-2 images only
// (tools/engine/engine2_only.txt). The poll task reads them at each turn open.
namespace agent {
namespace skillsdev {

// Every skill folder holding a SKILL.md (at most 8,192 B each, the store's file cap).
std::vector<plain::SkillFile> files();
// The index and the context skills of those files (plain::chatSkillsOf).
plain::ChatSkills chatSkills();
// The approved body of skill `id` (FileToolsIo::skillBody: read_file on skills/<id>.md); false when
// there is none. The file tools' device binding (lane E2-TOOLS) answers skillBody with this.
bool skillBody(const std::string& id, std::string& out);

}  // namespace skillsdev
}  // namespace agent
