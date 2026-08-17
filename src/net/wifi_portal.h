#pragma once
#include <Arduino.h>

// wifi_portal - AP + captive-portal + mDNS bring-up for the Nimbus config web UI.
//
// Adapted from Nuage-Solide src/wifi_portal.{h,cpp} (classic ESP32), simplified
// for the S3 (8 MB PSRAM). Dropped from the original: the captive-teardown /
// re-arm machinery and the mDNS contiguous-heap deferral guard (both no-PSRAM
// heap-survival hacks - see plan §1/§8), the netheal self-heal watchdog (a P5
// Orchestrator concern), and the logbuf logging. What remains is the
// board-agnostic skeleton: WIFI_AP_STA bring-up, a wildcard captive DNS, mDNS
// once STA connects, an async scan, and save-and-connect.
//
// STA creds persist via solide::memory (NVS keys in nimbus_config.h). The AP
// runs unconditionally so the config page is always reachable at nimbus.local
// on the LAN and at the AP IP over the "Nimbus-setup" network.

namespace nimbus::net {

// Bring up AP+STA, start the captive DNS, and begin STA if creds are saved.
// AP ssid defaults to "<name>-setup"; apPass defaults to the per-device stored
// passphrase (agent::store::setupApPass - generated on first boot, shown on the
// setup screen), falling back to NIMBUS_AP_PASS only if NVS is unavailable.
// Pass an empty apPass for an open AP.
void begin(const char* apSsid = nullptr, const char* apPass = nullptr);

// Call from loop(): pump the captive DNS and start mDNS once STA connects.
void process();

// Tear down the SoftAP, drop to STA-only (TFT white-screen mitigation - the AP
// beacon train is the prime idle-knock suspect). STA + LAN web stay up.
void dropSoftAP();

// Bring the SoftAP back (STA is down) so the setup/recovery web page is reachable
// whenever the device can't reach Wi-Fi. Dropped again on the next GOT_IP.
void restoreSoftAP();

bool   staConnected();
bool   staConfigured();        // an STA SSID is set (connected, or connecting/down)
bool   provisioned();          // STA creds are STORED in NVS (first-boot gate)
String staIp();
String apIp();
// True when `localIp` (the interface a request arrived on) is the SoftAP address -
// i.e. the client is on the password-gated setup AP, not the shared STA/LAN. Used to
// gate captive-portal token echoing so the device token is never reflected to a LAN peer.
bool   isApInterface(const IPAddress& localIp);
String apSsid();                // live setup-AP SSID, e.g. "Nimbus-2-setup"
String apPass();                // the passphrase the setup AP is ACTUALLY using ("" = open).
                                // Setup screens print this - it is the only place the
                                // owner can learn the per-device value.
int    rssi();
String mdnsName();              // "<name>.local" (identity-derived)
void   startMdns();

// Persist creds and (re)connect STA. Returns true (the connect is async).
// Now UPSERTS into the known-networks list rather than replacing a single slot.
bool   saveAndConnect(const String& ssid, const String& pass);

// ---- the escape hatch -----------------------------------------------------
// Drop the station and guarantee the setup AP, so the config page is reachable no
// matter how wrong the saved credentials are. Without this a bad password retries
// forever and the association attempts starve the AP's beacons on the shared radio.
void   publishSetupNetwork();
void   cancelSetupHold();          // resume normal joining

// ---- known networks -------------------------------------------------------
// The device remembers up to 5 networks so moving between them (home / office /
// hotspot) does not destroy the previous credentials. A PASSWORD IS NEVER RETURNED
// by any of these - knownNetworksJson() reports ssid/open/current only.
int    knownCount();
bool   addNetwork(const String& ssid, const String& pass, String* evicted = nullptr);
bool   forgetNetwork(const String& ssid);
String knownNetworksJson();

// Async scan: {"scanning":true} while running, else {"networks":[...]}.
String scanJson();

}  // namespace nimbus::net
