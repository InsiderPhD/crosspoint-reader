#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/settings_compat"
BINARY="$BUILD_DIR/SettingsCompatTest"

# ArduinoJson is header-only; use the copy PlatformIO fetched for any env.
ARDUINOJSON_DIR="$(find "$ROOT_DIR/.pio/libdeps" -maxdepth 3 -type d -path '*/ArduinoJson/src' 2>/dev/null | head -n 1)"
if [[ -z "$ARDUINOJSON_DIR" ]]; then
  echo "ArduinoJson not found under .pio/libdeps; run 'pio run -e simulator' (or any env) once first." >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"

c++ -std=c++20 -O2 -Wall -Wextra -DSETTINGS_COMPAT_HOST_TEST \
  -I"$ARDUINOJSON_DIR" -I"$ROOT_DIR/src" \
  "$ROOT_DIR/test/settings_compat/SettingsCompatTest.cpp" \
  "$ROOT_DIR/src/SettingsCompat.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
