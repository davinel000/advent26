#pragma once

#include <esp_err.h>
#include <stdint.h>

namespace music_box {

enum class Effect : uint8_t { dry, echo };
constexpr uint8_t kTestJingle = 0;

struct PlaybackResult {
  esp_err_t error;
  bool stopped;
};

esp_err_t startAudio();
bool hasMelody(uint8_t day);
void startPlayback(uint8_t day, Effect effect);
void stopPlayback();
bool takePlaybackResult(PlaybackResult& result);

}  // namespace music_box
