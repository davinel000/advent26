#include "remote_notes.h"

#include <mbedtls/base64.h>
#include <nvs.h>

#include <cstdio>
#include <cstring>

namespace music_box {
namespace {

constexpr uint32_t kMaxDurationMs = 60000;
constexpr unsigned long kUploadTimeoutMs = 300000;
static_assert(sizeof(Note) == 4);

nvs_handle_t notesHandle = 0;
bool notesReady = false;
Note staged[kMaxRemoteNotes];
uint32_t stagedTransfer = 0;
uint8_t stagedDay = 0;
size_t stagedSize = 0;
size_t stagedCount = 0;
unsigned long lastPart = 0;

bool validDay(uint8_t day) {
  return day >= 1 && day <= 24;
}

void keyForDay(uint8_t day, char (&key)[6]) {
  snprintf(key, sizeof(key), "day%02u", static_cast<unsigned>(day));
}

bool validSize(size_t size) {
  return size >= 2 * sizeof(Note) && size <= sizeof(staged) && size % sizeof(Note) == 0;
}

bool validNotes(const Note* notes, size_t count) {
  uint32_t duration = 0;
  bool hasPitch = false;
  for (size_t i = 0; i < count; ++i) {
    const auto& note = notes[i];
    if (!note.durationMs || note.durationMs > 10000 ||
        (note.frequencyHz && (note.frequencyHz < 80 || note.frequencyHz > 2500))) {
      return false;
    }
    duration += note.durationMs;
    hasPitch |= note.frequencyHz != 0;
  }
  return hasPitch && duration <= kMaxDurationMs &&
         notes[count - 1].frequencyHz == 0 && notes[count - 1].durationMs >= 60;
}

}  // namespace

void startRemoteNotes() {
  notesReady = nvs_open("musicbox-notes", NVS_READWRITE, &notesHandle) == ESP_OK;
}

bool hasRemoteNotes(uint8_t day) {
  if (!notesReady || !validDay(day)) {
    return false;
  }
  char key[6];
  keyForDay(day, key);
  size_t bytes = 0;
  return nvs_get_blob(notesHandle, key, nullptr, &bytes) == ESP_OK && validSize(bytes);
}

size_t loadRemoteNotes(uint8_t day, Note* destination) {
  if (!notesReady || !validDay(day)) {
    return 0;
  }
  char key[6];
  keyForDay(day, key);
  size_t bytes = sizeof(staged);
  if (nvs_get_blob(notesHandle, key, destination, &bytes) != ESP_OK || !validSize(bytes)) {
    return 0;
  }
  const size_t count = bytes / sizeof(Note);
  return validNotes(destination, count) ? count : 0;
}

const char* applyNotesCommand(uint8_t day, const NotesCommand& command, unsigned long now) {
  if (!notesReady) {
    return "notes_storage_unavailable";
  }
  if (!validDay(day) || !command.transfer) {
    return "invalid_note_slot";
  }
  if (command.phase == NotesPhase::restore) {
    char key[6];
    keyForDay(day, key);
    const auto result = nvs_erase_key(notesHandle, key);
    if (result != ESP_ERR_NVS_NOT_FOUND &&
        (result != ESP_OK || nvs_commit(notesHandle) != ESP_OK)) {
      return "notes_save_failed";
    }
    if (stagedDay == day) {
      stagedTransfer = 0;
    }
    return nullptr;
  }
  if (command.phase == NotesPhase::begin) {
    if (!validSize(command.position)) {
      return "invalid_note_size";
    }
    stagedTransfer = command.transfer;
    stagedDay = day;
    stagedSize = command.position;
    stagedCount = 0;
    lastPart = now;
    return nullptr;
  }
  if (day != stagedDay || command.transfer != stagedTransfer || now - lastPart > kUploadTimeoutMs) {
    return "no_active_upload";
  }
  if (command.phase == NotesPhase::append) {
    if (command.position != stagedCount) {
      return "invalid_note_offset";
    }
    size_t decoded = 0;
    auto* out = reinterpret_cast<unsigned char*>(staged) + stagedCount;
    if (mbedtls_base64_decode(out, stagedSize - stagedCount, &decoded,
                             reinterpret_cast<const unsigned char*>(command.data),
                             strlen(command.data)) != 0 || !decoded) {
      return "invalid_note_chunk";
    }
    stagedCount += decoded;
    lastPart = now;
    return nullptr;
  }
  if (stagedCount != stagedSize || !validNotes(staged, stagedCount / sizeof(Note))) {
    return "invalid_note_data";
  }
  char key[6];
  keyForDay(day, key);
  if (nvs_set_blob(notesHandle, key, staged, stagedSize) != ESP_OK || nvs_commit(notesHandle) != ESP_OK) {
    return "notes_save_failed";
  }
  stagedTransfer = 0;
  return nullptr;
}

}  // namespace music_box
