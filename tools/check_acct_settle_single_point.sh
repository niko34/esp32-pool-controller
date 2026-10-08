#!/usr/bin/env bash
# =============================================================================
# check_acct_settle_single_point.sh — accountPumpVolume() n'a qu'un appelant
# =============================================================================
# bug-115 (v2.61.0) : la correction de la rémanence de comptabilité (feature-110)
# repose sur un point de passage UNIQUE, settlePumpAccounting() — quiconque
# ajoute un appel direct à accountPumpVolume() ailleurs (une sortie anticipée
# "oubliée", un raccourci de debug, une future fonctionnalité pressée) rejoue
# le défaut : une fenêtre qui ne règle jamais sa comptabilité, ou qui la règle
# deux fois.
#
# CE QUE PROUVE CE SCRIPT (garantie STRUCTURELLE, pas testable nativement) :
# pump_controller.cpp n'est PAS compilable en env:native (FreeRTOS, Arduino,
# I2C) — aucun test unitaire natif ne peut démontrer que les quatre sorties
# anticipées d'updateDosingCore() atteignent toutes settlePumpAccounting().
# Ce script vérifie la condition NÉCESSAIRE à la place : `accountPumpVolume(`
# n'apparaît QU'AUX DEUX POINTS attendus —
#   1. sa DÉFINITION (`PumpControllerClass::accountPumpVolume`, pump_controller.cpp) ;
#   2. sa DÉCLARATION (pump_controller.h) ;
#   3. les DEUX appels, et SEULEMENT ceux-là, DANS LE CORPS de
#      PumpControllerClass::settlePumpAccounting().
# Un appel ailleurs — même dans updateDosingCore() elle-même — est un
# contournement du point de passage unique et doit faire échouer ce contrôle.
#
# ⛔ CE QUE CE SCRIPT NE PEUT PAS FERMER (même limite que check_fopen_guard.sh) :
# grep/awk travaillent LIGNE À LIGNE ; un appel reformaté sur plusieurs lignes
# échappe aux motifs. Et ce script ne prouve pas que settlePumpAccounting()
# elle-même est bien appelée par les quatre sorties anticipées — seulement
# qu'il n'existe pas d'appel DIRECT à accountPumpVolume() en dehors d'elle.
#
# Usage : ./tools/check_acct_settle_single_point.sh
# Sortie : 0 si conforme, 1 sinon (détail sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

FILE_H="src/pump_controller.h"
FILE_CPP="src/pump_controller.cpp"

if [ ! -f "$FILE_CPP" ] || [ ! -f "$FILE_H" ]; then
  echo "ERREUR : $FILE_CPP ou $FILE_H introuvable." >&2
  exit 1
fi

# Toutes les occurrences non-commentées de accountPumpVolume( dans src/,
# n'importe quel fichier .cpp/.h — une occurrence dans un AUTRE fichier que
# pump_controller.{cpp,h} est déjà, par construction, une violation.
all_matches=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 grep -HnE '(->|\.|::)?[[:space:]]*accountPumpVolume[[:space:]]*\(' \
  | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*)' \
  || true)

other_files=$(echo "$all_matches" | grep -vE "^(${FILE_CPP}|${FILE_H}):" || true)
if [ -n "$other_files" ]; then
  {
    echo "ERREUR bug-115 : accountPumpVolume() appelée hors de pump_controller.{cpp,h}."
    echo "$other_files"
  } >&2
  exit 1
fi

# Déclaration unique dans le header.
decl=$(grep -nE '^[[:space:]]*void[[:space:]]+accountPumpVolume[[:space:]]*\(' "$FILE_H" || true)
decl_count=$(echo "$decl" | grep -c . || true)
if [ "$decl_count" -ne 1 ]; then
  {
    echo "ERREUR bug-115 : attendu EXACTEMENT 1 déclaration de accountPumpVolume() dans $FILE_H, trouvé $decl_count."
    echo "$decl"
  } >&2
  exit 1
fi

# Définition unique dans le .cpp.
def=$(grep -nE '::[[:space:]]*accountPumpVolume[[:space:]]*\(' "$FILE_CPP" || true)
def_count=$(echo "$def" | grep -c . || true)
if [ "$def_count" -ne 1 ]; then
  {
    echo "ERREUR bug-115 : attendu EXACTEMENT 1 définition de accountPumpVolume() dans $FILE_CPP, trouvé $def_count."
    echo "$def"
  } >&2
  exit 1
fi

# Toutes les lignes restantes du .cpp qui invoquent accountPumpVolume( SANS
# qualification (::) et qui ne sont pas la ligne de définition : ce sont les
# appels. Il doit y en avoir EXACTEMENT 2, et tous deux DOIVENT être situés
# dans le corps de settlePumpAccounting() — entre sa signature et la première
# accolade fermante en colonne 0 qui suit.
calls=$(grep -nE '^[^/*].*[^:]accountPumpVolume[[:space:]]*\(' "$FILE_CPP" \
  | grep -vE '^[0-9]+:[[:space:]]*(//|\*)' \
  | grep -vE '::[[:space:]]*accountPumpVolume' \
  | grep -vE '^[0-9]+:void[[:space:]]+accountPumpVolume' \
  || true)
call_count=$(echo "$calls" | grep -c . || true)
if [ "$call_count" -ne 2 ]; then
  {
    echo "ERREUR bug-115 : attendu EXACTEMENT 2 appels à accountPumpVolume() dans $FILE_CPP (les deux de settlePumpAccounting()), trouvé $call_count."
    echo "$calls"
  } >&2
  exit 1
fi

settle_start=$(grep -nE '::[[:space:]]*settlePumpAccounting[[:space:]]*\(' "$FILE_CPP" | head -1 | cut -d: -f1 || true)
if [ -z "$settle_start" ]; then
  echo "ERREUR bug-115 : définition de settlePumpAccounting() introuvable dans $FILE_CPP." >&2
  exit 1
fi
settle_end=$(awk -v start="$settle_start" 'NR > start && /^}/ { print NR; exit }' "$FILE_CPP")
if [ -z "$settle_end" ]; then
  echo "ERREUR bug-115 : accolade fermante de settlePumpAccounting() introuvable." >&2
  exit 1
fi

outside=$(echo "$calls" | while IFS= read -r line; do
  [ -z "$line" ] && continue
  ln=$(echo "$line" | cut -d: -f1)
  if [ "$ln" -lt "$settle_start" ] || [ "$ln" -gt "$settle_end" ]; then
    echo "$line"
  fi
done)
if [ -n "$outside" ]; then
  {
    echo "ERREUR bug-115 : appel(s) à accountPumpVolume() HORS du corps de"
    echo "settlePumpAccounting() (lignes ${settle_start}-${settle_end}) :"
    echo "$outside"
  } >&2
  exit 1
fi

echo "OK — accountPumpVolume() n'a qu'un appelant : settlePumpAccounting() (lignes ${settle_start}-${settle_end})."

# =============================================================================
# GARDE DU WRAPPER — point bloquant relevé en revue (2026-10-08)
# =============================================================================
# Le contrôle ci-dessus ne vérifie que les APPELANTS d'accountPumpVolume(). Il
# laisse donc passer le scénario de régression le plus probable :
#
#   quelqu'un réintègre updateDosingCore() dans update(), retire
#   _acctSettledThisTick — et la garde RESTE VERTE.
#
# Les quatre appels locaux à settlePumpAccounting() dans les sorties anticipées
# font justement paraître le wrapper redondant. Il ne l'est pas : c'est LUI qui
# couvre une CINQUIÈME sortie que quelqu'un ajouterait sans y penser. Sans lui,
# le défaut de bug-115 rejoue en silence.
#
# On exige donc les trois lignes du wrapper dans le corps d'update().
WRAPPER_NEEDLES=(
  "_acctSettledThisTick = false;"
  "updateDosingCore("
  "if (!_acctSettledThisTick) settlePumpAccounting("
)
upd_start=$(grep -nE '^void[[:space:]]+PumpControllerClass::update\(\)' "$FILE_CPP" | head -1 | cut -d: -f1 || true)
if [ -z "$upd_start" ]; then
  echo "ERREUR bug-115 : PumpControllerClass::update() introuvable dans $FILE_CPP." >&2
  exit 1
fi
upd_end=$(awk -v start="$upd_start" 'NR > start && /^}/ { print NR; exit }' "$FILE_CPP")
upd_body=$(sed -n "${upd_start},${upd_end}p" "$FILE_CPP" | grep -vE '^[[:space:]]*(//|\*)')
for needle in "${WRAPPER_NEEDLES[@]}"; do
  if ! printf '%s' "$upd_body" | grep -qF "$needle"; then
    {
      echo "ERREUR bug-115 : le WRAPPER de PumpControllerClass::update() est incomplet."
      echo "Ligne manquante dans son corps : $needle"
      echo
      echo "Ce wrapper est le FILET STRUCTUREL qui garantit qu'une sortie anticipée"
      echo "FUTURE sera comptabilisee meme si personne n'y pense. Les quatre appels"
      echo "locaux le font paraitre redondant : il ne l'est pas. Ne pas le retirer."
    } >&2
    exit 1
  fi
done

echo "OK — wrapper update() intact (les 3 lignes du filet structurel sont presentes)."
