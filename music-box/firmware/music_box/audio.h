#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <string_view>

namespace music_box {

enum class Effect : uint8_t { dry, echo };
constexpr Effect kDefaultEffect = Effect::echo;
constexpr uint8_t kTestJingle = 0;
constexpr uint8_t kReadyTone = 25;

struct Note {
  uint16_t frequencyHz;
  uint16_t durationMs;
};

struct PlaybackResult {
  esp_err_t error;
  bool stopped;
};

esp_err_t startAudio();
void pollVolume();
unsigned volumePercent();
int resolveMelody(std::string_view name);
bool hasMelody(uint8_t day);
void startPlayback(uint8_t day, Effect effect);
void stopPlayback();
bool takePlaybackResult(PlaybackResult& result);

}  // namespace music_box
