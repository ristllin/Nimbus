#pragma once
#include <Arduino.h>

// ui_wifi - RETIRED in Phase 3 C1. The Connectivity controls (reach info, Wi-Fi
// join, Bluetooth bonds, token rotation, factory reset) moved into the Settings
// pane in ui_device.h so the whole device-config surface lives under one nav
// item. Kept as an empty fragment so web_pages.h's ordered list + the concat
// check stay stable; safe to drop from the manifest in a later cleanup.

static const char UI_WIFI[] PROGMEM = R"=====()=====";
