#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Compilation firmware ==="
pio run

echo ""
echo "=== Compilation filesystem ==="
"$SCRIPT_DIR/build_fs.sh"

echo ""
echo "✅ Build terminé — fichiers dans .pio/build/esp32dev/"
