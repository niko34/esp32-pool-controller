#!/usr/bin/env bash
# =============================================================================
# check_github_root_ca.sh — le paquet de racines TLS reste cohérent (bug-165)
# =============================================================================
# `src/github_root_ca.h` n'est pas compilable nativement : ce contrôle délègue
# à tools/github_root_ca_vectors.py, qui EXTRAIT les 5 certificats réels du
# fichier livré (openssl) et vérifie :
#   - il y en a bien 5 (3 racines d'origine + Sectigo E46/R46 ajoutées au
#     correctif bug-165) ;
#   - aucune des 3 racines d'origine n'a disparu (ISRG Root X1 ancre
#     objects.githubusercontent.com, le chemin de téléchargement OTA) ;
#   - les empreintes SHA-256 annoncées en commentaire correspondent aux
#     certificats réellement présents.
#
# ⛔ Ne valide PAS la chaîne TLS réelle servie par GitHub (nécessite le
# matériel ou une sonde réseau — hors de portée d'un test natif/CI).
#
# Usage : ./tools/check_github_root_ca.sh
# Sortie : 0 si conforme, 1 sinon (détail sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v openssl >/dev/null 2>&1; then
  echo "ERREUR : openssl introuvable sur le PATH" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "ERREUR : python3 introuvable sur le PATH" >&2
  exit 1
fi

python3 tools/github_root_ca_vectors.py
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR bug-165 : src/github_root_ca.h n'est plus structurellement conforme" >&2
  echo "(voir le détail ci-dessus)." >&2
  exit 1
fi

exit 0
