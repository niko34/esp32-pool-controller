#!/usr/bin/env bash
# =============================================================================
# Mesure de la couverture de tests des classes pures (sensor_filter, dosing_logic).
# =============================================================================
# Compile et exécute les tests natifs Unity avec instrumentation gcov (--coverage),
# puis génère un rapport via gcovr (texte console + HTML détaillé dans coverage/).
#
# Prérequis :
#   - gcovr (installé dans le venv PlatformIO : ~/.platformio/penv/bin/gcovr)
#   - /usr/bin/gcov (Apple LLVM, fourni par les Command Line Tools sur macOS)
#
# Usage :
#   ./tools/coverage.sh            # rapport texte + HTML (coverage/index.html)
#   ./tools/coverage.sh --open     # idem + ouvre le rapport HTML (macOS)
#
# N'altère PAS le firmware ni l'env `native` standard : utilise l'env dédié
# `native_coverage` (cf. platformio.ini).
# =============================================================================
set -euo pipefail

cd "$(dirname "$0")/.."   # racine du projet

ENV="native_coverage"
BUILD_DIR=".pio/build/${ENV}"
OUT_DIR="coverage"

# --- Outils : pio + gcovr + gcov ---------------------------------------------
PIO="${PIO:-pio}"
command -v "$PIO" >/dev/null 2>&1 || PIO="$HOME/.platformio/penv/bin/pio"

if command -v gcovr >/dev/null 2>&1; then
  GCOVR="gcovr"
elif [ -x "$HOME/.platformio/penv/bin/gcovr" ]; then
  GCOVR="$HOME/.platformio/penv/bin/gcovr"
else
  echo "ERREUR : gcovr introuvable. Installer : $HOME/.platformio/penv/bin/pip install gcovr" >&2
  exit 1
fi

GCOV="${GCOV:-/usr/bin/gcov}"   # surchargeable : GCOV=llvm-cov-gcov ./tools/coverage.sh

echo "==> pio    : $PIO"
echo "==> gcovr  : $GCOVR ($($GCOVR --version | head -1))"
echo "==> gcov   : $GCOV"

# --- 1. Purge des données de couverture précédentes --------------------------
find "$BUILD_DIR" -name '*.gcda' -delete 2>/dev/null || true
mkdir -p "$OUT_DIR"

# --- 2. Build + exécution des tests instrumentés -----------------------------
echo "==> Exécution des tests natifs instrumentés ($ENV)…"
"$PIO" test -e "$ENV"

# --- 3. Rapport gcovr (uniquement les 2 classes pures sous test) -------------
echo
echo "==> Rapport de couverture (sensor_filter, dosing_logic, schedule_logic, history_logic) :"
echo
COMMON_ARGS=(
  --root .
  --gcov-executable "$GCOV"
  --filter 'src/sensor_filter\.(cpp|h)'
  --filter 'src/dosing_logic\.(cpp|h)'
  --filter 'src/schedule_logic\.(cpp|h)'
  --filter 'src/history_logic\.(cpp|h)'
  "$BUILD_DIR"
)

# Résumé console (lignes + branches)
"$GCOVR" "${COMMON_ARGS[@]}" --txt --print-summary

# Rapport HTML détaillé
"$GCOVR" "${COMMON_ARGS[@]}" --html-details "$OUT_DIR/index.html" >/dev/null
echo
echo "==> Rapport HTML : $OUT_DIR/index.html"

if [ "${1:-}" = "--open" ] && command -v open >/dev/null 2>&1; then
  open "$OUT_DIR/index.html"
fi
