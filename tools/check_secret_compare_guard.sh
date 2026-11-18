#!/usr/bin/env bash
# =============================================================================
# check_secret_compare_guard.sh — le mot de passe ne se compare qu'en temps fixe
# (bug-183, condition de clôture du verdict security-reviewer)
# =============================================================================
# ⭐ CE SCRIPT EXISTE PARCE QU'AUCUN TEST NE PEUT LE FAIRE. Remplacer
# fixedWidthEquals par un memcmp à sortie anticipée rend EXACTEMENT la même
# valeur : la suite native ne voit rien passer (mutant M1, survivant assumé).
# ⛔ CE QUE CETTE GARDE NE PROUVE PAS : elle lit le SOURCE. fixedWidthEquals est
# `inline` dans un en-tête, et transformer « OR des XOR » en sortie anticipée est
# une optimisation LÉGALE du compilateur. Seul le DÉSASSEMBLAGE de l'ELF livré
# prouve la propriété sur le binaire (relevé consigné dans la spec bug-183, avec
# le SHA256 de l'ELF). Cette garde empêche la régression DÉLIBÉRÉE du source ;
# elle ne remplace pas ce relevé.
#
# Quatre règles :
#   A. `getPassword` ne réapparaît pas. Elle a été SUPPRIMÉE pour que le
#      compilateur — et non un grep — interdise la comparaison naïve.
#   B. `adminPassword` n'entre dans aucune comparaison, SAUF contre le littéral
#      public "admin" (état d'usine : il n'y a aucun secret dans cette valeur).
#   C. fixedWidthEquals garde sa forme : accumulation OR des XOR, un seul
#      `return`, et aucune sortie anticipée.
#   D. passwordMatches appelle RÉELLEMENT fixedWidthEquals — sans quoi A à C
#      protégeraient un comparateur que plus personne n'appelle.
#
# Les commentaires // sont retirés avant chaque grep.
# Usage : ./tools/check_secret_compare_guard.sh [racine]   Sortie : 0 propre, 1 sinon.
# =============================================================================
set -u
cd "${1:-$(dirname "$0")/..}"
rc=0

strip() { sed -e 's|//.*$||' "$1"; }

# --- Règle A : getPassword ne revient pas -----------------------------------
for f in src/*.cpp src/*.h; do
  if strip "$f" | grep -nE '\bgetPassword\s*\(' | grep -q .; then
    echo "ERREUR : $f réintroduit getPassword() — supprimée par bug-183 pour que le" >&2
    echo "         compilateur interdise toute comparaison du mot de passe hors" >&2
    echo "         AuthManager::passwordMatches(). Passer par passwordMatches()." >&2
    rc=1
  fi
done

# --- Règle B : adminPassword hors de toute comparaison ----------------------
# Toléré : comparaison contre le littéral "admin" (valeur publique d'usine).
for f in src/*.cpp src/*.h; do
  hits=$(strip "$f" \
    | grep -nE 'adminPassword' \
    | grep -E '==|!=|memcmp|strcmp|strncmp|\.equals[[:space:]]*\(|\.compareTo[[:space:]]*\(' \
    | grep -vE 'adminPassword[[:space:]]*(==|!=)[[:space:]]*"admin"' \
    | grep -vE '"admin"[[:space:]]*(==|!=)[[:space:]]*adminPassword' || true)
  if [ -n "$hits" ]; then
    echo "ERREUR : $f compare adminPassword directement :" >&2
    echo "$hits" | sed 's/^/           /' >&2
    echo "         Une comparaison par == / memcmp s'arrête au premier octet qui" >&2
    echo "         diffère : son temps trahit le préfixe correct. Utiliser" >&2
    echo "         AuthManager::passwordMatches() (SHA-256 + fixedWidthEquals)." >&2
    rc=1
  fi
done

# --- Règle C : fixedWidthEquals garde sa forme ------------------------------
f=src/secret_compare.h
if [ ! -f "$f" ]; then
  echo "ERREUR : $f manquant — c'est le seul comparateur à temps fixe du projet." >&2
  rc=1
else
  body=$(strip "$f" | sed -n '/inline bool fixedWidthEquals/,/^}/p')
  if [ -z "$body" ]; then
    echo "ERREUR : fixedWidthEquals introuvable dans $f (signature changée ?)." >&2
    rc=1
  else
    if ! printf '%s\n' "$body" | grep -qE 'diff[[:space:]]*\|='; then
      echo "ERREUR : fixedWidthEquals n'accumule plus les écarts par OR (diff |= ...)." >&2
      echo "         C'est CE motif qui rend le temps indépendant du contenu." >&2
      rc=1
    fi
    if printf '%s\n' "$body" | grep -qE '\bbreak\b|\bgoto\b|memcmp|strcmp|strncmp'; then
      echo "ERREUR : fixedWidthEquals contient une sortie anticipée (break/goto/memcmp)." >&2
      rc=1
    fi
    n=$(printf '%s\n' "$body" | grep -cE '\breturn\b')
    if [ "$n" -ne 1 ]; then
      echo "ERREUR : fixedWidthEquals contient $n 'return' au lieu d'un seul." >&2
      echo "         Un return dans la boucle = sortie anticipée = temps variable." >&2
      rc=1
    fi
  fi
fi

# --- Règle D : passwordMatches appelle bien le comparateur à temps fixe -----
# ⛔ Sans elle, les règles A à C protègeraient un comparateur que plus personne
# n'appelle : la propriété serait gardée, et le mot de passe comparé ailleurs.
f=src/auth.cpp
if [ ! -f "$f" ]; then
  echo "ERREUR : $f manquant." >&2
  rc=1
else
  body=$(strip "$f" | sed -n '/bool AuthManager::passwordMatches/,/^}/p')
  if [ -z "$body" ]; then
    echo "ERREUR : AuthManager::passwordMatches introuvable dans $f." >&2
    echo "         C'est le SEUL point de comparaison du mot de passe (bug-183)." >&2
    rc=1
  elif ! printf '%s\n' "$body" | grep -qE '\bfixedWidthEquals[[:space:]]*\('; then
    echo "ERREUR : passwordMatches n'appelle plus fixedWidthEquals — le" >&2
    echo "         comparateur à temps fixe serait protégé mais inutilisé." >&2
    rc=1
  elif printf '%s\n' "$body" | grep -qE 'memcmp|strcmp|strncmp'; then
    # ⚠️ Comparer les EMPREINTES par memcmp est PIRE que le défaut d'origine :
    # l'attaquant calcule les empreintes hors ligne, reconstitue celle du secret
    # octet par octet (~256 × 32 requêtes), puis attaque le mot de passe hors ligne.
    echo "ERREUR : passwordMatches contient memcmp/strcmp — comparaison à sortie" >&2
    echo "         anticipée sur les empreintes. Plus dangereux que le défaut" >&2
    echo "         d'origine : l'empreinte se reconstitue octet par octet." >&2
    rc=1
  fi
fi

[ $rc -eq 0 ] && echo "OK : comparaison du mot de passe à temps fixe (règles A, B, C, D)."
exit $rc
