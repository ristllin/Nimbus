// ledring - the smallest possible LED-ring data-path test. Nothing else boots:
// no Wi-Fi, no BLE, no display, no solide begin(). Just GPIO 21 driving 45
// WS2812B pixels, forever, two independent ways:
//
//   method A  Adafruit_NeoPixel (the production driver's own library + RMT path)
//             color-wipes RED -> GREEN -> BLUE -> WHITE, then all-off
//   method B  the Arduino core's neopixelWrite() (a SECOND, independent RMT
//             writer) blinks pixel 0 magenta - if A is dark but B lights one
//             pixel, the library/init is at fault; if BOTH are dark, the fault
//             is electrical (power, common ground, DIN wiring, or the strip)
//
// Progress prints on BOTH consoles every cycle (native USB + UART0), so any
// plugged cable shows life regardless of which port you're on.
//
// Bench notes (the two classics this sketch cannot fix):
//   * COMMON GROUND - a ring on a bench supply must share GND with the ESP;
//     the data line is referenced to it. Separate grounds = dark ring with
//     perfect 5 V and perfect data.
//   * 3.3 V data vs a stiff 5.00 V supply - WS2812B VIH is 0.7*VDD = 3.5 V,
//     above the S3's 3.3 V. Many strips tolerate it; some batches do not.
//     Dial the bench to ~4.5 V and the threshold falls below 3.3 V.
//
//   pio run -e ledring -t upload
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

static constexpr int      kPin   = 21;   // board_solide_s3.h led.din
static constexpr uint16_t kCount = 45;   // board_solide_s3.h led.count

static Adafruit_NeoPixel strip(kCount, kPin, NEO_GRB + NEO_KHZ800);
static uint32_t cycle = 0;

static void say(const char* msg) {
  Serial.println(msg);    // native USB (usbmodem)
  Serial0.println(msg);   // UART0 (the FTDI adapter)
}

static void wipe(uint8_t r, uint8_t g, uint8_t b, const char* name) {
  char line[64];
  snprintf(line, sizeof line, "  A: wipe %s", name);
  say(line);
  for (uint16_t i = 0; i < kCount; ++i) {
    strip.setPixelColor(i, strip.Color(r, g, b));
    strip.show();
    delay(12);
  }
  delay(600);
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(1500);            // let a just-enumerated USB console attach
  say("");
  say("=== ledring bring-up: GPIO 21 -> 45x WS2812B ===");
  say("watch the ring: A = full color wipes, B = pixel 0 magenta blink");
  strip.begin();
  strip.setBrightness(40);   // low - bench-friendly, same level as LEDTEST
  strip.clear();
  strip.show();
}

void loop() {
  char head[64];
  snprintf(head, sizeof head, "cycle %lu", (unsigned long)++cycle);
  say(head);

  // --- method A: the library the production driver uses -------------------
  wipe(255, 0, 0, "RED");
  wipe(0, 255, 0, "GREEN");
  wipe(0, 0, 255, "BLUE");
  wipe(255, 255, 255, "WHITE");
  say("  A: off");
  strip.clear();
  strip.show();
  delay(400);

  // --- method B: the core's own writer, pixel 0 only ----------------------
  say("  B: pixel0 magenta blink x3 (independent RMT path)");
  for (int i = 0; i < 3; ++i) {
    neopixelWrite(kPin, 40, 0, 40);
    delay(350);
    neopixelWrite(kPin, 0, 0, 0);
    delay(350);
  }

  say("  (verdict: A+B dark w/ common GND = electrical; B-only = library init)");
  delay(800);
}
