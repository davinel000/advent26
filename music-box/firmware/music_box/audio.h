#pragma once

#include <esp_err.h>

namespace music_box {

struct PlaybackResult {
  esp_err_t error;
  bool stopped;
};

esp_err_t startAudio();
void startJingle();
void stopJingle();
bool takePlaybackResult(PlaybackResult& result);

}  // namespace music_box
