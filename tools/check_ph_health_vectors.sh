#!/usr/bin/env bash
# =============================================================================
# check_ph_health_vectors.sh — les vecteurs de santé pH tournent vraiment
# =============================================================================
# feature-154 (pastilles de santé par critère sonde pH). Patron
# `check_sha256_vectors.sh` (feature-118) / `check_trace_logic_vectors.sh`
# (feature-153) : une sentinelle que personne ne lance ne protège de rien.
#
# CE QUE test/ph_health_vectors.js VÉRIFIE (et pourquoi il vise le code VIVANT,
# pas une copie) : l'écran « État sonde pH » juge chacun de ses trois critères
# (pente acide, pente base, décalage zéro) avec `classifyPhCriterion()` de
# `data/app.js` — MIROIR exact d'`isPhProbeHealthy()` côté firmware
# (`src/ph_guard_logic.h`), qui décide la bascule de sécurité pH. Le script
# EXTRAIT le texte réel de cette fonction (ancre + comptage de profondeur via
# `test/js_extract.js`, ADR-0099 amendé) au lieu d'en tester une copie, puis
# l'évalue contre des vecteurs couvrant les bornes exactes (90,0/110,0/15,0,
# incluses comme saines), le faux vert historique (115 %), le zéro négatif,
# les valeurs non exploitables, et une assertion STRUCTURELLE : le code
# extrait ne contient aucun littéral numérique hors 0 et 1 (ADR-0100 — les
# seuils de santé sont servis par le firmware, jamais réécrits côté client).
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_ph_health_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/ph_health_vectors.js" >&2
  exit 1
fi

node test/ph_health_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-154 : les vecteurs de santé pH ne correspondent plus au code livré" >&2
  echo "(data/app.js, classifyPhCriterion) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
