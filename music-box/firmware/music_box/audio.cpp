#include "audio.h"
#include "echo.h"
#include "remote_notes.h"

#include <Arduino.h>

#if __has_include("personal-melodies.h")
#include "personal-melodies.h"
#define MUSIC_BOX_PERSONAL_MELODIES
#endif

#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>

namespace music_box {
namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kBlockFrames = 256;
constexpr uint32_t kDmaBuffers = 6;
constexpr uint32_t kWriteTimeoutMs = 100;
constexpr uint32_t kFadeFrames = kSampleRate * 12 / 1000;
constexpr int32_t kPeakSample = 1 << 26;  // About 3.1% of signed 32-bit full scale.
constexpr uint32_t kEchoFrames = kSampleRate * kEchoDelayMs / 1000;
constexpr uint8_t kVolumePin = 34;  // ADC1, the inverted potentiometer in the Workshop sketches.
constexpr uint32_t kVolumePollMs = 25;
constexpr float kMaxMusicGain = 2.0f;  // Output peak remains at most 6.25% of full scale.
constexpr float kReadyGain = 0.23f;
constexpr unsigned kAdcMax = 4095;

// Original smoke-test jingle. The final rest exceeds the full DMA ring
// duration so the next serial prompt follows the last audible sample.
constexpr uint16_t kFinalRestMs = 60;
constexpr Note kJingle[] = {
    {392, 160}, {0, 35}, {523, 220}, {659, 160}, {587, 220},
    {0, 45}, {440, 180}, {784, 360}, {0, kFinalRestMs},
};
constexpr Note kReadyChime[] = {
    {784, 55}, {0, 130}, {784, 55}, {0, 130}, {784, 55},
    {0, 130}, {784, 55}, {0, 130}, {784, 55}, {0, kFinalRestMs},
};
static_assert(kFinalRestMs * kSampleRate / 1000 > kDmaBuffers * kBlockFrames);

i2s_chan_handle_t txChannel = nullptr;
int32_t samples[kBlockFrames][2];
TaskHandle_t audioTask = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<uint8_t> requestedDay{kTestJingle};
std::atomic<Effect> requestedEffect{Effect::dry};
std::atomic<bool> resultReady{false};
PlaybackResult result{};
ShortEcho<kEchoFrames> echo;
std::atomic<unsigned> volumeRaw{0};
unsigned long lastVolumeMs = 0;
Note downloadedNotes[kMaxRemoteNotes];
size_t downloadedCount = 0;

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

template <typename NoteType>
PlaybackResult playMelody(const NoteType* notes, size_t noteCount, Effect effect, bool quiet = false) {
  const bool useEcho = effect == Effect::echo;
  if (useEcho) {
    echo.reset();
  }
  bool stopping = false;
  uint32_t stopFadeLeft = kFadeFrames;
  float level = quiet ? kReadyGain : 0.0f;
  const auto writeTone = [&](uint16_t frequencyHz, uint32_t totalFrames) -> PlaybackResult {
    const float phaseStep = static_cast<float>(frequencyHz) / kSampleRate;
    float phase = 0.0f;

    for (uint32_t offset = 0; offset < totalFrames; offset += kBlockFrames) {
      stopping = stopping || stopRequested.load();
      const uint32_t frameCount = std::min(kBlockFrames, totalFrames - offset);
      const float target = quiet ? kReadyGain : kMaxMusicGain * volumeRaw.load() / kAdcMax;
      const float gainStep = (target - level) / kBlockFrames;
      for (uint32_t i = 0; i < frameCount; ++i) {
        const uint32_t frame = offset + i;
        const uint32_t edgeDistance = std::min(frame, totalFrames - 1 - frame);
        const float envelope =
            static_cast<float>(std::min(edgeDistance, kFadeFrames)) / kFadeFrames;
        const float triangle = 1.0f - 4.0f * std::abs(phase - 0.5f);
        const float stopGain = static_cast<float>(stopFadeLeft) / kFadeFrames;
        const float dry = frequencyHz == 0 ? 0.0f : kPeakSample * envelope * triangle;
        const float mixed = useEcho ? static_cast<float>(echo.mix(static_cast<int32_t>(dry))) : dry;
        level += gainStep;
        const float gain = std::clamp(level, 0.0f, kMaxMusicGain);
        const int32_t sample = static_cast<int32_t>(mixed * stopGain * gain);
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
      level = target;
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
    return {ESP_OK, false};
  };
  for (size_t i = 0; i < noteCount; ++i) {
    const auto note = notes[i];
    const auto written = writeTone(note.frequencyHz, kSampleRate * note.durationMs / 1000);
    if (written.error != ESP_OK || written.stopped) {
      return written;
    }
  }
  if (useEcho) {
    const auto tail = writeTone(0, kEchoFrames + kSampleRate * kFinalRestMs / 1000);
    if (tail.error != ESP_OK || tail.stopped) {
      return tail;
    }
  }
  return {ESP_OK, stopping};
}

void runAudio(void*) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const auto effect = requestedEffect.load();
    const auto day = requestedDay.load();
    if (day == kReadyTone) {
      result = playMelody(kReadyChime, std::size(kReadyChime), Effect::dry, true);
    } else if (downloadedCount) {
      result = playMelody(downloadedNotes, downloadedCount, effect);
    } else {
#ifdef MUSIC_BOX_PERSONAL_MELODIES
      const auto* personal = advent_music::findDay(day);
      if (personal) {
        result = playMelody(personal->notes, personal->noteCount, effect);
      } else
#endif
      {
        result = playMelody(kJingle, std::size(kJingle), effect);
      }
    }
    resultReady.store(true, std::memory_order_release);
  }
}

}  // namespace

esp_err_t startAudio() {
  analogReadResolution(12);
  analogSetPinAttenuation(kVolumePin, ADC_11db);
  volumeRaw.store(kAdcMax - std::min(kAdcMax, static_cast<unsigned>(analogRead(kVolumePin))));
  const esp_err_t status = initI2s();
  if (status != ESP_OK) {
    return status;
  }
  if (xTaskCreate(runAudio, "musicbox-audio", 3072, nullptr, 2, &audioTask) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void pollVolume() {
  const auto now = millis();
  if (now - lastVolumeMs < kVolumePollMs) {
    return;
  }
  lastVolumeMs = now;
  const auto raw = kAdcMax - std::min(kAdcMax, static_cast<unsigned>(analogRead(kVolumePin)));
  volumeRaw.store(raw < 48 ? 0 : raw > kAdcMax - 48 ? kAdcMax : raw);
}

unsigned volumePercent() {
  return (volumeRaw.load() * 100 + kAdcMax / 2) / kAdcMax;
}

int resolveMelody(std::string_view name) {
  if (name == "jingle1") {
    return kTestJingle;
  }
  if (name.size() == 5 && name.starts_with("day") &&
      name[3] >= '0' && name[3] <= '2' && name[4] >= '0' && name[4] <= '9') {
    const int day = (name[3] - '0') * 10 + name[4] - '0';
    return day >= 1 && day <= 24 ? day : -1;
  }
#if defined(MUSIC_BOX_PERSONAL_MELODIES) && defined(ADVENT_MUSIC_HAS_NAMES)
  const auto day = advent_music::dayForName(name);
  return day ? day : -1;
#else
  return -1;
#endif
}

bool hasMelody(uint8_t day) {
  if (day == kTestJingle || day == kReadyTone || hasRemoteNotes(day)) {
    return true;
  }
#ifdef MUSIC_BOX_PERSONAL_MELODIES
  return advent_music::findDay(day) != nullptr;
#else
  return false;
#endif
}

void startPlayback(uint8_t day, Effect effect) {
  downloadedCount = loadRemoteNotes(day, downloadedNotes);
  requestedDay.store(day);
  requestedEffect.store(effect);
  stopRequested.store(false);
  xTaskNotifyGive(audioTask);
}

void stopPlayback() {
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
