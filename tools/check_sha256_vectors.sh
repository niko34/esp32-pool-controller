#!/usr/bin/env bash
# =============================================================================
# check_sha256_vectors.sh — les vecteurs SHA-256 du JS d'upload tournent vraiment
# =============================================================================
# feature-118 (P2/security-reviewer, condition 1) : `test/sha256_vectors.js`
# était vert, versionné… et ORPHELIN — aucun des trois workflows ne l'appelait,
# et `pio test -e native` ne voit pas un fichier `.js`. « Une sentinelle que
# personne ne lance ne protège de rien. »
#
# CE QUE test/sha256_vectors.js VÉRIFIE (et pourquoi il vise le code VIVANT,
# pas une copie) : la page d'upload OTA (`data/app.js`) embarque une
# implémentation SHA-256 pure JS (crypto.subtle est indisponible en HTTP clair
# sur IP de LAN). Le script EXTRAIT le texte réel de `const SHA256_K = …` et
# `function createSha256() { … }` depuis data/app.js (ancres + comptage de
# profondeur, pas une recopie), l'évalue, et compare son résultat à
# `node:crypto` sur des vecteurs couvrant les bascules de bourrage (55/56/64/65
# octets) et un flux fragmenté > 1 Mo comme le fait réellement sha256OfFile().
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun `actions/setup-node` nécessaire, cohérent avec l'esprit de ce workflow
# (« sans toolchain, sans cache »). En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_sha256_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/sha256_vectors.js" >&2
  exit 1
fi

node test/sha256_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-118 : les vecteurs SHA-256 ne correspondent plus au code livré" >&2
  echo "(data/app.js, createSha256/SHA256_K) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
