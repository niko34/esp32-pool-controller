#!/bin/bash
# Script pour construire le filesystem LittleFS avec la bonne taille
# Layout v4 (ADR-0024) — Taille: 0x50000 = 327 680 bytes = 320KB
# (avant : 0x90000 = 576KB, layout v3)

set -e  # Arrêter en cas d'erreur

echo "🔧 Minification des fichiers web..."
node minify.js

echo ""
echo "📦 Building LittleFS with size 327680 bytes (320KB)..."
~/.platformio/packages/tool-mklittlefs/mklittlefs \
  -c data-build \
  -s 327680 \
  -p 256 \
  -b 4096 \
  .pio/build/esp32dev/littlefs.bin

if [ $? -eq 0 ]; then
  echo "✅ LittleFS built successfully"
  ls -lh .pio/build/esp32dev/littlefs.bin
else
  echo "❌ LittleFS build failed"
  exit 1
fi
