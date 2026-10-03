#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/cover_tone_curve"
BINARY="$BUILD_DIR/CoverToneCurveTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/cover_tone_curve/CoverToneCurveTest.cpp"
  "$ROOT_DIR/lib/GfxRenderer/CoverToneCurve.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/GfxRenderer"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
