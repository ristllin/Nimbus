#pragma once

// NVS keys that encode the board's PHYSICAL identity: which panel is fitted, how
// it is mounted, its touch calibration, and its typed-OTA device slug. These MUST
// survive a factory reset. A plain nvs_flash_erase() wipes them too, and on the
// a stale board could read a screenModel of "eink"; that value is unsupported
// now and boots an unsupported-display notice on the color panel
// from on its own. The factory-reset path reads these, erases, then writes them
// back (see src/main.cpp).
//
// Frozen machine keys (AGENTS.md): these literals mirror the AKEY_* macros in
// src/agent/agent_config.h, pinned by a static_assert where the device uses them.
// Kept here in portable, host-tested code so the preserve loop and its test share
// one source of truth. tftFlip is int-typed and handled alongside the string keys.

namespace nimbus {
namespace config {

inline constexpr const char* kIdentityStrKeys[] = {
    "scrModel",  // AKEY_SCREEN_MODEL - "eink" | "tft"
    "tchCal",    // AKEY_TOUCH_CAL    - touch calibration blob (stored as a string)
    "otaType",   // AKEY_OTA_TYPE     - typed-OTA device slug
};
inline constexpr int kIdentityStrKeyCount =
    (int)(sizeof(kIdentityStrKeys) / sizeof(kIdentityStrKeys[0]));

// The one int-typed identity key, preserved with the string keys above.
inline constexpr const char* kIdentityIntKeyTftFlip = "tftFlip";  // AKEY_TFT_FLIP

}  // namespace config
}  // namespace nimbus
