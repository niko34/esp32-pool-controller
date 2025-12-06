#!/bin/bash

# ESP32 Pool Controller v2.0 - Pre-Production Check Script
# Usage: ./pre_production_check.sh

set -e

echo "╔════════════════════════════════════════════════════════════╗"
echo "║   ESP32 Pool Controller v2.0 - Pre-Production Check       ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

ERRORS=0
WARNINGS=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

function check_pass() {
    echo -e "${GREEN}✓${NC} $1"
}

function check_fail() {
    echo -e "${RED}✗${NC} $1"
    ((ERRORS++))
}

function check_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
    ((WARNINGS++))
}

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "1. Vérification de la structure des fichiers"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

required_files=(
    "src/config.cpp"
    "src/config.h"
    "src/filtration.cpp"
    "src/filtration.h"
    "src/history.cpp"
    "src/history.h"
    "src/logger.cpp"
    "src/logger.h"
    "src/main.cpp"
    "src/mqtt_manager.cpp"
    "src/mqtt_manager.h"
    "src/pump_controller.cpp"
    "src/pump_controller.h"
    "src/sensors.cpp"
    "src/sensors.h"
    "src/web_server.cpp"
    "src/web_server.h"
    "data/config.html"
    "data/index.html"
    "data/setting.png"
    "platformio.ini"
    "CALIBRATION_GUIDE.md"
    "MIGRATION_GUIDE.md"
    "QUICK_START.md"
    "README.md"
    "SIMULATION_GUIDE.md"
    "WIRING_DIAGRAM.md"
)

for file in "${required_files[@]}"; do
    if [ -f "$file" ]; then
        check_pass "Fichier trouvé: $file"
    else
        check_fail "Fichier manquant: $file"
    fi
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "2. Vérification de la configuration de sécurité"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check simulation mode is disabled
if grep -A 1 "struct SimulationConfig" src/config.h | grep -q "bool enabled = false"; then
    check_pass "Mode simulation DÉSACTIVÉ (production ready)"
elif grep -A 1 "struct SimulationConfig" src/config.h | grep -q "bool enabled = true"; then
    check_fail "CRITIQUE: Mode simulation ACTIVÉ - DOIT être false pour production!"
else
    check_warning "Impossible de vérifier le mode simulation dans config.h"
fi

# Check safety limits are defined
if grep -q "maxPhMinusMlPerDay" src/config.h; then
    check_pass "Limites de sécurité journalières définies"
else
    check_fail "Limites de sécurité journalières manquantes"
fi

# Check watchdog is enabled
if grep -q "esp_task_wdt_init" src/main.cpp; then
    check_pass "Watchdog configuré"
else
    check_fail "Watchdog manquant dans main.cpp"
fi

# Check password masking
if grep -q '"\*\*\*\*\*\*"' src/web_server.cpp; then
    check_pass "Masquage mot de passe MQTT implémenté"
else
    check_warning "Vérifier le masquage des mots de passe dans web_server.cpp"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3. Vérification de la compilation"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if command -v pio &> /dev/null; then
    check_pass "PlatformIO installé"

    echo "   Tentative de compilation..."
    if pio run > /tmp/pio_build.log 2>&1; then
        check_pass "Compilation réussie"

        # Check binary size
        if [ -f ".pio/build/esp32dev/firmware.bin" ]; then
            SIZE=$(stat -f%z ".pio/build/esp32dev/firmware.bin" 2>/dev/null || stat -c%s ".pio/build/esp32dev/firmware.bin" 2>/dev/null)
            if [ ! -z "$SIZE" ]; then
                SIZE_KB=$((SIZE / 1024))
                if [ $SIZE_KB -lt 1500 ]; then
                    check_pass "Taille firmware: ${SIZE_KB}KB (OK)"
                else
                    check_warning "Taille firmware: ${SIZE_KB}KB (attention si >1400KB)"
                fi
            fi
        fi
    else
        check_fail "Échec de compilation (voir /tmp/pio_build.log)"
        cat /tmp/pio_build.log
    fi
else
    check_warning "PlatformIO non installé - compilation non vérifiée"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "4. Vérification de la documentation"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

docs=(
    "README.md"
    "CALIBRATION_GUIDE.md"
    "MIGRATION_GUIDE.md"
    "WIRING_DIAGRAM.md"
    "CHANGELOG.md"
    "QUICK_START.md"
    "IMPROVEMENTS_SUMMARY.md"
)

for doc in "${docs[@]}"; do
    if [ -f "$doc" ]; then
        lines=$(wc -l < "$doc")
        if [ $lines -gt 50 ]; then
            check_pass "$doc ($lines lignes)"
        else
            check_warning "$doc existe mais semble incomplet ($lines lignes)"
        fi
    else
        check_warning "Documentation manquante: $doc"
    fi
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "5. Checklist manuelle pré-production"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo ""
echo "⚠️  Les points suivants doivent être vérifiés MANUELLEMENT:"
echo ""
echo "   Configuration:"
echo "   [ ] Mode simulation = false dans config.h"
echo "   [ ] Limites journalières ajustées selon volume piscine"
echo "   [ ] Consignes pH/ORP correctes (7.0-7.4 / 650-750mV)"
echo "   [ ] Watchdog activé (30s)"
echo ""
echo "   Matériel:"
echo "   [ ] Capteurs pH et ORP calibrés (voir CALIBRATION_GUIDE.md)"
echo "   [ ] Sonde température DS18B20 fonctionne (pas -127°C)"
echo "   [ ] Pompes testées (sens rotation correct)"
echo "   [ ] Relais filtration fonctionne"
echo "   [ ] Câblage conforme (voir WIRING_DIAGRAM.md)"
echo "   [ ] Boîtier étanche IP65"
echo "   [ ] Protection électrique (différentiel 30mA)"
echo ""
echo "   Tests:"
echo "   [ ] Test dosage dans seau (PAS dans piscine!)"
echo "   [ ] WiFi se connecte automatiquement"
echo "   [ ] MQTT connecté et publish OK"
echo "   [ ] Interface web accessible"
echo "   [ ] Logs accessibles via /get-logs"
echo "   [ ] Alertes MQTT fonctionnent"
echo "   [ ] Limites de sécurité testées"
echo ""
echo "   Sécurité:"
echo "   [ ] Mot de passe MQTT non exposé (vérifier /get-config)"
echo "   [ ] Validation entrées testée (valeurs aberrantes rejetées)"
echo "   [ ] Watchdog ne déclenche pas en utilisation normale"
echo "   [ ] Backup configuration effectué"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Résumé"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ $ERRORS -eq 0 ] && [ $WARNINGS -eq 0 ]; then
    echo -e "${GREEN}✓ Toutes les vérifications automatiques ont réussi !${NC}"
elif [ $ERRORS -eq 0 ]; then
    echo -e "${YELLOW}⚠ $WARNINGS avertissement(s) détecté(s)${NC}"
    echo "  Vérifier les points ci-dessus avant la mise en production"
else
    echo -e "${RED}✗ $ERRORS erreur(s) et $WARNINGS avertissement(s) détecté(s)${NC}"
    echo "  CORRIGER les erreurs avant de continuer !"
    exit 1
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Prochaines étapes recommandées:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "1. Vérifier TOUS les points de la checklist manuelle ci-dessus"
echo "2. Lire QUICK_START.md pour les instructions de mise en route"
echo "3. Suivre CALIBRATION_GUIDE.md pour calibrer les capteurs"
echo "4. Effectuer tests dosage dans seau (PAS dans piscine)"
echo "5. Surveiller 48h minimum avant utilisation autonome"
echo ""
echo "📚 Documentation complète disponible dans:"
echo "   - README.md (manuel utilisateur)"
echo "   - QUICK_START.md (démarrage rapide)"
echo "   - CALIBRATION_GUIDE.md (calibration)"
echo "   - WIRING_DIAGRAM.md (câblage)"
echo ""
echo "⚠️  IMPORTANT:"
echo "   En cas de doute, NE PAS mettre en production."
echo "   Commencer en mode monitoring passif (dosage désactivé)."
echo ""

exit 0
