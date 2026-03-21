#!/bin/bash
# Script pour construire le filesystem LittleFS avec la bonne taille
# Taille: 0x130000 = 1 245 184 bytes = 1216KB

set -e  # Arrêter en cas d'erreur

echo "🔧 Minification des fichiers web..."
node minify.js

echo ""
echo "📦 Building LittleFS with size 1245184 bytes (1216KB)..."
~/.platformio/packages/tool-mklittlefs/mklittlefs \
  -c data-build \
  -s 1245184 \
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
