#!/usr/bin/env bash
# Generate packaging/naTE.icns from the committed PNGs in src/ui/resources/.
# macOS only — sips and iconutil are macOS built-ins; no extra dependencies.
#
# Run manually:  cmake --build build --target regen-icns
# Then commit:   git add packaging/naTE.icns
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RES="$ROOT/src/ui/resources"
ICONSET="$(mktemp -d)/naTE.iconset"
OUT="$ROOT/packaging/naTE.icns"

command -v sips     >/dev/null 2>&1 || { echo "sips not found — run this on macOS"; exit 1; }
command -v iconutil >/dev/null 2>&1 || { echo "iconutil not found — run this on macOS"; exit 1; }

mkdir -p "$ICONSET"

# Copy sizes we already have from the committed PNGs.
cp "$RES/nate_16.png"  "$ICONSET/icon_16x16.png"
cp "$RES/nate_32.png"  "$ICONSET/icon_16x16@2x.png"
cp "$RES/nate_32.png"  "$ICONSET/icon_32x32.png"
cp "$RES/nate_128.png" "$ICONSET/icon_128x128.png"
cp "$RES/nate_256.png" "$ICONSET/icon_128x128@2x.png"
cp "$RES/nate_256.png" "$ICONSET/icon_256x256.png"

# Sizes not in the committed set — resize from the 256px source via sips.
sips -z  64  64 "$RES/nate_256.png" --out "$ICONSET/icon_32x32@2x.png"   >/dev/null
sips -z 512 512 "$RES/nate_256.png" --out "$ICONSET/icon_256x256@2x.png" >/dev/null
sips -z 512 512 "$RES/nate_256.png" --out "$ICONSET/icon_512x512.png"    >/dev/null
sips -z 1024 1024 "$RES/nate_256.png" --out "$ICONSET/icon_512x512@2x.png" >/dev/null

iconutil -c icns -o "$OUT" "$ICONSET"
rm -rf "$(dirname "$ICONSET")"
echo "Generated $OUT"
