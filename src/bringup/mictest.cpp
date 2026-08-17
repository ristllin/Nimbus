// Minimal I2S mic bring-up as a VISUAL VU meter - verifies the INMP441/ICS-43434
// read path WITHOUT relying on serial (the V0.1 board's USB-CDC drops app output)
// and WITHOUT the full firmware (stuck on the board's SD/boot bring-up).
//   boot           -> blue sweep around the ring (confirms ring + 5 V bus work)
//   speak / tap mic -> ring lights GREEN, more LEDs = louder (mic is reading)
//   mic returns 0   -> ring goes RED (i2s init/wiring problem)
// Serial still prints rms best-effort. Flash: pio run -e mictest -t upload
#include <Arduino.h>
#include <math.h>

#include <solide/solide.h>
#include <solide/audio.h>
#include <solide/leds.h>
#include <solide/ring.h>

static const int N = 45;
static solide::ring::RGB g_frame[N];
static int16_t g_buf[4800];   // 300 ms @ 16 kHz - short bursts = responsive meter

static void ring(int lit, uint8_t r, uint8_t g, uint8_t b) {
  if (lit < 0) lit = 0; if (lit > N) lit = N;
  for (int i = 0; i < N; i++)
    g_frame[i] = (i < lit) ? solide::ring::RGB{r, g, b} : solide::ring::RGB{0, 0, 0};
  solide::leds::showFrame(g_frame, N);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== I2S MIC VU METER (SCK=15 WS=18 SD=16) ===");
  solide::begin();
  solide::leds::begin();
  solide::leds::setBrightness(40);
  for (int k = 0; k <= N; k++) { ring(k, 0, 0, 120); delay(18); }   // blue startup sweep
  delay(300);
  ring(0, 0, 0, 0);
  Serial.println("speak/tap the mic -> ring lights GREEN. RED ring = no mic data.");
}

void loop() {
  size_t n = solide::audio::recordToBuffer(g_buf, 4800, 300, nullptr);
  if (n == 0) { ring(N, 45, 0, 0); Serial.println("mic: NO DATA (i2s/wiring)"); delay(200); return; }
  double sq = 0;
  for (size_t i = 0; i < n; i++) { int v = g_buf[i]; sq += (double)v * v; }
  int rms = (int)sqrt(sq / n);
  int lit = rms / 40; if (lit < 1) lit = 1;   // always show >=1 LED when reading
  ring(lit, 0, 180, 40);
  Serial.printf("mic rms=%d lit=%d\n", rms, lit);
  delay(60);
}
