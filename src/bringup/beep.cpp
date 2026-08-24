// Minimal beep - the smallest possible speaker test. NOTHING but the board init
// and a tone loop: no WiFi, BLE, display, LED, tasks, web server. If THIS is silent,
// the fault is the amp/speaker/wiring/5V; if it beeps, something in the full Nimbus
// firmware is muting the amp.  Flash:  pio run -e beep -t upload
#include <Arduino.h>
#include <math.h>

#include <solide/solide.h>
#include <solide/audio.h>

static int16_t g_tone[6400];   // 0.4 s @ 16 kHz

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== MINIMAL BEEP TEST ===");
  Serial.println("board init...");
  solide::begin();
  const uint32_t rate = 16000;
  for (size_t i = 0; i < 6400; i++)
    g_tone[i] = (int16_t)(24000.0 * sinf(2.0 * M_PI * 880.0 * i / rate));
  Serial.println("beeping every 2 s at 880 Hz for the first 10 s, then SILENT - LISTEN.");
}

// BOUNDED: beep every 2 s only for the first BEEP_WINDOW_MS of uptime (~5 beeps),
// then go permanently silent (idle heartbeat). Never runs the amp past the window.
void loop() {
  static const uint32_t BEEP_WINDOW_MS = 10000;
  static uint32_t beeps = 0;
  static bool done = false;
  if (millis() < BEEP_WINDOW_MS) {
    bool ok = solide::audio::playPcm(g_tone, 6400, 16000);
    Serial.printf("beep %u played=%d  (heap=%u)\n", (unsigned)++beeps, (int)ok, (unsigned)ESP.getFreeHeap());
    delay(2000);
  } else {
    if (!done) { done = true; Serial.println("beep window done (>10 s) - idle, silent."); }
    delay(1000);   // quiet from here on; nothing touches the speaker
  }
}
