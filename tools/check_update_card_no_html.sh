#!/usr/bin/env bash
# =============================================================================
# check_update_card_no_html.sh — la carte « Mise à jour » n'écrit jamais de HTML (feature-151 D3-b2, B7)
# =============================================================================
# Région surveillée de data/app.js : de `let onlineUpdateBlocked` (début de la
# logique de mise à jour en ligne) jusqu'à la FIN de bindManualUpdate (mise à
# jour manuelle incluse). Elle affiche des textes qui viennent de l'extérieur
# (release GitHub : latest_version, release_notes, message ; appareil : codes
# d'erreur). D3-c y affichera en plus un message relayé depuis GitHub.
# Acquis à protéger : TOUT passe par textContent / createElement / replaceChildren.
#
# Rougit si la région contient innerHTML, insertAdjacentHTML, outerHTML ou
# document.write. Les lignes de pur commentaire sont ignorées (plusieurs
# écrivent « jamais innerHTML »). Ancres absentes => rouge (pas de faux vert).
#
# Usage : ./tools/check_update_card_no_html.sh [fichier.js]   Sortie : 0 propre, 1 sinon.
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
js="${1:-data/app.js}"

start=$(grep -n '^  let onlineUpdateBlocked = false;' "$js" | head -1 | cut -d: -f1)
bm=$(grep -n '^  function bindManualUpdate()' "$js" | head -1 | cut -d: -f1)
if [ -z "$start" ] || [ -z "$bm" ] || [ "$bm" -le "$start" ]; then
  echo "ERREUR : ancres de la carte de mise à jour introuvables dans $js (onlineUpdateBlocked / bindManualUpdate)." >&2
  exit 1
fi
end=$(awk -v s="$bm" 'NR>s && /^  function /{print NR-1; exit}' "$js")
[ -z "$end" ] && end=$(wc -l < "$js")

hits=$(sed -n "${start},${end}p" "$js" | grep -nE 'innerHTML|insertAdjacentHTML|outerHTML|document\.write' | grep -vE '^[0-9]+:[[:space:]]*//' || true)
if [ -n "$hits" ]; then
  echo "ERREUR D3-b2 (B7) : écriture HTML dans la carte de mise à jour ($js:${start}-${end}) :" >&2
  echo "$hits" >&2
  echo "Utiliser textContent / createElement / replaceChildren." >&2
  exit 1
fi
echo "OK — carte de mise à jour sans innerHTML/insertAdjacentHTML/outerHTML/document.write ($js:${start}-${end})."
