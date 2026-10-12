#!/usr/bin/env bash
# =============================================================================
# check_ph_tune_remove_vectors.sh — les vecteurs du bouton « Supprimer la
# correction » et de _fmtPh tournent vraiment (feature-157)
# =============================================================================
# Patron `check_ph_tune_vectors.sh` (feature-155) / `check_ph_health_vectors.sh`
# (feature-154) / `check_sha256_vectors.sh` (feature-118) / `check_trace_logic_
# vectors.sh` (feature-153) : une sentinelle que personne ne lance ne protège
# de rien.
#
# CE QUE test/ph_tune_remove_vectors.js VÉRIFIE : deux éléments de feature-157
# qu'aucun harnais du dépôt ne couvrait avant ce lot —
#   - `_fmtPh()` de `data/app.js` : le formatage à une décimale, virgule
#     française, qui EST la feature, et qui est PARTAGÉ par plusieurs écrans.
#     Un futur lot qui remettrait `toFixed(2)` pour un autre écran annulerait
#     feature-157 en silence, sans qu'aucun test ni aucune revue ne le voie ;
#   - `renderPhTuneRemoveRow()` : la condition d'affichage du bouton
#     « Supprimer la correction » et son MASQUAGE MUTUEL avec la bannière
#     « correction caduque » (`ph_offset_stale`) — les deux ne doivent jamais
#     être visibles en même temps, et le bouton ne doit jamais apparaître
#     pour une correction inconnue (offset absent/non numérique/non fini,
#     traité comme 0).
#
# Le script EXTRAIT le texte réel de ces deux fonctions (ancre + comptage de
# profondeur via `test/js_extract.js`, ADR-0099) au lieu d'en tester une
# copie, les assemble dans un bac à sable Node avec un bouchon DOM minimal
# (`$`, objets `{hidden, textContent}`, zéro dépendance npm), puis les évalue
# contre des vecteurs couvrant les bornes de `_fmtPh` (positif/négatif/nul/
# signe/virgule), son comportement RÉEL — documenté, pas corrigé — sur les
# valeurs non finies, et la matrice complète de `renderPhTuneRemoveRow`
# (offset nul sous toutes ses formes, affichage normal, masquage mutuel,
# égalité stricte de `ph_offset_stale`, garde défensive sur DOM partiel).
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_ph_tune_remove_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/ph_tune_remove_vectors.js" >&2
  exit 1
fi

node test/ph_tune_remove_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-157 : les vecteurs du bouton « Supprimer la correction » ne correspondent plus au code livré" >&2
  echo "(data/app.js, _fmtPh / renderPhTuneRemoveRow) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
