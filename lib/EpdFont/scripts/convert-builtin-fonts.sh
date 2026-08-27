#!/bin/bash

set -e

cd "$(dirname "$0")"

# Reader families (Bookerly, Inter, OpenDyslexic, Mono) are normalised to the same point
# sizes (6/8/10/12 = Small/Medium/Large/X-Large) so switching family keeps text at the same
# scale. NotoSans is no longer a reader family — only notosans_8_regular (the shared UI
# SMALL_FONT) and the legacy 12/14/16 sizes are still generated/registered.

# Script coverage the reader families deliberately do NOT carry. These blocks are
# only ever reached by book text, and they are the two most expensive in the set:
# Cyrillic and Vietnamese together are ~304KB of flash across the compressed
# families, ~30% of all font glyph data.
#
# They also set the RAM ceiling. Compressed fonts are grouped by Unicode block and
# each group decompresses whole into one contiguous malloc, so the largest block
# fixes the peak contiguous demand: Cyrillic alone is 38,711 bytes in
# notosans_16_bold — more than the BLE controller's 30KB. Dropping these two takes
# the worst case to 18,698 bytes.
#
# The UI fonts below (ubuntu 10/12 = UI_10/UI_12_FONT_ID, and notosans_8_regular =
# SMALL_FONT_ID) are generated WITHOUT these exclusions and WITHOUT --compress, so
# the four Cyrillic UI languages (Russian, Ukrainian, Belarusian, Kazakh) render
# exactly as before — including their own names in the language picker. Ubuntu
# covers 0x400-0x45F and 0x48A-0x4F9, which spans the Kazakh letters.
#
# What is given up: book *content* in Cyrillic or Vietnamese scripts.
READER_EXCLUDE_INTERVALS=(
  --exclude-intervals 0x0400,0x04FF  # Cyrillic
  --exclude-intervals 0x1EA0,0x1EF9  # Vietnamese Extended (precomposed tone marks)
  --exclude-intervals 0x01A0,0x01A1  # Latin Ext-B: Ơ/ơ, Vietnamese-only
  --exclude-intervals 0x01AF,0x01B0  # Latin Ext-B: Ư/ư, Vietnamese-only
)

READER_FONT_STYLES=("Regular" "Italic" "Bold")
BOOKERLY_FONT_SIZES=(6 8 10 12)
INTER_FONT_SIZES=(6 8 10 12)
NOTOSANS_FONT_SIZES=(12 14 16)
OPENDYSLEXIC_FONT_SIZES=(6 8 10 12 14)

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path --2bit --compress --pnum "${READER_EXCLUDE_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

for size in ${INTER_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="inter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Inter/Inter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path --2bit --compress --pnum "${READER_EXCLUDE_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path --2bit --compress --pnum "${READER_EXCLUDE_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

for size in ${OPENDYSLEXIC_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="opendyslexic_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/OpenDyslexic/OpenDyslexic-${style}.otf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path --2bit --compress "${READER_EXCLUDE_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

python3 fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf > ../builtinFonts/notosans_8_regular.h

MONO_FONT_SIZES=(6 8 10 12)
MONO_FONT_STYLES=("Regular" "Bold")

for size in ${MONO_FONT_SIZES[@]}; do
  for style in ${MONO_FONT_STYLES[@]}; do
    font_name="mono_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/JetBrainsMono/JetBrainsMono-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python3 fontconvert.py $font_name $size $font_path --2bit --compress --pnum "${READER_EXCLUDE_INTERVALS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
