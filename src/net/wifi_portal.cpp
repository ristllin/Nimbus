// wifi_portal - see wifi_portal.h. Adapted from Nuage-Solide src/wifi_portal.cpp.
#include "wifi_portal.h"
#include "wifi_store.h"   // known-networks list (NVS glue)
#include "wifi_link.h"    // CUM-207: failover supervisor - keep it seeded + off the AP
#include "nimbus_config.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <solide/memory.h>

#include <vector>

#include "../sys/config_nvs.h"
#include "../agent/store.h"   // setupApPass - the per-device stored AP passphrase
#include "nimbus/device_identity.h"
#include "version.h"   // NIMBUS_FW_VERSION -> mDNS TXT identity

namespace nimbus::net {

static DNSServer s_dns;
static bool      s_mdnsStarted = false;
static uint32_t  s_lastJoinMs = 0;   // millis() of the last credential test; 0 = none pending
static uint32_t  s_mdnsNextAttemptMs = 0;   // backoff between failed MDNS.begin()
static String    s_apSsid = NIMBUS_AP_SSID;
static String    s_apPass = NIMBUS_AP_PASS;   // effective passphrase (resolved in begin())
static String    s_mdnsHost = NIMBUS_MDNS_HOST;

// Resolve the device identity (nimbus::identity, P2). A stored name wins; on
// the very first boot we SCAN for sibling Nimbus APs currently advertising and
// auto-number ourselves ("Nimbus" -> "Nimbus-2" -> ...), then persist so the
// name stays stable across reboots. The scan needs the STA iface the AP_STA
// mode-set below just brought up, runs only on that one first boot (~2 s,
// before the loop watchdog is armed), and never on a provisioned device.
static String resolveDeviceName() {
  String name = sys::deviceName();
  if (name.length()) return name;
  std::vector<std::string> ssids;
  const int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  for (int i = 0; i < n; i++) ssids.push_back(std::string(WiFi.SSID(i).c_str()));
  WiFi.scanDelete();
  name = String(identity::pickSiblingName(identity::kBaseName, ssids).c_str());
  sys::saveDeviceName(name);
#ifdef NIMBUS_NOTIFIER_DEBUG
  Serial.printf("[net] first boot: %d APs visible -> device name '%s'\n", n, name.c_str());
#endif
  return name;
}

void begin(const char* apSsid, const char* apPass) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);

  // One name drives every surface: "<name>-setup" AP SSID, lowercased mDNS
  // label, BLE advertised name (ble_notifier reads sys::deviceName() at init),
  // and the orchestrator's prompt identity. First device keeps the historical
  // "Nimbus-setup"/"nimbus.local" (mdnsLabel("Nimbus") == "nimbus").
  const String devName = resolveDeviceName();
  const String derivedAp = devName + identity::kApSuffix;
  std::string label = identity::mdnsLabel(std::string(devName.c_str()));
  if (label.empty()) label = NIMBUS_MDNS_HOST;
  s_mdnsHost = label.c_str();

  const char* ssid = apSsid ? apSsid : derivedAp.c_str();
  // The AP passphrase is per-device: generated on first boot + stored in NVS
  // (agent::store::setupApPass), shown to the owner on the setup screen. The old
  // fleet-wide NIMBUS_AP_PASS survives only as the fallback for a device whose
  // NVS is unusable (setupApPass then can't persist; it still caches one stable
  // value per boot, so the screen and the radio agree). NVS is up by this point -
  // the STA-credential reads below have always run here.
  String stored = apPass ? String("") : agent::store::setupApPass();
  const char* pass = apPass ? apPass
                            : (stored.length() >= 8 ? stored.c_str() : NIMBUS_AP_PASS);
  s_apSsid = ssid;
  s_apPass = (pass && strlen(pass) >= 8) ? pass : "";

  WiFi.setHostname(s_mdnsHost.c_str());
  // softAP silently falls back to an open AP if the password is <8 chars, so
  // pass nullptr (open) explicitly when the caller wants no password.
  const bool apOk = s_apPass.length() ? WiFi.softAP(ssid, s_apPass.c_str())
                                      : WiFi.softAP(ssid);
#ifdef NIMBUS_NOTIFIER_DEBUG
  Serial.printf("[net] softAP('%s', pw=%d) -> ok=%d ip=%s mac=%s\n", ssid,
                int(pass ? strlen(pass) : 0), int(apOk),
                WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());
#endif
  s_dns.start(53, "*", WiFi.softAPIP());   // captive portal: all DNS -> AP IP
  WiFi.setAutoReconnect(true);

  // Load the known-networks list and fold in the legacy single slot. Must run before
  // anything asks provisioned(), which now counts the list.
  wifistore::begin();

  // Reconnect to the most recently used network. The store mirrors the list head back
  // into the legacy keys, so these stay the right credentials to try first.
  String sta = solide::memory::getString(NIMBUS_KEY_STA_SSID, "");
  if (sta.length()) {
    String p = solide::memory::getString(NIMBUS_KEY_STA_PASS, "");
    WiFi.begin(sta.c_str(), p.c_str());
  }
}

void process() {
  if (staConnected()) {
    if (!s_mdnsStarted) startMdns();
    s_lastJoinMs = 0;   // the join landed; the setup-AP watchdog has nothing to watch
  }
  s_dns.processNextRequest();
  // Note: unlike Nuage-Solide we keep the AP + DNS up unconditionally - on the
  // S3's 8 MB PSRAM the TIME_WAIT/max8 churn that forced the captive teardown
  // is gone, and an always-reachable config AP is preferable.
}

// Tear down the SoftAP and drop to STA-only. On a colour TFT board the SoftAP's
// continuous ~10 Hz beacon TX from the on-board antenna is the prime suspect for
// the "idle Wi-Fi knocks the panel white" fault (RF/current coupling into the
// unshielded panel stub). STA stays up, so Orchestrator (and the web UI over the
// LAN) keep working; only the "<name>-setup" config hotspot goes away.
void dropSoftAP() {
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

// Bring the SoftAP back (inverse of dropSoftAP) - called on a TFT board when STA
// goes DOWN, so the setup/recovery web entry point (phone -> "<name>-setup" ->
// config page) is always available exactly when it is needed: when the device
// can't reach Wi-Fi. It goes off again on the next GOT_IP, so the beacon train
// only exists while disconnected, never during normal connected operation.
void restoreSoftAP() {
  WiFi.mode(WIFI_AP_STA);
  if (s_apPass.length()) WiFi.softAP(s_apSsid.c_str(), s_apPass.c_str());
  else                   WiFi.softAP(s_apSsid.c_str());
  s_dns.stop();
  s_dns.start(53, "*", WiFi.softAPIP());
}

bool   staConnected() { return WiFi.status() == WL_CONNECTED; }

// Put the station back after a scan stood it down. Called on EVERY path that
// ends a scan - results, error, timeout - because the station must not be left
// down by a read-only operation.
static bool s_scanStoodDown = false;
static void rearmStationAfterScan() {
  if (!s_scanStoodDown) return;
  s_scanStoodDown = false;
  const String ssid = solide::memory::getString(NIMBUS_KEY_STA_SSID, "");
  if (!ssid.length()) return;
  const String pass = solide::memory::getString(NIMBUS_KEY_STA_PASS, "");
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

// "Configured" must read the STORED provisioning, not the live association:
// WiFi.SSID() is only non-empty while ASSOCIATED (the core queries the AP), so
// this returned false for a provisioned-but-down link - and worse, on a flaky AP
// it flapped 0↔1 with every reconnect attempt, firing the header glyph watcher
// (and thus a StatusIdle repaint) every few seconds (prism trace 2026-07-23).
bool   staConfigured() { return provisioned(); }
bool   provisioned() {
  // Counts the known-networks LIST, not the legacy single slot. Forgetting the last
  // network must return the device to its unprovisioned state (setup AP + onboarding),
  // and with the list as the source of truth that falls out for free.
  return wifistore::count() > 0;
}
String staIp()        { return WiFi.localIP().toString(); }
String apIp()         { return WiFi.softAPIP().toString(); }
bool   isApInterface(const IPAddress& localIp) {
  // The AP is always up (dual STA+AP), so softAPIP() is valid whenever a request could
  // arrive on it. A request whose local (destination) IP equals the SoftAP address came
  // in over the AP; anything else (0.0.0.0 included) is treated as NOT the AP - fail closed.
  const IPAddress ap = WiFi.softAPIP();
  return ap != IPAddress(0, 0, 0, 0) && localIp == ap;
}
String apSsid()       { return s_apSsid; }
String apPass()       { return s_apPass; }
int    rssi()         { return WiFi.RSSI(); }
String mdnsName()     { return s_mdnsHost + ".local"; }

void startMdns() {
  if (s_mdnsStarted) return;
  // MDNS.begin() rarely fails, but if it does don't hammer it every loop tick -
  // back off ~5 s between attempts (process() calls us on every iteration).
  const uint32_t now = millis();
  if (s_mdnsNextAttemptMs && now < s_mdnsNextAttemptMs) return;
  s_mdnsNextAttemptMs = now + 5000;
  // On the S3 the no-PSRAM contiguous-heap deferral guard is unnecessary; begin
  // mDNS unconditionally once STA is up.
  if (MDNS.begin(s_mdnsHost.c_str())) {
    // The service instance name is the human device name (e.g. "Nimbus-2"), so a
    // browser shows it verbatim instead of the lowercased host label. Falls back
    // to the host label if the name is unset (NVS unavailable).
    String devName = sys::deviceName();
    if (!devName.length()) devName = s_mdnsHost;
    MDNS.setInstanceName(devName.c_str());
    MDNS.addService("http", "tcp", 80);
    // Dedicated Nimbus service (`_nimbus._tcp`) so a host tool can DISCOVER and
    // DISAMBIGUATE Nimbus boards on the LAN by a unique service type - the bare
    // `_http._tcp` above is indistinguishable from any other web server. The TXT
    // record carries NON-SECRET identity only (name/fw/mode/mac); the auth token
    // and AP password are deliberately NEVER advertised - they stay obtainable
    // only via the physical Config QR. `mac` is the immutable per-board
    // discriminator (two boards can share a default name but never a MAC).
    MDNS.addService("nimbus", "tcp", 80);
    MDNS.addServiceTxt("nimbus", "tcp", "name", devName.c_str());
    MDNS.addServiceTxt("nimbus", "tcp", "fw", NIMBUS_FW_VERSION);
    MDNS.addServiceTxt("nimbus", "tcp", "mode",
                       sys::loadMode() == sys::Mode::Orchestrator ? "orchestrator"
                                                                  : "notifier");
    MDNS.addServiceTxt("nimbus", "tcp", "mac", WiFi.macAddress().c_str());
    s_mdnsStarted = true;
  }
}

bool saveAndConnect(const String& ssid, const String& pass) {
  // Any explicit join ends a "publish setup network" hold - otherwise the device
  // reconnects once and then never retries after the first transient drop,
  // because publishSetupNetwork() turned auto-reconnect off and nothing turns it
  // back on. Correcting a password through the escape hatch is the common path.
  WiFi.setAutoReconnect(true);
  // Remember it ALONGSIDE the others instead of overwriting the one slot. Saving a
  // new network used to destroy the previous one, which is how a device carried to a
  // second location ended up hunting forever for a network that no longer existed.
  // The network currently in use is protected from eviction, so adding one can never
  // cost you the connection you are adding it over.
  wifistore::add(ssid, pass, staConnected() ? WiFi.SSID() : String(""));
  link::markKnownDirty();   // the failover policy joins from this list - keep it current
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  s_lastJoinMs = millis() ? millis() : 1;   // watch this attempt; 0 is the "none" sentinel
  return true;
}

// --- the escape hatch -------------------------------------------------------
// Stop the station competing for the radio and guarantee the setup AP is up, so the
// config page is reachable no matter how wrong the saved credentials are. This is the
// way out of the trap: with a bad password the core retries forever, and the endless
// association attempts starve the AP's beacons on the shared 2.4 GHz radio.
void publishSetupNetwork() {
  s_lastJoinMs = 0;   // the station is being stood down; no join is in flight to watch
  link::setManualHold(true);   // owner wants the setup AP - hold the failover supervisor off
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  // Re-assert the AP if it never came up (or was dragged off-channel by the STA).
  if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
    if (s_apPass.length()) WiFi.softAP(s_apSsid.c_str(), s_apPass.c_str());
    else                   WiFi.softAP(s_apSsid.c_str());
    s_dns.stop();
    s_dns.start(53, "*", WiFi.softAPIP());
  }
}

// Resume normal joining (the owner fixed the credentials, or wants another go).
void cancelSetupHold() {
  link::setManualHold(false);   // resume normal joining - the supervisor may act again
  WiFi.setAutoReconnect(true);
  const String sta = solide::memory::getString(NIMBUS_KEY_STA_SSID, "");
  if (sta.length()) {
    const String p = solide::memory::getString(NIMBUS_KEY_STA_PASS, "");
    WiFi.begin(sta.c_str(), p.c_str());
    s_lastJoinMs = millis() ? millis() : 1;   // a fresh attempt begins; watch it too
  }
}

uint32_t msSinceJoinAttempt() {
  if (s_lastJoinMs == 0) return 0;              // nothing in flight
  const uint32_t d = millis() - s_lastJoinMs;
  return d == 0 ? 1 : d;                        // never report the "none" sentinel while pending
}

// --- known networks (thin pass-throughs; the store owns locking + persistence) ---
int  knownCount() { return wifistore::count(); }
bool forgetNetwork(const String& ssid) {
  const bool ok = wifistore::forget(ssid);
  if (ok) link::markKnownDirty();   // keep the failover policy's list in step
  return ok;
}
String knownNetworksJson() {
  return wifistore::json(staConnected() ? WiFi.SSID() : String(""));
}
bool addNetwork(const String& ssid, const String& pass, String* evicted) {
  const bool ok = wifistore::add(ssid, pass, staConnected() ? WiFi.SSID() : String(""), evicted);
  if (ok) link::markKnownDirty();
  return ok;
}

// Scan bookkeeping. WIFI_SCAN_FAILED (-2) means BOTH "no scan has been started" and
// "the scan failed" - one value, two meanings - and the old code read it only as the
// first. So a scan that genuinely could not run was silently restarted on every poll
// and always answered {"scanning":true}: the browser span forever on a ticking
// counter and no error could ever be reported, because failure was indistinguishable
// from idle.
//
// Scans really do fail here, routinely. With unreachable credentials the core's
// auto-reconnect keeps the single 2.4 GHz radio busy and a scan cannot get airtime
// (measured live: -2 while reconnecting, 7 networks the moment the station was
// quieted). Notifier mode is worse still - NimBLE's stack must live in internal SRAM
// and cannot move to PSRAM, leaving ~23 KB free against ~68 KB in Orchestrator, so
// the allocation itself can fail.
static bool     s_scanRequested = false;   // we asked -> a later -2 means FAILED
static uint8_t  s_scanFails     = 0;
static uint32_t s_scanStartMs   = 0;
static const uint8_t  kScanMaxFails  = 3;
static const uint32_t kScanTimeoutMs = 15000;

static String scanBusyError() {
  s_scanRequested = false;
  s_scanFails = 0;
  WiFi.scanDelete();
  rearmStationAfterScan();   // a failed scan must not leave the station down
  return String("{\"scanning\":false,\"error\":\"Couldn't search for networks. "
                "Wi-Fi is busy - try again in a moment.\"}");
}

String scanJson() {
  const int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_FAILED) {
    if (s_scanRequested) {
      // We DID ask and the driver reports failed. Retry a bounded number of times,
      // then say so instead of spinning.
      s_scanRequested = false;
      if (++s_scanFails >= kScanMaxFails) return scanBusyError();
      return String("{\"scanning\":true}");
    }
    WiFi.scanDelete();                     // clear any stale result before re-arming
    // If the station is NOT associated it is mid-retry, and those attempts own the
    // radio - the scan then fails every time (the reported "stuck on scanning...").
    // Stand it down for the scan. A CONNECTED station is left strictly alone: it is
    // not hammering, and dropping it would kill the very session asking to scan.
    // ⚠ Standing the station down means we OWN putting it back. An explicit
    // disconnect raises ASSOC_LEAVE, and the core short-circuits that reason
    // BEFORE it consults auto-reconnect (STA.cpp) - so nothing retries on its
    // own, and the policy engine that would try the next candidate (step 8) is
    // not landed. Without the re-arm below, opening Settings and clicking Scan
    // on a device that was merely mid-retry left it down until the next reboot.
    // This repo already paid for this exact bug once, in the WIFISCAN console
    // command; this was the second unpaired disconnect in the tree.
    if (!staConnected()) {
      s_scanStoodDown = true;
      WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
    }
    const int started = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
    s_scanRequested = (started != WIFI_SCAN_FAILED);
    s_scanStartMs = millis();
    if (!s_scanRequested && ++s_scanFails >= kScanMaxFails) return scanBusyError();
    return String("{\"scanning\":true}");
  }

  if (n == WIFI_SCAN_RUNNING) {           // -1: in progress
    // A scan that never lands would otherwise pin the UI on "scanning..." forever.
    if (s_scanRequested && (uint32_t)(millis() - s_scanStartMs) > kScanTimeoutMs)
      return scanBusyError();
    return String("{\"scanning\":true}");
  }

  s_scanRequested = false;                 // results are in
  s_scanFails = 0;
  rearmStationAfterScan();                 // ...and neither must a successful one
  JsonDocument doc;
  doc["scanning"] = false;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace nimbus::net
