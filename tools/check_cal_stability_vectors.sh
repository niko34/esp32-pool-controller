#!/usr/bin/env bash
# =============================================================================
# check_cal_stability_vectors.sh — la statistique de stabilité de calibration
# (bug-160) tourne vraiment, et son résiduel garanti est TENU
# =============================================================================
# Patron `check_cal_points_acquired_vectors.sh` (feature-159) : une sentinelle
# que personne ne lance ne protège de rien.
#
# CE QUE test/cal_stability_vectors.js VÉRIFIE : la fonction PURE
# `_calStabilityVerdict(product, samples, nowMs)` de `data/app.js`, qui
# remplace l'ancien critère `max − min` (0,9 pH / 20 s, feature-034) — annoncé
# « stable » à 0,8-1,1 pH de l'équilibre réel (chiffrage du gate
# pool-chemistry, bug-160). La nouvelle statistique (dérive médiane
# moitié récente / moitié ancienne, gardée par l'inégalité `G ≥ G_MIN` sur les
# temps RÉELLEMENT observés) garantit un résiduel borné PAR CONSTRUCTION,
# quelle que soit la cadence d'échantillonnage.
#
# Le script EXTRAIT le texte réel de `CAL_STAB`, `_calStabilityVerdict` et
# `pushStabilitySample` (ancre + comptage de profondeur via `test/js_extract.js`,
# ADR-0099) au lieu d'en tester une copie, puis l'évalue contre : des rampes
# exponentielles pH(t) = 4 + 3·e^(−t/τ) pour τ ∈ {10,30,60,120} s (jamais
# « stable » tant que le résiduel réel dépasse 0,10 pH — le défaut historique,
# matérialisé puis corrigé) ; un signal bruité sans dérive (⇒ stable, là où
# l'ancienne étendue pouvait dire « instable ») ; une cadence dégradée 30 s
# sans WebSocket (⇒ le verdict finit par être rendu, jamais muet en
# permanence) ; une rafale de 40 échantillons sur 18 s (⇒ G < G_MIN ⇒ MUET,
# preuve que la coupure au comptage seule ne suffit pas) ; l'invariance au
# recalage d'horloge (performance.now(), jamais Date.now()) ; le plafond de
# 256 échantillons ; un contrôle structurel que la durée affichée à
# l'utilisateur vient de la séparation RÉELLEMENT observée, jamais d'une
# constante nominale (le défaut Δ60 s/Δ20 s reconstitué sous une autre forme) ;
# le régime du SECOND TAMPON (revue bloquante du 2026-10-13 : une rampe qui
# repart d'un résidu modeste, pas de 3 pH — le seul régime où G_MIN mord
# réellement) ; et, sous un DOM minimal mocké, la priorité des SIX états et
# les textes de diagnostic T-D1/T-D2 VERBATIM.
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_cal_stability_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/cal_stability_vectors.js" >&2
  exit 1
fi

node test/cal_stability_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR bug-160 : les vecteurs de stabilité de calibration ne correspondent plus au code livré" >&2
  echo "(data/app.js, CAL_STAB / _calStabilityVerdict / pushStabilitySample) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
