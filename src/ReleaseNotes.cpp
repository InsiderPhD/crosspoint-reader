#include "ReleaseNotes.h"

#include "CrossPointState.h"

namespace ReleaseNotes {

const Item ITEMS[] = {
    {StrId::STR_NOTES_2_0_SYNC_BOOKS_TITLE, StrId::STR_NOTES_2_0_SYNC_BOOKS_BODY},
    {StrId::STR_NOTES_2_0_SYNC_STATS_TITLE, StrId::STR_NOTES_2_0_SYNC_STATS_BODY},
    {StrId::STR_NOTES_2_0_CROSSPOINT_SYNC_TITLE, StrId::STR_NOTES_2_0_CROSSPOINT_SYNC_BODY},
    {StrId::STR_NOTES_2_0_SLEEP_THEMES_TITLE, StrId::STR_NOTES_2_0_SLEEP_THEMES_BODY},
    {StrId::STR_NOTES_2_0_STATS_TITLE, StrId::STR_NOTES_2_0_STATS_BODY},
    {StrId::STR_NOTES_2_0_SWAP_TITLE, StrId::STR_NOTES_2_0_SWAP_BODY},
    {StrId::STR_NOTES_2_0_LAYOUT_TITLE, StrId::STR_NOTES_2_0_LAYOUT_BODY},
};
const size_t ITEM_COUNT = sizeof(ITEMS) / sizeof(ITEMS[0]);

bool shouldShow() { return APP_STATE.kpReleaseNotesSeen < RELEASE_NOTES_REVISION; }

void markSeen() {
  if (APP_STATE.kpReleaseNotesSeen >= RELEASE_NOTES_REVISION) return;
  APP_STATE.kpReleaseNotesSeen = RELEASE_NOTES_REVISION;
  APP_STATE.saveToFile();
}

}  // namespace ReleaseNotes
