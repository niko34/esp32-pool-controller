#!/bin/bash
# Script pour construire le filesystem LittleFS avec la bonne taille
# Layout v3 (ADR-0019) — Taille: 0x90000 = 589 824 bytes = 576KB
# (avant : 0xD0000 = 832KB, layout v2)

set -e  # Arrêter en cas d'erreur

echo "🔧 Minification des fichiers web..."
node minify.js

echo ""
echo "📦 Building LittleFS with size 589824 bytes (576KB)..."
~/.platformio/packages/tool-mklittlefs/mklittlefs \
  -c data-build \
  -s 589824 \
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
