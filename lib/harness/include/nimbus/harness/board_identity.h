#pragma once
#include <cstdint>
#include <cstring>

// board_identity - what the head says about the hardware it runs on (CUM-514).
//
// The identity line and the 'hardware' skill were written for the hand-built
// Solide S3, so a Freenove all-in-one described a 45-LED ring, resistive touch
// and an INMP441 microphone it does not have. One static entry per build (board
// slug + panel size, the same -DSOLIDE_BOARD token GET /api/state reports as
// "board") now carries every board fact the prompt states. Static strings only:
// they live in flash, so the turn path pays nothing. test/test_board_identity
// checks each entry against the solide-drivers board profile it describes.
namespace agent {

struct BoardIdentity {
  const char* slug;      // the build's -DSOLIDE_BOARD token ("solide_s3" | "freenove_s3")
  int         panelW;    // panel the build drives, landscape px (0 = any)
  int         panelH;
  const char* name;      // the identity line: "running in Orchestrator mode on a <name> desk device"
  const char* display;   // the capability line's screen fragment
  bool        physicalRing;  // a real addressable LED ring; false = the status ring is drawn on screen
  uint16_t    ledCount;      // pixels on the board's LED pin (the ring, or one status pixel)
  const char* hardware;      // the 'hardware' skill capsule body
};

inline constexpr BoardIdentity kBoardIdentities[] = {
    {"solide_s3", 0, 0, "Solide S3 (ESP32-S3-DevKitC-1)", "2.8\" color touchscreen", true, 45,
     "# Device hardware\n"
     "Board: Solide S3, a hand-built ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB PSRAM;\n"
     "TLS + the memory working set live in PSRAM, ~300 KB internal SRAM is scarce).\n"
     "Peripherals: 45-LED WS2812B ring; 2.8\" ILI9341 color screen with XPT2046 resistive\n"
     "touch; I2S MEMS microphone (INMP441/ICS-43434); MAX98357A I2S speaker amp (both audio\n"
     "paths verified via acoustic loopback); microSD over SPI (orchestrator data store);\n"
     "Wi-Fi 2.4 GHz; BLE (Notifier mode only); a 2S Li-ion pack with voltage sense.\n"
     "Call system.health for the LIVE status of each of these.\n"},
    {"freenove_s3", 320, 240, "Freenove ESP32-S3 2.8\" display board (FNK0104B)",
     "2.8\" color touchscreen", false, 1,
     "# Device hardware\n"
     "Board: Freenove ESP32-S3 2.8\" display board (FNK0104B), an off-the-shelf all-in-one\n"
     "ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM; TLS + the memory working set live in PSRAM,\n"
     "~300 KB internal SRAM is scarce). Nothing to wire.\n"
     "Peripherals: 2.8\" ILI9341 color screen (240x320) with FT6336U capacitive touch; NO\n"
     "LED ring - the status ring is drawn on the screen (one on-board status LED); ES8311\n"
     "audio codec with the on-board microphone and speaker amp; microSD over SDMMC\n"
     "(orchestrator data store); Wi-Fi 2.4 GHz; BLE (Notifier mode only); a 1S Li-ion\n"
     "battery input with voltage sense.\n"
     "Call system.health for the LIVE status of each of these.\n"},
    {"freenove_s3", 480, 320, "Freenove ESP32-S3 3.5\" display board", "3.5\" color touchscreen",
     false, 1,
     "# Device hardware\n"
     "Board: Freenove ESP32-S3 3.5\" display board, an off-the-shelf all-in-one ESP32-S3\n"
     "N16R8 (16 MB flash, 8 MB PSRAM; TLS + the memory working set live in PSRAM, ~300 KB\n"
     "internal SRAM is scarce). Nothing to wire.\n"
     "Peripherals: 3.5\" color screen (480x320) with capacitive touch; NO LED ring - the\n"
     "status ring is drawn on the screen (one on-board status LED); ES8311 audio codec with\n"
     "the on-board microphone and speaker amp; microSD over SDMMC (orchestrator data store);\n"
     "Wi-Fi 2.4 GHz; BLE (Notifier mode only); a 1S Li-ion battery input with voltage sense.\n"
     "Call system.health for the LIVE status of each of these.\n"},
    {"freenove_s3", 480, 480, "Freenove ESP32-S3 4.0\" display board", "4.0\" color touchscreen",
     false, 1,
     "# Device hardware\n"
     "Board: Freenove ESP32-S3 4.0\" display board, an off-the-shelf all-in-one ESP32-S3\n"
     "N16R8 (16 MB flash, 8 MB PSRAM; TLS + the memory working set live in PSRAM, ~300 KB\n"
     "internal SRAM is scarce). Nothing to wire.\n"
     "Peripherals: 4.0\" color screen (480x480) with capacitive touch; NO LED ring - the\n"
     "status ring is drawn on the screen (one on-board status LED); ES8311 audio codec with\n"
     "the on-board microphone and speaker amp; microSD over SDMMC (orchestrator data store);\n"
     "Wi-Fi 2.4 GHz; BLE (Notifier mode only); a 1S Li-ion battery input with voltage sense.\n"
     "Call system.health for the LIVE status of each of these.\n"},
};

// The identity for a slug no entry knows: board-neutral, never another board's facts.
inline constexpr BoardIdentity kGenericBoardIdentity = {
    "", 0, 0, "Nimbus (ESP32-S3)", "color touchscreen", true, 0,
    "# Device hardware\n"
    "Board: an ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM; TLS + the memory working set live\n"
    "in PSRAM, ~300 KB internal SRAM is scarce). The live manifest names its screen, ring,\n"
    "audio and storage; call system.health for the LIVE status of each.\n"};

// The identity for a build: board slug + panel size. An entry with panelW 0 fits any
// panel; otherwise the panel must match. An unknown slug gets kGenericBoardIdentity.
inline const BoardIdentity& boardIdentity(const char* slug, int panelW, int panelH) {
  for (const BoardIdentity& b : kBoardIdentities) {
    if (!slug || std::strcmp(b.slug, slug) != 0) continue;
    if (b.panelW == 0 || (b.panelW == panelW && b.panelH == panelH)) return b;
  }
  for (const BoardIdentity& b : kBoardIdentities)   // a known board, unlisted panel size
    if (slug && std::strcmp(b.slug, slug) == 0) return b;
  return kGenericBoardIdentity;
}

// ---- this build's board ---------------------------------------------------------
// The SAME token GET /api/state reports as "board": the build's -DSOLIDE_BOARD
// (solide-drivers defaults it to solide_s3 when unset - host tests, nimbusd), and
// the panel size -DNIMBUS_PANEL_W/H (default the 2.8" 320x240, as tft_render/theme.h).
#ifdef SOLIDE_BOARD
#define NIMBUS_BOARD_ID_STR2(x) #x
#define NIMBUS_BOARD_ID_STR(x) NIMBUS_BOARD_ID_STR2(x)
inline constexpr const char* kBuildBoardSlug = NIMBUS_BOARD_ID_STR(SOLIDE_BOARD);
#else
inline constexpr const char* kBuildBoardSlug = "solide_s3";
#endif
#ifdef NIMBUS_PANEL_W
inline constexpr int kBuildPanelW = NIMBUS_PANEL_W;
#else
inline constexpr int kBuildPanelW = 320;
#endif
#ifdef NIMBUS_PANEL_H
inline constexpr int kBuildPanelH = NIMBUS_PANEL_H;
#else
inline constexpr int kBuildPanelH = 240;
#endif

inline const BoardIdentity& buildBoardIdentity() {
  return boardIdentity(kBuildBoardSlug, kBuildPanelW, kBuildPanelH);
}

}  // namespace agent
