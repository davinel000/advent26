#pragma once

#include "audio.h"

#include <stddef.h>

namespace music_box {

constexpr size_t kMaxRemoteNotes = 512;
constexpr size_t kMaxChunkText = 72;

enum class NotesPhase : uint8_t { begin, append, commit, restore };

struct NotesCommand {
  uint32_t transfer;
  NotesPhase phase;
  uint16_t position;
  char data[kMaxChunkText + 1];
};

void startRemoteNotes();
bool hasRemoteNotes(uint8_t day);
size_t loadRemoteNotes(uint8_t day, Note* destination);
const char* applyNotesCommand(uint8_t day, const NotesCommand& command, unsigned long now);

}  // namespace music_box
