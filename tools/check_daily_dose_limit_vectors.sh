#!/usr/bin/env bash
# Vecteurs de la limite journalière réellement opposable (bug-208).
set -euo pipefail
cd "$(dirname "$0")/.."
node test/daily_dose_limit_vectors.js
