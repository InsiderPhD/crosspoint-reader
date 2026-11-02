#pragma once

#include <I18nKeys.h>

#include <cstddef>
#include <cstdint>

// What's New: the user-facing highlights of this release, compiled into flash
// (the table is constexpr data; the text is ordinary tr() strings).
//
// Bump RELEASE_NOTES_REVISION whenever the notes change for a release. Anyone
// whose APP_STATE.kpReleaseNotesSeen (state.json "kpReleaseNotesSeen") is
// lower sees the screen once after the update; Settings > System > Release
// Notes reopens it any time.
namespace ReleaseNotes {

constexpr uint16_t RELEASE_NOTES_REVISION = 1;
// Shown in the header. Not CROSSPOINT_VERSION, so an rc or patch build keeps
// the label of the release the notes describe.
constexpr char RELEASE_NOTES_VERSION[] = "2.0";

struct Item {
  StrId title;
  StrId body;
};

extern const Item ITEMS[];
extern const size_t ITEM_COUNT;

// True once per revision: the notes have not been shown on this card yet.
bool shouldShow();
// Record the current revision as seen (one state.json write, only if it changes).
void markSeen();

}  // namespace ReleaseNotes
