#pragma once

#include <string>
#include <vector>

// One StarDict dictionary found under /dictionaries or /.dictionaries: a
// subfolder holding <stem>.idx plus <stem>.dict or <stem>.dict.dz.
struct DictionaryEntry {
  std::string name;  // subfolder name (shown to the user, stored in settings)
  std::string stem;  // index basename without .idx
};

namespace DictionaryRegistry {

// Scan /dictionaries/*/ and /.dictionaries/*/ for dictionaries. Folders with
// multiple index stems are ambiguous and skipped. Result is sorted
// case-insensitively by name.
void discover(std::vector<DictionaryEntry>& out);

// Resolve a folder name to its extensionless base path
// ("/dictionaries/<folder>/<stem>" or "/.dictionaries/<folder>/<stem>").
// Returns false if the folder holds no usable dictionary in either root.
bool resolveBasePath(const char* folderName, std::string& basePathOut);

// --- Install support (DictionaryDownloadActivity) ---------------------------
//
// Names and filenames here come off a network catalog, so both validators are
// mandatory before either value is concatenated into an SD path. They reject
// anything that is not a plain basename: separators, "..", leading dots and
// any character outside [A-Za-z0-9._-].

// Validate a catalog dictionary id used as the destination folder name. Also
// enforces that it fits CrossPointSettings::dictionaryName, so anything that
// passes can be stored as the active dictionary without truncation.
bool isValidName(const char* name);

// Validate a catalog filename ("english.dict.dz"). Multiple dots are allowed
// (".dict.dz" is two extensions); a leading dot is not.
bool isValidFileName(const char* name);

// Resolve the folder a dictionary should be written to, creating it if needed.
// An existing install keeps its root (so an update to a dictionary the user
// keeps under /.dictionaries stays hidden); anything new lands in
// /dictionaries. Returns false if the directory could not be created.
bool ensureInstallDir(const char* name, std::string& dirOut);

// Delete a dictionary folder and every file in it, in whichever root holds it.
// Returns false only when the folder exists and could not be fully removed.
bool removeDictionary(const char* name);

}  // namespace DictionaryRegistry
