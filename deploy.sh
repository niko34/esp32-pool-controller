#!/bin/bash
# Script de déploiement complet pour ESP32 Pool Controller
# Usage: ./deploy.sh [firmware|fs|all]

set -e  # Arrêter en cas d'erreur

# Configuration
PORT="/dev/cu.usbserial-210"
BAUD="115200"
LITTLEFS_OFFSET="0x290000"
BUILD_DIR=".pio/build/esp32dev"

# Couleurs pour l'affichage
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_step() {
    echo -e "${BLUE}==>${NC} $1"
}

print_success() {
    echo -e "${GREEN}✅${NC} $1"
}

print_error() {
    echo -e "${RED}❌${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠️${NC} $1"
}

build_firmware() {
    print_step "Compilation du firmware..."
    pio run
    print_success "Firmware compilé"
}

build_filesystem() {
    print_step "Construction du filesystem LittleFS (1344KB)..."
    ./build_fs.sh
}

upload_firmware() {
    print_step "Upload du firmware..."
    pio run -t upload
    print_success "Firmware uploadé"
}

upload_filesystem() {
    print_step "Upload du filesystem LittleFS..."

    if [ ! -f "$BUILD_DIR/littlefs.bin" ]; then
        print_error "Le fichier littlefs.bin n'existe pas. Exécutez d'abord ./build_fs.sh"
        exit 1
    fi

    # Vérifier la taille du fichier
    SIZE=$(stat -f%z "$BUILD_DIR/littlefs.bin" 2>/dev/null || stat -c%s "$BUILD_DIR/littlefs.bin" 2>/dev/null)
    if [ "$SIZE" != "1376256" ]; then
        print_warning "Taille incorrecte du fichier littlefs.bin: $SIZE bytes (attendu: 1376256)"
        print_warning "Reconstruction avec build_fs.sh..."
        build_filesystem
    fi

    python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        write_flash "$LITTLEFS_OFFSET" "$BUILD_DIR/littlefs.bin"

    print_success "Filesystem uploadé"
}

show_usage() {
    echo "Usage: $0 [firmware|fs|all]"
    echo ""
    echo "Options:"
    echo "  firmware  - Compile et upload uniquement le firmware"
    echo "  fs        - Build et upload uniquement le filesystem"
    echo "  all       - Compile et upload firmware + filesystem (défaut)"
    echo ""
    echo "Exemples:"
    echo "  $0           # Déploiement complet"
    echo "  $0 firmware  # Uniquement le firmware"
    echo "  $0 fs        # Uniquement le filesystem"
}

# Programme principal
MODE="${1:-all}"

case "$MODE" in
    firmware)
        print_step "Déploiement du firmware uniquement"
        build_firmware
        upload_firmware
        ;;
    fs)
        print_step "Déploiement du filesystem uniquement"
        build_filesystem
        upload_filesystem
        ;;
    all)
        print_step "Déploiement complet (firmware + filesystem)"
        build_firmware
        build_filesystem
        upload_firmware
        upload_filesystem
        ;;
    -h|--help)
        show_usage
        exit 0
        ;;
    *)
        print_error "Option invalide: $MODE"
        show_usage
        exit 1
        ;;
esac

echo ""
print_success "Déploiement terminé avec succès!"
