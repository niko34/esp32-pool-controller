#!/usr/bin/env bash
# =============================================================================
# check_ota_wait_vectors.sh — les vecteurs de l'attente du retour de l'appareil
# après mise à jour tournent vraiment (bug-177)
# =============================================================================
# Patron `check_sig_parse_vectors.sh`. test/ota_wait_vectors.js EXTRAIT le texte
# réel (ADR-0099) de OTA_WAIT_CFG, otaProbeOutcome, otaRebootWaitStep,
# otaRecoveryTarget et otaFetchNoStore dans data/app.js — jamais une copie.
# Vecteur clé : aucune action reload pendant la grâce de 8 s (l'appareil répond
# 200 puis redémarre 3 s plus tard).
# Usage : ./tools/check_ota_wait_vectors.sh   (node requis sur le PATH)
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH" >&2
  exit 1
fi
node test/ota_wait_vectors.js
status=$?
if [ $status -ne 0 ]; then
  echo "ERREUR bug-177 : les vecteurs d'attente OTA ne correspondent plus au code livré (data/app.js)." >&2
  exit 1
fi
exit 0
