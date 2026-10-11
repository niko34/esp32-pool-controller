#!/usr/bin/env bash
# =============================================================================
# check_trace_logic_vectors.sh — la logique pure de « Signaux capteurs » tourne vraiment
# =============================================================================
# feature-153 (D3), patron `check_sha256_vectors.sh` (feature-118) : une
# sentinelle que personne ne lance ne protège de rien — c'était exactement le
# défaut de `sha256_vectors.js` avant feature-118 (vert, versionné, jamais
# appelé par aucun workflow).
#
# CE QUE test/trace_logic_vectors.js VÉRIFIE (et pourquoi il vise le code
# VIVANT, pas une copie) : la carte « Signaux capteurs » de l'onglet Avancé
# s'appuie sur trois fonctions PURES de `data/app.js` —
# `groupHistoryPoints()`, `seriesStats()`, `traceWindowDurationMs()` — sans
# dépendance au DOM ni à `authFetch` (contrainte D3). Le script EXTRAIT le
# texte réel de ces trois fonctions (ancres + comptage de profondeur, pas une
# recopie), l'évalue, et compare le résultat à des vecteurs couvrant les
# fenêtres partielles, la fenêtre entièrement vide (aucun point, pas un point
# à 0), les bornes de fenêtre, les `null` ignorés sans être comptés, et la
# dépendance de `traceWindowDurationMs` à `intervalMs`.
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun `actions/setup-node` nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_trace_logic_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/trace_logic_vectors.js" >&2
  exit 1
fi

node test/trace_logic_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-153 : la logique pure de « Signaux capteurs » ne correspond plus au code livré" >&2
  echo "(data/app.js, groupHistoryPoints/seriesStats/traceWindowDurationMs) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
