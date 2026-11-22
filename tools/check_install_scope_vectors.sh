#!/usr/bin/env bash
# Vecteurs de « ce que l'appareil sait » selon le raccordement (feature-210).
# ⛔ Garde aussi l'ABSENCE de la promesse fausse « garantit qu'aucun produit
#    n'est injecté sans circulation d'eau » dans les QUATRE fichiers où elle
#    vivait — elle y avait survécu en triple exemplaire.
set -euo pipefail
cd "$(dirname "$0")/.."
node test/install_scope_vectors.js
