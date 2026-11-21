#include "audio.h"

#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

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
TaskHandle_t audioTask = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<bool> resultReady{false};
PlaybackResult result{};

esp_err_t initI2s() {
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

esp_err_t writeFrames(size_t frameCount) {
  const size_t bytes = frameCount * sizeof(samples[0]);
  size_t written = 0;
  const esp_err_t status = i2s_channel_write(txChannel, samples, bytes, &written, kWriteTimeoutMs);
  if (status != ESP_OK) {
    return status;
  }
  return written == bytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

PlaybackResult playJingle() {
  bool stopping = false;
  uint32_t stopFadeLeft = kFadeFrames;
  for (const Note& note : kJingle) {
    const uint32_t totalFrames = kSampleRate * note.durationMs / 1000;
    const float phaseStep = static_cast<float>(note.frequencyHz) / kSampleRate;
    float phase = 0.0f;

    for (uint32_t offset = 0; offset < totalFrames; offset += kBlockFrames) {
      stopping = stopping || stopRequested.load();
      const uint32_t frameCount = std::min(kBlockFrames, totalFrames - offset);
      for (uint32_t i = 0; i < frameCount; ++i) {
        const uint32_t frame = offset + i;
        const uint32_t edgeDistance = std::min(frame, totalFrames - 1 - frame);
        const float envelope =
            static_cast<float>(std::min(edgeDistance, kFadeFrames)) / kFadeFrames;
        const float triangle = 1.0f - 4.0f * std::abs(phase - 0.5f);
        const float stopGain = static_cast<float>(stopFadeLeft) / kFadeFrames;
        const int32_t sample = note.frequencyHz == 0
                                   ? 0
                                   : static_cast<int32_t>(kPeakSample * envelope * triangle * stopGain);
        if (stopping && stopFadeLeft > 0) {
          --stopFadeLeft;
        }
        samples[i][0] = sample;
        samples[i][1] = sample;

        phase += phaseStep;
        if (phase >= 1.0f) {
          phase -= 1.0f;
        }
      }

      const esp_err_t status = writeFrames(frameCount);
      if (status != ESP_OK) {
        return {status, stopping};
      }
      if (stopping && stopFadeLeft == 0) {
        // Silence for a full DMA ring lets the last faded block finish before reporting stopped.
        memset(samples, 0, sizeof(samples));
        for (uint32_t i = 0; i < kDmaBuffers; ++i) {
          const esp_err_t drain = writeFrames(kBlockFrames);
          if (drain != ESP_OK) {
            return {drain, true};
          }
        }
        return {ESP_OK, true};
      }
    }
  }
  return {ESP_OK, stopping};
}

void runAudio(void*) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    result = playJingle();
    resultReady.store(true, std::memory_order_release);
  }
}

}  // namespace

esp_err_t startAudio() {
  const esp_err_t status = initI2s();
  if (status != ESP_OK) {
    return status;
  }
  if (xTaskCreate(runAudio, "musicbox-audio", 3072, nullptr, 2, &audioTask) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void startJingle() {
  stopRequested.store(false);
  xTaskNotifyGive(audioTask);
}

void stopJingle() {
  stopRequested.store(true);
}

bool takePlaybackResult(PlaybackResult& completed) {
  if (!resultReady.exchange(false, std::memory_order_acquire)) {
    return false;
  }
  completed = result;
  return true;
}

}  // namespace music_box
