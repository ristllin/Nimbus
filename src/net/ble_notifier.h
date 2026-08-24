#pragma once
#include <Arduino.h>

// ble_notifier - BLE GATT transport for Notifier-mode nsn frames (plan §3.6).
//
// The device is the GATT peripheral/server; the host broker
// (../nsnotify, notify/transport/ble_tx.py - https://github.com/ristllin/nsnotify)
// is the central and hardcodes the SAME pinned identity. One primary service,
// three characteristics:
//
//   SERVICE  e20b0001-9463-42a9-aaf8-8aa1fd518d52
//   FRAME    e20b0002-…  Write (WITH response), requires WRITE_ENC (bonded link),
//            <=71 B. ONE complete nsn wire packet per write -
//            [SOF 0xAA][LEN][payload<=68][CRC8-MAXIM], byte-identical to the
//            broker's encode_frame (nsn::kMaxPacket == frame.py's). No chunking or
//            re-framing: the incremental nsn::Decoder's SOF-hunt + CRC resyncs
//            free of charge, so a torn/short write can never wedge it.
//   STATUS   e20b0003-…  Notify, <=8 B, binary tag-prefixed (never panel text):
//            [0x01, protoVer, fwMaj, fwMin]  conn ack, once per CCCD subscribe
//            [0x02, seq]                     seq echo per frame applied to the Router
//            [0x03, …]                       reserved (button/encoder events, v2)
//   CONFIG   e20b0004-…  Read, 4 B snapshot [ver=1, ledCount, brightness,
//            flags bit0 = notifier mode]. Write reserved for v2 (accepted and
//            ignored). Diagnostic only; the broker does not consume it in v1.
//
// Advertising: ALWAYS on while BLE is enabled (no attention gating) - flags +
// complete 128-bit service UUID in ADV_IND (what the host scans by), complete
// local name "Nimbus" in the scan response, 100-250 ms interval, restarted from
// the disconnect callback (NimBLE stops on connect).
// ATT_MTU target 247 (host hard floor 74 = 71-byte packet + 3-byte ATT header).
//
// SECURITY: the link is BONDED + ENCRYPTED (LE Secure Connections, Just Works).
// FRAME needs WRITE_ENC and CONFIG needs encrypted read/write - so an unbonded
// central can connect but cannot paint the ring or read/write config until it
// bonds. On the broker's first frame, macOS auto-bonds (Just Works, no dialog)
// and everything after is transparent. NOT MITM/passkey: macOS won't surface a
// passkey dialog for a custom peripheral paired by a background CLI, so that
// pairing fails; Just Works over LE SC still gives an ECDH-encrypted, bonded link
// (passive-sniff safe), dropping only active-MITM protection during the one-time
// bond. (STATUS notify telemetry - version + seq - is intentionally ungated;
// NimBLE's CCCD permission is process-global so per-char flags there are inert.)
// The passkey / Pairing-screen path (onPassKeyDisplay, ScreenId::Pairing, the
// accessors below) is retained but DORMANT - it only fires if MITM is re-enabled.
// Bonds persist in NVS (survive reboot, no SD).
//
// Threading contract (HARD RULE): NimBLE callbacks (onWrite / onSubscribe /
// onDisconnect / onRead) run on the NimBLE host task and must NOT touch the
// Router / Mapper / ring / panel. The FRAME onWrite only copies the write's
// bytes into a FreeRTOS StreamBuffer (single writer = NimBLE task, single
// reader = main loop; a write that doesn't fit is dropped WHOLE and counted -
// frames are idempotent full state, the next one repairs). drain() runs in
// loop() on the main task and is the only place bytes meet a decoder; ALL GATT
// notifies (conn ack + seq echo) also originate there, so no GATT traffic ever
// starts on the callback task.

namespace nimbus {
class NotifierMode;  // fwd: drain() pumps queued bytes into its BLE decoder
}

namespace nimbus::net::ble {

// Start NimBLE, build the GATT table, begin advertising. Idempotent. Called
// from setup() - Notifier mode only in v1 (the Orchestrator loop never drains
// the notifier, so advertising there would accept frames nobody applies).
void begin();

// MAIN task, once per Notifier loop(): pump queued FRAME bytes through
// n.feedBle() (same Mapper/Router path as serial, dedicated per-transport
// decoder), emit the [0x02, seq] echo per applied frame, flush a pending conn
// ack, refresh the CONFIG snapshot cache. Returns true if Router state changed
// this call (same contract as NotifierMode::poll - the caller recomposes the
// ring and feeds last().screen to the render scheduler). No-op returning false
// when begin() never ran.
bool drain(NotifierMode& n, uint32_t nowMs);

// True while a central is connected.
bool connected();

// Runtime enable/disable of advertising, NO reboot (menu/NVS toggle). Enabling
// begins the stack if it was never started (first-time from the menu); disabling
// stops advertising so no new central can find the device (an in-flight link
// stays up until it drops, then is not re-advertised). Safe to call from the
// main task. Idempotent-ish: harmless to re-assert the current state.
void setEnabled(bool on);
bool enabled();  // last setEnabled/begin state (the user's intent)

// ---- Secure pairing (bonded, Just Works) accessors --------------------------
// The link is bonded + encrypted (Just Works - see the SECURITY note above); an
// unbonded central can't write FRAME / read-write CONFIG until it bonds (auto,
// no dialog). All safe to call from the main task (loop()): the values are set on
// the NimBLE task and polled here. pairingPasskey()/pairingActive() are DORMANT
// under Just Works - they only populate if MITM is re-enabled.
uint32_t pairingPasskey();  // current pairing's 6-digit code (0 = not pairing)
bool     pairingActive();   // true from passkey-display until auth-complete
int      numBonds();        // bonded centrals stored in NVS (survives reboot; 0 if BLE off)
void     forgetBonds();     // wipe all bonds (Connectivity "Forget devices")
String   macAddress();      // this device's BLE address ("(ble off)" if not started)

}  // namespace nimbus::net::ble
