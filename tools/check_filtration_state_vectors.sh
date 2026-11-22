#!/usr/bin/env bash
# Vecteurs de la bande « eau froide » et de l'état de filtration (bug-197).
set -euo pipefail
cd "$(dirname "$0")/.."
node test/filtration_state_vectors.js
