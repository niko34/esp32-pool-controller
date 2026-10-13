#!/usr/bin/env bash
# =============================================================================
# check_cal_points_acquired_vectors.sh — les vecteurs de « ce point de
# calibration est-il acquis ? » tournent vraiment (feature-159, correctif
# bloquant du code-reviewer)
# =============================================================================
# Patron `check_ph_health_vectors.sh` (feature-154) / `check_ph_tune_remove_
# vectors.sh` (feature-157) : une sentinelle que personne ne lance ne protège
# de rien.
#
# CE QUE test/cal_points_acquired_vectors.js VÉRIFIE : la fonction PURE
# `_calPointsAcquired(product, pts)` de `data/app.js`, qui décide quels
# points de calibration pH/ORP sont RÉELLEMENT acquis d'après le firmware.
# Elle corrige un défaut fail-open relevé en revue : un `pts` non numérique
# (undefined/null — `/data` pas encore répondu, ou hors ligne) se comportait
# comme « non calibré » au lieu de « inconnu », armant à tort tous les
# boutons « Calibrer » du pas-à-pas pour un point déjà étalonné.
#
# Le script EXTRAIT le texte réel de cette fonction (ancre + comptage de
# profondeur via `test/js_extract.js`, ADR-0099) au lieu d'en tester une
# copie, puis l'évalue contre la matrice complète : -1 (EZO injoignable),
# undefined/null (inconnu — le défaut corrigé), 0 (connu, rien calibré),
# 1 et 2 côté pH (asymétrie mid/low), et le seuil `>= 1` côté ORP.
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_cal_points_acquired_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/cal_points_acquired_vectors.js" >&2
  exit 1
fi

node test/cal_points_acquired_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-159 : les vecteurs de _calPointsAcquired ne correspondent plus au code livré" >&2
  echo "(data/app.js, _calPointsAcquired) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
