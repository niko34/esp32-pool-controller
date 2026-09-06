#!/bin/bash
# Script de déploiement complet pour ESP32 Pool Controller
# Usage: ./deploy.sh [firmware|fs|all|ota-firmware|ota-fs|ota-all]

set -e  # Arrêter en cas d'erreur

# Configuration
BAUD="115200"
LITTLEFS_OFFSET="0x730000"  # Layout v5 — était 0x390000 en v4, 0x350000 en v3
NVS_OFFSET="0x9000"         # partitions.csv : nvs, data, nvs, 0x9000, 0x5000
NVS_SIZE="0x5000"

# Environnement PlatformIO déployé : esp32dev_core3, seul env matériel depuis
# v2.42.1 (ADR-0069 ; core 3 par défaut depuis v2.20.4, ADR-0042 — bootloader ET
# application). PIO_ENV reste le point d'entrée unique et surchargeable.
# Doit rester cohérent avec default_envs de platformio.ini : l'env est passé
# explicitement en -e à chaque appel pio pour que le script ne dépende pas du
# défaut du fichier de config.
PIO_ENV="${PIO_ENV:-esp32dev_core3}"
BUILD_DIR=".pio/build/$PIO_ENV"
export PIO_ENV BUILD_DIR   # build_fs.sh écrit le littlefs.bin dans BUILD_DIR

# Port série USB. Le port était codé en dur sur /dev/cu.usbserial-0001, ce qui
# échoue silencieusement dès qu'on branche un AUTRE module (un adaptateur neuf
# n'énumère pas sous le même nom). Ordre : variable d'environnement, puis port
# historique s'il existe, puis premier port USB-série détecté.
#   PORT=/dev/cu.usbserial-10 ./deploy.sh all
PORT="${PORT:-}"
if [ -z "$PORT" ]; then
    if [ -e "/dev/cu.usbserial-0001" ]; then
        PORT="/dev/cu.usbserial-0001"
    else
        PORT=$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -1)
    fi
fi

# Moniteur série enchaîné après un flash USB (NO_MONITOR=1 pour s'en passer,
# par exemple dans un script non interactif).
NO_MONITOR="${NO_MONITOR:-0}"

# esptool : le paquet tool-esptoolpy est PARTAGÉ entre les environnements, et
# chacun impose sa version (core 2 → 4.5.1, core 3 → 5.1.2). Il bascule donc au
# gré du dernier build. Or esptool 5 a renommé les sous-commandes en tirets
# (erase-region) là où la 4 utilise des underscores. On sonde les capacités
# réelles plutôt que de supposer une version.
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
ESPTOOL_PY="$HOME/.platformio/penv/bin/python"
if "$ESPTOOL_PY" "$ESPTOOL" --help 2>&1 | grep -q "erase-region"; then
    CMD_ERASE_REGION="erase-region"      # esptool >= 5
else
    CMD_ERASE_REGION="erase_region"      # esptool 4.x
fi

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

require_port() {
    if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
        print_error "Aucun port série USB détecté (PORT='$PORT')."
        print_error "Branchez le module, ou forcez-le : PORT=/dev/cu.usbserial-XX $0 $MODE"
        echo "Ports disponibles :"
        ls /dev/cu.* 2>/dev/null | sed 's/^/  /'
        exit 1
    fi
    print_step "Port série : $PORT"
}

build_firmware() {
    print_step "Compilation du firmware ($PIO_ENV)..."
    pio run -e "$PIO_ENV"
    # L'archivage de l'ELF est désormais fait par le hook post-build
    # tools/archive_build.py (extra_scripts dans platformio.ini) : il range
    # builds/<VERSION>/<ENV>/firmware_<ts>.{elf,bin} et couvre AUSSI `pio run`
    # direct et ota_update.sh, que ce script ne voyait pas. L'ancien archive_elf()
    # d'ici déversait des ELF non versionnés à la racine de builds/ — doublon qui
    # repolluait le dossier après chaque ménage.
    print_success "Firmware compilé"
}

# Moniteur série enchaîné : il s'attache dans la seconde qui suit le dernier
# esptool, donc on capte le boot au plus tôt sans avoir à courir après.
# NE PAS écrire que l'ouverture du port redémarre la puce : selon l'adaptateur et
# l'état où esptool l'a laissée, ce n'est PAS garanti (vérifié : après un
# `--after no_reset` elle reste dans le bootloader ROM et le moniteur reste muet).
# Le seul redémarrage fiable est l'appui sur EN/RST, d'où le message ci-dessous.
# `|| true` : sortir par Ctrl+C renvoie 130, et `set -e` ferait échouer le script.
monitor_serial() {
    if [ "$NO_MONITOR" = "1" ]; then
        print_step "Moniteur série ignoré (NO_MONITOR=1)"
        return
    fi
    echo ""
    print_step "Moniteur série (Ctrl+C pour quitter) — $PORT @ $BAUD"
    print_step "Rien ne s'affiche ? Appuyez sur EN/RST sur la carte pour redémarrer."
    pio device monitor -e "$PIO_ENV" -p "$PORT" -b "$BAUD" || true
}

# Efface la SEULE partition NVS. Utile pour repartir d'une configuration vierge
# (et donc revoir le mot de passe AP) sans réécrire firmware et filesystem.
#
# HISTORIQUE D'UNE ERREUR, à ne pas refaire : cette fonction utilisait
# `--after no_reset` pour « garder » le boot de génération du mot de passe au
# profit du moniteur. Deux erreurs. D'abord esptool laisse alors la puce dans le
# bootloader ROM (« Staying in bootloader. ») et l'ouverture du moniteur ne l'en
# sort PAS : écran vide. Ensuite c'était inutile — tant que `wizardCompleted` est
# faux, main.cpp:556 réaffiche le mot de passe à CHAQUE démarrage de l'AP, il n'y
# a donc aucune occasion unique à préserver. Reset normal, et si l'utilisateur
# rate les premières lignes il appuie sur EN/RST.
erase_nvs() {
    print_step "Effacement de la partition NVS ($NVS_OFFSET, $NVS_SIZE) — configuration remise à zéro..."
    "$ESPTOOL_PY" "$ESPTOOL" \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        "$CMD_ERASE_REGION" "$NVS_OFFSET" "$NVS_SIZE"
    print_success "NVS effacée"
}

build_filesystem() {
    print_step "Construction du filesystem LittleFS (512KB)..."
    ./build_fs.sh
}

upload_firmware() {
    print_step "Upload du firmware..."
    # --upload-port est INDISPENSABLE : sans lui, pio prend l'upload_port de
    # platformio.ini (figé sur /dev/cu.usbserial-0001) et ignore le port détecté
    # ici — l'upload échoue alors que le module est bien branché ailleurs.
    pio run -e "$PIO_ENV" -t upload --upload-port "$PORT"
    # Effacer l'OTA data pour forcer le boot sur app0 (partition table à jour)
    print_step "Réinitialisation OTA data (boot → app0)..."
    "$ESPTOOL_PY" "$ESPTOOL" \
        --chip esp32 \
        --port "$PORT" \
        --baud "$BAUD" \
        "$CMD_ERASE_REGION" 0xE000 0x2000
    print_success "Firmware uploadé"
}

erase_flash() {
    print_step "Effacement complet de la flash (firmware, filesystem, NVS)..."
    "$ESPTOOL_PY" "$ESPTOOL" \
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
    if [ "$SIZE" != "524288" ]; then
        print_warning "Taille incorrecte du fichier littlefs.bin: $SIZE bytes (attendu: 524288)"
        print_warning "Reconstruction avec build_fs.sh..."
        build_filesystem
    fi

    "$ESPTOOL_PY" "$ESPTOOL" \
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
    echo "                  Remet la configuration à zéro et affiche le mot de passe AP"
    echo "                  dans le moniteur enchaîné (réaffiché à chaque boot tant que"
    echo "                  l'assistant de configuration n'est pas terminé)"
    echo ""
    echo "  Les modes USB enchaînent le moniteur série. Si l'écran reste muet,"
    echo "  appuyez sur EN/RST : c'est le seul redémarrage fiable."
    echo "    NO_MONITOR=1 $0 all      # ne pas enchaîner le moniteur"
    echo "    PORT=/dev/cu.usbserial-10 $0 all   # forcer le port série"
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
        require_port
        build_firmware
        upload_firmware
        monitor_serial
        ;;
    fs)
        print_step "Déploiement USB du filesystem uniquement"
        require_port
        build_filesystem
        upload_filesystem
        monitor_serial
        ;;
    all)
        print_step "Déploiement USB complet (firmware + filesystem)"
        require_port
        build_firmware
        build_filesystem
        upload_firmware
        upload_filesystem
        monitor_serial
        ;;
    factory)
        print_warning "Flash complet avec effacement NVS — un nouveau mot de passe WiFi AP sera généré"
        print_warning "TOUTE la configuration est perdue : WiFi, MQTT, offset pH, calibration."
        read -p "Confirmer ? (o/N) " confirm
        if [[ "$confirm" != "o" && "$confirm" != "O" ]]; then
            print_error "Annulé"
            exit 1
        fi
        require_port
        build_firmware
        build_filesystem
        erase_flash
        upload_firmware
        upload_filesystem
        # Pas de ré-effacement NVS ici : erase_flash l'a déjà vidée, donc
        # `wizardCompleted` est faux et main.cpp:556 réaffiche le mot de passe à
        # CHAQUE démarrage du point d'accès — tant que l'assistant n'est pas
        # terminé, il n'y a aucune fenêtre à rattraper.
        echo ""
        print_warning "Mot de passe AP — cherchez cette ligne (SSID : PoolControllerAP) :"
        print_warning "    AP Password: XXXXXXXX"
        print_warning "Elle est réaffichée à chaque démarrage tant que l'assistant n'est pas terminé :"
        print_warning "un appui sur EN/RST la fait réapparaître."
        monitor_serial
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
