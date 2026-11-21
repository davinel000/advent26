#include "audio.h"

#include <driver/i2s_std.h>

#include <algorithm>
#include <cmath>

namespace music_box {
namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kBlockFrames = 256;
constexpr uint32_t kDmaBuffers = 6;
constexpr uint32_t kWriteTimeoutMs = 100;
constexpr uint32_t kFadeFrames = kSampleRate * 12 / 1000;
constexpr int32_t kPeakSample = 1 << 26;  // About 3.1% of signed 32-bit full scale.

struct Note {
  uint16_t frequencyHz;  // Zero produces silence.
  uint16_t durationMs;
};

// Original smoke-test jingle. The final rest exceeds the full DMA ring
// duration so the next serial prompt follows the last audible sample.
constexpr uint16_t kFinalRestMs = 60;
constexpr Note kJingle[] = {
    {392, 160}, {0, 35}, {523, 220}, {659, 160}, {587, 220},
    {0, 45}, {440, 180}, {784, 360}, {0, kFinalRestMs},
};
static_assert(kFinalRestMs * kSampleRate / 1000 > kDmaBuffers * kBlockFrames);

i2s_chan_handle_t txChannel = nullptr;
int32_t samples[kBlockFrames][2];

}  // namespace

esp_err_t startAudio() {
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  channelConfig.dma_desc_num = kDmaBuffers;
  channelConfig.dma_frame_num = kBlockFrames;
  channelConfig.auto_clear_after_cb = true;  // Send zeros when no jingle is queued.

  esp_err_t result = i2s_new_channel(&channelConfig, &txChannel, nullptr);
  if (result != ESP_OK) {
    return result;
  }

  const i2s_std_config_t config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                     I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = GPIO_NUM_26,
          .ws = GPIO_NUM_25,
          .dout = GPIO_NUM_22,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {},
      },
  };

  result = i2s_channel_init_std_mode(txChannel, &config);
  if (result == ESP_OK) {
    result = i2s_channel_enable(txChannel);
  }
  if (result != ESP_OK) {
    i2s_del_channel(txChannel);
    txChannel = nullptr;
  }
  return result;
}

esp_err_t playJingle() {
  for (const Note& note : kJingle) {
    const uint32_t totalFrames = kSampleRate * note.durationMs / 1000;
    const float phaseStep = static_cast<float>(note.frequencyHz) / kSampleRate;
    float phase = 0.0f;

    for (uint32_t offset = 0; offset < totalFrames; offset += kBlockFrames) {
      const uint32_t frameCount = std::min(kBlockFrames, totalFrames - offset);
      for (uint32_t i = 0; i < frameCount; ++i) {
        const uint32_t frame = offset + i;
        const uint32_t edgeDistance = std::min(frame, totalFrames - 1 - frame);
        const float envelope =
            static_cast<float>(std::min(edgeDistance, kFadeFrames)) / kFadeFrames;
        const float triangle = 1.0f - 4.0f * std::abs(phase - 0.5f);
        const int32_t sample =
            note.frequencyHz == 0 ? 0 : static_cast<int32_t>(kPeakSample * envelope * triangle);
        samples[i][0] = sample;
        samples[i][1] = sample;

        phase += phaseStep;
        if (phase >= 1.0f) {
          phase -= 1.0f;
        }
      }

      const size_t bytes = frameCount * sizeof(samples[0]);
      size_t written = 0;
      const esp_err_t result = i2s_channel_write(txChannel, samples, bytes, &written,
                                                 kWriteTimeoutMs);
      if (result != ESP_OK) {
        return result;
      }
      if (written != bytes) {
        return ESP_ERR_INVALID_SIZE;
      }
    }
  }
  return ESP_OK;
}

}  // namespace music_box
