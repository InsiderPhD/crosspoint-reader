#!/usr/bin/env bash
set -euo pipefail

# CrossPoint Sync stats payloads (src/sync/StatsPayload): dates, weekdays,
# the history bitmap's bit order and the JSON shapes. No Arduino needed.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/stats_payload"
BINARY="$BUILD_DIR/StatsPayloadTest"

mkdir -p "$BUILD_DIR"

c++ -std=c++20 -O2 -Wall -Wextra \
  -I"$ROOT_DIR/src/sync" \
  "$ROOT_DIR/test/stats_payload/StatsPayloadTest.cpp" \
  "$ROOT_DIR/src/sync/StatsPayload.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
