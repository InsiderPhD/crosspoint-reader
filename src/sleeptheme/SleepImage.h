#pragma once

#include <string>

// The image a theme's sleep.image value shows: what the old Custom sleep
// screen drew. In order, a random image from the folder chosen in the file
// browser (APP_STATE.preferredSleepFolderPath), /sleep.bmp, then a random
// image from /.sleep or else /sleep. Random picks avoid recently shown images
// (history in APP_STATE).
namespace SleepImage {

// Path of the image to show, or false when there is none. Records a random
// pick in APP_STATE's history, so call it once per sleep.
bool pickPath(std::string& outPath);

}  // namespace SleepImage
