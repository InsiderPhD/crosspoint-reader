#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/library_index"
BINARY="$BUILD_DIR/LibraryTextTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/library_index/LibraryTextTest.cpp"
  "$ROOT_DIR/lib/LibraryIndex/LibraryText.cpp"
  "$ROOT_DIR/lib/LibraryIndex/LibraryFormat.cpp"
  "$ROOT_DIR/lib/Utf8/Utf8.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR/lib/LibraryIndex"
  -I"$ROOT_DIR/lib/Utf8"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
