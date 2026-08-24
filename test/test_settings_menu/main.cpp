#include <unity.h>

#include <string>

#include "nimbus/settings_menu.h"

using namespace nimbus;

void setUp() {}
void tearDown() {}

// --- helpers ---------------------------------------------------------------

static solide::menu::MenuView viewOf(const SettingsMenu& m) {
  solide::menu::MenuView v;
  m.view(v);
  return v;
}

static bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// Rotate CW `n` times.
static void cw(SettingsMenu& m, int n) {
  for (int i = 0; i < n; ++i) m.onRotate(+1);
}

// --- lifecycle -------------------------------------------------------------

static void test_open_close_visibility() {
  Config c;
  SettingsMenu m(c);
  TEST_ASSERT_FALSE(m.isOpen());
  TEST_ASSERT_FALSE(viewOf(m).visible);

  m.open();
  TEST_ASSERT_TRUE(m.isOpen());
  auto v = viewOf(m);
  TEST_ASSERT_TRUE(v.visible);
  TEST_ASSERT_EQUAL(13, int(v.items.size()));  // Mode, Profile, Tune, Connectivity, Reset, LED theme, Sounds, Voice, Volume, Self-test, Battery, SD, Close
  TEST_ASSERT_EQUAL(0, v.selected);

  m.close();
  TEST_ASSERT_FALSE(m.isOpen());
}

// Rotation wraps at both ends of the Main list.
static void test_rotate_wraps() {
  Config c;
  SettingsMenu m(c);
  m.open();
  const int n = 13;   // Main rows (incl. Self-test + Battery + SD)

  cw(m, n);  // full loop
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);

  m.onRotate(-1);  // wrap backward past 0 -> last
  TEST_ASSERT_EQUAL(n - 1, viewOf(m).selected);
  m.onRotate(+1);  // wrap forward -> 0
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);
}

// --- descend / ascend ------------------------------------------------------

static void test_click_descends_back_ascends() {
  Config c;
  SettingsMenu m(c);
  m.open();

  // Descend into Tune (row 2).
  cw(m, 2);
  m.onClick();
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL(kParamCount + 1, int(v.items.size()));  // params + Back
  TEST_ASSERT_TRUE(contains(v.title, "Customize"));

  // Back ascends and restores the cursor to the Tune row.
  m.onLongPress();
  v = viewOf(m);
  TEST_ASSERT_EQUAL(13, int(v.items.size()));
  TEST_ASSERT_EQUAL(2, v.selected);

  // Long-press at Main closes.
  m.onLongPress();
  TEST_ASSERT_FALSE(m.isOpen());
}

// The Back row inside a sub-list ascends, restoring the parent cursor.
static void test_back_row_ascends() {
  Config c;
  SettingsMenu m(c);
  m.open();
  cw(m, 1);       // Profile row
  m.onClick();    // -> ProfilePick (cursor starts on current profile)
  const int backRow = kProfileCount;  // profiles occupy 0..kProfileCount-1
  while (viewOf(m).selected != backRow) m.onRotate(+1);
  m.onClick();    // Back -> Main, cursor restored to the Profile row
  auto v = viewOf(m);
  TEST_ASSERT_TRUE(contains(v.title, "Settings"));
  TEST_ASSERT_EQUAL(1, v.selected);
}

// --- profile change --------------------------------------------------------

static void test_profile_change_applies_to_config() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  SettingsMenu m(c);
  m.open();

  // Move to the Profile row (row 1) and open the picker.
  while (viewOf(m).selected != 1) m.onRotate(+1);
  m.onClick();  // -> ProfilePick, cursor on the current profile (Balanced=1)
  TEST_ASSERT_EQUAL(int(ProfileId::Balanced), viewOf(m).selected);

  // Select Desk (index 2).
  while (viewOf(m).selected != int(ProfileId::Desk)) m.onRotate(+1);
  m.onClick();

  TEST_ASSERT_EQUAL(int(ProfileId::Desk), int(c.profile()));
  TEST_ASSERT_TRUE(m.dirty());
  // effective() now reflects Desk presets.
  TEST_ASSERT_EQUAL(presetValue(ProfileId::Desk, Param::RingBrightness),
                    c.effective(Param::RingBrightness));
  // Back at Main on the Profile row.
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);
}

// Switching profile preserves sparse overrides (the whole point of the model).
static void test_profile_switch_preserves_overrides() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  c.setOverride(Param::AttnLedIndex, 22);
  SettingsMenu m(c);
  m.open();
  while (viewOf(m).selected != 1) m.onRotate(+1);
  m.onClick();  // ProfilePick
  while (viewOf(m).selected != int(ProfileId::Desk)) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_TRUE(c.hasOverride(Param::AttnLedIndex));
  TEST_ASSERT_EQUAL(22, c.effective(Param::AttnLedIndex));
}

// --- tuning a param --------------------------------------------------------

// Navigate Main -> Tune -> edit the given param. Returns with cursor on the
// value row of the Edit screen (not yet adjusting).
static void gotoEdit(SettingsMenu& m, Param p) {
  m.open();
  while (viewOf(m).selected != 2) m.onRotate(+1);  // Tune row
  m.onClick();                                     // TuneList
  while (viewOf(m).selected != int(p)) m.onRotate(+1);
  m.onClick();                                     // Edit
}

// gotoEdit + click the value row to enter the adjusting sub-mode.
static void gotoAdjust(SettingsMenu& m, Param p) {
  gotoEdit(m, p);
  m.onClick();  // value row -> adjusting
}

static void test_tune_sets_override_and_dirty() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  SettingsMenu m(c);
  TEST_ASSERT_FALSE(c.hasOverride(Param::RingBrightness));

  gotoAdjust(m, Param::RingBrightness);
  m.clearDirty();  // isolate the edit's dirty flip
  const int32_t before = c.effective(Param::RingBrightness);  // 30

  m.onRotate(+1);  // +step (5)
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_TRUE(c.hasOverride(Param::RingBrightness));
  TEST_ASSERT_EQUAL(before + 5, c.effective(Param::RingBrightness));

  // The Edit screen now shows a Clear override row (3 items).
  TEST_ASSERT_EQUAL(3, int(viewOf(m).items.size()));
}

// Int params clamp at their bounds; no wrap.
static void test_tune_clamps_at_bounds() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  SettingsMenu m(c);
  gotoAdjust(m, Param::RingBrightness);  // Int, max 255, step 5

  for (int i = 0; i < 100; ++i) m.onRotate(+1);  // push past max
  TEST_ASSERT_EQUAL(255, c.effective(Param::RingBrightness));
  for (int i = 0; i < 100; ++i) m.onRotate(-1);  // push past min
  TEST_ASSERT_EQUAL(0, c.effective(Param::RingBrightness));
}

// AttnHue: -1 (auto) is the min sentinel; stepping down from 0 lands on auto.
static void test_attn_hue_auto_sentinel_boundary() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  SettingsMenu m(c);
  gotoAdjust(m, Param::AttnHue);  // starts at -1 (auto)
  TEST_ASSERT_EQUAL(-1, c.effective(Param::AttnHue));
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "Auto"));

  m.onRotate(+1);  // -1 + 8 clamped? -1+8 = 7
  TEST_ASSERT_EQUAL(7, c.effective(Param::AttnHue));
  m.onRotate(-1);  // 7 - 8 = -1 (clamped to min)
  TEST_ASSERT_EQUAL(-1, c.effective(Param::AttnHue));
  m.onRotate(-1);  // already at min, stays
  TEST_ASSERT_EQUAL(-1, c.effective(Param::AttnHue));
}

// Enum params (AttnAnim over ring::Anim 0..5) cycle with wrap.
static void test_enum_param_wraps() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  SettingsMenu m(c);
  gotoAdjust(m, Param::AttnAnim);  // preset 2 (breathe)
  TEST_ASSERT_EQUAL(2, c.effective(Param::AttnAnim));

  for (int i = 0; i < 4; ++i) m.onRotate(+1);  // 2 ->3->4->5->0 (wrap at 5)
  TEST_ASSERT_EQUAL(0, c.effective(Param::AttnAnim));
  m.onRotate(-1);  // 0 -> 5 (wrap)
  TEST_ASSERT_EQUAL(5, c.effective(Param::AttnAnim));
}

// The value row is a two-state control: rotation only edits while "adjusting".
static void test_adjust_submode_gates_rotation() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  SettingsMenu m(c);
  gotoEdit(m, Param::RingBrightness);  // cursor on value row, NOT adjusting

  // Not adjusting: rotation moves the cursor, never the value. With no override
  // yet, Edit has {value, Back} = 2 rows.
  const int32_t v0 = c.effective(Param::RingBrightness);
  m.onRotate(+1);
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);          // moved to Back
  TEST_ASSERT_EQUAL(v0, c.effective(Param::RingBrightness));
  TEST_ASSERT_FALSE(c.hasOverride(Param::RingBrightness));

  // Back to the value row, click to enter adjusting, then rotate edits.
  m.onRotate(-1);
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);
  m.onClick();                                        // enter adjusting
  m.onRotate(+1);
  TEST_ASSERT_EQUAL(v0 + 5, c.effective(Param::RingBrightness));

  // A long-press exits adjusting but stays in Edit (does not ascend).
  m.onLongPress();
  TEST_ASSERT_EQUAL(3, int(viewOf(m).items.size()));  // value, Clear, Back -> still Edit
  // Now rotation moves the cursor again.
  m.onRotate(+1);
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);
}

// Clearing an override from the Edit screen falls back to the preset and drops
// the Clear row without leaving the cursor dangling.
static void test_clear_override_row() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  c.setOverride(Param::RingFps, 55);
  SettingsMenu m(c);
  gotoEdit(m, Param::RingFps);
  TEST_ASSERT_EQUAL(3, int(viewOf(m).items.size()));  // value, Clear, Back

  m.onRotate(+1);                 // move cursor off the value row -> Clear row
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);
  m.onClick();                    // Clear override

  TEST_ASSERT_FALSE(c.hasOverride(Param::RingFps));
  TEST_ASSERT_EQUAL(presetValue(ProfileId::Balanced, Param::RingFps),
                    c.effective(Param::RingFps));
  // Clear row gone -> 2 items, cursor clamped in range.
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL(2, int(v.items.size()));
  TEST_ASSERT_TRUE(v.selected < 2);
}

// --- reset -----------------------------------------------------------------

static void test_reset_clears_all_overrides() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  c.setOverride(Param::RingFps, 5);
  c.setOverride(Param::DwellMs, 1000);
  SettingsMenu m(c);
  m.open();
  while (viewOf(m).selected != 8) m.onRotate(+1);  // Reset row (Main row 8)
  m.onClick();                                     // ConfirmReset
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL(2, int(v.items.size()));
  TEST_ASSERT_EQUAL(0, v.selected);  // defaults to "No"

  m.onRotate(+1);  // -> Yes
  m.onClick();

  TEST_ASSERT_FALSE(c.hasOverride(Param::RingFps));
  TEST_ASSERT_FALSE(c.hasOverride(Param::DwellMs));
  TEST_ASSERT_TRUE(m.dirty());
  // effective values back to presets.
  TEST_ASSERT_EQUAL(presetValue(ProfileId::Balanced, Param::RingFps),
                    c.effective(Param::RingFps));
  // Back at Main.
  TEST_ASSERT_EQUAL(13, int(viewOf(m).items.size()));
}

// The Sounds/Voice rows cycle in place (no submenu) and mark the menu dirty,
// mirroring the Mode-toggle contract: the device persists on dirty().
static void test_sfx_rows_cycle() {
  Config c;
  SettingsMenu m(c);
  m.open();
  m.setSfxLevel(0);
  m.setSfxVoice(0);
  m.clearDirty();
  // The audible rows live in the Sound submenu (Main row 4).
  cw(m, 4);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[4], "Sound >"));
  m.onClick();                                     // -> Sound submenu
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Sound"));
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "Sound effects: Off"));
  m.onClick();
  TEST_ASSERT_EQUAL(1, m.sfxLevel());
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "Low"));
  m.onClick(); m.onClick(); m.onClick();          // Medium -> High -> Off
  TEST_ASSERT_EQUAL(0, m.sfxLevel());
  // Sound theme row: one theme today - clicking cycles in place (stays Pulse),
  // still marks dirty so a future multi-theme cycle keeps this contract.
  cw(m, 1);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[1], "Pulse"));
  m.onClick();
  TEST_ASSERT_EQUAL(0, m.sfxVoice());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[1], "Pulse"));
  m.onClick(); m.onClick();
  TEST_ASSERT_EQUAL(0, m.sfxVoice());
  // Volume row (Sound row 2): CLASSIC adjust (owner 2026-07-13) - click captures
  // rotation (rotate = +/-5, ring gauge echoes), click again releases; the
  // cursor does NOT move while captured.
  cw(m, 1);                                        // Sound theme(1) -> Volume(2)
  m.setSfxVolume(50);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[2], "Volume: 50%"));
  m.onClick();                                     // capture
  TEST_ASSERT_TRUE(m.valueAdjusting());
  TEST_ASSERT_EQUAL(50, m.adjustValuePct());
  m.onRotate(+1); m.onRotate(+1);                  // +10
  TEST_ASSERT_EQUAL(60, m.sfxVolume());
  TEST_ASSERT_EQUAL(2, viewOf(m).selected);        // cursor pinned while captured
  m.onRotate(-1);
  TEST_ASSERT_EQUAL(55, m.sfxVolume());
  TEST_ASSERT_TRUE(m.dirty());
  m.onClick();                                     // release
  TEST_ASSERT_FALSE(m.valueAdjusting());
  m.onRotate(+1);                                  // rotation moves the cursor again
  TEST_ASSERT_EQUAL(3, viewOf(m).selected);
  // Back returns to Main with the cursor on the Sound row.
  m.onLongPress();
  TEST_ASSERT_EQUAL(4, viewOf(m).selected);
}

// The LED-theme submenu: pick a theme, dirty flips, themeSlug() reflects it.
static void test_theme_picker() {
  Config c;
  SettingsMenu m(c);
  m.setTheme(0);   // seed the first theme (teal)
  m.open();
  while (viewOf(m).selected != 5) m.onRotate(+1);  // LED theme row (Mode,Profile,Tune,Conn,Reset,LED theme,Close)
  auto v = viewOf(m);
  TEST_ASSERT_TRUE(v.items[5].rfind("Theme:", 0) == 0);
  m.onClick();     // -> ThemePick
  v = viewOf(m);
  TEST_ASSERT_EQUAL_STRING("Settings > Theme", v.title.c_str());
  TEST_ASSERT_EQUAL(themeCount() + 1, int(v.items.size()));  // themes + Back
  TEST_ASSERT_FALSE(m.dirty());
  m.onRotate(+1); m.onRotate(+1);  // move to theme index 2
  m.onClick();                     // select it
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_EQUAL(2, m.theme());
  TEST_ASSERT_EQUAL_STRING(themeAt(2).c_str(), m.themeSlug().c_str());
  TEST_ASSERT_EQUAL(5, viewOf(m).selected);  // back on Main, cursor on the theme row
}

// Reset "No" leaves overrides untouched.
static void test_reset_no_keeps_overrides() {
  Config c;
  c.setOverride(Param::RingFps, 5);
  SettingsMenu m(c);
  m.open();
  while (viewOf(m).selected != 4) m.onRotate(+1);
  m.onClick();      // ConfirmReset, cursor on No
  m.onClick();      // confirm No
  TEST_ASSERT_TRUE(c.hasOverride(Param::RingFps));
}

// --- mode toggle -----------------------------------------------------------

static void test_mode_toggle() {
  Config c;
  SettingsMenu m(c);
  m.setMode(Mode::Notifier);
  m.open();
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "Notifier"));
  m.onClick();  // toggle Mode (cursor starts at row 0)
  TEST_ASSERT_EQUAL(int(Mode::Orchestrator), int(m.mode()));
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "Orchestrator"));
  // Toggling stays on Main, cursor unchanged.
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);
}

// --- connectivity submenu + config QR --------------------------------------

// Main "Connectivity" (row 3) opens a submenu whose QR and recovery-code rows
// enter honest full-screen destinations: no clipped credential and no chevron
// that does nothing. Both are view-only and any event returns to Connectivity.
static void test_connectivity_and_config_qr() {
  Config c;
  SettingsMenu m(c);
  m.open();
  TEST_ASSERT_FALSE(m.showingConfigQr());

  // Connectivity is Main row 3 (Mode, Power profile, Tune, Connectivity, Reset, Close).
  while (viewOf(m).selected != 3) m.onRotate(+1);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[3], "Connectivity"));
  m.onClick();  // -> Connectivity submenu
  auto v = viewOf(m);
  TEST_ASSERT_TRUE(contains(v.title, "Connectivity"));
  TEST_ASSERT_EQUAL(7, int(v.items.size()));           // WiFi + Bluetooth + Forget + Re-probe SD + Config via QR + Token + Back
  TEST_ASSERT_TRUE(contains(v.items[0], "Wi-Fi"));
  TEST_ASSERT_TRUE(contains(v.items[1], "Bluetooth"));
  TEST_ASSERT_TRUE(contains(v.items[2], "Forget"));
  TEST_ASSERT_TRUE(contains(v.items[3], "Rescan SD card"));
  TEST_ASSERT_TRUE(contains(v.items[4], "Sign-in QR"));
  TEST_ASSERT_TRUE(contains(v.items[5], "Device sign-in code"));   // canonical (CUM-45)
  TEST_ASSERT_TRUE(contains(v.items[5], ">"));             // real full-screen destination

  // Config via QR (row 4) -> ConfigQr full-screen state.
  while (viewOf(m).selected != 4) m.onRotate(+1);          // -> Config via QR
  m.onClick();
  TEST_ASSERT_TRUE(m.showingConfigQr());
  TEST_ASSERT_TRUE(viewOf(m).items.empty());               // device draws the QR
  TEST_ASSERT_FALSE(m.dirty());                             // view-only
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Sign-in QR"));

  // Every event dismisses back to the Connectivity submenu (not Main).
  m.onRotate(+1);
  TEST_ASSERT_FALSE(m.showingConfigQr());
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Connectivity"));
  TEST_ASSERT_EQUAL(4, viewOf(m).selected);                // cursor on Config via QR (row 4)

  m.onClick();  TEST_ASSERT_TRUE(m.showingConfigQr());     // re-enter
  m.onClick();  TEST_ASSERT_FALSE(m.showingConfigQr());    // click dismisses
  m.onClick();  TEST_ASSERT_TRUE(m.showingConfigQr());     // re-enter
  m.onLongPress(); TEST_ASSERT_FALSE(m.showingConfigQr()); // long-press dismisses

  // Token row (5) opens a full-screen exact value, then returns to itself.
  while (viewOf(m).selected != 5) m.onRotate(+1);          // Token
  m.onClick();
  TEST_ASSERT_TRUE(m.showingTokenDetail());
  TEST_ASSERT_TRUE(viewOf(m).items.empty());
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Device sign-in code"));   // canonical (CUM-45)
  TEST_ASSERT_FALSE(m.dirty());
  m.onClick();
  TEST_ASSERT_FALSE(m.showingTokenDetail());
  TEST_ASSERT_EQUAL(5, viewOf(m).selected);

  m.onClick(); TEST_ASSERT_TRUE(m.showingTokenDetail());
  m.onRotate(+1); TEST_ASSERT_FALSE(m.showingTokenDetail());
  TEST_ASSERT_EQUAL(5, viewOf(m).selected);

  m.onClick(); TEST_ASSERT_TRUE(m.showingTokenDetail());
  m.onLongPress(); TEST_ASSERT_FALSE(m.showingTokenDetail());
  TEST_ASSERT_EQUAL(5, viewOf(m).selected);

  // Back row (6) returns to Main on the Connectivity row.
  while (viewOf(m).selected != 6) m.onRotate(+1);          // Back
  m.onClick();
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Settings"));
  TEST_ASSERT_EQUAL(3, viewOf(m).selected);                // Connectivity row
}

// The Connectivity > Bluetooth row toggles bleEnabled() (menu-visible, device
// applies it live + persists), dirties, shows the on/off label, and stays put so
// the flip is visible. It never enters the ConfigQr state.
static void test_connectivity_bluetooth_toggle() {
  Config c;
  SettingsMenu m(c);
  m.setBleEnabled(true);
  m.open();
  while (viewOf(m).selected != 3) m.onRotate(+1);          // Connectivity
  m.onClick();                                             // -> submenu, cursor on WiFi (0)
  m.onRotate(+1);                                          // WiFi -> Bluetooth (row 1)
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[1], "Bluetooth: On"));

  m.onClick();                                             // toggle -> off
  TEST_ASSERT_FALSE(m.bleEnabled());
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_TRUE(m.showingConnectivity());               // stayed in the submenu
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);                // stayed on the Bluetooth row
  TEST_ASSERT_TRUE(contains(viewOf(m).items[1], "Bluetooth: Off"));

  m.clearDirty();
  m.onClick();                                             // toggle -> on
  TEST_ASSERT_TRUE(m.bleEnabled());
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[1], "Bluetooth: On"));
}

// Connectivity > Forget paired devices (row 1) raises the device-drained
// forgetBondsRequested() signal, stays on the row (so the new count is visible),
// and does NOT dirty the config (bonds live in NimBLE's NVS, not the Config).
static void test_connectivity_forget_paired() {
  Config c;
  SettingsMenu m(c);
  m.open();
  while (viewOf(m).selected != 3) m.onRotate(+1);          // Connectivity
  m.onClick();                                             // -> submenu, cursor on WiFi (0)
  m.onRotate(+1);                                          // WiFi -> Bluetooth (row 1)
  m.onRotate(+1);                                          // Bluetooth -> Forget (row 2)
  TEST_ASSERT_EQUAL(2, viewOf(m).selected);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[2], "Forget"));
  TEST_ASSERT_TRUE(m.onForgetRow());
  TEST_ASSERT_FALSE(m.forgetBondsRequested());

  m.onClick();                                             // request forget
  TEST_ASSERT_TRUE(m.forgetBondsRequested());
  TEST_ASSERT_FALSE(m.dirty());                            // bonds aren't Config state
  TEST_ASSERT_TRUE(m.showingConnectivity());               // stayed in the submenu
  TEST_ASSERT_EQUAL(2, viewOf(m).selected);                // stayed on the Forget row

  m.clearForgetRequest();                                  // device drained it
  TEST_ASSERT_FALSE(m.forgetBondsRequested());
}

// Connectivity > Re-probe SD (row 3) raises the device-drained sdProbeRequested()
// signal (device calls memory::promoteSd() - SD.end()+begin()), stays on the row
// (so the new card state is visible), and does NOT dirty the config.
static void test_connectivity_sd_reprobe() {
  Config c;
  SettingsMenu m(c);
  m.open();
  while (viewOf(m).selected != 3) m.onRotate(+1);          // Connectivity
  m.onClick();                                             // -> submenu, cursor on WiFi (0)
  while (viewOf(m).selected != 3) m.onRotate(+1);          // -> Re-probe SD (row 3)
  TEST_ASSERT_TRUE(contains(viewOf(m).items[3], "Rescan SD card"));
  TEST_ASSERT_TRUE(m.onSdProbeRow());
  TEST_ASSERT_FALSE(m.sdProbeRequested());

  m.onClick();                                             // request re-probe
  TEST_ASSERT_TRUE(m.sdProbeRequested());
  TEST_ASSERT_FALSE(m.dirty());                            // SD mount isn't Config state
  TEST_ASSERT_TRUE(m.showingConnectivity());               // stayed in the submenu
  TEST_ASSERT_EQUAL(3, viewOf(m).selected);                // stayed on the Re-probe SD row

  m.clearSdProbeRequest();                                 // device drained it
  TEST_ASSERT_FALSE(m.sdProbeRequested());
}

// --- Wi-Fi: the on-device escape hatch -------------------------------------

// Main -> Connectivity -> click the Wi-Fi row (ConnWifi, index 0). Leaves the
// menu on the Wi-Fi submenu with the cursor on "Publish setup network".
static void gotoWifi(SettingsMenu& m) {
  m.open();
  while (viewOf(m).selected != 3) m.onRotate(+1);   // Connectivity
  m.onClick();                                     // -> submenu, cursor on Wi-Fi (0)
  m.onClick();                                     // -> Wi-Fi submenu
}

// THE bug this feature fixes: Connectivity > Wi-Fi was a read-only row whose
// click did nothing, so a device that could not join its network could only be
// recovered over USB. The row now opens a submenu - as a plain list on the
// EXISTING menu screen, not a new full-screen ScreenId (that number is mirrored
// positionally by the HIL harness).
static void test_wifi_row_opens_the_wifi_screen() {
  Config c;
  SettingsMenu m(c);
  m.open();
  while (viewOf(m).selected != 3) m.onRotate(+1);          // Connectivity
  m.onClick();
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);                // cursor starts on Wi-Fi
  TEST_ASSERT_TRUE(m.onWifiRow());
  TEST_ASSERT_FALSE(m.showingWifi());

  m.onClick();                                             // was a no-op; now descends
  TEST_ASSERT_TRUE(m.showingWifi());
  TEST_ASSERT_FALSE(m.showingConnectivity());
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL_STRING("Settings > Connectivity > Wi-Fi", v.title.c_str());
  TEST_ASSERT_EQUAL(4, int(v.items.size()));               // publish/choose/forget/Back
  TEST_ASSERT_EQUAL(0, v.selected);
  TEST_ASSERT_FALSE(m.dirty());                            // opening a screen changes nothing
  // A list screen, not one of the full-screen states.
  TEST_ASSERT_FALSE(m.showingConfigQr());
  TEST_ASSERT_FALSE(m.showingSelfTest());
  TEST_ASSERT_FALSE(m.showingBattery());

  m.onLongPress();                                         // back one level
  TEST_ASSERT_TRUE(m.showingConnectivity());
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);                // cursor restored to Wi-Fi

  m.onClick();                                             // re-enter
  while (viewOf(m).selected != 3) m.onRotate(+1);          // the Back row
  m.onClick();
  TEST_ASSERT_TRUE(m.showingConnectivity());
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);
}

// Copy is a contract on this surface: printable ASCII (the e-ink 5x7 font is
// 32-126), <=48 chars a line, "Wi-Fi" never "WiFi", sentence case.
static void test_wifi_menu_rows_wording() {
  Config c;
  SettingsMenu m(c);
  gotoWifi(m);
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL(4, int(v.items.size()));
  TEST_ASSERT_EQUAL_STRING("Publish setup network", v.items[0].c_str());
  TEST_ASSERT_EQUAL_STRING("Choose network >", v.items[1].c_str());
  TEST_ASSERT_EQUAL_STRING("Forget network >", v.items[2].c_str());
  TEST_ASSERT_EQUAL_STRING("< Back", v.items[3].c_str());
  TEST_ASSERT_TRUE(contains(v.title, "Wi-Fi"));
  TEST_ASSERT_FALSE(contains(v.title, "WiFi"));
  TEST_ASSERT_TRUE(v.title.size() <= 48);
  for (const std::string& it : v.items) {
    TEST_ASSERT_FALSE(contains(it, "WiFi"));
    TEST_ASSERT_TRUE(it.size() <= 48);
    for (unsigned char ch : it) TEST_ASSERT_TRUE(ch >= 32 && ch <= 126);
  }
  // Each action row explains itself; the Back row shows no pane.
  TEST_ASSERT_FALSE(std::string(m.helpText()).empty());
  m.onRotate(+1);
  TEST_ASSERT_FALSE(std::string(m.helpText()).empty());
  m.onRotate(+1);
  TEST_ASSERT_FALSE(std::string(m.helpText()).empty());
  m.onRotate(+1);
  TEST_ASSERT_EQUAL_STRING("", m.helpText());

  // Both picker breadcrumbs stay inside the title budget too.
  m.onRotate(+1); m.onRotate(+1);                          // wrap to Choose network
  m.onClick();
  TEST_ASSERT_EQUAL_STRING("Settings > Connectivity > Wi-Fi > Choose network",
                           viewOf(m).title.c_str());
  TEST_ASSERT_TRUE(viewOf(m).title.size() <= 48);
  m.onLongPress();
  m.onRotate(+1);                                          // -> Forget network
  m.onClick();
  TEST_ASSERT_EQUAL_STRING("Settings > Connectivity > Wi-Fi > Forget network",
                           viewOf(m).title.c_str());
  TEST_ASSERT_TRUE(viewOf(m).title.size() <= 48);
}

// "Publish setup network" raises the device-drained publishApRequested() signal
// (the device brings the setup AP up), stays on the row so the new status is
// visible, and does NOT dirty the config - the radio is not Config state.
// Mirrors test_connectivity_forget_paired exactly.
static void test_wifi_publish_ap_raises_request_and_shows_no_dirty() {
  Config c;
  SettingsMenu m(c);
  gotoWifi(m);
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "Publish setup network"));
  TEST_ASSERT_FALSE(m.publishApRequested());
  m.clearDirty();

  m.onClick();                                             // request publish
  TEST_ASSERT_TRUE(m.publishApRequested());
  TEST_ASSERT_FALSE(m.dirty());                            // not a config change
  TEST_ASSERT_TRUE(m.showingWifi());                       // stayed on the screen
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);                // and on the row

  m.clearPublishApRequest();                               // device drained it
  TEST_ASSERT_FALSE(m.publishApRequested());
  m.clearPublishApRequest();                               // idempotent
  TEST_ASSERT_FALSE(m.publishApRequested());

  m.onClick();                                             // a second attempt works
  TEST_ASSERT_TRUE(m.publishApRequested());
  TEST_ASSERT_FALSE(m.dirty());
}

// The picker is a list on the same menu screen: rotation walks it (well past the
// panel's ~6-9 row window - the RENDERER owns the scroll window, the FSM emits
// every row) and never leaves. Only Back or a long-press exits.
static void test_wifi_picker_scrolls_and_does_not_exit_on_rotate() {
  Config c;
  SettingsMenu m(c);
  m.setWifiScan({"net-a", "net-b", "net-c", "net-d",
                 "net-e", "net-f", "net-g", "net-h"});
  gotoWifi(m);
  m.onRotate(+1);                                          // -> Choose network
  TEST_ASSERT_FALSE(m.wifiScanRequested());
  m.onClick();
  TEST_ASSERT_TRUE(m.wifiScanRequested());                 // device drains -> real scan
  m.clearWifiScanRequest();
  TEST_ASSERT_TRUE(m.showingWifiPicker());
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL(9, int(v.items.size()));               // 8 networks + Back
  TEST_ASSERT_EQUAL(0, v.selected);

  for (int i = 1; i < 9; ++i) {
    m.onRotate(+1);
    TEST_ASSERT_TRUE(m.showingWifiPicker());               // still on the picker
    TEST_ASSERT_EQUAL(i, viewOf(m).selected);              // and it scrolled
    TEST_ASSERT_FALSE(m.wifiJoinRequested());              // rotation never joins
  }
  m.onRotate(+1);                                          // wraps forward
  TEST_ASSERT_TRUE(m.showingWifiPicker());
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);
  m.onRotate(-1);                                          // and backward
  TEST_ASSERT_TRUE(m.showingWifiPicker());
  TEST_ASSERT_EQUAL(8, viewOf(m).selected);

  m.onLongPress();                                         // the way out
  TEST_ASSERT_FALSE(m.showingWifiPicker());
  TEST_ASSERT_TRUE(m.showingWifi());
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);                // cursor on Choose network
}

// Picking a row raises the join request naming THAT network - the SSID the
// device hands to the radio, not the row above or below it.
static void test_wifi_pick_raises_join_with_the_right_ssid() {
  Config c;
  SettingsMenu m(c);
  m.setWifiScan({"cafe-guest", "TestNet", "neighbour-5g"});
  gotoWifi(m);
  m.onRotate(+1); m.onClick();                             // -> picker
  m.clearWifiScanRequest();
  m.onRotate(+1);                                          // -> TestNet (index 1)
  TEST_ASSERT_TRUE(contains(viewOf(m).items[1], "TestNet"));
  TEST_ASSERT_FALSE(m.wifiJoinRequested());
  m.clearDirty();

  m.onClick();
  TEST_ASSERT_TRUE(m.wifiJoinRequested());
  TEST_ASSERT_EQUAL_STRING("TestNet", m.wifiPickedSsid().c_str());
  TEST_ASSERT_FALSE(m.wifiForgetRequested());              // one click, one request
  TEST_ASSERT_FALSE(m.dirty());                            // credentials aren't Config state
  TEST_ASSERT_TRUE(m.showingWifi());                       // join under way -> status screen
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);                // cursor on Choose network
  m.clearWifiJoinRequest();                                // device drained it
  TEST_ASSERT_FALSE(m.wifiJoinRequested());

  // The LAST network is pickable - the Back row sits after it, not on it.
  m.onClick(); m.clearWifiScanRequest();                   // -> picker again
  while (viewOf(m).selected != 2) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_TRUE(m.wifiJoinRequested());
  TEST_ASSERT_EQUAL_STRING("neighbour-5g", m.wifiPickedSsid().c_str());
}

// Nothing in range is the case that MUST still have a way out: the picker keeps
// its Back row (never an empty screen the knob cannot leave) and the help pane
// says what to do instead.
static void test_wifi_pick_empty_shows_back_only() {
  Config c;
  SettingsMenu m(c);
  gotoWifi(m);                                             // no scan seeded
  m.onRotate(+1); m.onClick();                             // -> picker
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL(1, int(v.items.size()));
  TEST_ASSERT_EQUAL_STRING("< Back", v.items[0].c_str());
  TEST_ASSERT_EQUAL(0, v.selected);
  TEST_ASSERT_TRUE(contains(m.helpText(), "setup network"));

  m.onRotate(+1);                                          // harmless with one row
  TEST_ASSERT_TRUE(m.showingWifiPicker());
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);

  m.onClick();                                             // Back is a real exit
  TEST_ASSERT_FALSE(m.wifiJoinRequested());
  TEST_ASSERT_TRUE(m.wifiPickedSsid().empty());
  TEST_ASSERT_TRUE(m.showingWifi());
  TEST_ASSERT_EQUAL(1, viewOf(m).selected);

  // Same for an empty saved-network list.
  m.onRotate(+1); m.onClick();                             // -> Forget network
  v = viewOf(m);
  TEST_ASSERT_EQUAL(1, int(v.items.size()));
  TEST_ASSERT_EQUAL_STRING("< Back", v.items[0].c_str());
  m.onClick();
  TEST_ASSERT_FALSE(m.wifiForgetRequested());
  TEST_ASSERT_TRUE(m.showingWifi());
  TEST_ASSERT_EQUAL(2, viewOf(m).selected);
}

// Forget names the network it was pointing at, stays in the list (forgetting is
// often plural), and survives the device re-seeding a shorter list underneath it.
static void test_wifi_forget_raises_forget_with_ssid() {
  Config c;
  SettingsMenu m(c);
  m.setWifiKnown({"TestNet", "hotel-wifi"});
  gotoWifi(m);
  m.onRotate(+1); m.onRotate(+1);                          // -> Forget network
  TEST_ASSERT_TRUE(contains(viewOf(m).items[2], "Forget network >"));
  m.onClick();
  TEST_ASSERT_TRUE(m.showingWifiPicker());
  TEST_ASSERT_FALSE(m.showingWifi());
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL_STRING("Settings > Connectivity > Wi-Fi > Forget network",
                           v.title.c_str());
  TEST_ASSERT_EQUAL(3, int(v.items.size()));               // 2 saved + Back
  TEST_ASSERT_FALSE(m.wifiScanRequested());                // forgetting needs no radio
  m.onRotate(+1);                                          // -> hotel-wifi (index 1)
  m.clearDirty();

  m.onClick();
  TEST_ASSERT_TRUE(m.wifiForgetRequested());
  TEST_ASSERT_EQUAL_STRING("hotel-wifi", m.wifiPickedSsid().c_str());
  TEST_ASSERT_FALSE(m.wifiJoinRequested());                // forgetting is not joining
  TEST_ASSERT_FALSE(m.dirty());
  TEST_ASSERT_TRUE(m.showingWifiPicker());                 // stays so the list can shrink
  m.clearWifiForgetRequest();
  TEST_ASSERT_FALSE(m.wifiForgetRequested());

  m.setWifiKnown({"TestNet"});                        // device re-seeded after draining
  v = viewOf(m);
  TEST_ASSERT_EQUAL(2, int(v.items.size()));
  TEST_ASSERT_TRUE(v.selected < 2);                        // cursor survived the row vanishing

  m.onLongPress();
  TEST_ASSERT_TRUE(m.showingWifi());
  TEST_ASSERT_EQUAL(2, viewOf(m).selected);                // cursor on Forget network
}

// --- view() tracking -------------------------------------------------------

static void test_view_marks_overrides_and_selected() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  c.setOverride(Param::RingBrightness, 99);
  SettingsMenu m(c);
  m.open();
  while (viewOf(m).selected != 2) m.onRotate(+1);
  m.onClick();  // TuneList
  auto v = viewOf(m);
  // RingBrightness row carries the '*' override marker and its value.
  TEST_ASSERT_TRUE(contains(v.items[int(Param::RingBrightness)], "*"));
  TEST_ASSERT_TRUE(contains(v.items[int(Param::RingBrightness)], "99"));
  // A non-overridden row has no marker.
  TEST_ASSERT_FALSE(contains(v.items[int(Param::DwellMs)], "*"));
  // selected mirrors the FSM cursor.
  TEST_ASSERT_EQUAL(v.selected, viewOf(m).selected);
}

// --- breadcrumb titles -------------------------------------------------------

// Every screen titles itself with its full breadcrumb path (ASCII '>'
// separators - the e-ink font is 32-126 only) so the user always knows where
// they are. Exact-match per state.
static void test_titles_are_breadcrumb_paths() {
  Config c;
  SettingsMenu m(c);
  m.open();
  TEST_ASSERT_EQUAL_STRING("Settings", viewOf(m).title.c_str());

  cw(m, 1); m.onClick();  // -> ProfilePick
  TEST_ASSERT_EQUAL_STRING("Settings > Battery mode", viewOf(m).title.c_str());
  m.onLongPress();        // back to Main, cursor on the Power profile row (1)

  cw(m, 1); m.onClick();  // row 2 -> TuneList
  TEST_ASSERT_EQUAL_STRING("Settings > Customize", viewOf(m).title.c_str());

  m.onClick();            // param 0 -> Edit
  TEST_ASSERT_EQUAL_STRING("Settings > Customize > Ring level", viewOf(m).title.c_str());
  m.onLongPress();        // back to TuneList

  // Edit title carries the edited param's name; check a long one too.
  while (viewOf(m).selected != int(Param::TelemetryPeriodS)) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_EQUAL_STRING("Settings > Customize > On-screen status refresh",
                           viewOf(m).title.c_str());
  m.onLongPress();        // back to TuneList
  m.onLongPress();        // back to Main, cursor on the Tune row (2)

  cw(m, 1); m.onClick();  // row 3 -> Connectivity submenu (cursor on WiFi, row 0)
  TEST_ASSERT_EQUAL_STRING("Settings > Connectivity", viewOf(m).title.c_str());
  cw(m, 4); m.onClick();  // Config via QR (row 4) -> ConfigQr full-screen state
  TEST_ASSERT_EQUAL_STRING("Settings > Connectivity > Sign-in QR",
                           viewOf(m).title.c_str());
  m.onLongPress();        // back to Connectivity
  m.onLongPress();        // back to Main, cursor on the Connectivity row (3)

  cw(m, 1); m.onClick();  // row 4 -> Sound submenu
  TEST_ASSERT_EQUAL_STRING("Settings > Sound", viewOf(m).title.c_str());
  m.onLongPress();        // back to Main, cursor on the Sound row (4)

  while (viewOf(m).selected != 7) m.onRotate(+1);
  m.onClick();            // row 7 -> Software update
  TEST_ASSERT_EQUAL_STRING("Settings > Software update", viewOf(m).title.c_str());
  m.onLongPress();        // back to Main, cursor on the Software update row (7)

  cw(m, 1); m.onClick();  // row 8 -> ConfirmReset
  TEST_ASSERT_EQUAL_STRING("Settings > Reset to defaults?", viewOf(m).title.c_str());
}

// The Main row says "Battery mode" with the DISPLAY label (Dark/Balanced/Full),
// never the machine key (battery_saver/balanced/desk).
static void test_main_row_says_power_profile() {
  Config c;
  c.setProfile(ProfileId::Balanced);
  SettingsMenu m(c);
  m.open();
  TEST_ASSERT_EQUAL_STRING("Battery mode: Balanced", viewOf(m).items[1].c_str());
}

// --- parity rows (Screensaver / Software update / voice providers) -----------

// The Screensaver row cycles the bucket table Off -> 15 min -> 30 min -> 1 hr
// -> 2 hr -> 4 hr -> Off, dirtying on every step; an in-between value seeded by
// the console snaps to the next bucket up on the first click.
static void test_saver_cycle_and_snap() {
  Config c;
  SettingsMenu m(c);
  m.setSaverMinutes(0);
  m.open();
  while (viewOf(m).selected != 6) m.onRotate(+1);  // Screensaver row
  TEST_ASSERT_TRUE(contains(viewOf(m).items[6], "Screensaver: Off"));
  m.clearDirty();
  m.onClick();
  TEST_ASSERT_EQUAL(15, m.saverMinutes());
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[6], "15 min"));
  m.onClick(); m.onClick();
  TEST_ASSERT_EQUAL(60, m.saverMinutes());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[6], "1 hr"));
  m.onClick(); m.onClick();
  TEST_ASSERT_EQUAL(240, m.saverMinutes());
  m.onClick();                                     // wraps to Off
  TEST_ASSERT_EQUAL(0, m.saverMinutes());
  // A console-set 45 shows as raw minutes and snaps up to 60 on click.
  m.setSaverMinutes(45);
  TEST_ASSERT_TRUE(contains(viewOf(m).items[6], "45 min"));
  m.onClick();
  TEST_ASSERT_EQUAL(60, m.saverMinutes());
}

// Sound > Dictation / Spoken replies cycle Mistral <-> OpenAI and dirty.
static void test_provider_rows_cycle() {
  Config c;
  SettingsMenu m(c);
  m.setSttProvider(0);
  m.setTtsProvider(1);
  m.open();
  while (viewOf(m).selected != 4) m.onRotate(+1);
  m.onClick();                                     // -> Sound
  while (viewOf(m).selected != 3) m.onRotate(+1);  // -> Dictation row
  TEST_ASSERT_TRUE(contains(viewOf(m).items[3], "Dictation: Mistral"));
  TEST_ASSERT_TRUE(contains(viewOf(m).items[4], "Spoken replies: OpenAI"));
  m.clearDirty();
  m.onClick();
  TEST_ASSERT_EQUAL(1, m.sttProvider());
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[3], "Dictation: OpenAI"));
  m.onRotate(+1);                                  // -> Spoken replies
  m.onClick();
  TEST_ASSERT_EQUAL(0, m.ttsProvider());
  TEST_ASSERT_TRUE(contains(viewOf(m).items[4], "Spoken replies: Mistral"));
}

// Software update: the auto toggle dirties; Check raises a device-drained
// request WITHOUT dirtying; the Install row exists only when a version is
// available; ConfirmInstall defaults to Cancel and the confirm closes the menu.
static void test_update_menu_flow() {
  Config c;
  SettingsMenu m(c);
  m.setOtaAllowed(true);
  m.open();
  while (viewOf(m).selected != 7) m.onRotate(+1);
  m.onClick();                                     // -> Software update
  TEST_ASSERT_EQUAL(3, int(viewOf(m).items.size()));  // Auto, Check, Back (no Install)
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "Automatic updates: Off"));
  m.clearDirty();
  m.onClick();                                     // toggle auto
  TEST_ASSERT_TRUE(m.autoUpdate());
  TEST_ASSERT_TRUE(m.dirty());
  m.clearDirty();
  m.onRotate(+1);                                  // -> Check for updates
  TEST_ASSERT_FALSE(m.updateCheckRequested());
  m.onClick();
  TEST_ASSERT_TRUE(m.updateCheckRequested());      // device drains it
  TEST_ASSERT_FALSE(m.dirty());                    // a check is not config state
  m.clearUpdateCheckRequest();

  m.setUpdateAvailable("v9.9.9");                  // a check found one
  TEST_ASSERT_EQUAL(4, int(viewOf(m).items.size()));
  TEST_ASSERT_TRUE(contains(viewOf(m).items[2], "Install v9.9.9"));
  m.onRotate(+1);                                  // -> Install row
  m.onClick();                                     // -> ConfirmInstall
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Install v9.9.9?"));
  TEST_ASSERT_EQUAL(0, viewOf(m).selected);        // defaults to Cancel
  m.onClick();                                     // Cancel -> back to UpdateMenu
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Software update"));
  TEST_ASSERT_FALSE(m.updateInstallRequested());
  TEST_ASSERT_EQUAL(2, viewOf(m).selected);        // cursor restored to Install
  m.onClick();                                     // -> ConfirmInstall again
  m.onRotate(+1); m.onClick();                     // "Install and restart"
  TEST_ASSERT_TRUE(m.updateInstallRequested());
  TEST_ASSERT_FALSE(m.isOpen());                   // menu closed; OTA UX owns the screen
}

// In Notifier mode (otaAllowed=false) Check renders "(unavailable)", clicking
// it is a no-op, the Install row never appears, and the help pane explains why.
static void test_update_unavailable_in_notifier() {
  Config c;
  SettingsMenu m(c);
  m.setOtaAllowed(false);
  m.setUpdateAvailable("v9.9.9");                  // even with a version known
  m.open();
  while (viewOf(m).selected != 7) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_EQUAL(3, int(viewOf(m).items.size()));  // no Install row
  TEST_ASSERT_TRUE(contains(viewOf(m).items[1], "(unavailable)"));
  m.onRotate(+1);                                  // -> Check row
  m.onClick();                                     // no-op
  TEST_ASSERT_FALSE(m.updateCheckRequested());
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Software update"));  // still here
  TEST_ASSERT_TRUE(contains(m.helpText(), "Orchestrator"));
}

// --- help pane ---------------------------------------------------------------

// helpText() feeds the renderer's help pane: the pointed-at param's description
// in TuneList, the edited param's in Edit (any row), and "" everywhere else.
static void test_help_text_per_state() {
  Config c;
  SettingsMenu m(c);
  TEST_ASSERT_EQUAL_STRING("", m.helpText());  // Closed
  m.open();                                    // Main, cursor on row 0 (Mode)
  // Main now clarifies the ambiguous top-level rows (Mode / Power profile /
  // Sounds); the other rows still show no pane.
  TEST_ASSERT_FALSE(std::string(m.helpText()).empty());  // row 0 Mode: has help
  cw(m, 1);                                    // row 1 Power profile
  TEST_ASSERT_FALSE(std::string(m.helpText()).empty());  //   has help
  cw(m, 1);                                    // row 2 Tune
  TEST_ASSERT_EQUAL_STRING("", m.helpText());  //   no pane
  cw(m, 4);                                    // row 6 Sounds
  TEST_ASSERT_FALSE(std::string(m.helpText()).empty());  //   has help
  cw(m, 7);                                    // wrap back to row 0 (Mode; 13 rows now)

  cw(m, 1); m.onClick();                       // ProfilePick (from row 1)
  TEST_ASSERT_FALSE(std::string(m.helpText()).empty());  // per-profile description
  m.onLongPress();

  cw(m, 1); m.onClick();                       // TuneList, cursor on param 0
  for (int i = 0; i < kParamCount; ++i) {      // every param row describes itself
    TEST_ASSERT_EQUAL_STRING(paramDescription(Param(i)), m.helpText());
    m.onRotate(+1);
  }
  TEST_ASSERT_EQUAL_STRING("", m.helpText());  // Back row: no pane

  m.onRotate(+1);                              // wrap to param 0 (posture)
  m.onClick();                                 // -> Edit, cursor on the value row
  TEST_ASSERT_EQUAL_STRING(paramDescription(Param::Posture), m.helpText());
  m.onRotate(+1);                              // Back row: Edit still shows help
  TEST_ASSERT_EQUAL_STRING(paramDescription(Param::Posture), m.helpText());
  m.onRotate(-1);
  m.onClick();                                 // adjusting: still shows help
  TEST_ASSERT_EQUAL_STRING(paramDescription(Param::Posture), m.helpText());
  m.onLongPress();                             // leave adjusting
  m.onLongPress();                             // back to TuneList
  m.onLongPress();                             // back to Main
  TEST_ASSERT_EQUAL_STRING("", m.helpText());

  cw(m, 1); m.onClick();                       // Connectivity submenu (row 3), cursor on Wi-Fi
  TEST_ASSERT_EQUAL_STRING(                     // Wi-Fi row: static help fallback
      "Wi-Fi status and this device's IP address. Click to join, "
      "forget, or publish the setup network.",
      m.helpText());
  m.onRotate(+1);                              // Bluetooth row: static help fallback
  TEST_ASSERT_EQUAL_STRING(
      "Lets the nimbus-notify broker on your computer send session "
      "status to this device.",
      m.helpText());
  m.onRotate(+1);                              // Forget row: static help fallback
  TEST_ASSERT_EQUAL_STRING(
      "Unpairs every bonded computer. Each re-pairs by itself the "
      "next time it connects.",
      m.helpText());
  m.onRotate(+1);                              // Re-probe SD row: static help fallback
  TEST_ASSERT_EQUAL_STRING(
      "Remounts a reseated card without restarting.",
      m.helpText());
  m.onRotate(+1);                              // Config via QR row: no pane
  TEST_ASSERT_EQUAL_STRING("", m.helpText());
  m.onLongPress();                             // back to Main, cursor on Connectivity (row 3)

  while (viewOf(m).selected != 8) m.onRotate(+1);
  m.onClick();                                 // ConfirmReset (row 8)
  TEST_ASSERT_EQUAL_STRING("", m.helpText());
}

// Every string the menu ever shows must be printable ASCII (32-126): the e-ink
// 5x7 font renders anything else as '?', so one UTF-8 arrow becomes three '?'s
// on the physical panel (observed live - the "many ???" bug). Walk EVERY menu
// state (main, profile pick, tune list, every param's edit incl. adjusting,
// confirm) and assert every title/item character is in range.
static void assertAsciiView(SettingsMenu& m, const char* where) {
  solide::menu::MenuView v = viewOf(m);
  for (unsigned char c : v.title)
    TEST_ASSERT_TRUE_MESSAGE(c >= 32 && c <= 126, where);
  for (const std::string& item : v.items)
    for (unsigned char c : item)
      TEST_ASSERT_TRUE_MESSAGE(c >= 32 && c <= 126, where);
}

// The device seeds NIMBUS_FW_VERSION into the Main title band; unset keeps the
// plain "Settings" so every other test (and a version-less host build) is unchanged.
static void test_fw_version_in_main_title() {
  Config c;
  SettingsMenu m(c);
  m.open();
  TEST_ASSERT_EQUAL_STRING("Settings", viewOf(m).title.c_str());
  m.setFwVersion("v2.0.0");
  TEST_ASSERT_EQUAL_STRING("Settings  v2.0.0", viewOf(m).title.c_str());
  for (unsigned char ch : viewOf(m).title)
    TEST_ASSERT_TRUE(ch >= 32 && ch <= 126);   // e-ink 5x7 font: printable ASCII only
  // Submenu breadcrumbs stay unversioned (band width is the deep path's budget).
  while (viewOf(m).selected != 3) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_EQUAL_STRING("Settings > Connectivity", viewOf(m).title.c_str());
}

static void test_all_views_are_printable_ascii() {
  Config c;
  SettingsMenu m(c);
  m.open();
  assertAsciiView(m, "main");

  m.onRotate(+1); m.onClick();  // Profile picker
  assertAsciiView(m, "profile-pick");
  m.onLongPress();              // back to main

  m.onRotate(+1); m.onClick();  // Tune list (main row 2)
  assertAsciiView(m, "tune-list");
  for (int i = 0; i < kParamCount; ++i) {
    m.onClick();                // enter Edit for the selected param
    assertAsciiView(m, "edit");
    m.onClick();                // enter adjusting
    m.onRotate(+1);             // change value (adds the override marker row)
    assertAsciiView(m, "edit-adjusting");
    m.onClick();                // leave adjusting
    m.onLongPress();            // back to tune list
    assertAsciiView(m, "tune-list-after-edit");
    m.onRotate(+1);             // next param
  }
  m.onLongPress();              // back to main, cursor on Tune (row 2)

  m.onRotate(+1); m.onClick();  // Connectivity submenu (main row 3), cursor on Wi-Fi
  assertAsciiView(m, "connectivity");

  // Wi-Fi escape hatch. An SSID is arbitrary bytes broadcast by someone else's
  // router, so seed UTF-8 (and an over-long) name and prove the FSM sanitises
  // + clips it - every consumer of view() must be safe, not just the e-ink
  // renderer. The RAW bytes still have to reach the radio, so the picked SSID
  // is checked against the original.
  m.setWifiScan({"caf\xC3\xA9-guest", "\xE2\x9C\x93 tick net",
                 "wide-open-network-with-a-very-long-name-that-runs-past-the-panel"});
  m.setWifiKnown({"h\xC3\xB4tel-wifi"});
  m.onClick();                  // Wi-Fi row (0) -> Wi-Fi submenu
  assertAsciiView(m, "wifi-menu");
  m.onRotate(+1); m.onClick();  // Choose network -> picker
  assertAsciiView(m, "wifi-pick");
  TEST_ASSERT_TRUE(contains(viewOf(m).items[0], "caf??-guest"));  // 2 UTF-8 bytes -> 2 '?'
  for (const std::string& it : viewOf(m).items) TEST_ASSERT_TRUE(it.size() <= 48);
  m.onClick();                  // pick row 0 -> join request, back on the Wi-Fi menu
  TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9-guest", m.wifiPickedSsid().c_str());
  m.clearWifiJoinRequest();
  m.onRotate(+1); m.onClick();  // Forget network -> saved list
  assertAsciiView(m, "wifi-forget");
  m.onLongPress();              // back to the Wi-Fi submenu
  m.onLongPress();              // back to Connectivity, cursor on Wi-Fi (row 0)

  cw(m, 4); m.onClick();        // Sign-in QR (row 4) -> full-screen ConfigQr state
  assertAsciiView(m, "config-qr");
  m.onLongPress();              // back to Connectivity
  m.onLongPress();              // back to main, cursor on Connectivity (row 3)

  m.onRotate(+1); m.onClick();  // Sound submenu (main row 4)
  assertAsciiView(m, "sound");
  m.onLongPress();              // back to main, cursor on Sound (row 4)

  while (viewOf(m).selected != 7) m.onRotate(+1);
  m.setUpdateAvailable("v9.9.9");
  m.onClick();                  // Software update (main row 7)
  assertAsciiView(m, "update-menu");
  cw(m, 2); m.onClick();        // Install row -> ConfirmInstall
  assertAsciiView(m, "confirm-install");
  m.onLongPress();              // back to UpdateMenu
  m.onLongPress();              // back to main, cursor on Software update (7)

  m.onRotate(+1); m.onClick();  // ConfirmReset (main row 8)
  assertAsciiView(m, "confirm-reset");
}

// Self-test + Battery Main rows launch full-screen views (like ConfigQr): any
// encoder event dismisses them back to Main with the cursor on the row.
static void test_selftest_battery_fullscreen() {
  Config c;
  SettingsMenu m(c);
  m.open();
  // Self-test row (index 9): click -> full-screen; rotate dismisses.
  while (viewOf(m).selected != 9) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_TRUE(m.showingSelfTest());
  TEST_ASSERT_EQUAL(0, int(viewOf(m).items.size()));   // full-screen, not a list
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Self-test"));
  m.onRotate(+1);
  TEST_ASSERT_FALSE(m.showingSelfTest());
  TEST_ASSERT_EQUAL(9, viewOf(m).selected);

  // Battery row (index 10): click -> full-screen; click dismisses.
  while (viewOf(m).selected != 10) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_TRUE(m.showingBattery());
  TEST_ASSERT_TRUE(contains(viewOf(m).title, "Battery"));
  m.onClick();
  TEST_ASSERT_FALSE(m.showingBattery());
  TEST_ASSERT_EQUAL(10, viewOf(m).selected);
}

// Display flip (TFT only): the row shows just before Done on TFT, is hidden on
// e-ink, and toggling it dirties + updates the label. Guards the conditional-row
// remap (mainRowAt) that the new RowFlip introduced.
static void test_flip_row_present_on_tft() {
  Config c;
  SettingsMenu m(c);
  m.setScreenIsTft(true);
  m.setScreenFlip(false);
  m.open();
  auto v = viewOf(m);
  const int n = int(v.items.size());
  TEST_ASSERT_EQUAL_STRING("Done", v.items[n - 1].c_str());
  TEST_ASSERT_EQUAL_STRING("Display flip: Off", v.items[n - 2].c_str());
}

static void test_flip_row_hidden_on_eink() {
  Config c;
  SettingsMenu m(c);
  m.setScreenIsTft(false);   // e-ink: no flip row
  m.open();
  auto v = viewOf(m);
  TEST_ASSERT_EQUAL_STRING("Done", v.items[v.items.size() - 1].c_str());
  for (const auto& s : v.items)
    TEST_ASSERT_TRUE(s.find("Display flip") == std::string::npos);
}

static void test_flip_toggle_dirties_and_labels() {
  Config c;
  SettingsMenu m(c);
  m.setScreenIsTft(true);
  m.setScreenFlip(false);
  m.open();
  const int flipIdx = int(viewOf(m).items.size()) - 2;   // just before Done
  while (viewOf(m).selected != flipIdx) m.onRotate(+1);
  m.onClick();
  TEST_ASSERT_TRUE(m.screenFlip());
  TEST_ASSERT_TRUE(m.dirty());
  TEST_ASSERT_EQUAL_STRING("Display flip: On", viewOf(m).items[flipIdx].c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_selftest_battery_fullscreen);
  RUN_TEST(test_fw_version_in_main_title);
  RUN_TEST(test_all_views_are_printable_ascii);
  RUN_TEST(test_open_close_visibility);
  RUN_TEST(test_rotate_wraps);
  RUN_TEST(test_click_descends_back_ascends);
  RUN_TEST(test_back_row_ascends);
  RUN_TEST(test_profile_change_applies_to_config);
  RUN_TEST(test_profile_switch_preserves_overrides);
  RUN_TEST(test_tune_sets_override_and_dirty);
  RUN_TEST(test_tune_clamps_at_bounds);
  RUN_TEST(test_attn_hue_auto_sentinel_boundary);
  RUN_TEST(test_enum_param_wraps);
  RUN_TEST(test_adjust_submode_gates_rotation);
  RUN_TEST(test_clear_override_row);
  RUN_TEST(test_reset_clears_all_overrides);
  RUN_TEST(test_sfx_rows_cycle);
  RUN_TEST(test_saver_cycle_and_snap);
  RUN_TEST(test_provider_rows_cycle);
  RUN_TEST(test_update_menu_flow);
  RUN_TEST(test_update_unavailable_in_notifier);
  RUN_TEST(test_theme_picker);
  RUN_TEST(test_reset_no_keeps_overrides);
  RUN_TEST(test_connectivity_and_config_qr);
  RUN_TEST(test_connectivity_bluetooth_toggle);
  RUN_TEST(test_connectivity_forget_paired);
  RUN_TEST(test_connectivity_sd_reprobe);
  RUN_TEST(test_wifi_row_opens_the_wifi_screen);
  RUN_TEST(test_wifi_menu_rows_wording);
  RUN_TEST(test_wifi_publish_ap_raises_request_and_shows_no_dirty);
  RUN_TEST(test_wifi_picker_scrolls_and_does_not_exit_on_rotate);
  RUN_TEST(test_wifi_pick_raises_join_with_the_right_ssid);
  RUN_TEST(test_wifi_pick_empty_shows_back_only);
  RUN_TEST(test_wifi_forget_raises_forget_with_ssid);
  RUN_TEST(test_mode_toggle);
  RUN_TEST(test_view_marks_overrides_and_selected);
  RUN_TEST(test_titles_are_breadcrumb_paths);
  RUN_TEST(test_main_row_says_power_profile);
  RUN_TEST(test_help_text_per_state);
  RUN_TEST(test_flip_row_present_on_tft);
  RUN_TEST(test_flip_row_hidden_on_eink);
  RUN_TEST(test_flip_toggle_dirties_and_labels);
  return UNITY_END();
}
