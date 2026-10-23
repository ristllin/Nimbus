#include "prefs_subsystem.h"

#include <LittleFS.h>

#include "fs_atomic.h"                  // writeFileAtomic / flashMounted
#include "memory_subsystem.h"           // Lock + nowHours + scratchpad + vectors
#include "orchestrator.h"               // inScheduledTurn + the turn taint accessor
#include "telegram.h"                   // the owner notice channel
#include "../sys/agent_log.h"
#include "nimbus/orch/rbac.h"           // Role

namespace agent {
namespace prefs {

namespace {

// The file seam over LittleFS (/data/prefs.txt): atomic writes, tolerated
// absence (the first boot's seed marker).
class LittleFsPrefsIO : public nimbus::orch::PrefsStoreIO {
 public:
  bool load(std::string& out) override {
    if (!flashMounted()) return false;
    memory::Lock g;   // the same recursive lock the engines take (withStore re-enters)
    File f = LittleFS.open(nimbus::orch::kPrefsPath, FILE_READ);
    if (!f) return false;
    out.resize(f.size() > 4096 ? 4096 : f.size());
    const size_t got = out.empty() ? 0 : f.read((uint8_t*)out.data(), out.size());
    f.close();
    out.resize(got);
    return true;
  }
  bool saveChecked(const std::string& blob) override {
    return writeFileAtomic(nimbus::orch::kPrefsPath, blob, "prefs");
  }
};

LittleFsPrefsIO g_io;
nimbus::orch::OwnerPrefs g_prefs;
bool g_begun = false;

void notifyOwner(const std::string& text) {
  const String chat = orchestrator::firstAllowedChat();
  if (chat.length()) telegram::send(chat, String(text.c_str()), /*block=*/false);
}

}  // namespace

void begin() {
  if (!g_begun) {
    g_prefs.begin(&g_io);
    g_begun = true;
  }
}

nimbus::orch::OwnerPrefs& store() { return g_prefs; }

bool withStore(const std::function<void(nimbus::orch::OwnerPrefs&)>& fn) {
  if (!flashMounted()) return false;
  memory::Lock g;
  begin();
  fn(g_prefs);
  return true;
}

nimbus::orch::PrefsToolContext context() {
  nimbus::orch::PrefsToolContext c;
  c.withPrefs = [](const std::function<void(nimbus::orch::OwnerPrefs&)>& fn) {
    return withStore(fn);
  };
  c.inScheduledTurn = [] { return orchestrator::inScheduledTurn(); };
  c.externalContentSeen = [] { return orchestrator::turnReadOutsideContent(); };
  c.nowHours = [] { return memory::nowHours(); };
  c.notify = [](const std::string& text) { notifyOwner(text); };
  return c;
}

std::string blockForRole(bool ownerFacing) {
  if (!ownerFacing) return std::string();   // the rules are the owner's (D5)
  if (!flashMounted()) return std::string();
  memory::Lock g;
  begin();
  return g_prefs.renderBlock();
}

uint32_t filtersForRole(bool ownerFacing) {
  if (!ownerFacing || !flashMounted()) return 0;
  memory::Lock g;
  begin();
  return g_prefs.activeFilters();
}

void seedFirstBoot(const std::string& devName) {
  if (!flashMounted()) return;
  {
    memory::Lock g;
    begin();
    if (g_prefs.seeded() || g_prefs.fileExisted()) return;   // only the very first boot
  }
  // Candidates: every stored memory (pinned ones first by the picker's sort) and
  // the scratchpad's long tier - the standing rules the owner already gave, in
  // whatever tier they happened to land in (L19-50: the 19 Oct drink default
  // lived only in RAM; pinned rules and standing defaults both qualify).
  std::vector<nimbus::orch::SeedCandidate> cands;
  const std::string dev = devName;
  {
    memory::Lock g;
    memory::vectors().forEachEntry([&](size_t, const nimbus::orch::VectorMemory::EntryView& v) {
      nimbus::orch::SeedCandidate c;
      c.text = v.content;
      c.pinned = v.permanentFlag;
      c.importance = v.importance;
      c.createdHours = v.createdAtHours;
      cands.push_back(std::move(c));
    });
    for (const std::string& t : memory::scratchpad().items(nimbus::orch::Tier::Long)) {
      nimbus::orch::SeedCandidate c;
      c.text = t;
      c.pinned = true;   // the long tier is the model's own "keep this forever" slot
      cands.push_back(std::move(c));
    }
  }
  const std::vector<std::string> picked = nimbus::orch::pickSeedRules(cands, dev);
  std::vector<std::string> ids;
  {
    memory::Lock g;
    const nimbus::orch::PrefErr e = g_prefs.seed(picked, memory::nowHours(), &ids);
    if (e != nimbus::orch::PrefErr::Ok) {
      alogf("prefs: first-boot seed not saved (%s)", nimbus::orch::prefErrWords(e));
      return;
    }
  }
  alogf("prefs: first-boot seed added %d standing rule(s)", (int)ids.size());
  // The one listing (owed even when the seed added nothing, so the seed never
  // runs again and the owner is told once either way).
  std::string listing;
  {
    memory::Lock g;
    listing = nimbus::orch::seedListingText(g_prefs);
    if (g_prefs.listingOwed()) g_prefs.markListed();
  }
  if (!listing.empty()) notifyOwner(listing);
  else
    alog("prefs: first-boot seed found no standing rules; nothing listed");
}

}  // namespace prefs
}  // namespace agent
