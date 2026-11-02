#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/highlight_offset_finder"
BINARY="$BUILD_DIR/HighlightOffsetFinderTest"

mkdir -p "$BUILD_DIR"

INCLUDES=(
  -I"$ROOT_DIR/test/highlight_offset_finder/stubs"
  -I"$ROOT_DIR/lib/KOReaderSync"
  -I"$ROOT_DIR/lib/Epub"
  -I"$ROOT_DIR/lib/Utf8"
  -I"$ROOT_DIR/lib/XmlParserUtils"
  -I"$ROOT_DIR/lib/expat"
  -I"$ROOT_DIR/src/clippings"
)

# expat is C; build it separately so the C++ flags do not apply to it.
EXPAT_OBJECTS=()
for src in xmlparse xmlrole xmltok; do
  cc -O2 -DXML_GE=0 -DXML_CONTEXT_BYTES=1024 -DHAVE_ARC4RANDOM_BUF -I"$ROOT_DIR/lib/expat" \
    -c "$ROOT_DIR/lib/expat/$src.c" -o "$BUILD_DIR/$src.o"
  EXPAT_OBJECTS+=("$BUILD_DIR/$src.o")
done

c++ -std=c++20 -O2 -Wall -Wextra "${INCLUDES[@]}" \
  "$ROOT_DIR/test/highlight_offset_finder/HighlightOffsetFinderTest.cpp" \
  "$ROOT_DIR/src/clippings/HighlightOffsetFinder.cpp" \
  "$ROOT_DIR/lib/KOReaderSync/ChapterXPathResolver.cpp" \
  "$ROOT_DIR/lib/Utf8/Utf8.cpp" \
  "${EXPAT_OBJECTS[@]}" -o "$BINARY"

"$BINARY" "$@"
