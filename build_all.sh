#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/.pio/build/esp32dev"
DEST_DIR="$SCRIPT_DIR"

echo "=== Compilation firmware ==="
pio run

echo ""
echo "=== Compilation filesystem ==="
"$SCRIPT_DIR/build_fs.sh"

echo ""
echo "=== Copie des binaires ==="
cp "$BUILD_DIR/firmware.bin" "$DEST_DIR/firmware.bin"
cp "$BUILD_DIR/littlefs.bin" "$DEST_DIR/littlefs.bin"

echo "✅ firmware.bin  → $DEST_DIR"
echo "✅ littlefs.bin  → $DEST_DIR"
