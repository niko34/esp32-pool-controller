#!/usr/bin/env bash
# =============================================================================
# check_ph_tune_vectors.sh — les vecteurs d'accord pH sur un test manuel
# tournent vraiment (feature-155)
# =============================================================================
# Patron `check_ph_health_vectors.sh` (feature-154) / `check_sha256_vectors.sh`
# (feature-118) / `check_trace_logic_vectors.sh` (feature-153) : une
# sentinelle que personne ne lance ne protège de rien.
#
# CE QUE test/ph_tune_vectors.js VÉRIFIE : `evaluatePhTunePreview()` de
# `data/app.js` est l'APERÇU client de l'accord de la mesure pH sur un test
# manuel — JAMAIS la décision (condition 1 du gate pool-chemistry : c'est le
# FIRMWARE qui calcule et applique l'offset, via `evaluatePhTune()` de
# `src/ph_tune_logic.h`, couvert par `test/test_native_ph_tune`). Le script
# EXTRAIT le texte réel de cette fonction (ancre + comptage de profondeur via
# `test/js_extract.js`, ADR-0099) au lieu d'en tester une copie, puis
# l'évalue contre des vecteurs couvrant la mesure indisponible, les bornes
# ±0,50/±0,51, l'écart +0,80 (non écrêté dans l'aperçu — c'est le serveur qui
# refuse), le Piège 1 (valeurAtteinte = phValueRaw + ecart, jamais
# valeurAmenee), une assertion STRUCTURELLE (ADR-0100 : aucun littéral
# numérique hors 0 et 1), et une comparaison directe de verdict avec un
# MIROIR de la logique de bornes serveur (coherence aperçu/serveur).
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_ph_tune_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/ph_tune_vectors.js" >&2
  exit 1
fi

node test/ph_tune_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-155 : les vecteurs d'accord pH ne correspondent plus au code livré" >&2
  echo "(data/app.js, evaluatePhTunePreview) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
