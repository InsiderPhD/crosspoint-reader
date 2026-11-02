#pragma once

#include <string.h>
#include <strings.h>

#include <string>

// Files no web request (file manager, WebDAV, plugin endpoints) may read or
// write: they hold obfuscated passwords and tokens that must stay on the
// device. The file manager's own checks only look at the LAST path segment, so
// "/.crosspoint/wifi.json" (last segment "wifi.json") passed them. Adapted from
// upstream CrossPoint #3824 (ProtectedPaths.h), extended to this fork's stores.
namespace protectedpaths {

// Prefix match on the canonical path, so each store's .json, legacy .bin and
// .tmp/.bak siblings are all covered. settings.* carries the OPDS credentials.
inline constexpr const char* SENSITIVE_PREFIXES[] = {
    "/.crosspoint/wifi.",       "/.crosspoint/koreader.", "/.crosspoint/hardcover.",
    "/.crosspoint/bookfusion.", "/.crosspoint/settings.", "/.crosspoint/katiepoint-settings.",
    "/.crosspoint/opds.",
};

// SdFat also opens a name by its generated 8.3 alias (".crosspoint" is
// CROSSP~1), which no spelling check can map back to the long name.
inline bool isShortAlias(const char* first, const char* last) {
  const char* tilde = static_cast<const char*>(memchr(first, '~', static_cast<size_t>(last - first)));
  if (!tilde || tilde + 1 >= last || tilde[1] < '0' || tilde[1] > '9') return false;
  const char* dot = static_cast<const char*>(memchr(first, '.', static_cast<size_t>(last - first)));
  const size_t base = static_cast<size_t>((dot ? dot : last) - first);
  const size_t ext = dot ? static_cast<size_t>(last - dot - 1) : 0;
  return base <= 8 && ext <= 3;
}

inline bool isSensitivePath(const char* path) {
  // Canonicalise first ("//", "/./" and ".." would otherwise dodge the prefix
  // match while still opening the same file).
  std::string canon;
  canon.reserve(strlen(path) + 1);
  int depth = 0;
  for (const char* seg = path; *seg;) {
    while (*seg == '/') seg++;
    const char* end = strchr(seg, '/');
    const size_t len = end ? static_cast<size_t>(end - seg) : strlen(seg);
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      const size_t slash = canon.rfind('/');
      canon.erase(slash == std::string::npos ? 0 : slash);
      if (depth > 0) depth--;
    } else {
      // SdFat skips leading spaces and drops trailing dots and spaces when it
      // opens a name, so compare the name it would actually open.
      const char* first = seg;
      const char* last = seg + len;
      while (first < last && *first == ' ') first++;
      while (last > first && (last[-1] == '.' || last[-1] == ' ')) last--;
      // The stores sit two levels deep; an alias further down cannot reach one.
      if (depth < 2 && isShortAlias(first, last)) return true;
      if (first < last) {
        canon += '/';
        canon.append(first, static_cast<size_t>(last - first));
        depth++;
      }
    }
    seg += len;
  }
  for (const char* prefix : SENSITIVE_PREFIXES) {
    // FAT names are case-insensitive.
    if (strncasecmp(canon.c_str(), prefix, strlen(prefix)) == 0) return true;
  }
  return false;
}

}  // namespace protectedpaths
