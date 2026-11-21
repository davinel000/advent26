#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>

namespace music_box {

constexpr uint32_t kEchoDelayMs = 200;

// A single repeat. Three quarters dry plus one quarter delayed preserves
// the dry signal's peak bound even when both signals line up.
template <size_t DelayFrames>
class ShortEcho {
 public:
  void reset() {
    history_.fill(0);
    position_ = 0;
  }

  int32_t mix(int32_t dry) {
    constexpr int32_t kHistoryScale = 4096;
    const int32_t delayed = static_cast<int32_t>(history_[position_]) * kHistoryScale;
    history_[position_] = static_cast<int16_t>(dry / kHistoryScale);
    if (++position_ == DelayFrames) {
      position_ = 0;
    }
    return dry * 3 / 4 + delayed / 4;
  }

 private:
  std::array<int16_t, DelayFrames> history_{};
  size_t position_ = 0;
};

}  // namespace music_box
