#!/usr/bin/env bash
# Rasterises packaging/nate.svg at standard icon sizes and writes
# src/ui/resources/AppIcon.h containing the PNG bytes as C arrays.
# Run after any change to nate.svg:
#   ./scripts/gen-icon-header.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SVG="$ROOT/packaging/nate.svg"
RESDIR="$ROOT/src/ui/resources"
OUT="$RESDIR/AppIcon.h"

command -v rsvg-convert >/dev/null 2>&1 || { echo "rsvg-convert not found"; exit 1; }
command -v xxd            >/dev/null 2>&1 || { echo "xxd not found"; exit 1; }

mkdir -p "$RESDIR"

for size in 16 32 48 128 256; do
    rsvg-convert -w "$size" -h "$size" "$SVG" -o "$RESDIR/nate_${size}.png"
done

{
echo "// Auto-generated — do not edit. Re-run scripts/gen-icon-header.sh to update."
echo "#pragma once"
echo "#include <cstddef>"

for size in 16 32 48 128 256; do
    png="$RESDIR/nate_${size}.png"
    varname="kAppIcon${size}Png"
    echo ""
    echo "// ${size}x${size} PNG"
    xxd -i "$png" \
        | sed "s|unsigned char .*\[\]|static const unsigned char ${varname}[]|" \
        | grep -v "unsigned int "
done

echo ""
echo "// Icon table indexed by size"
echo "struct NateIconEntry { const unsigned char* data; std::size_t len; int size; };"
echo "static const NateIconEntry kAppIcons[] = {"
for size in 16 32 48 128 256; do
    echo "    { kAppIcon${size}Png, sizeof(kAppIcon${size}Png), ${size} },"
done
echo "};"
} > "$OUT"

echo "Generated $OUT"
