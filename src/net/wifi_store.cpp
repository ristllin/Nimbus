#include "wifi_store.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <solide/memory.h>

#include "nimbus_config.h"

namespace nimbus::net::wifistore {

namespace {

using nimbus::wifi::KnownNet;

std::vector<KnownNet> s_nets;
SemaphoreHandle_t     s_mux = nullptr;

// Recursive so the public helpers can call each other without deadlocking.
struct Lock {
  Lock() {
    if (!s_mux) s_mux = xSemaphoreCreateRecursiveMutex();
    if (s_mux) xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
  }
  ~Lock() { if (s_mux) xSemaphoreGiveRecursive(s_mux); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// `preferSsid` is the network the device is CURRENTLY USING, when it has one.
//
// ⚠ This is not cosmetic. Until step 8 lands, the legacy single slot is the ONLY
// network wifi_portal::begin() joins at boot - and upsertNetwork puts a
// newly-added network at the HEAD. Mirroring the head therefore meant that
// SAVING A SECOND NETWORK silently re-pointed the device at the newest and least
// proven one: add your phone hotspot as a backup while sitting on your home
// Wi-Fi, and the next boot chases the hotspot instead. Caught on hardware -
// Board 2 left a working network for one that did not exist and came back
// `sta=0 ip=0.0.0.0 reason=201 NO_AP_FOUND`.
//
// So the mirror prefers the working network and falls back to the head only when
// nothing is connected. Adding a network must never change what the device joins.
void persistLocked(const std::string& preferSsid = "") {
  const std::string blob = nimbus::wifi::dumpNetworks(s_nets);
  solide::memory::setString(NIMBUS_KEY_STA_NETS, String(blob.c_str()));
  if (s_nets.empty()) {
    solide::memory::setString(NIMBUS_KEY_STA_SSID, String(""));
    solide::memory::setString(NIMBUS_KEY_STA_PASS, String(""));
    return;
  }
  const nimbus::wifi::KnownNet* pick = nullptr;
  if (!preferSsid.empty()) {
    const int at = nimbus::wifi::findNetwork(s_nets, preferSsid);
    if (at >= 0) pick = &s_nets[(size_t)at];
  }
  if (!pick) {
    // No caller-supplied preference - a momentary link drop makes staConnected()
    // false, and falling back to the HEAD there is what re-pointed a device at a
    // network it had merely saved. Keep whatever the slot already names.
    const String cur = solide::memory::getString(NIMBUS_KEY_STA_SSID, "");
    const int at = cur.length()
        ? nimbus::wifi::findNetwork(s_nets, std::string(cur.c_str())) : -1;
    pick = (at >= 0) ? &s_nets[(size_t)at] : &s_nets[0];
  }
  solide::memory::setString(NIMBUS_KEY_STA_SSID, String(pick->ssid.c_str()));
  solide::memory::setString(NIMBUS_KEY_STA_PASS, String(pick->pass.c_str()));
}

}  // namespace

void begin() {
  Lock g;
  const String blob = solide::memory::getString(NIMBUS_KEY_STA_NETS, "");
  if (!nimbus::wifi::loadNetworks(std::string(blob.c_str()), s_nets)) {
    // A corrupt blob must not brick provisioning: start empty and let the legacy
    // slot (or the owner) repopulate. Never leaves a half-parsed list behind.
    s_nets.clear();
  }
  // Fold the legacy single slot in. Idempotent, so this is safe on every boot; it
  // only writes when something actually changed.
  const String ls = solide::memory::getString(NIMBUS_KEY_STA_SSID, "");
  const String lp = solide::memory::getString(NIMBUS_KEY_STA_PASS, "");
  if (nimbus::wifi::migrateLegacySlot(s_nets, std::string(ls.c_str()),
                                      std::string(lp.c_str())))
    persistLocked();
}

int count() {
  Lock g;
  return (int)s_nets.size();
}

bool getAt(int i, KnownNet& out) {
  Lock g;
  if (i < 0 || i >= (int)s_nets.size()) return false;
  out = s_nets[(size_t)i];
  return true;
}

void all(std::vector<KnownNet>& out) {
  Lock g;
  out = s_nets;
}

int indexOf(const String& ssid) {
  Lock g;
  return nimbus::wifi::findNetwork(s_nets, std::string(ssid.c_str()));
}

bool add(const String& ssid, const String& pass, const String& protectSsid,
         String* evictedSsid) {
  Lock g;
  KnownNet n;
  n.ssid = ssid.c_str();
  n.pass = pass.c_str();
  KnownNet evicted;
  const auto r = nimbus::wifi::upsertNetwork(s_nets, n, nimbus::wifi::kMaxKnownNetworks,
                                             std::string(protectSsid.c_str()), &evicted);
  if (r == nimbus::wifi::UpsertResult::Rejected) return false;
  if (r == nimbus::wifi::UpsertResult::Evicted && evictedSsid)
    *evictedSsid = evicted.ssid.c_str();
  // Keep the boot slot pointed at the network in use, not the one just added.
  persistLocked(std::string(protectSsid.c_str()));
  return true;
}

bool forget(const String& ssid) {
  Lock g;
  if (!nimbus::wifi::forgetNetwork(s_nets, std::string(ssid.c_str()))) return false;
  persistLocked();
  return true;
}

bool rename(const String& from, const String& to) {
  Lock g;
  const std::string f(from.c_str()), t(to.c_str());
  // Renaming a network to its own name is a no-op, NOT a delete. Without this
  // the upsert below took its Updated path (erase + re-insert at head) and the
  // forget afterwards removed that very entry - the network and its password
  // gone, from a command that should have changed nothing.
  if (f == t) return true;
  const int at = nimbus::wifi::findNetwork(s_nets, f);
  if (at < 0) return false;
  const KnownNet original = s_nets[(size_t)at];   // for rollback
  KnownNet moved = original;                      // carries the password with it
  moved.ssid = t;
  // Drop the OLD entry FIRST. Upserting while it was still present made the list
  // look one longer than it is, so a rename at capacity evicted an innocent
  // bystander to free a slot that was about to be freed anyway.
  nimbus::wifi::forgetNetwork(s_nets, f);
  // Protect the renamed network itself from eviction - it is the one the caller
  // is actively working on, and it is what `add()` does with its own target.
  KnownNet evicted;
  const auto r = nimbus::wifi::upsertNetwork(s_nets, moved, nimbus::wifi::kMaxKnownNetworks,
                                             t, &evicted);
  if (r == nimbus::wifi::UpsertResult::Rejected) {
    // A refused rename must not cost the owner the network it was renaming.
    nimbus::wifi::upsertNetwork(s_nets, original, nimbus::wifi::kMaxKnownNetworks, f, &evicted);
    return false;
  }
  persistLocked();
  return true;
}

void noteJoined(const String& ssid, uint32_t epochDay) {
  Lock g;
  // touchNetwork returns false when the head is already this network with today's
  // stamp - the common case on every reconnect. Skipping the write is what keeps NVS
  // erase cycles from being spent on a no-op.
  if (nimbus::wifi::touchNetwork(s_nets, std::string(ssid.c_str()), epochDay))
    persistLocked();
}

String json(const String& currentSsid) {
  Lock g;
  ArduinoJson::JsonDocument d;
  d["max"] = nimbus::wifi::kMaxKnownNetworks;
  ArduinoJson::JsonArray arr = d["networks"].to<ArduinoJson::JsonArray>();
  for (const KnownNet& n : s_nets) {
    ArduinoJson::JsonObject o = arr.add<ArduinoJson::JsonObject>();
    o["ssid"] = n.ssid;
    // Passwords NEVER leave this module - not even masked. The web UI only needs to
    // know whether a network is open, so that is all it is told.
    o["open"] = n.pass.empty();
    o["auto"] = n.autoJoin;
    o["current"] = (currentSsid.length() > 0 && n.ssid == std::string(currentSsid.c_str()));
  }
  String out;
  serializeJson(d, out);
  return out;
}

}  // namespace nimbus::net::wifistore
