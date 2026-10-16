#!/usr/bin/env bash
# =============================================================================
# check_cal_session_reentry_vectors.sh — rouvrir l'écran d'étalonnage dégèle
# bien les boutons (bug-167, défaut trouvé en revue)
# =============================================================================
# Patron `check_cal_points_acquired_vectors.sh` (feature-159) : une sentinelle
# que personne ne lance ne protège de rien.
#
# CE QUE test/cal_session_reentry_vectors.js VÉRIFIE : que le gel des boutons
# « Calibrer » porte sur l'AVANCEMENT DE LA SESSION EN COURS et jamais sur
# l'HISTORIQUE de la sonde (`phCalPoints`). Les deux coïncident sur une sonde
# neuve et divergent sur toutes les autres — c'est toute l'origine de bug-167 :
# la sonde de ce bassin rapportait déjà 2 points, donc les boutons naissaient
# gris et l'étalonnage était impossible.
#
# ⚠️ CE HARNAIS EXISTE PARCE QUE LE BUG A ÉTÉ MANQUÉ TROIS FOIS, chaque fois
# par LECTURE STATIQUE du code :
#   1. 1ʳᵉ correction — un seul des DEUX sites de gel avait été vu ; le second
#      réarmait puis re-figeait les boutons en silence.
#   2. Clôture documentaire — le critère « sortir puis revenir dégèle tout » a
#      été coché après relecture : la remise à zéro EXISTE bien… mais elle
#      s'exécute APRÈS le calcul du gel.
#   3. Le défaut réel, trouvé en revue : `resetCalStepper()` tournait AVANT
#      `_calSwitchEnter()` (async), et `_calRefreshFrozenButtons()` N'ACTIVE
#      jamais un bouton — donc le gel de la 2ᵉ ouverture ne retombait plus.
# ⇒ Lire qu'une remise à zéro existe ne dit RIEN sur le moment où elle tourne.
#   Seul un harnais qui REJOUE une 2ᵉ entrée le prouve.
#
# Le script EXTRAIT le texte réel des fonctions de `data/app.js` (ancre +
# comptage de profondeur via `test/js_extract.js`, ADR-0099) au lieu d'en
# tester une copie — une copie serait restée verte pendant que le code livré
# régressait.
#
# Mutations tuées (vérifiées à l'écriture du harnais) :
#   (a) gel inconditionnel sur `phCalPoints` dans `resetCalStepper`  → 12 vecteurs
#   (b) `Object.assign` de re-gel réintroduit au premier chargement  →  1 vecteur
#   (c) ordre inversé dans `_calEnterScreen` (l'ordre EST le correctif) → 3 vecteurs
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_cal_session_reentry_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/cal_session_reentry_vectors.js" >&2
  exit 1
fi

node test/cal_session_reentry_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR bug-167 : le gel des boutons « Calibrer » ne correspond plus au code livré" >&2
  echo "(data/app.js : _calEnterScreen / _calResetSessionState / resetCalStepper)." >&2
  echo "⚠️ Vérifier en particulier l'ORDRE : la remise à zéro de l'état de session doit" >&2
  echo "   s'exécuter AVANT resetCalStepper(), jamais après — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
