#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/glyph_bitmap"
BINARY="$BUILD_DIR/GlyphBitmapTest"

mkdir -p "$BUILD_DIR"

c++ -std=c++20 -O2 -Wall -Wextra -pedantic -I"$ROOT_DIR/lib/GfxRenderer" \
  "$ROOT_DIR/test/glyph_bitmap/GlyphBitmapTest.cpp" -o "$BINARY"

"$BINARY" "$@"
