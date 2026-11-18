#!/usr/bin/env bash
# Vecteurs de la teinte des pastilles du tableau de bord (feature-195, B1).
set -euo pipefail
cd "$(dirname "$0")/.."
node test/dash_pill_vectors.js
