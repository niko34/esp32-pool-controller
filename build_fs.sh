#!/bin/bash
# Script pour construire le filesystem LittleFS avec la bonne taille
# Layout v5 — Taille: 0x80000 = 524 288 bytes = 512KB
# (avant : 0x50000 = 320KB en v4, 0x90000 = 576KB en v3)

set -e  # Arrêter en cas d'erreur

# Le filesystem est INDÉPENDANT du core, mais il doit être écrit là où le
# déploiement le cherche. BUILD_DIR est exporté par deploy.sh ; en appel direct
# on retombe sur l'environnement par défaut (core 3 depuis v2.20.4, ADR-0042).
PIO_ENV="${PIO_ENV:-esp32dev_core3}"
BUILD_DIR="${BUILD_DIR:-.pio/build/$PIO_ENV}"
mkdir -p "$BUILD_DIR"

echo "🔧 Minification des fichiers web..."
node minify.js

echo ""
echo "📦 Building LittleFS with size 524288 bytes (512KB)..."
~/.platformio/packages/tool-mklittlefs/mklittlefs \
  -c data-build \
  -s 524288 \
  -p 256 \
  -b 4096 \
  "$BUILD_DIR/littlefs.bin"

if [ $? -eq 0 ]; then
  echo "✅ LittleFS built successfully"
  ls -lh "$BUILD_DIR/littlefs.bin"
else
  echo "❌ LittleFS build failed"
  exit 1
fi
