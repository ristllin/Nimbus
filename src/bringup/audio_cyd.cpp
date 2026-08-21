// Freenove ESP32-S3 CYD audio bring-up - ES8311 codec (speaker + mic), isolated.
//
// No Nimbus, no console dependency: the SPEAKER is the readout. Each loop:
//   1) plays a 440 Hz beep  -> proves the codec DAC + speaker + I2S TX path;
//   2) records ~1.5 s then plays it back -> proves the codec ADC + mic + I2S RX
//      (speak during the record and you hear yourself a moment later).
// So a working board beeps, then echoes your voice. Silence = the codec register
// init, the I2S clocking, or the amp is wrong - bisect with the beep vs the echo.
//
//   pio run -e audio-cyd -t upload --upload-port <freenove usbmodem>
#include <Arduino.h>
#include <math.h>

#include "solide/audio.h"

using namespace solide;

static constexpr int kRate = 16000;
static int16_t g_tone[kRate / 4];          // 250 ms
static int16_t g_rec[kRate * 3 / 2];       // 1.5 s capture

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[audio-cyd] ES8311 beep + loopback bring-up");
  for (int i = 0; i < kRate / 4; ++i)
    g_tone[i] = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * 440.0f * i / kRate));
  audio::begin();
  audio::setVolume(0.7f);
}

void loop() {
  Serial.println("[audio-cyd] beep");
  audio::playPcm(g_tone, kRate / 4, kRate);
  delay(400);

  Serial.println("[audio-cyd] recording ~1.5s - say something...");
  const size_t got = audio::recordToBuffer(g_rec, kRate * 3 / 2, 1500, nullptr);
  Serial.printf("[audio-cyd] captured %u samples; playing back\n", (unsigned)got);
  if (got) audio::playPcm(g_rec, got, kRate);
  delay(800);
}
