#!/usr/bin/env bash
# =============================================================================
# check_auth_fail_guard.sh — le ralentissement des echecs ne touche JAMAIS le jeton (bug-189, A2)
# =============================================================================
# ⭐ CE SCRIPT EXISTE PARCE QU'AUCUN TEST NATIF NE PEUT LE FAIRE. Les handlers HTTP ne se
# compilent pas en natif : brancher authFailCheck() sur le chemin du JETON VALIDE (checkAuth,
# ou /auth/change-password sans mot de passe actuel) laisse la suite native VERTE (mutant 4,
# survivant de test_native_auth_fail_backoff). Or c'est la contrainte redhibitoire de la fiche :
# l'OTA est le seul canal de reprise, un proprietaire exclu = appareil perdu.
# ⛔ CE QU'ELLE NE PROUVE PAS : elle lit le SOURCE (commentaires // retires). Elle voit la
# forme de l'appel, pas son effet ; un appel indirect (pointeur, macro) lui echapperait.
#
# Regles :
#   A. authFailCheck( n'est appele que dans src/web_routes_auth.cpp, exactement 2 fois.
#   B. Le 2e appel (change-password) est precede, dans les 3 lignes utiles, de la condition
#      complete `!currentPassword.isEmpty() && !virginState && !authManager.checkTokenAuth(req)`
#      (jamais sur le chemin jeton valide — l'UI envoie jeton ET mot de passe — ni vierge).
#   C. authFailRecordFailure( : seulement dans web_routes_auth.cpp, exactement 2 fois.
#   D. auth.cpp et auth_admission_logic.h n'appellent aucune fonction authFail* hors
#      authFailBackoffClearExemptions (qui ne ralentit rien : elle vide la table).
# Usage : ./tools/check_auth_fail_guard.sh [racine]   Sortie : 0 propre, 1 sinon.
# =============================================================================
set -u
cd "${1:-$(dirname "$0")/..}"
rc=0
strip() { sed -e 's|//.*$||' "$1"; }
R=src/web_routes_auth.cpp

count_in() { strip "$2" | grep -cE "\b$1\s*\("; }

# --- Regle A ---
for f in src/*.cpp src/*.h; do
  [ "$f" = "src/auth_fail_backoff_logic.h" ] && continue
  n=$(count_in authFailCheck "$f")
  if [ "$f" = "$R" ]; then
    [ "$n" -eq 2 ] || { echo "ERREUR : $f contient $n appel(s) a authFailCheck (attendu : 2)." >&2; rc=1; }
  elif [ "$n" -ne 0 ]; then
    echo "ERREUR : $f appelle authFailCheck — le ralentissement ne doit jamais toucher le chemin du jeton (A2)." >&2; rc=1
  fi
done

# --- Regle B ---
ln=$(strip "$R" | grep -nE '\bauthFailCheck\s*\(' | sed -n 2p | cut -d: -f1)
if [ -z "$ln" ]; then
  echo "ERREUR : 2e appel a authFailCheck introuvable dans $R." >&2; rc=1
else
  ctx=$(strip "$R" | sed -n "$((ln>4?ln-4:1)),${ln}p")
  if ! printf '%s\n' "$ctx" | grep -qF '!currentPassword.isEmpty() && !virginState && !authManager.checkTokenAuth(req)'; then
    echo "ERREUR : la garde de /auth/change-password n'est plus conditionnee par" >&2
    echo "         '!currentPassword.isEmpty() && !virginState && !authManager.checkTokenAuth(req)' :" >&2
    echo "         un jeton valide (l'UI envoie jeton ET mot de passe) serait freine (A2)." >&2; rc=1
  fi
fi

# --- Regle C ---
for f in src/*.cpp src/*.h; do
  [ "$f" = "src/auth_fail_backoff_logic.h" ] && continue
  n=$(count_in authFailRecordFailure "$f")
  if [ "$f" = "$R" ]; then
    [ "$n" -eq 2 ] || { echo "ERREUR : $f contient $n appel(s) a authFailRecordFailure (attendu : 2)." >&2; rc=1; }
  elif [ "$n" -ne 0 ]; then
    echo "ERREUR : $f appelle authFailRecordFailure hors de $R." >&2; rc=1
  fi
done

# --- Regle D ---
for f in src/auth.cpp src/auth_admission_logic.h; do
  if strip "$f" | grep -E '\bauthFail[A-Za-z]*\s*\(' | grep -vE 'authFailBackoffClearExemptions' | grep -q .; then
    echo "ERREUR : $f appelle une fonction authFail* (chemin d'admission : A2)." >&2; rc=1
  fi
  if strip "$f" | grep -q 'auth_fail_backoff_logic.h'; then
    echo "ERREUR : $f inclut auth_fail_backoff_logic.h (chemin d'admission : A2)." >&2; rc=1
  fi
done

[ "$rc" -eq 0 ] && echo "check_auth_fail_guard : OK"
exit $rc
