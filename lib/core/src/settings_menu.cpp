#include "nimbus/settings_menu.h"

#include <string>

namespace nimbus {

namespace {
// Battery-mode display labels (owner 2026-07-18: ONE vocabulary - the mode IS
// the light level; the old separate "light" knob is gone). Machine keys stay
// battery_saver/balanced/desk (NVS + /api + the AI config schema).
const char* kProfileLabels[kProfileCount] = {"Dark", "Balanced", "Full"};

// The ring::Anim names, indexed by value (0..5), for the AttnAnim editor.
const char* kAnimNames[] = {"Off", "Solid", "Breathe", "Comet", "Blink", "Fade"};

std::string toStr(int32_t v) { return std::to_string(v); }

// Screensaver delay buckets. The color panel's backlight is the largest
// continuous draw on the device - larger than the 45-LED ring - so resting it IS
// the power saving, and an hour of backlight at an empty desk is the most
// wasteful thing the device can do. Minutes are the right granularity.
constexpr uint16_t kSaverStepsTft[] = {0, 1, 2, 5, 10, 15, 30, 60};

// Display text for a screensaver delay. Bucket values get friendly units; a
// console-set in-between value shows as raw minutes rather than being lied about.
std::string saverText(uint16_t m) {
  if (m == 0) return "Off";
  if (m % 60 == 0) return std::to_string(m / 60) + (m == 60 ? " hr" : " hr");
  return std::to_string(m) + " min";
}

const char* kVoiceProvNames[] = {"Mistral", "OpenAI"};

// An SSID is arbitrary bytes off the air and the 5x7 font only draws
// 32-126, so one UTF-8 network name would paint a row of '?'-noise (the "many
// ???" bug, live). Sanitise + clip HERE, in the FSM, so every consumer of
// view() - panel, serial RENDER?, a future surface - is safe by construction
// rather than by each renderer remembering. The raw bytes are kept separately
// (pickedSsid_) because the radio needs the real SSID, not this display copy.
std::string wifiRowText(const std::string& ssid, size_t maxChars = 44) {
  std::string o;
  o.reserve(ssid.size());
  for (unsigned char c : ssid) o += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
  if (o.size() > maxChars) o = o.substr(0, maxChars - 3) + "...";
  return o;
}

// Display-capitalize a machine slug for a menu row ("teal" -> "Teal"). The slug
// itself stays lowercase on the wire (NVS/API/theme list).
std::string titleCase(const std::string& s) {
  if (s.empty() || s[0] < 'a' || s[0] > 'z') return s;
  std::string out = s;
  out[0] = char(out[0] - 'a' + 'A');
  return out;
}

// Human value for a param's current effective/override value in the Tune list
// and Edit spinner. Keeps enum/sentinel params readable.
std::string valueLabel(Param p, int32_t v) {
  switch (p) {
    case Param::Posture:
      return v <= 0 ? "Dark" : v == 1 ? "Calm" : "Full";
    case Param::TgLowBattPing:
      return v ? "On" : "Off";
    case Param::AttnHue:
      if (v < 0) return "Auto";
      if (v == 255) return "White";
      return toStr(v);
    case Param::AttnAnim:
      if (v >= 0 && v < int(sizeof(kAnimNames) / sizeof(kAnimNames[0])))
        return kAnimNames[v];
      return toStr(v);
    case Param::AttnHoldMs:
      return toStr(v / 1000) + "s";   // ms -> seconds, readable at minute scale
    default:
      return toStr(v);
  }
}
}  // namespace

const char* modeName(Mode m) { return m == Mode::Orchestrator ? "orchestrator" : "notifier"; }
// Display label for menu rows (the lowercase modeName() is a wire/renderer
// contract - the renderer string-matches it; only the copy is Title Case).
const char* modeLabel(Mode m) { return m == Mode::Orchestrator ? "Orchestrator" : "Notifier"; }

void SettingsMenu::open() { enter(State::Main); }
void SettingsMenu::close() {
  state_ = State::Closed;
  sel_ = 0;
  adjusting_ = false;
}

void SettingsMenu::enter(State s) {
  state_ = s;
  sel_ = 0;
  adjusting_ = false;
  // ConfirmReset defaults to "No" (row 0) - a destructive action must not be
  // one accidental click away.
}

int SettingsMenu::itemCount() const {
  switch (state_) {
    case State::Closed:      return 0;
    case State::Main:        return kMainRows;
    case State::ProfilePick: return kProfileCount + 1;             // profiles + Back
    case State::ThemePick:   return themeCount() + 1;              // themes + Back
    case State::TuneList:    return kParamCount + 1;               // params + Back
    case State::Edit:        return cfg_->hasOverride(editing_) ? 3 : 2;  // value [+clear] +back
    case State::ConfirmReset: return 2;                            // No / Yes
    case State::Connectivity: return kConnRows;                    // Config via QR + Back
    case State::WifiMenu:    return kWifiRows;                     // publish/choose/forget + Back
    // Both pickers always carry a Back row, so an empty list is a way out, not
    // a dead end (and rotation always has somewhere to go).
    case State::WifiPick:    return int(scan_.size()) + 1;
    case State::WifiForget:  return int(known_.size()) + 1;
    case State::ConfigQr:    return 0;   // full-screen QR, not a list (any event exits)
    case State::TokenDetail: return 0;   // full sign-in code, not a list
    case State::SelfTest:    return 0;   // full-screen results (device-filled), any event exits
    case State::Battery:     return 0;   // full-screen detail (device-filled), any event exits
    case State::Sound:       return kSoundRows;
    case State::UpdateMenu:  // Auto + Check + [Install] + Back
      return 3 + ((otaAllowed_ && !updateVersion_.empty()) ? 1 : 0);
    case State::ConfirmInstall: return 2;  // Cancel / Install and restart
    case State::Display:     return kDispRows;   // Display flip + Back
  }
  return 0;
}

// Visible Main index -> logical row. Every row is shown, so the mapping is
// identity (kept as a seam in case a row becomes conditional again).
SettingsMenu::MainRow SettingsMenu::mainRowAt(int idx) const {
  return MainRow(idx);
}

void SettingsMenu::clampSel() {
  const int n = itemCount();
  if (n <= 0) {
    sel_ = 0;
    return;
  }
  if (sel_ < 0) sel_ = 0;
  if (sel_ >= n) sel_ = n - 1;
}

void SettingsMenu::onRotate(int dir) {
  if (volAdjusting_) {          // Volume captured: rotate = +/-5, clamped
    int v = sfxVolume_ + dir * 5;
    sfxVolume_ = v < 0 ? 0 : (v > 100 ? 100 : v);
    dirty_ = true;              // device persists + applies live
    return;
  }
  if (state_ == State::Closed) return;
  if (state_ == State::ConfigQr) {  // any event dismisses the full-screen QR
    enter(State::Connectivity);
    sel_ = ConnConfigQr;
    return;
  }
  if (state_ == State::TokenDetail) {
    enter(State::Connectivity);
    sel_ = ConnToken;
    return;
  }
  if (state_ == State::SelfTest) { enter(State::Main); sel_ = RowSelfTest; return; }
  if (state_ == State::Battery)  { enter(State::Main); sel_ = RowBattery;  return; }
  const int d = (dir >= 0) ? 1 : -1;

  // In Edit while adjusting, rotation steps the value; otherwise it moves the
  // cursor like a normal list.
  if (state_ == State::Edit && adjusting_) {
    const int32_t cur = cfg_->effective(editing_);
    const int32_t next = stepParam(editing_, cur, d);
    if (next != cur) {
      cfg_->setOverride(editing_, next);
      dirty_ = true;
    }
    return;
  }

  const int n = itemCount();
  if (n <= 0) return;
  sel_ = (sel_ + d + n) % n;  // wrap
}

void SettingsMenu::onClick() {
  switch (state_) {
    case State::Closed:
      return;

    case State::Main:
      switch (mainRowAt(sel_)) {
        case RowMode:
          mode_ = (mode_ == Mode::Notifier) ? Mode::Orchestrator : Mode::Notifier;
          dirty_ = true;
          return;
        case RowProfile:
          enter(State::ProfilePick);
          sel_ = int(cfg_->profile());  // start on the current profile
          return;
        case RowTheme:
          enter(State::ThemePick);
          sel_ = theme_;                // start on the current theme
          return;
        case RowSound:
          enter(State::Sound);
          return;
        case RowSaver: {                // cycle Off -> 1 -> 2 -> 5 -> 10 -> 15 -> 30 -> 60 -> Off
          const uint16_t* steps = kSaverStepsTft;
          const int count = int(sizeof(kSaverStepsTft) / sizeof(kSaverStepsTft[0]));
          int next = 0;                 // wrap default: back to Off
          for (int i = 0; i < count; ++i)
            if (steps[i] > saverMin_) { next = steps[i]; break; }
          saverMin_ = uint16_t(next);
          dirty_ = true;
          return;
        }
        case RowUpdate:
          enter(State::UpdateMenu);
          return;
        case RowTune:
          enter(State::TuneList);
          return;
        case RowConn:
          enter(State::Connectivity);
          return;
        case RowReset:
          enter(State::ConfirmReset);  // defaults to No (row 0)
          return;
        case RowSelfTest:
          enter(State::SelfTest);   // device runs the health check + renders it full-screen
          return;
        case RowBattery:
          enter(State::Battery);    // device renders live battery detail full-screen
          return;
        case RowSdCard:
          // Same action as Connectivity > Re-probe SD: reseat the card, click,
          // and the device re-inits the bus + refreshes this row's status.
          sdProbeRequested_ = true;
          return;
        case RowDisplay:                 // open the Display submenu (screen flip, ...)
          enter(State::Display);
          return;
        case RowClose:
          close();
          return;
      }
      return;

    case State::Display:
      if (sel_ == DispFlip) {            // TFT only: turn the screen 180 degrees
        screenFlip_ = !screenFlip_;      // device applies (setFlip) + persists on dirty()
        dirty_ = true;
        return;                          // stay on the row so the state flip is visible
      }
      enter(State::Main);                // Back
      sel_ = RowDisplay;
      return;

    case State::ProfilePick:
      if (sel_ >= kProfileCount) {  // Back row
        enter(State::Main);
        sel_ = RowProfile;
        return;
      }
      // Re-selecting the already-active profile changes nothing; don't dirty
      // the config (that would force a no-op persist / NVS write in main.cpp).
      if (ProfileId(sel_) != cfg_->profile()) {
        cfg_->setProfile(ProfileId(sel_));
        dirty_ = true;
      }
      enter(State::Main);
      sel_ = RowProfile;
      return;

    case State::ThemePick:
      if (sel_ >= themeCount()) {  // Back row
        enter(State::Main);
        sel_ = RowTheme;
        return;
      }
      if (sel_ != theme_) {   // only dirty on a real change (avoids a no-op NVS write)
        theme_ = sel_;
        dirty_ = true;
      }
      enter(State::Main);
      sel_ = RowTheme;
      return;

    case State::TuneList:
      if (sel_ >= kParamCount) {  // Back row
        enter(State::Main);
        sel_ = RowTune;
        return;
      }
      editing_ = Param(sel_);
      enter(State::Edit);  // cursor on the value row
      return;

    case State::Connectivity:
      if (sel_ == ConnWifi) {
        enter(State::WifiMenu);   // the escape hatch: recover the network on the device alone
        return;
      }
      if (sel_ == ConnBluetooth) {
        bleEnabled_ = !bleEnabled_;  // device applies (net::ble::setEnabled) + persists
        dirty_ = true;
        return;                      // stay on the row so the state flip is visible
      }
      if (sel_ == ConnForget) {
        forgetRequested_ = true;  // device drains -> net::ble::forgetBonds()
        return;                   // stay on the row so the new count is visible
      }
      if (sel_ == ConnSdProbe) {
        sdProbeRequested_ = true;  // device drains -> memory::promoteSd() (SD.end()+begin())
        return;                    // stay on the row so the new card state is visible
      }
      if (sel_ == ConnConfigQr) {
        enter(State::ConfigQr);   // device shows the full-screen QR until any event
        return;
      }
      if (sel_ == ConnToken) {
        enter(State::TokenDetail);   // device renders the full, untruncated code
        return;
      }
      enter(State::Main);         // Back
      sel_ = RowConn;
      return;

    case State::WifiMenu:
      switch (sel_) {
        case WifiPublishAp:
          // Device drains -> wifi::Inputs::reqPublishAp (the AP comes up and
          // joining pauses). NOT dirty: the radio is not Config state.
          publishApRequested_ = true;
          return;                 // stay on the row so the new status is visible
        case WifiChooseNet:
          wifiScanRequested_ = true;   // the list is only useful if it is fresh
          enter(State::WifiPick);
          return;
        case WifiForgetNet:
          enter(State::WifiForget);
          return;
        default:                  // Back
          enter(State::Connectivity);
          sel_ = ConnWifi;
          return;
      }

    case State::WifiPick:
      if (sel_ >= int(scan_.size())) {   // Back row (the only row when nothing is in range)
        enter(State::WifiMenu);
        sel_ = WifiChooseNet;
        return;
      }
      pickedSsid_ = scan_[size_t(sel_)];  // the RAW SSID, not the display copy
      wifiJoinRequested_ = true;          // device drains -> wifi::Inputs::reqJoin*
      enter(State::WifiMenu);             // the join is under way; the Wi-Fi row shows it
      sel_ = WifiChooseNet;
      return;

    case State::WifiForget:
      if (sel_ >= int(known_.size())) {   // Back row
        enter(State::WifiMenu);
        sel_ = WifiForgetNet;
        return;
      }
      pickedSsid_ = known_[size_t(sel_)];
      wifiForgetRequested_ = true;
      // Stay in the list: forgetting is often plural, and the device re-seeds
      // setWifiKnown() after draining so the list visibly shrinks.
      clampSel();
      return;

    case State::Edit: {
      if (sel_ == 0) {
        // Toggle the value row's adjusting sub-mode.
        adjusting_ = !adjusting_;
        return;
      }
      const bool hasOv = cfg_->hasOverride(editing_);
      const int clearRow = hasOv ? 1 : -1;
      const int backRow = hasOv ? 2 : 1;
      if (sel_ == clearRow) {
        cfg_->clearOverride(editing_);
        dirty_ = true;
        clampSel();  // the Clear row just vanished; keep the cursor valid
        return;
      }
      if (sel_ == backRow) {
        const Param p = editing_;
        enter(State::TuneList);
        sel_ = int(p);
        return;
      }
      return;
    }

    case State::ConfigQr:  // any click returns to the Connectivity submenu
      enter(State::Connectivity);
      sel_ = ConnConfigQr;
      return;

    case State::TokenDetail:  // any click returns to its source row
      enter(State::Connectivity);
      sel_ = ConnToken;
      return;

    case State::SelfTest:  // any click dismisses the full-screen results
      enter(State::Main);
      sel_ = RowSelfTest;
      return;

    case State::Battery:   // any click dismisses the full-screen detail
      enter(State::Main);
      sel_ = RowBattery;
      return;

    case State::Sound:
      switch (sel_) {
        case SndEffects:                // cycle Off -> Low -> Medium -> High
          sfxLevel_ = (sfxLevel_ + 1) % 4;
          dirty_ = true;
          return;
        case SndVoicePack:              // cycle sound themes (one for now)
          sfxVoice_ = (sfxVoice_ + 1) % 1;
          dirty_ = true;
          return;
        case SndVolume:                 // classic adjust: click captures rotation,
          volAdjusting_ = !volAdjusting_;   // rotate = value (ring gauge echoes),
          return;                           // click again releases
        case SndStt:                    // cycle Mistral <-> OpenAI
          sttProv_ ^= 1;
          dirty_ = true;
          return;
        case SndTts:
          ttsProv_ ^= 1;
          dirty_ = true;
          return;
        default:                        // Back
          enter(State::Main);
          sel_ = RowSound;
          return;
      }

    case State::UpdateMenu: {
      const bool hasInstall = otaAllowed_ && !updateVersion_.empty();
      if (sel_ == 0) {                  // Automatic updates toggle
        autoUpdate_ = !autoUpdate_;
        dirty_ = true;
        return;
      }
      if (sel_ == 1) {                  // Check for updates (no-op when unavailable)
        if (otaAllowed_) updateCheckRequested_ = true;   // device drains; NOT dirty
        return;                          // stay on the row; status reseeds live
      }
      if (hasInstall && sel_ == 2) {    // Install <ver> -> confirm screen
        enter(State::ConfirmInstall);   // defaults to Cancel (row 0)
        return;
      }
      enter(State::Main);               // Back
      sel_ = RowUpdate;
      return;
    }

    case State::ConfirmInstall:
      if (sel_ == 1) {                  // Install and restart
        updateInstallRequested_ = true; // device drains -> otaupd::requestInstall
        close();                        // the OTA UX owns the screen from here
        return;
      }
      enter(State::UpdateMenu);         // Cancel
      sel_ = 2;                          // back on the Install row
      clampSel();
      return;

    case State::ConfirmReset:
      if (sel_ == 1) {  // Yes, clear all
        // Only dirty (and thus persist) if there was actually something to
        // clear; confirming Reset with no overrides is a no-op.
        bool hadOverrides = false;
        for (int i = 0; i < kParamCount; ++i)
          if (cfg_->hasOverride(Param(i))) { hadOverrides = true; break; }
        if (hadOverrides) {
          cfg_->clearAllOverrides();
          dirty_ = true;
        }
      }
      enter(State::Main);
      sel_ = RowReset;
      return;
  }
}

void SettingsMenu::onLongPress() {
  volAdjusting_ = false;   // any back gesture releases the volume capture
  switch (state_) {
    case State::Closed:
      return;
    case State::Main:
      close();
      return;
    case State::ProfilePick:
      enter(State::Main);
      sel_ = RowProfile;
      return;
    case State::ThemePick:
      enter(State::Main);
      sel_ = RowTheme;
      return;
    case State::TuneList:
      enter(State::Main);
      sel_ = RowTune;
      return;
    case State::Edit: {
      if (adjusting_) {  // first back-press just leaves adjusting mode
        adjusting_ = false;
        return;
      }
      const Param p = editing_;
      enter(State::TuneList);
      sel_ = int(p);
      return;
    }
    case State::ConfirmReset:
      enter(State::Main);
      sel_ = RowReset;
      return;
    case State::Connectivity:
      enter(State::Main);
      sel_ = RowConn;
      return;
    case State::WifiMenu:
      enter(State::Connectivity);
      sel_ = ConnWifi;
      return;
    case State::WifiPick:
      enter(State::WifiMenu);
      sel_ = WifiChooseNet;
      return;
    case State::WifiForget:
      enter(State::WifiMenu);
      sel_ = WifiForgetNet;
      return;
    case State::ConfigQr:
      enter(State::Connectivity);
      sel_ = ConnConfigQr;
      return;
    case State::TokenDetail:
      enter(State::Connectivity);
      sel_ = ConnToken;
      return;
    case State::SelfTest:
      enter(State::Main);
      sel_ = RowSelfTest;
      return;
    case State::Battery:
      enter(State::Main);
      sel_ = RowBattery;
      return;
    case State::Sound:
      enter(State::Main);
      sel_ = RowSound;
      return;
    case State::Display:
      enter(State::Main);
      sel_ = RowDisplay;
      return;
    case State::UpdateMenu:
      enter(State::Main);
      sel_ = RowUpdate;
      return;
    case State::ConfirmInstall:
      enter(State::UpdateMenu);
      sel_ = 2;      // back on the Install row (clamped if it vanished)
      clampSel();
      return;
  }
}

int SettingsMenu::editValuePct() const {
  if (state_ != State::Edit || !cfg_) return 0;
  const ParamMeta m = paramMeta(editing_);
  const int32_t cur = cfg_->effective(editing_);
  if (m.max <= m.min) return 0;
  int32_t pct = (int32_t)(((int64_t)(cur - m.min) * 100) / (m.max - m.min));
  return pct < 0 ? 0 : (pct > 100 ? 100 : (int)pct);
}

const char* SettingsMenu::helpText() const {
  // TuneList: describe the param row the cursor points at (not the Back row).
  if (state_ == State::TuneList && sel_ < kParamCount)
    return paramDescription(Param(sel_));
  // Edit: describe the edited param on every row, adjusting or not.
  if (state_ == State::Edit) return paramDescription(editing_);
  // Main: the top-level rows read ambiguously on their own (a profile looks like
  // a "battery state"; "Full" looks like it might mean sound). Clarify the three
  // rows the owner flagged, right at the top level. (<=144 chars: 3 wrapped lines.)
  if (state_ == State::Main) {
    switch (mainRowAt(sel_)) {
      case RowDisplay:
        return "Screen orientation and other display settings.";
      case RowMode:
        return "Notifier: a Bluetooth status light for your coding "
               "sessions. Orchestrator: the AI assistant (Telegram + voice).";
      case RowProfile:
        return "Battery mode: Dark = no lights, Balanced = themed cue + dimmer + "
               "shorter holds, Full = every session an arc. Each is a customizable "
               "preset.";
      case RowSound:
        return "Sound effects, sound theme, speaker volume, and the voice "
               "services for dictation and spoken replies.";
      case RowSaver:
        return "After this long with nothing new to show, the screen rests "
               "on the Nimbus mark. Tap the screen to wake it.";
      case RowUpdate:
        return "Check for and install firmware updates. Every update is "
               "signed and verified before it runs.";
      case RowSdCard:
        return "Use a FAT32-formatted card (exFAT will not mount). Not "
               "detected? Reseat the card, then click this row to retry.";
      default: break;
    }
  }
  // ProfilePick: spell out what each battery mode does. The mode carries its
  // light behavior (owner 2026-07-18) - no separate light level to explain.
  if (state_ == State::ProfilePick && sel_ < kProfileCount) {
    switch (sel_) {
      case 0: return "Dark: no lights - only an error breathes red. Slow screen "
                     "refresh. Best on battery.";
      case 1: return "Balanced: theme active as a single soft cue, lower "
                     "brightness, shorter light hold times. Medium refresh.";
      case 2: return "Full: every session a color arc at full brightness, fast "
                     "screen refresh. Best on USB power.";
      default: break;
    }
  }
  // Sound submenu rows.
  if (state_ == State::Sound) {
    switch (sel_) {
      case SndEffects:
        return "How talkative the voice clips are (Off/Low/Medium/High). "
               "Sound only - separate from the battery mode.";
      case SndVolume:
        return "Speaker volume. Click, turn to adjust, click again to set.";
      case SndStt:
        return "Turns your voice into text - from the microphone or "
               "Telegram voice notes.";
      case SndTts:
        return "The voice service used when Nimbus speaks replies aloud.";
      default: break;
    }
  }
  // Display submenu: the flip row explains what it does; Back has no pane.
  if (state_ == State::Display && sel_ == DispFlip)
    return "Turns the screen 180 degrees for an upside-down mount. "
           "Takes effect right away.";
  // Software update rows: live status while it exists; the Notifier-mode
  // explanation on the disabled Check row.
  if (state_ == State::UpdateMenu) {
    if (sel_ == 1 && !otaAllowed_)
      return "Updating needs memory that Bluetooth is using. Switch to "
             "Orchestrator mode, then check again.";
    if (!updateStatus_.empty()) return updateStatus_.c_str();
    if (sel_ == 0)
      return "Installs new firmware when the device is idle and charged, "
             "then restarts.";
    return "";
  }
  if (state_ == State::ConfirmInstall)
    return "Downloads, verifies the signature, and restarts. Takes about "
           "two minutes. Keep the device powered on.";
  // Connectivity Wi-Fi row: read-only live status (device overlays SSID/IP).
  if (state_ == State::Connectivity && sel_ == ConnWifi)
    return "Wi-Fi status and this device's IP address. Click to join, "
           "forget, or publish the setup network.";
  // Wi-Fi submenu: what each escape-hatch row actually does.
  if (state_ == State::WifiMenu) {
    switch (sel_) {
      case WifiPublishAp:
        return "Turns the setup network on so a phone can reach this device. "
               "Joining a network pauses while it is up.";
      case WifiChooseNet:
        return "Shows the Wi-Fi networks in range. Click one to join it.";
      case WifiForgetNet:
        return "Removes a saved network so this device stops trying to "
               "join it.";
      default: return "";        // Back row: no pane
    }
  }
  if (state_ == State::WifiPick)
    return scan_.empty() ? "No networks in range. Publish the setup network "
                           "and set Wi-Fi up from a phone."
                         : "Turn to see more networks. Click one to join it.";
  if (state_ == State::WifiForget)
    return known_.empty() ? "No saved networks yet."
                          : "Click a network to remove it from this device.";
  // Connectivity Bluetooth row: static fallback (the device overlays LIVE
  // status - advertising / linked / off - via onBluetoothRow()).
  if (state_ == State::Connectivity && sel_ == ConnBluetooth)
    return "Lets the nimbus-notify broker on your computer send session "
           "status to this device.";
  // Forget row: static fallback (the device overlays the live bond count via
  // onForgetRow()). Removes the bond so the Mac must re-bond (Just Works, auto).
  if (state_ == State::Connectivity && sel_ == ConnForget)
    return "Unpairs every bonded computer. Each re-pairs by itself the "
           "next time it connects.";
  // Re-probe SD row: static fallback (device overlays the live card state).
  if (state_ == State::Connectivity && sel_ == ConnSdProbe)
    return "Remounts a reseated card without restarting.";
  return "";  // no pane anywhere else (renderer hides it on empty)
}

void SettingsMenu::view(solide::menu::MenuView& out) const {
  out.items.clear();
  out.visible = isOpen();
  out.selected = sel_;

  switch (state_) {
    case State::Closed:
      out.title.clear();
      out.selected = 0;
      return;

    case State::Main: {
      out.title = fwVersion_.empty() ? "Settings" : ("Settings  " + fwVersion_);
      out.items.push_back(std::string("Mode: ") + modeLabel(mode_));
      out.items.push_back(std::string("Battery mode: ") + profileLabel(cfg_->profile()));
      out.items.push_back("Customize >");     // ASCII only: font is 32-126
      out.items.push_back("Connectivity >");   // Wi-Fi/Bluetooth config via the QR->web page
      out.items.push_back("Sound >");          // effects/sound theme/volume/providers
      out.items.push_back(std::string("Theme: ") + titleCase(themeAt(theme_)));
      out.items.push_back(std::string("Screensaver: ") + saverText(saverMin_));
      out.items.push_back("Software update >");
      out.items.push_back("Reset to defaults");
      out.items.push_back("Self-test >"); // '>' = opens a screen (menu convention)
      out.items.push_back("Battery >");   // live battery detail full-screen
      out.items.push_back(std::string("SD card: ") + (sdStatus_.empty() ? "?" : sdStatus_));
      out.items.push_back("Display >");          // screen flip (+ touch calibration)
      out.items.push_back("Done");
      return;
    }

    case State::Display: {
      out.title = "Settings > Display";
      out.items.push_back(std::string("Display flip: ") + (screenFlip_ ? "On" : "Off"));
      out.items.push_back("< Back");
      return;
    }

    case State::SelfTest:
      out.title = "Settings > Self-test";
      return;   // device runs hw::runNow() + fills ScreenCtx.selfTest (full-screen)

    case State::Battery:
      out.title = "Settings > Battery";
      return;   // device fills the battery detail from the model (full-screen)

    case State::ProfilePick: {
      out.title = "Settings > Battery mode";
      for (int i = 0; i < kProfileCount; ++i) {
        std::string row = kProfileLabels[i];
        if (ProfileId(i) == cfg_->profile()) row += " *";  // ASCII check marker
        out.items.push_back(row);
      }
      out.items.push_back("< Back");
      return;
    }

    case State::ThemePick: {
      out.title = "Settings > Theme";
      const int n = themeCount();
      for (int i = 0; i < n; ++i) {
        std::string row = titleCase(themeAt(i));
        if (i == theme_) row += " *";   // ASCII marker for the active theme
        out.items.push_back(row);
      }
      out.items.push_back("< Back");
      return;
    }

    case State::TuneList: {
      out.title = "Settings > Customize";
      for (int i = 0; i < kParamCount; ++i) {
        const Param p = Param(i);
        std::string row = std::string(paramLabel(p)) + ": " +
                          valueLabel(p, cfg_->effective(p));
        if (cfg_->hasOverride(p)) row += " *";
        out.items.push_back(row);
      }
      out.items.push_back("< Back");
      return;
    }

    case State::Edit: {
      out.title = std::string("Settings > Customize > ") + paramLabel(editing_);
      const std::string val = valueLabel(editing_, cfg_->effective(editing_));
      // Chevrons signal the value is being adjusted (rotation captured);
      // brackets show it is merely selected.
      out.items.push_back(adjusting_ ? ("< " + val + " >")   // rotate adjusts
                                     : ("[ " + val + " ]"));
      if (cfg_->hasOverride(editing_)) out.items.push_back("Reset to default");
      out.items.push_back("< Back");
      return;
    }

    case State::ConfirmReset:
      out.title = "Settings > Reset to defaults?";
      out.items.push_back("Cancel");
      out.items.push_back("Reset all");
      return;

    case State::Sound: {
      out.title = "Settings > Sound";
      static const char* kLvl[] = {"Off", "Low", "Medium", "High"};
      static const char* kVoice[] = {"Pulse"};
      out.items.push_back(std::string("Sound effects: ") + kLvl[sfxLevel_ & 3]);
      out.items.push_back(std::string("Sound theme: ") + kVoice[sfxVoice_ % 1]);
      // Chevrons while captured (P2.2): the row looked IDENTICAL whether or not
      // rotation was adjusting the value - same convention as the Tune Edit row.
      out.items.push_back(volAdjusting_
          ? ("< Volume: " + std::to_string(sfxVolume_) + "% >")
          : ("Volume: " + std::to_string(sfxVolume_) + "%"));
      out.items.push_back(std::string("Dictation: ") + kVoiceProvNames[sttProv_ & 1]);
      out.items.push_back(std::string("Spoken replies: ") + kVoiceProvNames[ttsProv_ & 1]);
      out.items.push_back("< Back");
      return;
    }

    case State::UpdateMenu:
      out.title = "Settings > Software update";
      out.items.push_back(std::string("Automatic updates: ") + (autoUpdate_ ? "On" : "Off"));
      out.items.push_back(otaAllowed_ ? "Check for updates"
                                      : "Check for updates (unavailable)");
      if (otaAllowed_ && !updateVersion_.empty())
        out.items.push_back("Install " + updateVersion_);
      out.items.push_back("< Back");
      return;

    case State::ConfirmInstall:
      out.title = "Settings > Install " + updateVersion_ + "?";
      out.items.push_back("Cancel");
      out.items.push_back("Install and restart");
      return;

    case State::Connectivity:
      out.title = "Settings > Connectivity";
      // Wi-Fi row: the device overlays the live STA IP + join state via
      // wifiRowLabel() (same "Wi-Fi: <status> >" shape); this static label is
      // the no-device fallback. The chevron is the promise that it opens the
      // Wi-Fi screen - the row is no longer read-only status.
      out.items.push_back("Wi-Fi: - >");
      // Bluetooth row: the persisted on/off intent. The device OVERLAYS this
      // label with live status (advertising / linked / off) via showingConnectivity().
      out.items.push_back(std::string("Bluetooth: ") + (bleEnabled_ ? "On" : "Off"));
      // Forget paired devices: the "remove device" half of the keyboard-model
      // pairing. The device overlays the live bond count into this row.
      out.items.push_back("Forget paired devices");
      // Re-probe SD: force a real SD bus re-init (end()+begin()) so a card that
      // was re-seated after boot re-mounts without a reboot. The device overlays
      // the live card state (mounted / absent / lost) into this row.
      out.items.push_back("Rescan SD card");
      // Scanning the QR opens the on-device web page where WiFi credentials,
      // BLE, and everything else are entered from a phone (a far better UX than
      // a knob-driven character spinner). Live status shows on that page.
      out.items.push_back("Sign-in QR >");
      // The value cannot fit safely in a two-column row. This is a real
      // destination: the device renders the exact token full-screen.
      out.items.push_back("Device sign-in code >");
      out.items.push_back("< Back");
      return;

    case State::WifiMenu:
      out.title = "Settings > Connectivity > Wi-Fi";
      // The escape hatch, in the order you need it when you are locked out:
      // get a page you can reach, then join, then clean up.
      out.items.push_back("Publish setup network");
      out.items.push_back("Choose network >");   // ASCII only: font is 32-126
      out.items.push_back("Forget network >");
      out.items.push_back("< Back");
      return;

    case State::WifiPick:
      out.title = "Settings > Connectivity > Wi-Fi > Choose network";
      // Every row, unwindowed: drawMenu already scrolls the list around
      // `selected`, so a second scroll model here would only fight it.
      for (const std::string& ssid : scan_) out.items.push_back(wifiRowText(ssid));
      out.items.push_back("< Back");
      return;

    case State::WifiForget:
      out.title = "Settings > Connectivity > Wi-Fi > Forget network";
      for (const std::string& ssid : known_) out.items.push_back(wifiRowText(ssid));
      out.items.push_back("< Back");
      return;

    case State::ConfigQr:
      // No list items: the device swaps in the full-screen ConfigQr screen
      // (showingConfigQr()) and draws the QR from the live config URL. The
      // title still carries the breadcrumb for the (unused) menu fallback.
      out.title = "Settings > Connectivity > Sign-in QR";
      return;

    case State::TokenDetail:
      out.title = "Settings > Connectivity > Device sign-in code";
      return;
  }
}

}  // namespace nimbus
