#!/bin/bash
# Script pour construire le filesystem LittleFS avec la bonne taille
# Layout v2 (ADR-0015) — Taille: 0xD0000 = 851 968 bytes = 832KB
# (avant repartitionnement : 0x110000 = 1 114 112 bytes = 1088KB)

set -e  # Arrêter en cas d'erreur

echo "🔧 Minification des fichiers web..."
node minify.js

echo ""
echo "📦 Building LittleFS with size 851968 bytes (832KB)..."
~/.platformio/packages/tool-mklittlefs/mklittlefs \
  -c data-build \
  -s 851968 \
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
