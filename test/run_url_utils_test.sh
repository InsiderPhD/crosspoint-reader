#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/url_utils"
BINARY="$BUILD_DIR/UrlUtilsTest"

mkdir -p "$BUILD_DIR"

c++ -std=c++20 -O2 -Wall -Wextra -pedantic -I"$ROOT_DIR" \
  "$ROOT_DIR/test/url_utils/UrlUtilsTest.cpp" \
  "$ROOT_DIR/src/util/UrlUtils.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
