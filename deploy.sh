#!/bin/bash
# Script de déploiement complet pour ESP32 Pool Controller
# Usage: ./deploy.sh [firmware|fs|all|ota-firmware|ota-fs|ota-all]

set -e  # Arrêter en cas d'erreur

# Configuration
PORT="/dev/cu.usbserial-0001"
BAUD="115200"
LITTLEFS_OFFSET="0x2D0000"
BUILD_DIR=".pio/build/esp32dev"

# Hôte OTA : nom mDNS ou IP directe
# Surcharger via variable d'environnement : OTA_HOST=192.168.1.42 ./deploy.sh ota-all
OTA_HOST="${OTA_HOST:-poolcontroller.local}"

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
    print_step "Construction du filesystem LittleFS (1088KB)..."
    ./build_fs.sh
}

upload_firmware() {
    print_step "Upload du firmware..."
    pio run -t upload
    # Effacer l'OTA data pour forcer le boot sur app0 (partition table à jour)
    print_step "Réinitialisation OTA data (boot → app0)..."
    ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        erase_region 0xE000 0x2000
    print_success "Firmware uploadé"
}

erase_flash() {
    print_step "Effacement complet de la flash (firmware, filesystem, NVS)..."
    ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        erase_flash
    print_success "Flash effacée"
}

upload_filesystem() {
    print_step "Upload du filesystem LittleFS..."

    if [ ! -f "$BUILD_DIR/littlefs.bin" ]; then
        print_error "Le fichier littlefs.bin n'existe pas. Exécutez d'abord ./build_fs.sh"
        exit 1
    fi

    # Vérifier la taille du fichier
    SIZE=$(stat -f%z "$BUILD_DIR/littlefs.bin" 2>/dev/null || stat -c%s "$BUILD_DIR/littlefs.bin" 2>/dev/null)
    if [ "$SIZE" != "1114112" ]; then
        print_warning "Taille incorrecte du fichier littlefs.bin: $SIZE bytes (attendu: 1114112)"
        print_warning "Reconstruction avec build_fs.sh..."
        build_filesystem
    fi

    ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        write_flash "$LITTLEFS_OFFSET" "$BUILD_DIR/littlefs.bin"

    print_success "Filesystem uploadé"
}

show_usage() {
    echo "Usage: $0 [firmware|fs|all|factory|ota-firmware|ota-fs|ota-all]"
    echo ""
    echo "Options USB (connexion série requise) :"
    echo "  firmware      - Compile et upload uniquement le firmware"
    echo "  fs            - Build et upload uniquement le filesystem"
    echo "  all           - Compile et upload firmware + filesystem (défaut)"
    echo "  factory       - Efface toute la flash puis compile et upload firmware + filesystem"
    echo "                  Génère un nouveau mot de passe WiFi AP (NVS effacée)"
    echo ""
    echo "Options OTA (WiFi, pas d'USB) :"
    echo "  ota-firmware  - Compile et envoie uniquement le firmware en OTA"
    echo "  ota-fs        - Build et envoie uniquement le filesystem en OTA"
    echo "  ota-all       - Compile et envoie firmware + filesystem en OTA"
    echo ""
    echo "  Les options OTA appellent ota_update.sh qui accepte un hostname ou une IP :"
    echo "    poolcontroller.local  — mDNS (défaut, nécessite support mDNS sur le réseau)"
    echo "    192.168.1.x           — IP directe (fonctionne sans mDNS)"
    echo ""
    echo "Exemples:"
    echo "  $0                # Déploiement USB complet (NVS préservée)"
    echo "  $0 factory        # Flash complet avec nouveau mot de passe AP"
    echo "  $0 ota-all        # Déploiement OTA complet (WiFi, via mDNS)"
    echo "  $0 ota-firmware   # Firmware uniquement en OTA (WiFi, via mDNS)"
    echo ""
    echo "  Pour OTA sans mDNS (IP directe) :"
    echo "    ./ota_update.sh both 192.168.1.42 monmotdepasse"
}

# Programme principal
MODE="${1:-all}"

case "$MODE" in
    firmware)
        print_step "Déploiement USB du firmware uniquement"
        build_firmware
        upload_firmware
        ;;
    fs)
        print_step "Déploiement USB du filesystem uniquement"
        build_filesystem
        upload_filesystem
        ;;
    all)
        print_step "Déploiement USB complet (firmware + filesystem)"
        build_firmware
        build_filesystem
        upload_firmware
        upload_filesystem
        ;;
    factory)
        print_warning "Flash complet avec effacement NVS — un nouveau mot de passe WiFi AP sera généré"
        print_warning "Ouvrir le moniteur série après le boot pour récupérer le mot de passe"
        read -p "Confirmer ? (o/N) " confirm
        if [[ "$confirm" != "o" && "$confirm" != "O" ]]; then
            print_error "Annulé"
            exit 1
        fi
        build_firmware
        build_filesystem
        erase_flash
        upload_firmware
        upload_filesystem
        print_success "Flash complet terminé — lancer 'pio device monitor -b 115200' et redémarrer l'ESP32 pour récupérer le mot de passe AP"
        ;;
    ota-firmware)
        print_step "Déploiement OTA du firmware uniquement (hôte: $OTA_HOST)"
        build_firmware
        ./ota_update.sh firmware "$OTA_HOST"
        ;;
    ota-fs)
        print_step "Déploiement OTA du filesystem uniquement (hôte: $OTA_HOST)"
        build_filesystem
        ./ota_update.sh filesystem "$OTA_HOST"
        ;;
    ota-all)
        print_step "Déploiement OTA complet (firmware + filesystem) (hôte: $OTA_HOST)"
        build_firmware
        build_filesystem
        ./ota_update.sh both "$OTA_HOST"
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
