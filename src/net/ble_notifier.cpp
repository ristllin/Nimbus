// ble_notifier - see ble_notifier.h for the GATT table + threading contract.
#include "ble_notifier.h"

#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include "modes/notifier_mode.h"
#include "nimbus_config.h"
#include "sfx/sound_fx.h"
#include "sys/config_nvs.h"   // device identity -> advertised name

namespace nimbus::net::ble {

// ---- Pinned identity (build spec §1) ----------------------------------------
// The host's ble_tx.py hardcodes the SAME four values - never regenerate.
static const char* kServiceUuid = "e20b0001-9463-42a9-aaf8-8aa1fd518d52";
static const char* kFrameUuid   = "e20b0002-9463-42a9-aaf8-8aa1fd518d52";
static const char* kStatusUuid  = "e20b0003-9463-42a9-aaf8-8aa1fd518d52";
static const char* kConfigUuid  = "e20b0004-9463-42a9-aaf8-8aa1fd518d52";
// Advertised name. A build-time -DNIMBUS_BLE_NAME=\"...\" (the bttest bench
// env) still wins outright; otherwise the name is the DEVICE IDENTITY
// (sys::deviceName(), P2 - "Nimbus", "Nimbus-2", or the owner's chosen name),
// which auto-numbers on first boot so two stock boards never collide - macOS
// hides the BLE MAC (opaque CoreBluetooth UUID), so name is the only reliable
// way a scanner/broker tells two boards apart.
static String s_deviceName;   // resolved once in begin(), before init()
static const char* deviceNameStr() {
#ifdef NIMBUS_BLE_NAME
  return NIMBUS_BLE_NAME;                      // bench override (bttest)
#else
  if (s_deviceName.length() == 0) {
    s_deviceName = sys::deviceName();
    if (s_deviceName.length() == 0) s_deviceName = "Nimbus";
  }
  return s_deviceName.c_str();
#endif
}

constexpr uint8_t kProtoVer = 2;   // v2: device decodes harness+title TLVs (nsn_proto v2).
                                   // Advertised in the conn-ack so a v2 broker sends v2 frames.
// No repo-wide firmware version exists yet; bump these when the BLE surface
// changes so the host's conn ack log can tell devices apart.
constexpr uint8_t kFwMajor = 1;
constexpr uint8_t kFwMinor = 0;

constexpr uint16_t kMtu         = 247;   // target; the host's hard floor is 74
constexpr size_t   kRxCapacity  = 1024;  // ~14 max-size frames
constexpr size_t   kDrainBudget = 256;   // bytes fed per drain() call

static NimBLEServer*         s_server = nullptr;
static NimBLECharacteristic* s_status = nullptr;
// User's runtime BLE-advertising enable (menu/NVS). When false the GATT stack
// stays initialized but does not advertise, so no new central can find the
// device; gates the onDisconnect re-advertise so a disable-while-connected
// stays disabled after the peer drops.
static bool                  s_enabled = true;

// RX queue: single writer (NimBLE host task, FRAME onWrite) -> single reader
// (main loop, drain()) - the one pairing StreamBuffers are safe for.
static StreamBufferHandle_t s_rx = nullptr;

// Cross-task flags: set on the NimBLE host task, consumed on the main task.
static volatile bool     s_connected  = false;
static volatile bool     s_ackPending = false;  // CCCD subscribe -> conn ack due
static volatile uint32_t s_dropped    = 0;      // whole writes dropped (RX full)

// CONFIG snapshot cache, refreshed by drain() on the main task so the NimBLE
// task's onRead never touches the Mapper (single-byte volatile, no lock needed).
static volatile uint8_t s_brightness = 0;

// ---- Secure pairing state (bonded; passkey vars DORMANT under Just Works) ----
// Written on the NimBLE host task (security callbacks), read on the main task
// via the accessors + drain(). Single-word volatiles, no lock - same cross-task
// pattern as s_connected/s_ackPending; a torn read self-heals on the next loop.
// s_passkey/s_pairingActive only populate if MITM is re-enabled; Just Works bonds
// silently (onPassKeyDisplay never fires), leaving these at their defaults.
static volatile uint32_t s_passkey       = 0;      // CURRENT pairing's 6-digit code (0 = none)
static volatile bool     s_pairingActive = false;  // true from passkey-display until auth-complete
static volatile bool     s_passkeyFresh  = false;  // onPassKeyDisplay set it -> drain() echoes on serial
static volatile bool     s_pairDone      = false;  // onAuthenticationComplete set it -> drain() logs result
static volatile bool     s_pairOk        = false;  // last auth: encrypted AND bonded

namespace {

class SrvCb final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    s_connected = true;
    // NimBLE host task - sfx::fire is a cheap gated queue-send, safe here.
    ::sfx::fire(nimbus::sfx::Ev::BleUp);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    s_connected = false;
    ::sfx::fire(nimbus::sfx::Ev::BleDown);
    // NimBLE stops advertising on connect; resume so the host's reconnect loop
    // can rescan (advertiseOnDisconnect is off - this is the one restart path).
    // Suppressed when the user has disabled BLE so a disable-while-connected
    // takes full effect once the peer drops.
    if (s_enabled) NimBLEDevice::startAdvertising();
  }
  // --- MITM passkey pairing (IO cap DISPLAY_ONLY) - DORMANT under Just Works --
  // Fires only if MITM is re-enabled; the active Just Works config bonds without
  // a passkey, so this never runs in v1 production.
  uint32_t onPassKeyDisplay() override {
    // NimBLE host task. If passkey pairing were on: DISPLAY_ONLY => WE choose the
    // code the Mac must type.
    // Fresh random 6-digit per pairing (never fixed); stash it for the main task
    // to render on e-ink + echo on serial, and return it so the SM uses it as
    // the expected passkey. We deliberately never call setSecurityPasskey(), so
    // NimBLE routes the display here instead of a constant (NimBLEServer.cpp
    // ~L672: it only calls this while the stored passkey is the 123456 default).
    const uint32_t pk = esp_random() % 1000000u;   // 000000-999999
    s_passkey       = pk;
    s_pairingActive = true;
    s_passkeyFresh  = true;
    return pk;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    // NimBLE host task. Pairing finished (success or failure). Flag for the main
    // task; NEVER touch the panel here (threading contract). getNumBonds() is a
    // read of NimBLE's NVS store - safe from this task.
    s_pairOk        = info.isEncrypted() && info.isBonded();
    s_pairingActive = false;
    s_passkey       = 0;
    s_pairDone      = true;
    if (s_pairOk) ::sfx::fire(nimbus::sfx::Ev::BleBond);   // fresh bond greeting
  }
};

class FrameCb final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    // NimBLE host task: queue bytes only - never decode or touch the Router
    // here (threading contract, ble_notifier.h). One write = one whole nsn
    // packet; anything oversize is a protocol violation, drop it cheap.
    NimBLEAttValue v = c->getValue();
    if (!s_rx || v.size() == 0 || v.size() > nsn::kMaxPacket) return;
    if (xStreamBufferSpacesAvailable(s_rx) < v.size()) {
      // Drop the WHOLE write, never a prefix: frames are idempotent full
      // state, so the next one repairs; a partial would just cost the decoder
      // a resync. Counted so drain() can surface the overflow under debug.
      s_dropped = s_dropped + 1;
      return;
    }
    xStreamBufferSend(s_rx, v.data(), v.size(), 0);
  }
};

class StatusCb final : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&,
                   uint16_t subValue) override {
    // Notifications enabled -> queue the [0x01, …] conn ack. Flushed by
    // drain() so the notify originates on the main task like every other.
    if (subValue & 0x0001) s_ackPending = true;
  }
};

class ConfigCb final : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    // [ver, ledCount, brightness, flags]; bit0 = notifier mode active, always
    // set in v1 (begin() only runs in Notifier mode - see main.cpp).
    const uint8_t snap[4] = {1, uint8_t(NIMBUS_RING_LEDS), s_brightness, 0x01};
    c->setValue(snap, sizeof snap);
  }
  // Write reserved for v2: accepted and ignored (onRead rebuilds the value, so
  // whatever a central wrote never reads back).
  void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) override {}
};

}  // namespace

// ⚠ ANTI-BRICK. NimBLE's controller needs a large contiguous chunk of INTERNAL
// SRAM. On the tightest config (Notifier + colour TFT), esp_nimble_hci_init()
// fails with ESP_ERR_NO_MEM (257) and NimBLE then asserts on a null mutex ->
// panic -> reboot -> the SAME thing next boot, forever, because the operating
// mode is persisted in NVS. A mode switch must NEVER be able to brick the device.
// So refuse to bring up Bluetooth when the largest free internal block is below
// what the controller needs, and keep booting WITHOUT it: skipping BLE also frees
// the ~20-30 KB it would have reserved, so the web server comes up and the owner
// can switch back to Orchestrator. The floor is deliberately generous - a
// recoverable, Bluetooth-less device beats an OOM boot loop.
constexpr size_t kBleHeapFloor = 60 * 1024;

void begin() {
  if (s_server) return;  // idempotent

  const size_t intFree  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t intBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  log_w("[ble] pre-init internal heap: free=%u largestBlock=%u (need >=%u)",
        (unsigned)intFree, (unsigned)intBlock, (unsigned)kBleHeapFloor);
  if (intBlock < kBleHeapFloor) {
    log_e("[ble] SKIPPED to avoid an OOM boot loop - largest internal block %u < %u. "
          "The device stays up without Bluetooth; switch to Orchestrator, or free "
          "internal memory. (Notifier needs BLE to drive the ring.)",
          (unsigned)intBlock, (unsigned)kBleHeapFloor);
    return;
  }

  s_rx = xStreamBufferCreate(kRxCapacity, /*xTriggerLevelBytes=*/1);

  NimBLEDevice::init(deviceNameStr());
  NimBLEDevice::setMTU(kMtu);

  // ---- Secure the link: bonded, LE Secure Connections, Just Works -----------
  // Closes the v1 "anyone in radio range can paint the ring" hole: FRAME/CONFIG
  // require an ENCRYPTED (bonded) link, so an unbonded central can connect but
  // can't write frames or read/write config. On the broker's first frame, macOS
  // auto-bonds (Just Works, no dialog) and everything after is transparent.
  //
  // Why Just Works and NOT MITM/passkey: MITM ("Passkey Entry") needs a system
  // dialog for the user to type the code the device shows - but macOS does NOT
  // surface that dialog for a CUSTOM BLE peripheral whose pairing is triggered by
  // a background CLI (the broker); such pairings fail. The only ways to get the
  // passkey UI (masquerade as HID, or a foreground Mac app) were both rejected.
  // Just Works over LE Secure Connections still gives an ECDH-encrypted, bonded
  // link (protected against PASSIVE sniffing); it drops only active-MITM
  // protection during the one-time pairing. The passkey / Pairing-screen path
  // below (SrvCb::onPassKeyDisplay, ScreenId::Pairing) is retained but DORMANT -
  // it only activates if MITM is re-enabled here (e.g. a future HID mode).
  // Bonds persist in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST=1), independent of the SD
  // config blob, so a bond survives reboot with no SD card.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  s_server = NimBLEDevice::createServer();
  static SrvCb s_srvCb;
  s_server->setCallbacks(&s_srvCb);
  s_server->advertiseOnDisconnect(false);  // restarted explicitly in onDisconnect

  NimBLEService* svc = s_server->createService(kServiceUuid);

  // FRAME requires an ENCRYPTED (bonded) link - WRITE_ENC, but NOT WRITE_AUTHEN:
  // authenticated (MITM) encryption is unavailable under Just Works, so requiring
  // it would reject even a legitimately bonded broker. WRITE_ENC still rejects any
  // UNBONDED central, which is the actual hole we're closing. Deliberately WRITE
  // (with response), NOT WRITE_NR: an unbonded write then returns an ATT
  // insufficient-encryption error, which is what makes macOS auto-bond (Just Works)
  // on the broker's first frame (a no-response write is dropped silently and can't
  // trigger pairing). The broker writes with response=True to match, then retries.
  NimBLECharacteristic* frame = svc->createCharacteristic(
      kFrameUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC,
      nsn::kMaxPacket);
  static FrameCb s_frameCb;
  frame->setCallbacks(&s_frameCb);

  // STATUS is notify-only telemetry (conn-ack proto/fw version + per-frame seq
  // echo). Its CCCD subscribe is deliberately NOT encryption-gated: in NimBLE the
  // CCCD descriptor's permission is a process-global READ|WRITE, NOT derived from
  // the characteristic's flags, so READ_ENC/READ_AUTHEN here would be inert (there
  // is no plain-READ value path for them to guard). An unbonded central can thus
  // subscribe and read this low-sensitivity telemetry - acceptable: it cannot
  // paint the ring (FRAME) or read/write CONFIG, both of which ARE genuinely
  // encryption+auth gated on their value attributes above. (Gating the CCCD too
  // would mean raising the global ble_gatts_set_clt_cfg_perm_flags - not worth the
  // internal-API reach for a version byte and a counter.)
  s_status = svc->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::NOTIFY, 8);
  static StatusCb s_statusCb;
  s_status->setCallbacks(&s_statusCb);

  // CONFIG: encrypted read/write (bonded link required), no AUTHEN - same Just
  // Works reasoning as FRAME above.
  NimBLECharacteristic* config = svc->createCharacteristic(
      kConfigUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
          NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_ENC,
      4);
  static ConfigCb s_configCb;
  config->setCallbacks(&s_configCb);

  svc->start();

  // ADV_IND: flags + the complete 128-bit service UUID (18 of 31 bytes - this
  // is what the host scans by); the local name rides in the scan response.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(kServiceUuid));
  NimBLEAdvertisementData scanData;
  scanData.setName(deviceNameStr());
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->setMinInterval(160);  // 100 ms (0.625 ms units)
  adv->setMaxInterval(400);  // 250 ms
  adv->start();
  s_enabled = true;

#ifdef NIMBUS_NOTIFIER_DEBUG
  Serial.printf("[ble] advertising '%s' addr=%s svc=%s heap=%u\n", deviceNameStr(),
                NimBLEDevice::getAddress().toString().c_str(), kServiceUuid,
                unsigned(ESP.getFreeHeap()));
#endif
}

void setEnabled(bool on) {
  s_enabled = on;
  if (!s_server) {          // not yet initialized: enabling begins the stack
    if (on) begin();         // (heavy one-shot; safe from the main task, no lock)
    return;
  }
  if (on) NimBLEDevice::startAdvertising();
  else    NimBLEDevice::stopAdvertising();
#ifdef NIMBUS_NOTIFIER_DEBUG
  Serial.printf("[ble] %s\n", on ? "advertising" : "stopped");
#endif
}

// ⚠ Guard on s_server (like numBonds/bleMac): when the anti-brick path SKIPS BLE,
// s_server stays null while s_enabled is still its true default, so without this the
// header would claim "Bluetooth on" for a controller that never came up.
bool enabled()     { return s_enabled && s_server != nullptr; }

bool drain(NotifierMode& n, uint32_t nowMs) {
  if (!s_rx) return false;  // begin() never ran (Orchestrator mode)

  // Conn ack queued by onSubscribe on the NimBLE task - notified from HERE so
  // all GATT notifies originate on the main task (threading contract).
  if (s_ackPending) {
    s_ackPending = false;
    const uint8_t ack[4] = {0x01, kProtoVer, kFwMajor, kFwMinor};
    s_status->setValue(ack, sizeof ack);
    s_status->notify();
  }

  // Pairing passkey / result surfaced on serial (main task, not the security
  // callback - threading contract). The e-ink Pairing screen is driven from the
  // main loop via pairingActive()/pairingPasskey(); this echo is how a screenless
  // bench board (the spare) reads the code to complete OS pairing.
#if defined(NIMBUS_NOTIFIER_DEBUG) || defined(NIMBUS_TEST)
  if (s_passkeyFresh) {
    s_passkeyFresh = false;
    Serial.printf("[ble] passkey %06u  <- type this on the Mac to pair\n",
                  unsigned(s_passkey));
  }
  if (s_pairDone) {
    s_pairDone = false;
    Serial.printf("[ble] pairing %s (bonds=%d)\n",
                  s_pairOk ? "OK" : "FAILED", NimBLEDevice::getNumBonds());
  }
#endif

#ifdef NIMBUS_NOTIFIER_DEBUG
  static uint32_t s_lastDropped = 0;
  if (s_dropped != s_lastDropped) {
    s_lastDropped = s_dropped;
    Serial.printf("[ble] rx overflow: %u writes dropped\n",
                  unsigned(s_lastDropped));
  }
#endif

  // Bounded pump, mirroring poll()'s per-call budget: bytes -> the dedicated
  // BLE decoder inside NotifierMode -> same Mapper/Router as serial. The
  // shared 5 s link-timeout still runs in the serial path every loop, so
  // "quiet on ALL transports" clears the ring with no extra logic here.
  uint8_t buf[kDrainBudget];
  const size_t got = xStreamBufferReceive(s_rx, buf, sizeof buf, 0);
  bool changed = false;
  for (size_t i = 0; i < got; ++i) {
    const uint32_t before = n.bleFrames();
    changed |= n.feedBle(buf[i], nowMs);
    if (n.bleFrames() != before) {
      const uint8_t echo[2] = {0x02, n.bleLastSeq()};  // seq echo, main task
      s_status->setValue(echo, sizeof echo);
      s_status->notify();
    }
  }
  if (got) s_brightness = n.mapper().brightness();  // CONFIG snapshot cache
  return changed;
}

bool connected() { return s_connected; }

// ---- Secure pairing surface (all safe from the main task) -------------------
uint32_t pairingPasskey() { return s_passkey; }
bool     pairingActive()  { return s_pairingActive; }
int      numBonds()       { return s_server ? NimBLEDevice::getNumBonds() : 0; }
void     forgetBonds()    { if (s_server) NimBLEDevice::deleteAllBonds(); }

String macAddress() {
  // Stack must be up (begin() ran) - getAddress() reads the controller's BDA.
  return s_server ? String(NimBLEDevice::getAddress().toString().c_str())
                  : String("(ble off)");
}

}  // namespace nimbus::net::ble
