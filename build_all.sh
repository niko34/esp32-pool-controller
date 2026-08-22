#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

PIO_ENV="${PIO_ENV:-esp32dev_core3}"
export PIO_ENV

echo "=== Compilation firmware ($PIO_ENV) ==="
pio run -e "$PIO_ENV"

echo ""
echo "=== Compilation filesystem ==="
"$SCRIPT_DIR/build_fs.sh"

echo ""
echo "✅ Build terminé — fichiers dans .pio/build/$PIO_ENV/"
