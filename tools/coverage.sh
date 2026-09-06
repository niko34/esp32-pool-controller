#!/usr/bin/env bash
# =============================================================================
# Mesure de la couverture de tests des classes pures compilées par [env:native].
# Filtre LARGE (`src/`) depuis le 2026-09-06 : il doit rester IDENTIQUE à celui de
# .github/workflows/tests-natifs.yml, sans quoi le chiffre local et le chiffre CI
# divergent — et c'est le local qu'on croit.
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

# --- 3. Rapport gcovr (tous les modules purs compilés par [env:native]) ------
echo
echo "==> Rapport de couverture (tous les modules purs compilés par [env:native]) :"
echo
COMMON_ARGS=(
  --root .
  --gcov-executable "$GCOV"
  --filter 'src/'
  "$BUILD_DIR"
)

# Résumé console (lignes + branches).
# MÊME garde que la CI (`--fail-under-line 90` dans tests-natifs.yml) : sans elle, le
# script local affichait un chiffre sans jamais échouer, et une chute sous le seuil ne
# se découvrait qu'APRÈS le push. Le code de sortie est retenu pour être rendu en fin
# de script, afin que le rapport HTML soit tout de même produit en cas d'échec.
SEUIL_RC=0
"$GCOVR" "${COMMON_ARGS[@]}" --txt --print-summary --fail-under-line 90 || SEUIL_RC=$?

# Rapport HTML détaillé
"$GCOVR" "${COMMON_ARGS[@]}" --html-details "$OUT_DIR/index.html" >/dev/null
echo
echo "==> Rapport HTML : $OUT_DIR/index.html"

if [ "${1:-}" = "--open" ] && command -v open >/dev/null 2>&1; then
  open "$OUT_DIR/index.html"
fi

if [ "$SEUIL_RC" -ne 0 ]; then
  echo
  echo "❌ Couverture de lignes SOUS le seuil de 90 % — la CI refusera ce push." >&2
  exit "$SEUIL_RC"
fi
