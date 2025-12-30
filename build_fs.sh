#!/bin/bash
# Script pour construire le filesystem LittleFS avec la bonne taille
# Taille: 0x150000 = 1 376 256 bytes = 1344KB

echo "Building LittleFS with size 1376256 bytes (1344KB)..."
~/.platformio/packages/tool-mklittlefs/mklittlefs \
  -c data \
  -s 1376256 \
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
