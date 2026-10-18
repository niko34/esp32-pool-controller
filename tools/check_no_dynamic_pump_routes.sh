#!/usr/bin/env bash
# =============================================================================
# check_no_dynamic_pump_routes.sh — ce qui a été retiré reste retiré (bug-170)
# =============================================================================
# Patron `check_no_peek_flag.sh` (feature-128/ADR-0081) et
# `check_removed_measurement_vocabulary.sh` (feature-152) : une règle écrite
# n'est pas une règle vérifiée.
#
# CE QUI A ÉTÉ RETIRÉ, ET POURQUOI (ADR-0117) :
# `POST /pump1/duty/:duty` et `/pump2/duty/:duty` démarraient une pompe DOSEUSE
# à un rapport cyclique arbitraire avec pour SEUL contrôle l'authentification —
# ni présence d'eau, ni limite journalière/horaire, ni garde d'étalonnage —
# alors que la route VOISINE `/pumpN/on` passait, elle, par
# `injectionAllowedOrReject()`.
#
# ⚠️ ET PIRE : elles posaient `manualMode[i]`, or le bloc de suspension globale
# ne coupe une pompe que `if (!manualMode[i])`. Une pompe démarrée ainsi
# continuait de tourner — filtration arrêtée ou non, étalonnage ouvert ou non —
# jusqu'à un arrêt EXPLICITE. Elles ne journalisaient rien non plus.
#
# ⚠️ POURQUOI UN SCRIPT PLUTÔT QU'UN TEST : les handlers `AsyncWebServer` ne
# sont pas instanciables dans l'environnement natif (ADR-0017, logique pure) ;
# un test à base de mock testerait le mock, pas le routage. Le risque réel
# n'est pas une régression de comportement, c'est une RÉINTRODUCTION : dans six
# mois, quelqu'un a besoin d'un balayage de duty scripté, retrouve la route
# dans un vieux `curl`, et la réécrit « comme avant ».
#
# CE QUE CE SCRIPT VÉRIFIE :
#   1. `handleDynamicPumpRoutes` n'existe plus dans `src/` (hors pierre tombale) ;
#   2. le corps de `server->onNotFound(...)` ne fait QUE répondre 404 — aucune
#      inspection d'URL (`startsWith`, `url()`), aucune commande d'actionneur
#      (`setManualPump`).
#
# ⛔ CE N'EST PAS UNE INTERDICTION DU BESOIN : pour piloter une pompe à la main,
# `/pumpN/on` accepte `?duty=N`, borné et DERRIÈRE les gardes. Rien n'est perdu.
#
# Usage : ./tools/check_no_dynamic_pump_routes.sh
# Sortie : 0 si conforme, 1 sinon (détail sur stdout).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

erreurs=0

# --- 1. Le handler ne doit exister nulle part ailleurs que dans son épitaphe --
# La pierre tombale de web_routes_control.cpp CITE volontairement le nom, pour
# qu'un `grep` « est-ce que ça existe ? » y atterrisse. On ne compte donc que
# les occurrences en dehors des lignes de commentaire.
vivantes="$(grep -rn 'handleDynamicPumpRoutes' src/ 2>/dev/null \
            | grep -vE ':[[:space:]]*//' || true)"
if [ -n "$vivantes" ]; then
  echo "ERREUR : handleDynamicPumpRoutes() est RÉAPPARUE dans src/ :" >&2
  echo "$vivantes" >&2
  echo "  → routage dynamique vers un actionneur : interdit par ADR-0117." >&2
  erreurs=1
fi

# --- 2. onNotFound ne fait QUE 404 ---------------------------------------------
fichier="src/web_server.cpp"
if [ ! -r "$fichier" ]; then
  echo "ERREUR : $fichier introuvable." >&2
  exit 1
fi

corps="$(awk '
  /server->onNotFound\(/ { dedans = 1 }
  dedans { print }
  dedans && /^[[:space:]]*\}\);[[:space:]]*$/ { dedans = 0 }
' "$fichier" | grep -vE '^[[:space:]]*//')"

if [ -z "$corps" ]; then
  echo "ERREUR : bloc server->onNotFound( introuvable dans $fichier." >&2
  echo "  → le script ne peut plus rien garantir : l'adapter, pas le supprimer." >&2
  exit 1
fi

for motif in 'startsWith' 'setManualPump' 'url()'; do
  if echo "$corps" | grep -q "$motif"; then
    echo "ERREUR : '$motif' est réapparu dans le corps de onNotFound() :" >&2
    echo "$corps" | grep -n "$motif" >&2
    echo "  → onNotFound ne sert QUE le 404 (ADR-0117). Un routage dynamique" >&2
    echo "    échappe par construction à l'inventaire des routes : c'est ce qui" >&2
    echo "    a permis à un chemin de démarrage de pompe d'échapper à TOUTES" >&2
    echo "    les gardes (bug-170)." >&2
    erreurs=1
  fi
done

if [ "$erreurs" -ne 0 ]; then
  exit 1
fi

echo "OK : aucun routage dynamique vers un actionneur (ADR-0117, bug-170)."
exit 0
