#pragma once
// relay_client - the device half of the cumulo-nimbus cloud tunnel. One resident
// WSS the device dials OUT to the relay (Telegram model: its own TLS session, OUTSIDE
// the work-slot arbiter). Answers tunneled HTTP requests by replaying them into the
// device's own web server over a loopback socket, stamping its LAN webAuthToken (which
// never leaves the device). Orchestrator-mode ONLY: begin() is called from
// orchestratorBegin(), and gates itself on cloudOptIn + a heap floor.
//
// Portable wire logic lives in lib/core/nimbus/cloud (host-tested); this file is the
// device glue (task, TLS, loopback, NVS, status).
#include <Arduino.h>
#include <ArduinoJson.h>

namespace nimbus {
namespace relay {

enum class State { Disabled, Idle, Pairing, Connecting, Online, Backoff };

// Spawn the relay task (idempotent). Call from orchestratorBegin() only.
void begin();
bool isRunning();

// Control surface (called from the web/console tasks; staged, drained by the relay
// task, mirroring the Telegram token-swap discipline).
void requestOptIn(bool on);   // enable/disable the relay (persists cloudOptIn)
void requestPair();           // begin pairing: POST /pair/init, then poll /pair/poll
void requestUnpair();         // wipe the credential and drop the link

// Status (safe from any task; snapshots guarded by a spinlock).
State state();
const char* stateName();
bool online();
bool pairingActive();
String claimCode();           // current pairing code ("" if not pairing)
String claimUrl();            // claim URL for the e-ink QR ("" if not pairing)
String statusLine();          // one calm human line for web/console
void statusInto(JsonObject obj); // fill an object for /api/state cloud{} and /api/cloud
void statusJson(String& out); // same, serialized to a string (console)
int stackMinFree();           // task stack high-water in bytes (-1 if no task)

// CLOUDLOOP: replay `GET <path>` into the local web server and return the HTTP status,
// writing a short body preview into `out`. Proves the loopback path with no cloud
// dependency. Runs on the CALLING task.
int loopbackSelfTest(const String& path, String& out);

}  // namespace relay
}  // namespace nimbus
