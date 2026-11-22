#!/usr/bin/env bash
# =============================================================================
# check_ph_stale_banner_vectors.sh — les vecteurs du bandeau « Correction à
# vérifier » et des seuils servis tournent vraiment (feature-170, V7 / V8 / V9)
# =============================================================================
# Patron `check_ph_tune_remove_vectors.sh` (feature-157) : une sentinelle que
# personne ne lance ne protège de rien. Le harnais test/ph_stale_banner_vectors.js
# EXTRAIT le code réel de data/app.js (ADR-0099) et l'évalue sous Node.
#
# Usage : ./tools/check_ph_stale_banner_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon.
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/ph_stale_banner_vectors.js" >&2
  exit 1
fi

node test/ph_stale_banner_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-170 : les vecteurs du bandeau de correction ne correspondent plus au code livré" >&2
  echo "(data/app.js, renderPhOffsetStaleBanner / _applyPhOffsetThresholds) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
