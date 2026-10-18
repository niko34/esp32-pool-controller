#!/usr/bin/env bash
# =============================================================================
# check_dose_suspend_outranks_mixing_vectors.sh — les vecteurs bug-172 (H5) tournent
# =============================================================================
# bug-172 : l'utilisateur a vu « pause mélange : 207 s » affiché alors que la
# filtration était arrêtée — le décompte ne menait à rien, la vraie cause ne
# s'écoulant pas. Patron `check_ph_health_vectors.sh` (feature-154) :
# `test/dose_suspend_outranks_mixing_vectors.js` EXTRAIT le texte réel de
# `_doseSuspendOutranksMixing()` depuis `data/app.js` (ancre + comptage de
# profondeur via `test/js_extract.js`, ADR-0099) au lieu d'en tester une
# copie, puis l'évalue contre des vecteurs couvrant les quatre codes de
# suspension globale (2/7/24/25), le code voisin NON concerné (8, un vrai
# mélange en cours qui ne doit jamais se taire), et les cas d'absence
# d'information.
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_dose_suspend_outranks_mixing_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/dose_suspend_outranks_mixing_vectors.js" >&2
  exit 1
fi

node test/dose_suspend_outranks_mixing_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR bug-172 : la priorité des causes de suspension sur le décompte mélange ne correspond plus au code livré" >&2
  echo "(data/app.js, _doseSuspendOutranksMixing) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
