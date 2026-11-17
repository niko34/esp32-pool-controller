#!/usr/bin/env bash
# =============================================================================
# check_ota_manager_isolated.sh — aucun appelant de otaManager / ArduinoOTA hors de ota_manager.*
# (feature-151 D3-c, condition 8 du verdict security-reviewer P1)
# ArduinoOTA est un chemin d'écriture flash qui CONTOURNE le refus d'authenticité de /update et
# /download-update : le rebrancher sans passer par la vérification de signature annulerait D3-c.
# Les commentaires // sont retirés avant le grep.
# Usage : ./tools/check_ota_manager_isolated.sh [racine]   Sortie : 0 propre, 1 sinon.
# =============================================================================
set -u
cd "${1:-$(dirname "$0")/..}"
rc=0
for f in src/*.cpp src/*.h; do
  case "$f" in src/ota_manager.cpp|src/ota_manager.h) continue;; esac
  if sed -e 's|//.*$||' "$f" | grep -qE '\botaManager\b|ota_manager\.h|ArduinoOTA'; then
    echo "ERREUR : $f référence otaManager / ota_manager.h / ArduinoOTA (contournement du refus D3-c)" >&2
    rc=1
  fi
done
exit $rc
