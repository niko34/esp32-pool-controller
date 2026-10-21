#!/usr/bin/env bash
# =============================================================================
# check_sig_parse_vectors.sh — les vecteurs du parseur du fichier de
# signature OTA ".sig" tournent vraiment (feature-151 D1-bis)
# =============================================================================
# Patron `check_resolve_selector_mode_vectors.sh` (feature-161) /
# `check_ph_health_vectors.sh` (feature-154) : une sentinelle que personne ne
# lance ne protège de rien.
#
# CE QUE test/sig_parse_vectors.js VÉRIFIE : la fonction PURE
# `parseSigFile(text)` de `data/app.js` (et son auxiliaire
# `parseOtaManifestLine`), qui valide le fichier ".sig" optionnel de la carte
# "Mise à jour manuelle" AVANT tout envoi — 2 lignes (manifeste signé en
# clair, signature base64), grammaire STRICTE en miroir de
# `src/ota_manifest_logic.{h,cpp}`. ⛔ Aucune cryptographie n'est exécutée ici
# (la page TRANSPORTE, l'appareil VÉRIFIE) : ce script ne couvre que le
# PARSING et son rejet propre (codes "vide"/"lignes"/"manifeste"/"signature").
#
# Le script EXTRAIT le texte réel des deux fonctions (ancre + comptage de
# profondeur via `test/js_extract.js`, ADR-0099) au lieu d'en tester une
# copie, puis les évalue contre la matrice complète : nominal (firmware et
# filesystem), tolérance \r\n et LF final unique, refus de tout contenu
# au-delà de la ligne 2, grammaire du manifeste (préfixe, genre, version
# SemVer sans zéro de tête, hex 64 caractères STRICTEMENT minuscules), et
# grammaire de la signature (512 caractères base64 exactement, alphabet
# strict).
#
# NODE : ubuntu-latest (runners GitHub-hébergés) embarque Node.js par défaut —
# aucun actions/setup-node nécessaire. En local, `node` doit être sur le PATH.
#
# Usage : ./tools/check_sig_parse_vectors.sh
# Sortie : 0 si tous les vecteurs sont conformes, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH — impossible de lancer test/sig_parse_vectors.js" >&2
  exit 1
fi

node test/sig_parse_vectors.js
status=$?

if [ $status -ne 0 ]; then
  echo "ERREUR feature-151 : les vecteurs de parseSigFile ne correspondent plus au code livré" >&2
  echo "(data/app.js, parseSigFile / parseOtaManifestLine) — voir le détail ci-dessus." >&2
  exit 1
fi

exit 0
