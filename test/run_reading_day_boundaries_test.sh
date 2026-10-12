#!/usr/bin/env bash
set -euo pipefail

# Day boundaries and streaks across daylight-saving changes. Builds the real
# src/util/TimeUtils.cpp + TimeZoneRegistry.cpp against small host stubs, so the
# zone table and the local-day conversion under test are the firmware's own.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/reading_day_boundaries"
BINARY="$BUILD_DIR/ReadingDayBoundariesTest"

mkdir -p "$BUILD_DIR"

# The stubs directory comes first so its CrossPointSettings.h / HalClock.h win.
c++ -std=c++20 -O2 -Wall -Wextra \
  -I"$ROOT_DIR/test/reading_day_boundaries/stubs" \
  -I"$ROOT_DIR/src/util" \
  "$ROOT_DIR/test/reading_day_boundaries/ReadingDayBoundariesTest.cpp" \
  "$ROOT_DIR/src/util/TimeUtils.cpp" \
  "$ROOT_DIR/src/util/TimeZoneRegistry.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
