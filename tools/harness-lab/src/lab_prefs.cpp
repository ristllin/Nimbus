#include "lab_prefs.h"

#include "lab_rig.h"

namespace lab {

namespace {

// The in-memory file seam (the device: /data/prefs.txt on LittleFS).
class MemPrefsIO : public nimbus::orch::PrefsStoreIO {
 public:
  bool load(std::string& out) override {
    if (!existed) return false;
    out = blob;
    return true;
  }
  bool saveChecked(const std::string& b) override {
    blob = b;
    existed = true;
    return true;
  }
  std::string blob;
  bool existed = false;
};

MemPrefsIO& io() {
  static MemPrefsIO io_;
  return io_;
}

}  // namespace

LabPrefs::LabPrefs() {
  prefs_.begin(&io());   // a fresh store: no file, the seed marker unset
}

void LabPrefs::registerTools(nimbus::orch::ToolRegistry& reg) {
  nimbus::orch::PrefsToolContext c;
  c.withPrefs = [this](const std::function<void(nimbus::orch::OwnerPrefs&)>& fn) {
    fn(prefs_);
    return true;
  };
  c.inScheduledTurn = [] { return g_labScheduledTurn; };
  c.externalContentSeen = [] { return false; };   // the lab taints nothing
  c.nowHours = [this] { return nowHours_; };
  c.notify = [](const std::string&) {};   // no Telegram in the lab
  nimbus::orch::registerPrefsTools(reg, c);
  ctx_ = c;
}

void LabPrefs::bindStanding(nimbus::orch::MemoryContext& mc) {
  mc.saveStanding = [this](const std::string& text, const nimbus::orch::Principal& who,
                           bool& handled) {
    return nimbus::orch::prefsSaveStanding(ctx_, text, who, handled);
  };
}

std::string LabPrefs::blockFor(const std::string& chatId) {
  // The lab has one owner: every chat the rig calls "owner" is the owner's.
  // Member/guest turns pass a different chat id and see nothing.
  if (chatId.rfind("owner", 0) != 0) return std::string();
  return prefs_.renderBlock();
}

}  // namespace lab
