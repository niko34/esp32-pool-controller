#!/usr/bin/env bash
# =============================================================================
# check_decimal_format_vectors.sh — les vecteurs de la convention décimale tournent
# vraiment (bug-199 M1-M4, M6). Patron check_ph_stale_banner_vectors.sh.
# Le harnais test/decimal_format_vectors.js EXTRAIT le code réel de data/app.js
# (ADR-0099) et l'évalue sous Node.   Sortie : 0 conforme, 1 sinon.
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH" >&2; exit 1
fi
node test/decimal_format_vectors.js || {
  echo "ERREUR bug-199 : la convention décimale ne correspond plus au code livré (data/app.js)" >&2
  exit 1
}
