#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/html_to_plain_text"
BINARY="$BUILD_DIR/HtmlToPlainTextTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/html_to_plain_text/HtmlToPlainTextTest.cpp"
  "$ROOT_DIR/src/util/HtmlToPlainText.cpp"
  "$ROOT_DIR/lib/Epub/Epub/htmlEntities.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/src"
  -I"$ROOT_DIR/src/util"
  -I"$ROOT_DIR/lib/Epub"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
