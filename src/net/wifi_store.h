#pragma once
#include <Arduino.h>

#include <string>
#include <vector>

#include "nimbus/wifi/known_networks.h"

// wifi_store - NVS glue for the known-networks list. Deliberately separate from the
// link policy so each is reviewable on its own: this file only moves bytes, every
// decision lives in the portable, host-tested lib/core code.
//
// Concurrency: the list is READ on the AsyncTCP task (the web UI asks for it) and
// WRITTEN on the main task, so every accessor takes a recursive mutex. The lock is
// never held across a blocking call.

namespace nimbus::net::wifistore {

// Load from NVS, folding the legacy single slot in as entry #1 on first run.
// Idempotent - safe on every boot. Persists only if the migration changed anything.
void begin();

int  count();
bool getAt(int i, nimbus::wifi::KnownNet& out);
void all(std::vector<nimbus::wifi::KnownNet>& out);
int  indexOf(const String& ssid);

// Add or update. `protectSsid` is never evicted to make room (the network in use).
// Returns false only when the SSID itself is unusable.
bool add(const String& ssid, const String& pass, const String& protectSsid,
         String* evictedSsid = nullptr);

bool forget(const String& ssid);

// Re-key an entry: same password, corrected SSID. Exists because a mistyped network
// name is otherwise unrecoverable without the owner re-entering the password - the
// store never hands one out, so nothing else can move it. Returns false if `from` is
// absent or `to` is unusable. The password is never read outside this module.
bool rename(const String& from, const String& to);

// Record a successful join: promote to head + stamp the day. Writes NVS only when
// the list actually changed, because this runs on every reconnect and NVS has finite
// erase cycles.
void noteJoined(const String& ssid, uint32_t epochDay);

// Sanitized for the web/API: ssid, whether it is open, and whether it is in use.
// A PASSWORD IS NEVER SERIALIZED OUT OF THIS MODULE.
String json(const String& currentSsid);

}  // namespace nimbus::net::wifistore
