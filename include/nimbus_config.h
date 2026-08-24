#pragma once
// Nimbus device configuration - single source of truth for timings, thresholds
// and defaults. Hardware pin choices live in solide-drivers' board header
// (solide/boards/board_solide_s3.h); this file holds Nimbus policy only.

// ---- Interaction defaults (profile presets may override; see profile.cpp) --
// How long after the last detent a busy panel may hold the cursor glow as the
// "panel syncing" shimmer. Beyond this window a busy panel is doing unrelated
// work (ambient coalesce flush, attention screen) and must not resurrect the
// glow. The value is a legacy of the slow-panel era (a dwell plus two full
// refreshes); the ring cursor still honors it, so it is kept byte-stable.
#define NIMBUS_CURSOR_SYNC_HOLD_MS 4728

// ---- LED ring ---------------------------------------------------------------
#define NIMBUS_RING_LEDS             45
#define NIMBUS_ATTN_LED_INDEX         0   // Passive posture: the one attention LED
#define NIMBUS_ATTN_HUE_AUTO         -1   // sentinel: derive hue from top-priority state
#define NIMBUS_ATTN_PERIOD_MS      2400   // breathe period for the attention LED
// How long a POST /api/preview holds the ring on the requested profile's look
// before auto-reverting to the live composed state (src/main.cpp startPreview()).
#define NIMBUS_PREVIEW_MS          4000

// ---- Battery policy (hardware TBD - consumed by lib/core/power) -------------
#define NIMBUS_BATT_T1_PCT           20   // force Battery Saver + warn
#define NIMBUS_BATT_T2_PCT            8   // flush journals + clean deep sleep
#define NIMBUS_BATT_HYST_PCT          5   // exit thresholds at T + HYST
#define NIMBUS_VBUS_DEBOUNCE_MS    1500   // stable VBUS before auto-Desk switch

// Battery hardware pins - PLACEHOLDERS until a fuel gauge + VBUS sense are added
// to the board (solide-drivers v0.1.0 has neither). Reserve a spare GPIO for
// VBUS and the I2C bus for a MAX17048-class gauge (plan §1). Define
// NIMBUS_HAS_FUEL_GAUGE to compile + use src/hw/power_fuelgauge; without it the
// firmware runs on NullMonitor (desk-powered, battery UI hidden).
#define NIMBUS_FUELGAUGE_SDA          8   // I2C SDA (verify against final board)
#define NIMBUS_FUELGAUGE_SCL          9   // I2C SCL (verify against final board)
#define NIMBUS_VBUS_SENSE_PIN         4   // VBUS divider -> ADC/GPIO (reserved)

// ADC battery voltage sampling - the resistor-divider alternative to the I2C fuel
// gauge (no extra chip; coarser SoC). Define NIMBUS_HAS_BATTERY_ADC to compile + use
// src/hw/power_battery_adc. Wiring (board hardware.md): tap BAT+ BEFORE the DC-DC,
// e.g. a 2S pack (6.0-8.4 V) via 220k(top)/100k(bot) into GPIO4. ⚠ Must be an ADC1
// pin (GPIO1-10) - WiFi owns ADC2. Free ADC1 spares: 3, 4, 5, 6, 9.
#define NIMBUS_BATT_SENSE_PIN         4   // ADC1 GPIO on the divider node
#define NIMBUS_BATT_DIVIDER_X100    320   // (Rtop+Rbot)/Rbot * 100 - 220k+100k = ÷3.2
#define NIMBUS_BATT_CELLS             2   // series cells (2 = 2S 18650 pack, 1 = LiPo)
#define NIMBUS_BATT_NOMINAL_MAH    3500   // per-cell nominal (LiitoKala INR18650-35E,
                                          // externally measured) - absolute mAh anchor:
                                          // capacityMah = health% * this (battery-measurement)
#define NIMBUS_BATT_VBUS_PIN         -1   // digital USB-present pin, or -1 to infer from V

// ---- WiFi / captive portal + web config (src/net) ---------------------------
// The AP hosts the config UI + a captive DNS; STA (once creds are saved) gives
// the config page reachability on the LAN via mDNS. The config page exposes no
// secrets, but a WPA2 AP still matches the fleet style and needs a >=8-char
// password (softAP silently opens if it's shorter).
#define NIMBUS_AP_SSID       "Nimbus-setup"
// FALLBACK ONLY: the setup AP normally uses the per-device passphrase generated
// on first boot + stored in NVS (agent::store::setupApPass, shown on the setup
// screen). This constant is used only if that store is unusable - it must never
// again be the fleet-wide shipped password.
#define NIMBUS_AP_PASS       "nimbus1234"   // WPA2 min 8 chars; "" => open AP
#define NIMBUS_MDNS_HOST     "nimbus"       // -> http://nimbus.local
// NVS keys (<=15 chars) for STA creds, stored via solide::memory::setString.
// The single slot is now the MRU MIRROR of the known-networks list below, kept so an
// OTA rollback to an older image still finds the network most likely to work.
#define NIMBUS_KEY_STA_SSID  "staSsid"
#define NIMBUS_KEY_STA_PASS  "staPass"
// Known networks (<=5) as one JSON blob. NVS and not a LittleFS file: net::begin()
// runs long before LittleFS is mounted, so a file-backed list would read empty on the
// boot-join path. It also keeps factory reset honest - nvs_flash_erase() still wipes
// every saved network, exactly as the Danger-zone copy promises.
#define NIMBUS_KEY_STA_NETS  "staNets"
