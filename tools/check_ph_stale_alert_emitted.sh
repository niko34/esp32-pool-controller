#!/usr/bin/env bash
# =============================================================================
# check_ph_stale_alert_emitted.sh — l'alerte « correction à vérifier » est bien
# ÉMISE, au bon endroit, sans lire la config hors de son verrou (feature-170 (C32))
# =============================================================================
# ⭐ Principe posé par feature-170 : UN VECTEUR NATIF NE PROUVE JAMAIS UN SITE
# D'APPEL. test/test_native_ph_stale_message/ prouve le TEXTE ; il ne peut pas
# prouver que markPhCalibrationStamped() l'envoie. Sans ce contrôle, la condition
# reposerait sur « quelqu'un appelle bien ceci, au bon endroit » — c'est-à-dire
# sur rien. Ce script lit le corps de la fonction dans src/config.cpp et exige :
#
#   1. l'appel  publishAlert("ph_offset_stale"  EXISTE dans le corps ;
#   2. il vient APRÈS le dernier xSemaphoreGiveRecursive(configMutex) : on
#      n'appelle pas le MQTT (publication réseau, verrous propres) sectionné
#      critique tenue ;
#   3. aucune lecture de `mqttCfg` ne subsiste APRÈS ce give (feature-170 (C32),
#      F3 : lire la config sans le verrou qui la protège est interdit —
#      les trois instantanés phOffset / phOffsetSeq / phCalSeq se prennent DANS
#      la section critique) ;
#   4. la décision passe par l'accesseur pur isPhOffsetSupersededByCalibration()
#      — jamais `offset != 0`, qui recréerait le verdict en deux expressions.
#
# ⛔ ÉCHEC FERMÉ : si la fonction, le give ou l'appel est introuvable, le script
# ÉCHOUE. Une sentinelle qui ne trouve plus ce qu'elle garde et rend 0 est une
# garantie fausse.
#
# ⛔ CE QUE CE SCRIPT NE PEUT PAS FERMER : il travaille ligne à ligne sur le texte
# (commentaires `//` retirés). Une lecture de mqttCfg cachée derrière un
# accesseur ou une macro lui échappe ; un appel construit dynamiquement aussi.
#
# Usage : ./tools/check_ph_stale_alert_emitted.sh [fichier]   (défaut src/config.cpp)
# Sortie : 0 si conforme, 1 sinon (diagnostic sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

FILE="${1:-src/config.cpp}"
FUNC='markPhCalibrationStamped'

fail() { echo "ERREUR feature-170 (C32) : $1" >&2; exit 1; }

[ -f "$FILE" ] || fail "fichier introuvable : $FILE"

# Début de la définition (colonne 0, pas un appel ni une déclaration).
start=$(grep -HnE "^void[[:space:]]+${FUNC}[[:space:]]*\(\)[[:space:]]*\{" "$FILE" | head -1 | cut -d: -f2)
[ -n "$start" ] || fail "définition de ${FUNC}() introuvable dans $FILE (fonction renommée ou déplacée ?)"

# Fin : première accolade fermante en colonne 0 après le début.
end=$(awk -v s="$start" 'NR>s && /^}/ {print NR; exit}' "$FILE")
[ -n "$end" ] || fail "fin de ${FUNC}() introuvable"

# Corps SANS commentaires `//`, numéroté avec les numéros de ligne du fichier.
body=$(awk -v s="$start" -v e="$end" 'NR>=s && NR<=e { l=$0; sub(/\/\/.*/, "", l); print NR ":" l }' "$FILE")

# 1. L'appel existe.
call=$(printf '%s\n' "$body" | grep -E 'publishAlert[[:space:]]*\([[:space:]]*"ph_offset_stale"' | head -1 | cut -d: -f1)
[ -n "$call" ] || fail "aucun publishAlert(\"ph_offset_stale\" dans ${FUNC}() : l'étalonnage ne notifie plus personne hors écran."

# 2. Il est APRÈS le dernier give du mutex de config.
give=$(printf '%s\n' "$body" | grep -E 'xSemaphoreGiveRecursive[[:space:]]*\([[:space:]]*configMutex' | tail -1 | cut -d: -f1)
[ -n "$give" ] || fail "aucun xSemaphoreGiveRecursive(configMutex) dans ${FUNC}() : verrou jamais rendu ou sentinelle aveugle."
if [ "$call" -le "$give" ]; then
  fail "publishAlert (ligne $call) est émis AVANT la libération de configMutex (ligne $give) : publication réseau sous verrou de config."
fi

# 3. Aucune lecture de mqttCfg après le give.
late=$(printf '%s\n' "$body" | awk -F: -v g="$give" '$1>g' | grep -n 'mqttCfg' || true)
if [ -n "$late" ]; then
  {
    echo "ERREUR feature-170 (C32)/F3 : mqttCfg lu APRÈS la libération de configMutex (ligne $give) :"
    printf '%s\n' "$body" | awk -F: -v g="$give" '$1>g' | grep 'mqttCfg'
    echo "Prendre les instantanés (phOffset, phOffsetSeq, phCalSeq) DANS la section critique."
  } >&2
  exit 1
fi

# 4. La décision passe par l'accesseur pur unique, avant l'émission.
dec=$(printf '%s\n' "$body" | grep -E 'isPhOffsetSupersededByCalibration[[:space:]]*\(' | head -1 | cut -d: -f1)
[ -n "$dec" ] || fail "la décision n'appelle plus isPhOffsetSupersededByCalibration() : verdict recopié (piège « même notion, deux expressions »)."
if [ "$dec" -ge "$call" ]; then
  fail "isPhOffsetSupersededByCalibration() (ligne $dec) n'est pas évalué avant l'émission (ligne $call)."
fi

echo "OK — ${FUNC}() : publishAlert ligne $call, après le give ligne $give, sans lecture de mqttCfg ensuite, décision ligne $dec."
