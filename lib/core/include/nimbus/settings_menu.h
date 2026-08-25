#pragma once
#include <cstdint>

#include <string>
#include <vector>

#include "nimbus/profile.h"
#include "nimbus/theme.h"   // themeAt/themeList for the Theme picker
#include "solide/menu.h"  // solide::menu::MenuView - portable view struct

// settings_menu - the portable, host-testable settings FSM for Nimbus.
//
// A gesture-driven menu over a nimbus::Config. It is pure state + deterministic
// transitions: no Arduino, no display, no NVS. The device glue feeds gesture
// events (onRotate/onClick/onLongPress) from touch, reads view() into a ScreenCtx, and
// drains dirty() to persist + re-render.
//
// Tree (every screen titles itself with its full breadcrumb path, ASCII '>'
// separators - e.g. "Settings > Tune > ring_brightness"):
//   Main ┬ Mode            -> toggles Notifier/Orchestrator (view-only flag)
//        ├ Battery mode ›  -> ProfilePick: single-select ProfileId (setProfile)
//        ├ Customize ›     -> TuneList: every Param, its effective() value, an
//        │                     override marker (*) -> Edit one param
//        ├ Connectivity ›  -> Connectivity: Wi-Fi › (WifiMenu: publish the setup
//        │                     network, pick one to join, forget a saved one) +
//        │                     Bluetooth on/off (live status) +
//        │                     Config via QR -> ConfigQr (full-screen link to
//        │                     the on-device WiFi/BLE page)
//        ├ Display ›       -> Display: screen flip (180) [+ touch calibration]
//        ├ Reset to defaults -> ConfirmReset: "Reset all" clears all overrides
//        └ Done            -> back to the normal UI
//
// Editing a param steps its value within paramMeta() bounds and commits it as a
// sparse override (Config::setOverride) live; switching profile via ProfilePick
// leaves overrides intact. Every mutation flips dirty().
//
// Edit-screen interaction: the value row is a two-state control. Click it to
// enter "adjusting" - rotate then steps the value; click (or long-press) exits
// adjusting. When not adjusting, rotate moves the cursor over {value, Clear
// override?, Back} so every row stays reachable with a single step.
namespace nimbus {

// The Notifier/Orchestrator mode. The menu only toggles the flag and marks the
// config dirty; persistence of `mode` is a simple NVS key the device owns.
enum class Mode : uint8_t { Notifier = 0, Orchestrator = 1 };
const char* modeName(Mode m);   // machine key "notifier"/"orchestrator" (renderer contract)
const char* modeLabel(Mode m);  // display label "Notifier"/"Orchestrator" (menu rows)

class SettingsMenu {
 public:
  explicit SettingsMenu(Config& cfg) : cfg_(&cfg) {}

  // Lifecycle. open() enters at Main; close() returns to the normal UI.
  void open();
  void close();
  bool isOpen() const { return state_ != State::Closed; }

  // Gesture events (already debounced to one unit per detent).
  void onRotate(int dir);   // +1 / -1: move cursor, or adjust value in Edit
  void onClick();           // descend / commit / run the selected row
  void onLongPress();       // back one level; from Main, closes
  void onBack() { onLongPress(); }

  // Mode is menu-visible state the device syncs with NVS. Set it before opening
  // so the Mode row shows the persisted value; read it back after edits.
  void setMode(Mode m) { mode_ = m; }
  Mode mode() const { return mode_; }

  // BLE-advertising enable, shown+toggled in the Connectivity submenu. Like
  // mode, it is menu-visible state the device syncs with NVS: seed it before
  // opening, read it back after a toggle (dirty()), apply + persist device-side.
  void setBleEnabled(bool on) { bleEnabled_ = on; }
  bool bleEnabled() const { return bleEnabled_; }

  // LED colour theme, picked in the Theme submenu. Like mode/BLE it is menu-visible
  // state the device syncs with NVS: seed the index (themeIndexOf(store::theme()))
  // before opening; on dirty() read themeSlug() back, persist + re-render the ring.
  void setTheme(int idx) { theme_ = idx; }
  int  theme() const { return theme_; }
  std::string themeSlug() const { return themeAt(theme_); }

  // Sound cue rows (Main): the ACTIVE mode's sound level (0 none / 1 light /
  // 2 medium / 3 heavy - click cycles) and the sound theme (pulse).
  // Same NVS-sync contract as theme: the device seeds both before opening and
  // persists on dirty().
  void setSfxLevel(int lvl) { sfxLevel_ = lvl < 0 ? 0 : (lvl > 3 ? 3 : lvl); }
  int  sfxLevel() const { return sfxLevel_; }
  void setSfxVoice(int v) { sfxVoice_ = ((v % 3) + 3) % 3; }
  int  sfxVoice() const { return sfxVoice_; }
  void setSfxVolume(int v) { sfxVolume_ = v < 0 ? 0 : (v > 100 ? 100 : v); }
  int  sfxVolume() const { return sfxVolume_; }

  // Screensaver idle delay (minutes; 0 = off). The row cycles a bucket table
  // (Off/15/30/60/120/240); a console-set in-between value displays as-is and
  // is only rewritten when the user actually cycles the row.
  void setSaverMinutes(uint16_t m) { saverMin_ = m; }
  uint16_t saverMinutes() const { return saverMin_; }

  // Software update submenu state (all device-seeded; same NVS-sync contract
  // as theme/sfx). otaAllowed=false renders Check as "(unavailable)" and makes
  // it a no-op - OTA is Orchestrator-mode-only (Notifier's BLE owns the RAM).
  void setAutoUpdate(bool on) { autoUpdate_ = on; }
  bool autoUpdate() const { return autoUpdate_; }
  void setOtaAllowed(bool on) { otaAllowed_ = on; }
  void setUpdateStatus(const std::string& st) { updateStatus_ = st; }
  void setUpdateAvailable(const std::string& ver) { updateVersion_ = ver; }
  bool showingUpdate() const {
    return state_ == State::UpdateMenu || state_ == State::ConfirmInstall;
  }
  bool updateCheckRequested() const { return updateCheckRequested_; }
  void clearUpdateCheckRequest() { updateCheckRequested_ = false; }
  bool updateInstallRequested() const { return updateInstallRequested_; }
  void clearUpdateInstallRequest() { updateInstallRequested_ = false; }

  // Voice providers (Sound submenu cycles; 0 Mistral / 1 OpenAI).
  void setSttProvider(int v) { sttProv_ = v ? 1 : 0; }
  int  sttProvider() const { return sttProv_; }
  void setTtsProvider(int v) { ttsProv_ = v ? 1 : 0; }
  int  ttsProvider() const { return ttsProv_; }

  // True while the Connectivity submenu is showing; lets the device overlay the
  // Bluetooth row with LIVE status (advertising/linked/off) it alone knows.
  // Firmware version shown in the Main title band ("Settings  v2.0.0"); set by
  // the device at boot from NIMBUS_FW_VERSION. Empty (default) keeps the plain
  // "Settings" title, so host tests without a version stay untouched.
  void setFwVersion(const std::string& v) { fwVersion_ = v; }

  bool showingConnectivity() const { return state_ == State::Connectivity; }
  // Cursor on the WiFi status row - the device overlays the live STA IP + join
  // state (which the portable FSM can't know) into that row + the help pane.
  bool onWifiRow() const {
    return state_ == State::Connectivity && sel_ == ConnWifi;
  }
  // True when the cursor is on the Bluetooth row (device fills the live help pane).
  bool onBluetoothRow() const {
    return state_ == State::Connectivity && sel_ == ConnBluetooth;
  }
  // Cursor on the "Forget paired devices" row - the device overlays the live bond
  // count (which the portable FSM can't know) into that row + the help pane.
  bool onForgetRow() const {
    return state_ == State::Connectivity && sel_ == ConnForget;
  }
  // Set true when the user clicks "Forget paired devices"; the device drains it
  // (net::ble::forgetBonds() + clearForgetRequest()). Mirrors the bleEnabled sync.
  bool forgetBondsRequested() const { return forgetRequested_; }
  void clearForgetRequest() { forgetRequested_ = false; }

  // --- Wi-Fi: the ON-DEVICE escape hatch -----------------------------------
  // Clicking the Connectivity > Wi-Fi row opens a submenu instead of doing
  // nothing. That row used to be read-only status, which meant a device that
  // could not join its network could only be recovered over USB - the one
  // situation where the web UI is out of reach is exactly the situation the web
  // UI was the only cure for.
  //
  // Same contract as Forget paired devices / Rescan SD card: the FSM raises a
  // request flag, the device drains it (radio work is not portable) and clears
  // it. NONE of these are Config state, so none of them dirty().
  //
  // Screens: WifiMenu (publish / choose / forget / back), WifiPick (scan
  // results) and WifiForget (saved networks). All three are plain lists
  // rendered by the SAME ScreenId::Menu path as every other submenu - the
  // renderer's scroll window keeps `selected` visible, so the FSM emits every
  // row and owns no scroll offset of its own.
  bool showingWifi() const { return state_ == State::WifiMenu; }
  bool showingWifiPicker() const {
    return state_ == State::WifiPick || state_ == State::WifiForget;
  }

  // Networks the picker lists. Each element is ONE SSID exactly as the radio /
  // known-networks store reported it; the menu displays a printable-ASCII,
  // width-clipped copy (an SSID is arbitrary bytes off the air and the panel
  // font is 32-126 only) while wifiPickedSsid() hands back the ORIGINAL bytes,
  // because that is what has to go to the radio. Re-seeding while the picker is
  // open is safe: a shrinking list clamps the cursor.
  void setWifiScan(const std::vector<std::string>& rows) { scan_ = rows; clampSel(); }
  void setWifiKnown(const std::vector<std::string>& rows) { known_ = rows; clampSel(); }

  // The SSID the pending join/forget request refers to (empty until one is
  // raised). One field serves both because a click raises exactly one request.
  const std::string& wifiPickedSsid() const { return pickedSsid_; }

  // "Publish setup network": bring the setup access point up so a phone can
  // reach the web UI. The device drains it (wifi::Inputs::reqPublishAp).
  bool publishApRequested() const { return publishApRequested_; }
  void clearPublishApRequest() { publishApRequested_ = false; }

  // Raised on entering the picker: the list is only useful if it is fresh, and
  // only the device can run a scan. The rows arrive back via setWifiScan().
  bool wifiScanRequested() const { return wifiScanRequested_; }
  void clearWifiScanRequest() { wifiScanRequested_ = false; }

  // A network was picked to join / to forget; the SSID is wifiPickedSsid().
  bool wifiJoinRequested() const { return wifiJoinRequested_; }
  void clearWifiJoinRequest() { wifiJoinRequested_ = false; }
  bool wifiForgetRequested() const { return wifiForgetRequested_; }
  void clearWifiForgetRequest() { wifiForgetRequested_ = false; }

  // Cursor on the "Re-probe SD" row - the device overlays the live card state
  // (mounted / absent / lost) into that row so the owner sees why memory demoted.
  bool onSdProbeRow() const {
    return state_ == State::Connectivity && sel_ == ConnSdProbe;
  }
  // Set true when the user clicks "Re-probe SD"; the device drains it
  // (memory::promoteSd() - real SD.end()+begin() re-init) + clearSdProbeRequest().
  bool sdProbeRequested() const { return sdProbeRequested_; }
  void clearSdProbeRequest() { sdProbeRequested_ = false; }

  // SD status shown on the Main "SD: <status>" row (owner ask 2026-07-13): the
  // device seeds a short live string - "14.9 GB free" / "none - reseat?" /
  // "lost - reseat?". Clicking the row requests the same re-probe as the
  // Connectivity row (sdProbeRequested), so reseat -> click -> remount works
  // straight from the main menu.
  void setSdStatus(const std::string& st) { sdStatus_ = st; }

  // RING-ECHO feedback (owner ask 2026-07-13): a slow panel refresh lagged, but
  // the LEDs are instant, so while the menu is open the device paints the menu
  // STATE on the ring after every detent - list mode: N segments = N options,
  // the bright one = the cursor (you SEE each tick land); value-edit mode: a
  // proportional gauge. These expose what the device needs to draw that.
  bool editAdjusting() const { return state_ == State::Edit && adjusting_; }
  int  editValuePct() const;   // 0-100: edited param's value within its min..max
  // Classic value-adjust (owner): click Volume -> rotation changes the value
  // (ring gauge echoes it live) -> click again to exit. valueAdjusting() covers
  // BOTH that and the Tune-param Edit adjusting state; adjustValuePct() is the
  // 0-100 the device paints as the gauge.
  bool valueAdjusting() const { return volAdjusting_ || editAdjusting(); }
  int  adjustValuePct() const { return volAdjusting_ ? sfxVolume_ : editValuePct(); }

  // True while the "Config QR" row is active: the device renders the full-screen
  // ConfigQr screen (a scannable link to the on-device config page) INSTEAD of
  // the menu list, until any gesture returns to Main. Lets the menu stay a
  // pure list FSM while the device owns the QR pixels (net-dependent URL).
  bool showingConfigQr() const { return state_ == State::ConfigQr; }
  // The compact Connectivity row cannot fit the recovery token. Activating it
  // opens a full-screen exact value instead of presenting a chevron that does
  // nothing or a clipped credential that cannot be transcribed.
  bool showingTokenDetail() const { return state_ == State::TokenDetail; }
  // True while ROTATION IS CAPTURED editing a value (Volume on Main, or a Tune
  // param in Edit) - the device inverts the selected row so "editing" is
  // unmistakable from "navigating" (owner P2.2: volume gave no visual feedback).

  // Settings > Display > Display flip (colour panel only): turns the screen 180
  // degrees for an upside-down mount. Same NVS-sync contract as theme/sfx - the
  // device seeds it before opening and persists on dirty(). Lives in the Display
  // submenu (CUM-188).
  void setScreenFlip(bool on) { screenFlip_ = on; }
  bool screenFlip() const { return screenFlip_; }

  // Whether this board has a physical LED ring. Seeded from solide::board().hasRing
  // before opening; on a ringless board the Customize (Tune) list hides the
  // ring-only params (CUM-187). Default true so a ring board (Solide S3) and the
  // host tests are unaffected.
  void setHasRing(bool on) { hasRing_ = on; }
  bool hasRing() const { return hasRing_; }

  // Settings > Display > Calibrate touch: raises a request the device drains to run
  // the on-device tap-the-crosses calibration (CUM-189). Like the other device-work
  // requests (SD re-probe, forget bonds) it is not Config state, so it never dirty()s.
  bool calibrateRequested() const { return calibrateRequested_; }
  void clearCalibrateRequest() { calibrateRequested_ = false; }

  bool adjustingValue() const {
    return volAdjusting_ || (state_ == State::Edit && adjusting_);
  }

  // Full-screen Self-test / Battery views (same pattern as ConfigQr): launched
  // from their Main rows, the device renders the live results/detail INSTEAD of
  // the menu list until ANY gesture returns to Main. The portable FSM only
  // owns the entry/exit; the device fills the pixels (runs the self-test engine /
  // reads the battery model) since that state isn't knowable to the pure FSM.
  bool showingSelfTest() const { return state_ == State::SelfTest; }
  bool showingBattery() const { return state_ == State::Battery; }

  // Cheap cursor queries. view() reports both, but building a MenuView
  // allocates a vector of strings - far too heavy for a caller that only wants
  // to know where the cursor is, and outright unsafe for one holding a
  // spinlock (the touch tap that steps the cursor onto a row does both).
  int selected() const { return sel_; }
  int rowCount() const { return itemCount(); }

  // Fill a caller-provided MenuView (title + items + selected) that maps
  // directly onto ScreenCtx. visible == isOpen().
  void view(solide::menu::MenuView& out) const;
  solide::menu::MenuView view() const {
    solide::menu::MenuView v;
    view(v);
    return v;
  }

  // One-line description of the param the cursor points at (TuneList rows) or
  // the param being edited (Edit, any row, adjusting or not). "" when no help
  // applies (Back row, every other state) - the renderer hides the help pane
  // on empty. Always printable ASCII (backed by paramDescription()).
  const char* helpText() const;

  // The device drains this to know it must persist the Config/mode and
  // re-render the ring/screen. Set by any mutation (profile, override, reset,
  // mode toggle). clearDirty() is called after a successful persist.
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

 private:
  enum class State : uint8_t {
    Closed, Main, ProfilePick, TuneList, Edit, ConfirmReset, Connectivity,
    ConfigQr, TokenDetail, ThemePick, SelfTest, Battery, Sound, UpdateMenu, ConfirmInstall,
    WifiMenu, WifiPick, WifiForget, Display };

  // Rows on the Main screen, in display order. Sound absorbs the old
  // Sounds/Voice/Volume rows (one submenu for everything audible); Screensaver
  // cycles in place; Software update opens its own submenu.
  // RowDisplay (Display >) opens the Display submenu (screen flip, and touch
  // calibration once it lands); it sits before RowClose. mainRowAt() maps the
  // visible index to the logical row.
  enum MainRow : int {
    RowMode = 0, RowProfile, RowTune, RowConn, RowSound, RowTheme,
    RowSaver, RowUpdate, RowReset, RowSelfTest, RowBattery, RowSdCard,
    RowDisplay, RowClose, kMainRows };

  // Rows in the Sound submenu, in display order. Dictation/Spoken replies cycle
  // the STT/TTS provider (0 Mistral / 1 OpenAI - device maps string<->index).
  enum SoundRow : int {
    SndEffects = 0, SndVoicePack, SndVolume, SndStt, SndTts, SndBack, kSoundRows };

  // Rows in the Connectivity submenu, in display order. ConnToken opens the
  // full-screen recovery token (the compact row cannot show it without clipping).
  // ⚠ Do NOT insert or renumber: main.cpp overlays these rows by literal index
  // and the HIL suite counts taps to reach them, so a shifted row makes
  // both land somewhere else and assert something unrelated - silently. New
  // Wi-Fi affordances hang off the EXISTING ConnWifi row's submenu instead.
  enum ConnRow : int {
    ConnWifi = 0, ConnBluetooth, ConnForget, ConnSdProbe, ConnConfigQr, ConnToken,
    ConnBack, kConnRows };

  // Rows in the Wi-Fi submenu (Connectivity > Wi-Fi), in display order.
  enum WifiRow : int {
    WifiPublishAp = 0, WifiChooseNet, WifiForgetNet, WifiRowBack, kWifiRows };

  // Rows in the Display submenu (Settings > Display), in display order. Groups the
  // screen flip and the touch-calibration entry per the CUM-163 IA.
  enum DispRow : int { DispFlip = 0, DispCalibrate, DispBack, kDispRows };

  int itemCount() const;   // rows in the current list state
  MainRow mainRowAt(int idx) const;  // visible Main index -> logical row (identity)
  // Customize (TuneList) is filtered by hasRing_ (CUM-187): these map between the
  // VISIBLE row index and the underlying Param so the ring-only params can be
  // hidden without the row index and the Param ordinal drifting apart.
  int   visibleParamCount() const;
  Param tuneParamAt(int visibleIdx) const;
  int   visibleIndexOf(Param p) const;
  void clampSel();         // keep sel_ in [0, itemCount()-1]
  void enter(State s);     // switch state, reset cursor to a sane default

  Config* cfg_;
  Mode    mode_ = Mode::Notifier;
  bool    bleEnabled_ = true;   // Connectivity > Bluetooth toggle (NVS-synced)
  int     sfxLevel_ = 0;        // Main > Sounds (active mode's level, NVS-synced)
  int     sfxVoice_ = 0;        // Settings > Sound theme (0 pulse)
  int     sfxVolume_ = 50;      // Main > Volume (master speaker volume 0-100, NVS-synced)
  std::string fwVersion_;       // Main title suffix (device-seeded; "" = none)
  int     theme_ = 0;           // Theme submenu index into themeList() (NVS-synced)
  bool    forgetRequested_ = false;  // Connectivity > Forget paired devices (device drains)
  std::vector<std::string> scan_;    // Wi-Fi > Choose network rows (device-seeded SSIDs)
  std::vector<std::string> known_;   // Wi-Fi > Forget network rows (device-seeded SSIDs)
  std::string pickedSsid_;           // SSID the pending join/forget request names
  bool    publishApRequested_ = false;   // Wi-Fi > Publish setup network (device drains)
  bool    wifiScanRequested_ = false;    // entering the picker asks for a fresh scan
  bool    wifiJoinRequested_ = false;    // a scan row was picked (pickedSsid_)
  bool    wifiForgetRequested_ = false;  // a saved network was picked (pickedSsid_)
  bool    sdProbeRequested_ = false; // Re-probe SD rows (Main SD + Connectivity; device drains)
  std::string sdStatus_;             // Main "SD:" row text (device-seeded live)
  bool    volAdjusting_ = false;     // Sound > Volume row captured rotation (classic adjust)
  uint16_t saverMin_ = 5;            // Main > Screensaver idle minutes (0 = off, NVS-synced)
  bool screenFlip_ = false;          // Settings > Display > Display flip (TFT only, NVS-synced)
  bool hasRing_ = true;              // board has a physical LED ring (CUM-187 hide gate)
  bool calibrateRequested_ = false;  // Settings > Display > Calibrate touch (device drains)
  bool    autoUpdate_ = false;       // Software update > Automatic updates (NVS-synced)
  int     sttProv_ = 0;              // Sound > Dictation (0 Mistral / 1 OpenAI, NVS-synced)
  int     ttsProv_ = 0;              // Sound > Spoken replies (0 Mistral / 1 OpenAI, NVS-synced)
  bool    otaAllowed_ = true;        // false in Notifier mode (BLE owns the update RAM)
  std::string updateStatus_;         // Software update live status line (device-seeded)
  std::string updateVersion_;        // non-empty => an installable version was found
  bool    updateCheckRequested_ = false;    // Software update > Check (device drains)
  bool    updateInstallRequested_ = false;  // ConfirmInstall > Install (device drains)
  State   state_ = State::Closed;
  int     sel_ = 0;        // cursor in the current list
  Param   editing_ = Param::Posture;  // valid only in State::Edit
  bool    adjusting_ = false;         // Edit: value row is capturing rotation
  bool    dirty_ = false;
};

}  // namespace nimbus
