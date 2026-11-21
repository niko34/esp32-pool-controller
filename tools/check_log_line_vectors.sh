#!/usr/bin/env bash
# Vecteurs du rendu du journal (bug-179) : formatLogLine, rendu en texte, incrémental.
set -euo pipefail
cd "$(dirname "$0")/.."
node test/log_line_vectors.js
