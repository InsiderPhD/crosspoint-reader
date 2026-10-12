#pragma once

#include <string>

// The two "use this for..." choices kept in CrossPointState, shared by every
// screen that offers them (file browser lists, Book Info, the image viewer) so
// they all write the same fields with the same side effects.
namespace SleepBootImages {

// Makes `path` the folder the Custom sleep screen picks from; empty restores
// the default /.sleep, /sleep lookup. Choosing a folder also switches a sleep
// screen mode that would ignore it to Custom.
void setSleepFolder(const std::string& path);

// Makes `path` (a BMP) the image shown at boot; empty restores the built-in logo.
void setBootImage(const std::string& path);

}  // namespace SleepBootImages
