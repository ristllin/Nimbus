// test_console - implementation. The ENTIRE body is gated on NIMBUS_TEST; in a
// production build (esp32s3/notifierdbg) this compiles to an empty translation
// unit (zero symbols, zero code, zero behavior change). The header's inline
// no-op stubs cover the API on that side, so main.cpp's tc::* calls vaporize.
//
// See test_console.h for the design rationale (single Serial reader, command-
// only in every mode, orthogonality to NIMBUS_NOTIFIER_DEBUG).
#include "test_console.h"
#include "sys/config_nvs.h"    // STATUS nvsdeg= - NVS running on defaults?
#include "net/relay_client.h"  // CLOUD? / CLOUDPAIR / CLOUDOFF / CLOUDLOOP
#include "net/wifi_portal.h"   // WIFIKNOWN? - saved-network list
#include "net/wifi_store.h"    // WIFIRENAME - re-key a network, keeping its password
#include "hw/tft_out.h"        // TFTHEALTH? - panel watchdog counter
#include "hw/touch_input.h"    // TAP/TAPUP - synthetic taps (the ENC seam's counterpart)
#include "solide/touch.h"
#include "solide/display_tft.h"   // TFTID? - shared-MISO readback diagnostic       // TOUCH? - raw XPT2046 state for calibration
#include "nimbus/touch_cal.h"   // TCAL - shared parser with the web field
#include <WiFi.h>              // WIFISCAN - what the radio can see

#include "agent/orchestrator.h"  // PROMPT? - dump the last composed World system prompt
#include "agent/telegram.h"      // TGSEND - media-send smoke test
#include "agent/store.h"
#include "version.h"
#include "agent/memory_subsystem.h"    // STATUS - storage tier + vector stats
#include "sys/agent_log.h"           // alogf - MEDIATEST result to /api/log
#include "agent/adapters/audio_tts.h"  // SPKSAY - TTS + speaker test
#include "agent/adapters/audio_stt.h"  // MICREC - mic + STT test
#include "nimbus/fault.h"              // FAULT - resilience capability injection
#include "sfx/sound_fx.h"              // SFX - play a sound clip by slug
#include "sfx/music.h"                 // PLAY - music player drill (CUM-40)
#include "nimbus/orch/media.h"         // validMusicName
#include "sfx/sfx_sync.h"              // sfxsync - sync status in STATUS
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <solide/audio.h>
#include <solide/leds.h>               // LEDSTATE - physical ring-layer diagnostic

#if defined(NIMBUS_TEST)

#include <esp_system.h>
#include <solide/selftest.h>
#include "hw/selftest.h"               // SELFTEST - firmware health-check engine
#include "sys/ota_update.h"            // OTA? / OTACHECK / OTAAPPLY / OTASIM / OTAURL
#include <esp_ota_ops.h>               // running-partition label for OTA?

namespace nimbus::tc {
namespace {

Hooks    s_h;                 // captured by begin()
String   s_line;             // command-line accumulator (also the frame-tee prefix buffer)
bool     s_inputLog = false;  // INPUTLOG on/off - gates onEncoder() echo (F1)
bool     s_badge = false;     // F9 e-ink error badge armed
int      s_badgeReason = 0;   // last WiFi disconnect reason

constexpr size_t kLineCap = 512;   // matches the existing orch-console cap
constexpr int    kByteBudget = 256;  // bytes drained per pump call (loop stays responsive)

// Synthetic encoder-event queue (single-producer/single-consumer, both on the
// main task: dispatch() runs from pumpOrch()/pumpNotifier() and the loop drains
// via popInjectedEncoder()). A tiny ring is plenty - the harness injects one
// event per line and reads the RENDER echo before sending the next.
constexpr int kEncQCap = 16;
int s_encQ[kEncQCap];
int s_encHead = 0, s_encTail = 0;
void encPush(int code) {
  const int nxt = (s_encHead + 1) % kEncQCap;
  if (nxt == s_encTail) return;  // full: drop (harness reads between sends)
  s_encQ[s_encHead] = code;
  s_encHead = nxt;
}

// One-slot screensaver command mailbox (same producer/consumer pair as the
// encoder queue): -1 = force the saver screen now, >=0 = new threshold minutes.
int  s_saverCmd = 0;
bool s_saverPending = false;

void reply(const String& s) {
  Serial.println(s);
  Serial.flush();
}

// Fill the four ring-summary fields via the hook (guarded - provider takes the
// config lock itself). Defaults are a dark, idle ring.
void ringSummary(int& seg, bool& single, bool& dark, uint8_t& bright) {
  seg = 0; single = false; dark = true; bright = 0;
  if (s_h.ringSummary) s_h.ringSummary(seg, single, dark, bright);
}

// Dispatch one complete command line (no trailing newline). Trims leading ws.
void dispatch(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "PING") {
    reply("PONG");                                       // F13 liveness / reconnect probe
    return;
  }
  if (line.startsWith("SFXVOL ")) {
    // Set master speaker volume 0-100 (persisted; applies to SFX + TTS + beep).
    int v = line.substring(7).toInt();
    agent::store::setSfxVolume((uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v));
    ::sfx::refreshConfig();
    Serial.printf("SFXVOL -> %u%%\n", agent::store::sfxVolume());
    return;
  }
  if (line.startsWith("SFX ")) {
    // Play a sound clip by event slug (bypasses the level gate - a test
    // affordance; still respects the speaker fault + voice mute).
    String slug = line.substring(4); slug.trim();
    const bool ok = ::sfx::play(slug.c_str());
    Serial.printf("SFX %s -> %s (level=%u theme=%s tier=%s vol=%u)\n", slug.c_str(),
                  ok ? "queued" : "unknown slug", ::sfx::level(), ::sfx::theme(),
                  ::sfx::tierStr(), agent::store::sfxVolume());
    return;
  }
  if (line == "PLAY" || line.startsWith("PLAY ")) {
    // Music player drill (CUM-40): "PLAY" all of /music, "PLAY <name>" one track,
    // "PLAY stop|pause". WAV plays; MP3 needs the Helix decoder build.
    String arg = line.length() > 4 ? line.substring(5) : String(""); arg.trim();
    String low = arg; low.toLowerCase();
    if (low == "stop")       { music::stop();  Serial.println("PLAY stop"); }
    else if (low == "pause") { music::pause(); Serial.println("PLAY pause"); }
    else if (arg.length() == 0) { int n = music::playAll(); Serial.printf("PLAY all -> %d track(s), mp3=%s\n", n, music::mp3Supported() ? "yes" : "no"); }
    else { bool ok = nimbus::orch::validMusicName(arg.c_str()) && music::playNow({std::string(arg.c_str())}) >= 0; Serial.printf("PLAY %s -> %s\n", arg.c_str(), ok ? "queued" : "invalid name"); }
    return;
  }
  if (line == "STATUS") {
    const int  mode = s_h.mode ? s_h.mode() : 0;
    const bool wifi = s_h.wifiStaUp ? s_h.wifiStaUp() : false;
    const String ip = s_h.wifiIp ? s_h.wifiIp() : String("0.0.0.0");
    const int  rssi = s_h.rssi ? s_h.rssi() : 0;
    // Storage-tier + memory fields (docs/orchestrator-storage.md) so HIL can assert
    // the degraded/PSRAM paths without a camera: sd=present|absent, psram present,
    // vector count/cap, flashfull, and the internal-heap low-water mark (proves the
    // VDB working set is NOT eating the ~300 KB internal SRAM).
    agent::memory::Stats ms = agent::memory::stats();
    bool drainA = false, drainD = false; uint16_t restMv = 0;
    if (s_h.drainState) s_h.drainState(drainA, drainD, restMv);
    // ⚠ scr= sits AFTER heap= on purpose. tests/hil/device.py pins
    // "mode=<n> wifi=<n> ip=.. rssi=.. heap=.." exactly and only becomes
    // tolerant (.*?) after heap=, so a field inserted before that point turns
    // the whole existing HIL suite red.
    // It reports the driver that ACTUALLY bound, not the stored preference:
    // when the fail-soft path trips they differ, and that is precisely the case
    // a test has to be able to see.
    const char* scr = (s_h.screenIsTft && s_h.screenIsTft()) ? "tft" : "eink";
    // board= is the compile-time SOLIDE_BOARD slug (pinout identity, distinct from
    // the runtime scr=). Appended at the very END so nothing before heap= shifts.
#ifndef SOLIDE_BOARD
#define SOLIDE_BOARD solide_s3
#endif
#define NIMBUS_STR2(x) #x
#define NIMBUS_STR(x) NIMBUS_STR2(x)
    Serial.printf("STATUS fw=" NIMBUS_FW_VERSION " build=" NIMBUS_FW_BUILD " mode=%d wifi=%d ip=%s rssi=%d heap=%u scr=%s want=%s minheap=%u psram=%u "
                  "nvsdeg=%d sd=%s sdlost=%d vec=%d/%d flashfull=%d faults=0x%02x jobs=%d sfx=%s/%u/%s vol=%u sync=%s drain=%d/%d restmv=%u ota=%s lastOta=%s uptime=%lu board=%s\n",
                  mode,
                  int(wifi), ip.c_str(), rssi, unsigned(ESP.getFreeHeap()),
                  scr, agent::store::screenModel().c_str(),
                  unsigned(ESP.getMinFreeHeap()), unsigned(ESP.getPsramSize()),
                  int(nimbus::sys::nvsDegraded()),
                  ms.sdPresent ? "present" : "absent", int(agent::memory::sdLost()),
                  ms.vectorCount, ms.maxVectors,
                  int(ms.flashFull), unsigned(nimbus::fault::mask()),
                  // READ-ONLY router job count. The only other reads were
                  // posture-scaled (bright=/seg=) or a different field
                  // entirely (/api/state's `jobs`), so there was no way to ask
                  // "are the sessions still there?" without feeding a frame,
                  // which itself refreshes them.
                  s_h.jobCount ? s_h.jobCount() : -1,
                  ::sfx::tierStr(), ::sfx::level(), ::sfx::theme(),
                  agent::store::sfxVolume(), sfxsync::statusStr(),
                  int(drainA), int(drainD), restMv,
                  otaupd::statusStr(), otaupd::lastResult().c_str(),
                  (unsigned long)millis(), NIMBUS_STR(SOLIDE_BOARD));
    Serial.flush();
    return;
  }
  if (line == "FAULT?") {
    // Resilience: report the injected-fault mask (which capabilities are simulated-absent).
    Serial.printf("FAULT mask=0x%02x", unsigned(nimbus::fault::mask()));
    for (uint8_t i = 0; i < nimbus::fault::COUNT; i++)
      if (nimbus::fault::active(nimbus::fault::Cap(i)))
        Serial.printf(" %s", nimbus::fault::name(nimbus::fault::Cap(i)));
    Serial.println();
    Serial.flush();
    return;
  }
  if (line.startsWith("FAULT ")) {
    // FAULT <sd|memory|mic|speaker|led|screen> <on|off>  |  FAULT clear
    // Marks a capability simulated-absent so its degraded path runs on demand.
    String rest = line.substring(6); rest.trim();
    if (rest == "clear" || rest == "none" || rest == "off") {
      nimbus::fault::clearAll();
      agent::memory::applyConfig();
      Serial.println("FAULT cleared mask=0x00");
      Serial.flush();
      return;
    }
    int sp = rest.indexOf(' ');
    String capS = (sp < 0) ? rest : rest.substring(0, sp);
    String onS  = (sp < 0) ? String("on") : rest.substring(sp + 1); onS.trim();
    const bool on = !(onS == "off" || onS == "0" || onS == "false");
    nimbus::fault::Cap c;
    if (!nimbus::fault::parse(capS.c_str(), c)) {
      Serial.printf("FAULT unknown capability '%s' (sd|memory|mic|speaker|led|screen)\n", capS.c_str());
      Serial.flush();
      return;
    }
    nimbus::fault::set(c, on);
    if (c == nimbus::fault::SD) agent::memory::applyConfig();   // re-apply degraded cap live
    Serial.printf("FAULT %s=%d mask=0x%02x\n", nimbus::fault::name(c), int(on),
                  unsigned(nimbus::fault::mask()));
    Serial.flush();
    return;
  }
  if (line == "RENDER?") {
    int seg; bool single; bool dark; uint8_t bright;
    ringSummary(seg, single, dark, bright);
    const uint8_t screen = s_h.curScreen ? s_h.curScreen() : 0;
    const uint8_t post   = s_h.posture ? s_h.posture() : 0;
    Serial.printf("RENDER screen=%d posture=%d seg=%d single=%d dark=%d bright=%d\n",
                  int(screen), int(post), seg, int(single), int(dark), int(bright));
    Serial.flush();
    return;
  }
  if (line == "MENU?") {
    Serial.println(s_h.menuView ? s_h.menuView() : String("MENU (no hook)"));
    Serial.flush();
    return;
  }
  if (line == "LEDSTATE") {
    // Physical ring-layer diagnostic (vs RENDER?'s COMPOSED intent): reports what
    // the driver is ACTUALLY driving - is the raw showFrame layer live, or has it
    // gone stale and fallen back to a Pattern (e.g. a stuck white boot Pulse)?
    const solide::leds::State st = solide::leds::currentState();
    Serial.printf("LEDSTATE raw=%d pattern=%s rgb=%d,%d,%d bright=%d seg=%d alive=%d\n",
                  int(st.rawFrame),
                  solide::leds::patternName(solide::leds::Pattern(st.pattern)),
                  int(st.r), int(st.g), int(st.b), int(st.bright), st.segCount,
                  int(st.taskAlive));
    Serial.flush();
    return;
  }
  if (line == "LEDTEST") {
    // Eyes-on data-path test. TEST led only proves the RMT worker task exists;
    // it cannot distinguish a healthy task from DIN/DOUT swapped, a missing
    // common ground, or GPIO21 not reaching the first pixel. Patterns animate
    // on the driver's own task, so the main loop may block briefly here.
    const solide::leds::State before = solide::leds::currentState();
    solide::leds::clearFrame();
    solide::leds::setBrightness(40);
    const uint8_t rgb[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    const char* names[3] = {"red", "green", "blue"};
    for (int i = 0; i < 3; ++i) {
      Serial.printf("LEDTEST %s\n", names[i]);
      Serial.flush();
      solide::leds::show(solide::leds::Pattern::Solid,
                         rgb[i][0], rgb[i][1], rgb[i][2]);
      delay(1000);
    }
    solide::leds::off();
    solide::leds::setBrightness(before.bright);
    Serial.println("LEDTEST done (expected: red, green, blue; normal control resumed)");
    Serial.flush();
    return;
  }
  if (line == "INPUTLOG on") {
    s_inputLog = true;  reply("INPUTLOG on");  return;      // F1
  }
  if (line == "INPUTLOG off") {
    s_inputLog = false; reply("INPUTLOG off"); return;
  }
  if (line.startsWith("TURN ")) {
    String t = line.substring(5);
    Serial.printf("TURN <- %s\n", t.c_str());               // F17 - reply arrives async
    Serial.flush();
    if (s_h.turn) s_h.turn(t);  // -> orchestrator; reply mirrored as ORCH REPLY [serial]
    return;
  }
  if (line.startsWith("SPKSAY ")) {
    // TTS the text and play it on the speaker. Watchdog off during the blocking
    // network synth + playback (both exceed the main-loop budget individually).
    String txt = line.substring(7);
    Serial.printf("SPKSAY synth+play: %s\n", txt.c_str()); Serial.flush();
    esp_task_wdt_delete(nullptr);
    size_t n = agent::tts::synthesizeToFile(txt, "/tts.wav", "wav");
    bool played = n ? solide::audio::playWavFile(LittleFS, "/tts.wav") : false;
    esp_task_wdt_add(nullptr);
    Serial.printf("SPKSAY bytes=%u played=%d (did you HEAR it?)\n", (unsigned)n, (int)played);
    return;
  }
  if (line == "WEBTOK?") {
    // Print the per-device web/MCP auth token (TEST build only) so the HIL harness can
    // exercise the authenticated path; on a real device the owner reads it off the QR.
    Serial.printf("WEBTOK %s\n", agent::store::webAuthToken().c_str());
    return;
  }
  if (line == "CLOUD?") {
    String j;
    nimbus::relay::statusJson(j);
    Serial.printf("CLOUD %s\n", j.c_str());
    return;
  }
  if (line == "CLOUDPAIR") {
    // Ensure opted in, then start pairing. The code + claim URL print via CLOUD? and
    // show on the e-ink screen; the owner claims it at app.cumulo-nimbus.ai.
    nimbus::relay::requestOptIn(true);
    nimbus::relay::requestPair();
    Serial.println("CLOUDPAIR started (poll CLOUD? for the code)");
    return;
  }
  if (line == "CLOUDOFF") {
    nimbus::relay::requestUnpair();
    nimbus::relay::requestOptIn(false);
    Serial.println("CLOUDOFF (unpaired + disabled)");
    return;
  }
  if (line.startsWith("CLOUDLOOP")) {
    // Prove the loopback replay path with no cloud dependency: GET a local route
    // through the same code the tunnel uses. Default /api/state.
    String path = line.length() > 10 ? line.substring(10) : "/api/state";
    path.trim();
    if (path.isEmpty()) path = "/api/state";
    String body;
    int st = nimbus::relay::loopbackSelfTest(path, body);
    Serial.printf("CLOUDLOOP %s -> %d\n%s\n", path.c_str(), st, body.c_str());
    return;
  }
  if (line == "WIFIAP" || line == "WIFIAP on") {
    // Force the setup network to be reachable NOW: drop the station so it stops
    // competing for the radio. This is the on-device escape hatch, driven from the
    // console until the menu row lands.
    nimbus::net::publishSetupNetwork();
    Serial.printf("WIFIAP on ssid=%s ip=%s (station stopped)\n",
                  nimbus::net::apSsid().c_str(), nimbus::net::apIp().c_str());
    Serial.flush();
    return;
  }
  if (line == "WIFIAP off") {
    nimbus::net::cancelSetupHold();
    Serial.println("WIFIAP off (joining resumed)");
    Serial.flush();
    return;
  }
  if (line == "WIFISCAN") {
    // What the radio can actually SEE. Blocking is fine for a console command (the
    // WDT is released around it, the MICREC pattern); the running firmware only ever
    // scans asynchronously.
    Serial.println("WIFISCAN scanning...");
    Serial.flush();
    esp_task_wdt_delete(nullptr);
    // Quiet the station first. With a stored network that is out of range the core's
    // auto-reconnect owns the radio continuously and the scan just returns -2 - the
    // same radio starvation that hides the setup AP.
    const bool wasJoined = (WiFi.status() == WL_CONNECTED);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
    delay(300);
    const int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
    WiFi.setAutoReconnect(true);
    // ...and REJOIN. setAutoReconnect(true) alone does not: an explicit
    // disconnect() raises ASSOC_LEAVE, which the core short-circuits BEFORE it
    // consults the auto-reconnect flag. So the station stayed down until the
    // next reboot while the comment above claimed it was "restored below" - a
    // diagnostic that silently takes the device off the network is worse than
    // no diagnostic.
    if (wasJoined) {
      WiFi.begin();
      Serial.println("WIFISCAN rejoining...");
    }
    esp_task_wdt_add(nullptr);
    Serial.printf("WIFISCAN n=%d\n", n);
    for (int i = 0; i < n; i++)
      Serial.printf("  ssid=%s rssi=%d enc=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                    int(WiFi.encryptionType(i) != WIFI_AUTH_OPEN));
    WiFi.scanDelete();
    Serial.flush();
    return;
  }
  if (line.startsWith("WIFIRENAME ")) {
    // Correct a mistyped network name while KEEPING its password. Without this a
    // typo is unrecoverable without re-entering the secret, since the store never
    // hands one out. The password is moved inside the store and never printed.
    String rest = line.substring(11);
    const int bar = rest.indexOf('|');
    if (bar < 0) { Serial.println("WIFIRENAME usage: WIFIRENAME <old>|<new>"); return; }
    String from = rest.substring(0, bar), to = rest.substring(bar + 1);
    from.trim(); to.trim();
    const bool ok = nimbus::net::wifistore::rename(from, to);
    Serial.printf("WIFIRENAME '%s' -> '%s' : %s\n", from.c_str(), to.c_str(),
                  ok ? "ok" : "failed");
    if (ok) Serial.printf("WIFIKNOWN n=%d %s\n", nimbus::net::knownCount(),
                          nimbus::net::knownNetworksJson().c_str());
    Serial.flush();
    return;
  }
  if (line == "WIFIKNOWN?") {
    // The saved-network list. NAMES ONLY - a password must never reach a console
    // transcript or an HIL log. Proves on real hardware that the legacy single slot
    // migrated into the list and that the list survives a restart.
    Serial.printf("WIFIKNOWN n=%d %s\n", nimbus::net::knownCount(),
                  nimbus::net::knownNetworksJson().c_str());
    return;
  }
  if (line == "MEDIATEST") {
    // Self-test the durable media sidecar path end-to-end on the real SD: write a
    // small file to LittleFS, stream-capture it via captureMediaFile (-> content-
    // addressed /mem/blobs/<hash>.bin + an episodic Audio row), then report over
    // /api/log (HTTP-readable). captured=1 + episodic++ proves the streaming copy +
    // episodic append work on the card. SD-gated: captured=0 with no card.
    const char* p = "/mediatest.bin";
    File f = LittleFS.open(p, FILE_WRITE);
    if (f) { f.print("nimbus media self-test payload 0123456789abcdef"); f.close(); }
    int before = agent::memory::stats().episodicMsgs;
    bool ok = agent::memory::captureMediaFile("mediatest", "user", nimbus::orch::MsgKind::Audio,
                                              "media self-test", p, "bin");
    int after = agent::memory::stats().episodicMsgs;
    LittleFS.remove(p);
    agent::alogf("[mediatest] captured=%d episodic %d->%d haveSd=%d", (int)ok, before, after,
                 (int)agent::memory::haveSd());
    Serial.printf("MEDIATEST captured=%d episodic %d->%d haveSd=%d\n", (int)ok, before, after,
                  (int)agent::memory::haveSd());
    return;
  }
  if (line == "MICREC") {
    // Record ~4 s from the mic, then transcribe it. Reports sample count (mic
    // captured audio?) + transcript (STT works?). Watchdog off during record+STT.
    Serial.println("MICREC recording 4s - SPEAK NOW..."); Serial.flush();
    esp_task_wdt_delete(nullptr);
    size_t bytes = solide::audio::recordToFile(LittleFS, "/mic.pcm", 4000, nullptr);
    String tr = agent::stt::transcribePcm("/mic.pcm", 16000);  // mic is 16 kHz mono;
                                                               // WAV header streams inline
    esp_task_wdt_add(nullptr);
    Serial.printf("MICREC bytes=%u transcript=\"%s\"\n", (unsigned)bytes, tr.c_str());
    return;
  }
  if (line.startsWith("VOICE ")) {
    // Simulate a voice transcript: inject a turn on the "voice" chatId so the reply
    // renders on the e-ink (Ask screen), exactly like a real hold-to-talk would -
    // testable without the physical mic.
    String t = line.substring(6);
    Serial.printf("VOICE inject <- %s\n", t.c_str()); Serial.flush();
    agent::telegram::injectMessage("voice", t);
    return;
  }
  if (line.startsWith("TTSTG ")) {
    // TTS the text (configured provider) and send it to the owner as a Telegram
    // audio message - lets us HEAR the voice even with the bench speaker dead.
    String txt = line.substring(6);
    String allow = agent::store::telegramAllowlist();
    int c = allow.indexOf(','); String chat = (c > 0) ? allow.substring(0, c) : allow; chat.trim();
    if (chat.length() == 0) { Serial.println("TTSTG: no allowlist chat"); return; }
    Serial.printf("TTSTG synth (%s): %s\n", agent::store::ttsProvider().c_str(), txt.c_str()); Serial.flush();
    esp_task_wdt_delete(nullptr);
    size_t n = agent::tts::synthesizeToFile(txt, "/tts.mp3", "mp3");
    esp_task_wdt_add(nullptr);
    bool ok = n ? agent::telegram::sendMedia(chat, "audio", "/tts.mp3", "Nimbus voice") : false;
    Serial.printf("TTSTG bytes=%u queued=%d\n", (unsigned)n, (int)ok);
    return;
  }
  if (line == "TGSEND") {
    // Media-send smoke test: write a small file to LittleFS and send it as a
    // Telegram document to the first allowlisted chat.
    String allow = agent::store::telegramAllowlist();
    int c = allow.indexOf(',');
    String chat = (c > 0) ? allow.substring(0, c) : allow; chat.trim();
    if (chat.length() == 0) { Serial.println("TGSEND: no allowlist chat configured"); return; }
    File f = LittleFS.open("/tgtest.txt", FILE_WRITE);
    if (f) {
      f.printf("Nimbus media-send test. uptime=%lus heap=%u\n",
               (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap());
      f.close();
    }
    bool ok = agent::telegram::sendMedia(chat, "document", "/tgtest.txt", "Nimbus test document");
    Serial.printf("TGSEND queued=%d chat=%s\n", (int)ok, chat.c_str());
    return;
  }
  if (line == "PROMPT?") {
    // Dump the World system prompt composed on the most recent turn, framed so a
    // test can capture it whole. Chunked with small flushes because it is multi-KB
    // and the shared serial TX (20 ms bounded) tears large single bursts.
    String p = agent::orchestrator::lastInstructions();
    Serial.printf("PROMPT BEGIN len=%u\n", (unsigned)p.length());
    Serial.flush();
    const int chunk = 256;
    for (int i = 0; i < (int)p.length(); i += chunk) {
      Serial.print(p.substring(i, i + chunk));
      Serial.flush();
      delay(5);
    }
    Serial.print("\nPROMPT END\n");
    Serial.flush();
    return;
  }
  if (line.startsWith("WIFI ")) {
    // WIFI <ssid>|<pass>  (F8). The password may contain anything except '|'.
    String rest = line.substring(5);
    int bar = rest.indexOf('|');
    String ssid = bar < 0 ? rest : rest.substring(0, bar);
    String pass = bar < 0 ? String("") : rest.substring(bar + 1);
    Serial.printf("WIFI provisioning ssid=%s\n", ssid.c_str());  // async GOT_IP/DISCONNECTED
    Serial.flush();
    if (s_h.wifi) s_h.wifi(ssid, pass);
    return;
  }
  if (line == "REBOOT") {
    reply("REBOOTING");                                    // F13 flap/recover
    if (s_h.reboot) s_h.reboot();
    return;
  }
  if (line == "HANG") {
    reply("HANGING");                                      // F12 - proves the WDT once added
    if (s_h.hang) s_h.hang();
    return;
  }
  if (line.startsWith("TEST ")) {
    // Passthrough to the solide-drivers self-test (RESULT lines print directly),
    // so the HIL selftest_gate runs against the PRODUCT firmware, not just the
    // 08_selftest_console example.
    solide::selftest::run(line.substring(5).c_str());
    return;
  }
  if (line == "SELFTEST" || line == "SELFTEST FULL") {
    // Firmware self-test engine (the same one the menu / /api/selftest / the
    // device.selftest tool use). FULL adds the audible acoustic tests, but only
    // when the device isn't silent (so a bench-silent board doesn't blare).
    const bool wantAudible = line.endsWith("FULL");
    esp_task_wdt_delete(nullptr);
    auto items = nimbus::hw::runNow(wantAudible && !::sfx::isSilent());
    esp_task_wdt_add(nullptr);
    Serial.print(nimbus::hw::selfTestText(items));
    Serial.printf("SELFTEST %s\n", nimbus::hw::selfTestSummary(items).c_str());
    return;
  }
  if (line == "OTA?") {
    // OTA state one-liner: machine state, progress, latest release seen by the
    // last check, and the persisted last outcome (ok/rollback/sha-fail/...).
    Serial.printf("OTA state=%s err=%s pct=%d latest=%s notes=%s lastOta=%s pend=%d boots=%d prev=%s slot=%s\n",
                  otaupd::statusStr(), otaupd::lastError()[0] ? otaupd::lastError() : "-",
                  otaupd::progressPct(),
                  otaupd::latestSeen().c_str(), otaupd::latestNotes().c_str(),
                  otaupd::lastResult().c_str(), agent::store::otaPending(),
                  agent::store::otaBootCount(), agent::store::otaPrevSlot().c_str(),
                  esp_ota_get_running_partition()->label);
    return;
  }
  if (line == "OTACHECK") {
    Serial.printf("OTACHECK %s\n", otaupd::requestCheck() ? "started" : "refused");
    return;
  }
  if (line == "OTAAPPLY" || line.startsWith("OTAAPPLY ")) {
    const bool dry = line.indexOf(" dry") > 0, force = line.indexOf(" force") > 0;
    const char* why = "";
    const bool ok = otaupd::requestInstall(dry, force, &why);
    Serial.printf("OTAAPPLY %s%s%s\n", ok ? "started" : "refused",
                  ok ? "" : ": ", ok ? "" : why);
    return;
  }
  if (line.startsWith("OTAURL ")) {
    // RAM-only manifest-URL override for HIL (points the next OTACHECK/OTAAPPLY
    // at a local TLS server; lost on reboot).
    String u = line.substring(7); u.trim();
    if (u == "clear") u = "";   // back to the real GitHub manifest URL
    otaupd::setManifestUrl(u);
    Serial.printf("OTAURL %s\n", u.c_str());
    return;
  }
  if (line.startsWith("OTASIM ")) {
    // Rollback-drill seams: `OTASIM arm <slotLabel>` arms the pend guard as if we
    // just flipped from <slotLabel>; `OTASIM crash` makes every boot abort() while
    // pending (synthetic bad image); `OTASIM clear` drops all sim state.
    String a = line.substring(7); a.trim();
    if (a.startsWith("arm")) { String l = a.substring(3); l.trim(); otaupd::simArm(l); Serial.printf("OTASIM armed prev=%s\n", l.c_str()); }
    else if (a == "crash") { otaupd::simCrash(true); Serial.println("OTASIM crash-on-boot ARMED (while pending)"); }
    else if (a == "clear") { otaupd::simClear(); Serial.println("OTASIM cleared"); }
    else Serial.println("OTASIM arm <slot>|crash|clear");
    return;
  }
  if (line == "SDCHECK") {
    // SD graceful degradation: force one liveness probe + note (bypasses the loop's
    // cadence gate). Repeat to drive the demote/promote state machine deterministically
    // (2 failing probes -> demote; 2 passing -> promote). Pair with `FAULT sd_io on|off`.
    bool ok = agent::memory::forceSdProbe();
    Serial.printf("SDCHECK probe=%d sdlost=%d\n", int(ok), int(agent::memory::sdLost()));
    return;
  }
  if (line == "CTX?" || line.startsWith("CTX? ")) {
    String chat = line.length() > 4 ? line.substring(5) : String("");
    chat.trim();
    reply(s_h.ctxInfo ? s_h.ctxInfo(chat) : String("ERR ctx unavailable"));
    return;
  }
  if (line.startsWith("COMPACT")) {
    String chat = line.length() > 7 ? line.substring(8) : String("");
    chat.trim();
    if (!chat.length()) { reply("ERR usage: COMPACT <chat>"); return; }
    reply(s_h.compactNow ? s_h.compactNow(chat) : String("ERR compact unavailable"));
    return;
  }
  if (line.startsWith("MEMFILL ")) {
    // MEMFILL <epi|vec> <n> [bytes] - chunked ≤200/call; loop the command to reach
    // a cap. Session "hiltest-*" rows; cleanup = normal retention/prune paths.
    String rest = line.substring(8);
    int sp1 = rest.indexOf(' ');
    if (sp1 < 0) { reply("ERR usage: MEMFILL <epi|vec> <n> [bytes]"); return; }
    String kind = rest.substring(0, sp1);
    String tail = rest.substring(sp1 + 1); tail.trim();
    int sp2 = tail.indexOf(' ');
    int n = (sp2 < 0 ? tail : tail.substring(0, sp2)).toInt();
    int bytes = sp2 < 0 ? 64 : tail.substring(sp2 + 1).toInt();
    if (n <= 0) { reply("ERR usage: MEMFILL <epi|vec> <n> [bytes]"); return; }
    reply(s_h.memFill ? s_h.memFill(kind, n, bytes) : String("ERR memfill unavailable"));
    return;
  }
  if (line.startsWith("EPIQ ")) {
    String rest = line.substring(5); rest.trim();
    String before;
    if (rest.startsWith("@")) {            // "@<cursor> <text>"
      int sp = rest.indexOf(' ');
      if (sp < 0) { reply("ERR usage: EPIQ [@<cursor>] <text>"); return; }
      before = rest.substring(1, sp);
      rest = rest.substring(sp + 1); rest.trim();
    }
    if (!rest.length()) { reply("ERR usage: EPIQ [@<cursor>] <text>"); return; }
    reply(s_h.epiQuery ? s_h.epiQuery(before, rest) : String("ERR epiq unavailable"));
    return;
  }
  if (line == "DREAM") {
    // Force the reserved dream loop's two-phase fire NOW (maintenance +
    // reflection turn). Blocks while the turn runs (like TURN); reply echoes a
    // summary. Test-only affordance for HIL verification of dreaming.
    if (s_h.dreamNow) {
      reply(s_h.dreamNow());
    } else {
      reply("ERR dream unavailable (Orchestrator mode only)");
    }
    return;
  }
  if (line.startsWith("SLEEPMV")) {
    // SLEEPMV [<mv>] - read or set the low-battery deep-sleep threshold.
    // 0 = OFF.
    //
    // ⚠ SAFETY KNOB. It exists for the bench case this hit on first bring-up:
    // a board with NO PACK CONNECTED reads ~0 mV on the divider, which is
    // indistinguishable from a flat battery, so the device deep-sleeps every few
    // seconds and cannot be worked on at all. Setting it to 0 disarms the
    // protection PERMANENTLY (it is NVS-backed, not reset at reboot) - with a
    // real pack fitted that risks a deep discharge the pack may not recover
    // from. Restore it (default 6000) the moment a battery is connected.
    String a = line.substring(7); a.trim();
    if (a.length() == 0) {
      Serial.printf("SLEEPMV %u%s\n", unsigned(agent::store::sleepMv()),
                    agent::store::sleepMv() == 0 ? " (OFF - protection disarmed)" : "");
      Serial.flush();
      return;
    }
    const long mv = a.toInt();
    if (mv < 0 || mv > 6800) { reply("ERR sleepmv 0-6800 (0 = off)"); return; }
    agent::store::setSleepMv(uint16_t(mv));
    Serial.printf("SLEEPMV -> %ld%s\n", mv,
                  mv == 0 ? " (OFF - restore to 6000 when a pack is fitted)" : "");
    Serial.flush();
    return;
  }
  if (line == "SLEEP") {
    // Enter the low-batt deep sleep immediately (mechanics test). The board goes
    // dark: knob rotation or the 5-min timer wakes it; USB console dies with it.
    if (s_h.sleepNow) {
      Serial.println("SLEEP entering low-battery deep sleep (rotate knob or wait 5 min)");
      Serial.flush();
      s_h.sleepNow();
    } else {
      reply("ERR sleep unavailable");
    }
    return;
  }
  if (line == "BATTRESET") {
    // Throw away everything the model LEARNED by observation (rate EWMA + the
    // as-new health baselines) while keeping the owner's BATTCAL anchor. Needed
    // after a drain campaign taught it a synthetic ~1 A load - that state persists
    // to NVS, so flashing the fix does not undo the poisoning.
    if (s_h.battReset) {
      String r = s_h.battReset();
      Serial.printf("BATTRESET %s\n", r.c_str());
    } else {
      reply("ERR battreset unavailable");
    }
    Serial.flush();
    return;
  }
  if (line == "BATTCAL") {
    // Owner asserts "the pack is full right now": anchor 100% to the current
    // reading (fixes the S3 ADC's under-read of a full 2S pack) and persist it.
    if (s_h.battCal) {
      String r = s_h.battCal();
      Serial.printf("BATTCAL %s\n", r.c_str());
    } else {
      reply("ERR battcal unavailable");
    }
    return;
  }
  if (line.startsWith("DRAIN ")) {
    // Battery drain campaign: pin the ring to ~2 A solid white until 'off'. `deep`
    // suppresses the clean T2 shutdown so the pack runs to the real cutoff.
    String a = line.substring(6); a.trim();
    const bool on   = a.startsWith("on");
    const bool deep = a.indexOf("deep") >= 0;
    if (s_h.drain) Serial.printf("DRAIN %s\n", s_h.drain(on, deep).c_str());
    else           reply("ERR drain unavailable");
    return;
  }
  if (line.startsWith("STORAGE ")) {
    // Discharge down to a storage SoC (~70%) then hold. `off` cancels.
    String a = line.substring(8); a.trim();
    const int pct = a.startsWith("off") ? 0 : a.toInt();
    if (s_h.storage) Serial.printf("STORAGE %s\n", s_h.storage(pct).c_str());
    else             reply("ERR storage unavailable");
    return;
  }
  if (line == "SAVER" || line.startsWith("SAVER ")) {
    // Force the screensaver screen now (bare SAVER), or set + persist the idle
    // threshold in minutes (SAVER <0-1440>, 0 = off). Applied by the main loop.
    int v = -1;
    if (line.length() > 5) {
      String a = line.substring(6); a.trim();
      v = a.toInt();
      if (v < 0 || v > 1440 || (v == 0 && a != "0")) { reply("ERR saver want 0-1440 minutes (bare SAVER = force now)"); return; }
    }
    s_saverCmd = v;
    s_saverPending = true;
    reply(v < 0 ? "SAVER< force" : (String("SAVER< min=") + v));
    return;
  }
  if (line.startsWith("ENC ")) {
    // Inject a synthetic encoder event so the settings menu can be driven from
    // the HIL harness (no physical knob). Codes match solide::input::Event.
    String a = line.substring(4); a.trim();
    int code = 0;
    if (a == "CW")         code = 1;
    else if (a == "CCW")   code = 2;
    else if (a == "CLICK") code = 3;
    else if (a == "LONG")  code = 4;
    if (code) { encPush(code); Serial.printf("ENC< %s\n", a.c_str()); Serial.flush(); }
    else      reply("ERR enc want CW|CCW|CLICK|LONG");
    return;
  }
  if (line == "SW?") {
    // Raw debounced switch level - isolates a physical-button fault from a
    // decode/render fault. 1 = pressed (GPIO48 pulled low).
    const int sw = (s_h.swRaw && s_h.swRaw()) ? 1 : 0;
    Serial.printf("SW %d\n", sw);
    Serial.flush();
    return;
  }
  if (line.startsWith("TAP ")) {
    // Inject a synthetic tap so the touch UI is HIL-driveable with no finger -
    // the ENC seam's counterpart. "TAP <x> <y>" presses and releases at a panel
    // coordinate; "TAP <x> <y> HOLD" presses and stays down (release with TAPUP)
    // so press-and-hold to talk can be exercised.
    String a = line.substring(4); a.trim();
    const int sp = a.indexOf(' ');
    if (sp <= 0) { reply("ERR tap want <x> <y> [HOLD]"); return; }
    const int x = a.substring(0, sp).toInt();
    String rest = a.substring(sp + 1); rest.trim();
    const int sp2 = rest.indexOf(' ');
    const int y = (sp2 > 0 ? rest.substring(0, sp2) : rest).toInt();
    const bool hold = rest.endsWith("HOLD");
    // ⚠ Bounds come from the DRIVER, never hardcoded. These were literal
    // 240x320 and survived the rotation to landscape, so every tap past x=240 -
    // which includes the gear, at the top-RIGHT - was refused with "out of
    // range" while the panel was happily 320 wide.
    const int panelW = solide::display_tft::kW;
    const int panelH = solide::display_tft::kH;
    if (x < 0 || x >= panelW || y < 0 || y >= panelH) {
      Serial.printf("ERR tap out of range (%dx%d)\n", panelW, panelH);
      return;
    }
    // injectTap queues press-then-release across SEPARATE polls; injectDown
    // holds until TAPUP. Injecting a down/up pair in one dispatch would have
    // both consumed by a single poll and no gesture would ever fire.
    if (hold) nimbus::hw::touch::injectDown(int16_t(x), int16_t(y));
    else      nimbus::hw::touch::injectTap(int16_t(x), int16_t(y));
    Serial.printf("TAP< %d %d%s\n", x, y, hold ? " HOLD" : "");
    Serial.flush();
    return;
  }
  if (line == "TAPUP") {
    nimbus::hw::touch::injectUp();
    reply("TAPUP<");
    return;
  }
  if (line.startsWith("PANELPROBE")) {
    String a = line.substring(10); a.trim();
    if (!a.length()) { reply("ERR panelprobe want 0|1"); return; }
    const bool on = (a.toInt() != 0);
    Serial.printf("PANELPROBE -> %d %s\n", int(on),
                  (s_h.panelProbe && s_h.panelProbe(on)) ? "applied" : "unavailable");
    return;
  }
  if (line.startsWith("PROFILE")) {
    String a = line.substring(7); a.trim();
    if (!a.length()) { reply("ERR profile want 0|1|2 (0=Dark 1=Balanced 2=Full)"); return; }
    const int p = a.toInt();
    if (p < 0 || p > 2) { reply("ERR profile want 0|1|2"); return; }
    const bool ok = s_h.setProfile ? s_h.setProfile(p) : false;
    Serial.printf("PROFILE -> %d %s\n", p, ok ? "applied" : "unavailable");
    return;
  }
  if (line == "TFTBREAK") {
    // DRILL (mirrors OTASIM): reproduce the white screen on demand by hard-
    // resetting the panel behind the firmware's back, so the recovery path is
    // PROVEN rather than assumed. The panel loses its configuration exactly as
    // it does in the field; the dirty-gate health check should notice within
    // kHealMs and repaint without a restart.
    solide::display_tft::holdReset(true);
    solide::display_tft::holdReset(false);
    Serial.printf("TFTBREAK panel reset behind the driver; healthy=%d (expect 0)\n",
                  int(solide::display_tft::healthy()));
    return;
  }
  if (line == "TFTHEALTH?") {
    Serial.printf("TFTHEALTH healthy=%d heals=%lu backlight=%u\n",
                  int(solide::display_tft::healthy()),
                  (unsigned long)nimbus::hw::tft::healCount(),
                  unsigned(nimbus::hw::tft::backlight()));
    return;
  }
  if (line == "TOUCHISO?") {
    // ISOLATION TEST - the decisive one for the dead touch. The panel's SDO and
    // the touch's T_DO are bridged onto one MISO line. Holding the panel in
    // RESET makes it release that pin; if the touch controller answers ONLY in
    // that window, the panel is not tri-stating and the bridge is a HARDWARE
    // conflict (needs a series resistor / separate MISO), not a firmware bug.
    uint16_t ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0;
    const bool before = solide::touch::readRaw(ax, ay, az);
    solide::display_tft::holdReset(true);
    const bool during = solide::touch::readRaw(bx, by, bz);
    solide::display_tft::holdReset(false);
    solide::display_tft::reinit();
    solide::display_tft::setFlip(agent::store::tftFlip());
    Serial.printf("TOUCHISO normal=%d,%d,%u(%d) panelInReset=%d,%d,%u(%d)\n",
                  ax, ay, az, int(before), bx, by, bz, int(during));
    return;
  }
  if (line.startsWith("TFTHZ")) {
    // Sweep the panel clock and MEASURE the pixel path at each step, instead of
    // asking someone to look at the glass. Prints mismatches per 64 pixels.
    String a = line.substring(5); a.trim();
    if (a.length()) {
      solide::display_tft::setPanelHz(uint32_t(a.toInt()) * 1000000u);
      Serial.printf("TFTHZ -> %u MHz\n", unsigned(solide::display_tft::panelHz() / 1000000u));
      return;
    }
    const uint32_t save = solide::display_tft::panelHz();
    Serial.println("TFTHZ sweep (mismatched pixels per 64, 3 runs each):");
    for (uint32_t mhz : {4u, 10u, 20u, 26u, 40u}) {
      solide::display_tft::setPanelHz(mhz * 1000000u);
      int a1 = solide::display_tft::pixelRoundTrip(64);
      int a2 = solide::display_tft::pixelRoundTrip(64);
      int a3 = solide::display_tft::pixelRoundTrip(64);
      Serial.printf("  %2u MHz : %2d %2d %2d\n", unsigned(mhz), a1, a2, a3);
    }
    solide::display_tft::setPanelHz(save);
    Serial.printf("TFTHZ restored %u MHz\n", unsigned(save / 1000000u));
    return;
  }
  if (line == "TFTPWR?") {
    // Is the panel POWERED AND DISPLAYING, or merely storing pixels?
    // TFTFILL? proves GRAM accepts and keeps data; it says nothing about whether
    // the glass is lit by it. RDDPM (0x0A) carries booster / sleep / display-on,
    // and if it is readable it is the only way to tell those apart from here.
    // Dumped at several widths because a wrong dummy-cycle count shows up as a
    // BIT-SHIFTED value rather than an obviously bad one.
    for (int w = 1; w <= 3; w++)
      Serial.printf("TFTPWR rddpm(w=%d)=0x%08lX\n", w,
                    (unsigned long)solide::display_tft::readReg(0x0A, w));
    Serial.printf("TFTPWR rddst=0x%08lX  madctl_expect=0x%02X\n",
                  (unsigned long)solide::display_tft::readReg(0x09, 4),
                  solide::display_tft::flipped() ? 0xE8 : 0x28);
    // Now force sleep-out + display-on and re-read: if the register is real, it
    // must CHANGE. If it reads identically either way it is not reporting state
    // and must not be trusted as a health signal.
    solide::display_tft::rearm();
    delay(20);
    Serial.printf("TFTPWR after-rearm rddpm=0x%08lX rddst=0x%08lX\n",
                  (unsigned long)solide::display_tft::readReg(0x0A, 1),
                  (unsigned long)solide::display_tft::readReg(0x09, 4));
    return;
  }
  if (line == "TFTFILL?") {
    // Drive the REAL full-panel path (the same fill the driver uses), then read
    // pixels back from the far corners. If the panel holds a full frame, the
    // blit, the window addressing and the display are all sound and any blank
    // screen is downstream of the panel entirely.
    struct { const char* name; uint16_t c; } steps[] = {
      {"red", 0xF800}, {"green", 0x07E0}, {"blue", 0x001F},
    };
    for (auto& st : steps) {
      solide::display_tft::fill(st.c);
      delay(60);
      const uint16_t a = solide::display_tft::readPixel(2, 2);
      const uint16_t b = solide::display_tft::readPixel(
          solide::display_tft::kW / 2, solide::display_tft::kH / 2);
      const uint16_t c = solide::display_tft::readPixel(
          solide::display_tft::kW - 3, solide::display_tft::kH - 3);
      Serial.printf("TFTFILL %-5s want=0x%04X  tl=0x%04X mid=0x%04X br=0x%04X  %s\n",
                    st.name, st.c, a, b, c,
                    (a == st.c && b == st.c && c == st.c) ? "OK" : "MISMATCH");
    }
    return;
  }
  if (line == "TFTID?") {
    // Decides WHERE the touch fault is. The panel's SDO and the touch's T_DO are
    // bridged onto one MISO line (GPIO 1) on this module. If the PANEL reads back
    // plausibly, MISO + the pin are fine and the fault is in the touch path; if
    // nothing reads back, the shared MISO line itself is the problem.
    const uint32_t id   = solide::display_tft::readReg(0x04, 3);   // RDDID
    const uint32_t dst  = solide::display_tft::readReg(0x09, 4);   // RDDST
    const uint32_t pm   = solide::display_tft::readReg(0x0A, 1);   // RDDPM
    Serial.printf("TFTID id=0x%06lX status=0x%08lX power=0x%02lX\n",
                  (unsigned long)id, (unsigned long)dst, (unsigned long)pm);
    return;
  }
  if (line.startsWith("TFTFLIP")) {
    // Which end of the LANDSCAPE colour panel is up. The mounting decides it and
    // software cannot detect it, so this exists to dial it in on a live device
    // rather than guess at build time and reflash. MADCTL-only: instant, and it
    // does not blank the panel.
    String a = line.substring(7); a.trim();
    if (!a.length()) {
      Serial.printf("TFTFLIP %d\n", int(agent::store::tftFlip()));
      return;
    }
    const bool on = (a.toInt() != 0);
    agent::store::setTftFlip(on);
    if (s_h.tftFlip) s_h.tftFlip(on);
    Serial.printf("TFTFLIP -> %d\n", int(on));
    return;
  }
  if (line.startsWith("TCAL")) {
    // Read or set the touch calibration measured from TOUCH?/bringup corner
    // values: "TCAL" prints it, "TCAL <minX,maxX,minY,maxY[,flags]>" stores and
    // applies it (flags: 1 swapXY, 2 invertX, 4 invertY). Parsed with the SAME
    // portable parser the web field uses, so the two cannot disagree.
    String a = line.substring(4); a.trim();
    if (a.length() == 0) {
      const String cur = agent::store::touchCal();
      Serial.printf("TCAL %s\n", cur.length() ? cur.c_str() : "(defaults)");
      Serial.flush();
      return;
    }
    nimbus::touch::Cal c;
    if (!nimbus::touch::parseCal(std::string(a.c_str()), c)) {
      reply("ERR tcal want minX,maxX,minY,maxY[,flags] (0-4095, min<max, flags 0-7)");
      return;
    }
    agent::store::setTouchCal(a);
    solide::touch::Calibration sc;
    sc.minX = c.minX; sc.maxX = c.maxX;
    sc.minY = c.minY; sc.maxY = c.maxY;
    sc.swapXY = c.swapXY; sc.invertX = c.invertX; sc.invertY = c.invertY;
    solide::touch::setCalibration(sc);
    Serial.printf("TCAL< %s (applied + persisted)\n", a.c_str());
    Serial.flush();
    return;
  }
  if (line == "TOUCH?") {
    // Raw XPT2046 state - isolates a touch-hardware fault from a hit-test or
    // render fault, and gives the corner values the calibration is read from.
    uint16_t rx = 0, ry = 0, rz = 0;
    const bool hit = solide::touch::readRaw(rx, ry, rz);
    // ⚠ z is the PRESSURE ESTIMATE (z1 - z2 + 4095), not a raw register. An idle
    // panel reads ~10 and a dead chip reads exactly 4095 - so a LOW z proves the
    // controller is answering, and raw=0,0 with a low z is simply "no finger"
    // (x/y are only sampled above the threshold). Read the other way round, this
    // line reads like dead hardware; it was, once.
    const solide::touch::Point p = solide::touch::read();
    Serial.printf("TOUCH present=%d raw=%u,%u z=%u down=%d px=%d,%d\n",
                  int(solide::touch::present()), rx, ry, rz, int(p.down), p.x, p.y);
    (void)hit;
    Serial.flush();
    return;
  }
  if (line == "BLE?") {
    // BLE radio state: enabled = advertising-enabled, connected = a central is
    // linked. Lets the harness assert the Connectivity > Bluetooth toggle works.
    int en = 0, conn = 0;
    if (s_h.bleState) s_h.bleState(en, conn);
    Serial.printf("BLE enabled=%d connected=%d\n", en, conn);
    Serial.flush();
    return;
  }
  if (line == "BLEMAC?") {
    // Device BLE address - both boards advertise "Nimbus", so a client targets
    // the spare by ADDRESS, not scan-by-name.
    Serial.printf("BLEMAC %s\n", s_h.bleMac ? s_h.bleMac().c_str() : "(n/a)");
    Serial.flush();
    return;
  }
  if (line == "BONDS?") {
    // Bonded-central count (persists in NVS across reboot) + whether a pairing
    // is mid-flight. Asserts bonding survives reboot + Forget wipes it.
    int bonds = 0, pairing = 0;
    if (s_h.bleBonds) s_h.bleBonds(bonds, pairing);
    Serial.printf("BONDS count=%d pairing=%d\n", bonds, pairing);
    Serial.flush();
    return;
  }
  if (line == "FORGETBONDS") {
    if (s_h.bleForget) s_h.bleForget();
    Serial.println("BONDS cleared");
    Serial.flush();
    return;
  }
  if (line == "DEGHOST") {
    // Force the next panel push down the de-ghost (OTP full-update) path - the one
    // refresh that composites the 3-colour panel's red plane. Exercises the
    // stuck-red fix (driver-side red-RAM blank) on demand instead of a 15-min idle
    // wait; the render lands within the scheduler tick and logs
    // "epd: de-ghost full-update" to /api/log.
    if (s_h.deghost) { s_h.deghost(); Serial.println("DEGHOST scheduled"); }
    else Serial.println("DEGHOST unavailable (no hook)");
    Serial.flush();
    return;
  }
  if (line == "RAWFRAME?") {
    // 1 while the Active-posture Animator owns the ring via showFrame() (P-E).
    const int rf = (s_h.rawFrameActive && s_h.rawFrameActive()) ? 1 : 0;
    Serial.printf("RAWFRAME %d\n", rf);
    Serial.flush();
    return;
  }
  if (line.startsWith("NSNFEED ")) {
    // Feed a synthetic nsn frame (hex) through the SAME decoder/mapper/router
    // path a real BLE frame takes, so the notifier UI can be driven with no
    // broker and no BLE. The host builds the frame with the reference encoder
    // (../nsnotify notify/broker/frame.py), which keeps the wire format's
    // single source of truth - this end only moves bytes.
    //
    // ⚠ Needed because macOS BLE permissions block host-side bleak on this
    // bench, so without it the session-card UI can only ever be observed in its
    // EMPTY state.
    //
    // The frame is stamped with millis() at the moment the console pump runs,
    // which is LATER in the loop iteration than the cached now=millis() the
    // following tick() uses - so the stamp is a few ms in the future. That is
    // fine (Mapper::timeout compares signed), and it is the same reason real
    // BLE frames are safe: net::ble::drain() feeds the loop's own `now`.
    // ⚠ It was NOT fine while that comparison was unsigned: the tiny negative
    // wrapped to ~4.29e9 ms and the next tick expired every job, so an injected
    // frame vanished within one iteration and the cards never rendered. Fixed
    // in notifier_map.cpp; regression-tested by test_notifier's
    // test_timeout_ignores_a_frame_stamped_in_the_future.
    String hex = line.substring(8); hex.trim();
    if (hex.length() < 2 || (hex.length() % 2) != 0) {
      reply("ERR nsnfeed want an even-length hex string");
      return;
    }
    if (!s_h.nsnFeedByte) { reply("ERR nsnfeed unavailable (orchestrator mode?)"); return; }
    size_t n = 0;
    for (size_t i = 0; i + 1 < size_t(hex.length()); i += 2) {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
      if (hi < 0 || lo < 0) { reply("ERR nsnfeed non-hex character"); return; }
      s_h.nsnFeedByte(uint8_t((hi << 4) | lo));
      n++;
    }
    Serial.printf("NSNFEED< %u bytes jobs=%d\n", unsigned(n),
                  s_h.jobCount ? s_h.jobCount() : -1);
    Serial.flush();
    return;
  }
  if (line.startsWith("SCREEN ")) {
    // SCREEN <eink|tft>: persist the fitted panel + restart (screenModel is
    // boot-resolved, like MODE).
    //
    // ⚠ This exists because of a real bootstrap trap: a fresh TFT board boots
    // the DEFAULT "eink", drives the e-paper driver onto pins that have a TFT,
    // and the panel stays dark - including the setup screen that would have told
    // the owner the AP name and QR. Without a cable-only way to set this, the
    // recovery path is displayed on the very screen that cannot come up.
    String a = line.substring(7); a.trim();
    if (a != "eink" && a != "tft") { reply("ERR screen must be eink|tft"); return; }
    agent::store::setScreenModel(a);
    Serial.printf("SCREEN -> %s (persist + reboot)\n", a.c_str());
    Serial.flush();
    delay(120);
    ESP.restart();
    return;
  }
  if (line == "SCREEN?") {
    Serial.printf("SCREEN want=%s bound=%s\n",
                  agent::store::screenModel().c_str(),
                  (s_h.screenIsTft && s_h.screenIsTft()) ? "tft" : "eink");
    Serial.flush();
    return;
  }
  if (line.startsWith("MODE ")) {
    // MODE <0|1>: persist + restart (mode is boot-resolved). The harness's
    // ensure_mode() waits for the fresh READY beacon to confirm.
    const int m = line.substring(5).toInt();
    if (m == 0 || m == 1) {
      Serial.printf("MODE -> %d (persist + reboot)\n", m);
      Serial.flush();
      if (s_h.setMode) s_h.setMode(m);
    } else {
      reply("ERR mode must be 0|1");
    }
    return;
  }

  // Unknown: echo the first token so the harness can see what was rejected.
  int sp = line.indexOf(' ');
  String verb = sp < 0 ? line : line.substring(0, sp);
  Serial.printf("ERR unknown '%s'\n", verb.c_str());
  Serial.flush();
}

}  // namespace

void begin(const Hooks& h) { s_h = h; }

void ready() {
  const int mode = s_h.mode ? s_h.mode() : 0;
  const String ip = s_h.wifiIp ? s_h.wifiIp() : String("0.0.0.0");
  Serial.printf("READY mode=%d ip=%s\n", mode, ip.c_str());  // F11/F14 boot beacon
  Serial.flush();
}

void pumpOrch() {
  // Serial is otherwise idle in Orchestrator mode: read whole lines, dispatch.
  int budget = kByteBudget;
  while (Serial.available() > 0 && budget-- > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (s_line.length()) { dispatch(s_line); s_line = ""; }
    } else if (s_line.length() < kLineCap) {
      s_line += char(c);
    }
    // (chars past the cap are dropped until the next newline - matches the cap
    //  behavior of the existing orch mini-console.)
  }
}

void onRender(uint8_t screenId, uint8_t posture, int segCount, bool single,
              bool dark, uint8_t bright) {
  // Same schema as the RENDER? reply so L3 render tests can poll OR consume this
  // push stream deterministically (F2/F4). Emitted on every render.
  Serial.printf("RENDER screen=%d posture=%d seg=%d single=%d dark=%d bright=%d\n",
                int(screenId), int(posture), segCount, int(single), int(dark),
                int(bright));
  Serial.flush();
}

void onEncoder(const char* en) {
  if (!s_inputLog) return;                                  // F1 - gated by INPUTLOG
  Serial.printf("ENC %s\n", en);
  Serial.flush();
}

bool popInjectedEncoder(int& codeOut) {
  if (s_encTail == s_encHead) return false;                 // empty
  codeOut = s_encQ[s_encTail];
  s_encTail = (s_encTail + 1) % kEncQCap;
  return true;
}

bool popSaverCmd(int& minsOut) {
  if (!s_saverPending) return false;
  minsOut = s_saverCmd;
  s_saverPending = false;
  return true;
}

// One badge per failure EPISODE, not per retry: Arduino re-emits DISCONNECTED on
// every reconnect attempt (~3 s), and each re-armed badge made main.cpp repaint
// StatusIdle - invisible on the status screen (identical-frame skip) but a REAL
// full refresh whenever the Screensaver was up, flapping logo->status->logo at the
// retry cadence ("standby screen refreshes non stop with no WiFi", owner
// 2026-07-23). Latch after the first arm; a successful association re-arms.
static bool s_badgeLatched = false;

void onWifiReason(int reason) {
  if (!s_badgeLatched) { s_badge = true; s_badgeLatched = true; }
  s_badgeReason = reason;                                   // reason stays fresh for F9
  Serial.printf("WIFI_DISCONNECTED reason=%d\n", reason);   // every event - HIL parses these
  Serial.flush();
}

void onWifiGotIp(const String& ip) {
  s_badge = false;         // a successful association clears any pending auth-fail badge
  s_badgeLatched = false;  // ...and re-arms the one-shot for the NEXT failure episode
  Serial.printf("WIFI_GOT_IP %s\n", ip.c_str());            // F8
  Serial.flush();
}

bool errorBadgePending(int& reasonOut) {
  reasonOut = s_badgeReason;
  return s_badge;
}

void clearErrorBadge() { s_badge = false; }

}  // namespace nimbus::tc

#endif  // NIMBUS_TEST
