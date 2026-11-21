#include "audio.h"

namespace {

constexpr unsigned long kSerialBaud = 115200;
bool audioReady = false;

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);

  const esp_err_t result = music_box::startAudio();
  audioReady = result == ESP_OK;
  if (audioReady) {
    Serial.println("Ready. Send p to play the test jingle.");
  } else {
    Serial.printf("I2S setup failed (%s). Reset to retry.\n", esp_err_to_name(result));
  }
}

void loop() {
  if (Serial.read() != 'p' || !audioReady) {
    delay(1);
    return;
  }

  Serial.println("Playing test jingle.");
  const esp_err_t result = music_box::playJingle();

  // Drop characters buffered during this test instead of queuing more plays.
  for (int pending = Serial.available(); pending > 0; --pending) {
    Serial.read();
  }

  if (result == ESP_OK) {
    Serial.println("Done. Send p to play again.");
  } else {
    audioReady = false;
    Serial.printf("I2S write failed (%s). Reset to retry.\n", esp_err_to_name(result));
  }
}
