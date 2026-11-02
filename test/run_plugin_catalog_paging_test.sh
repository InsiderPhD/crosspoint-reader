#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/plugin_catalog_paging"
BINARY="$BUILD_DIR/PluginCatalogPagingTest"

mkdir -p "$BUILD_DIR"

c++ -std=c++20 -O2 -Wall -Wextra -pedantic \
  -I"$ROOT_DIR/src" -I"$ROOT_DIR/src/activities/plugins" \
  "$ROOT_DIR/test/plugin_catalog_paging/PluginCatalogPagingTest.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
