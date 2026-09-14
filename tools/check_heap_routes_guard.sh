#!/usr/bin/env bash
# =============================================================================
# check_heap_routes_guard.sh — la garde est LA PREMIÈRE INSTRUCTION (S6)
# =============================================================================
# feature-128 : src/web_routes_heap.cpp porte une convention inverse de
# web_routes_debug.cpp (où 4 routes sur 5 sont sans authentification) — AUCUNE
# route de ce fichier ne répond sans garde, et la garde est la PREMIÈRE
# instruction de chaque handler, jamais la deuxième.
#
# Motif : le défaut du 07/09 (un handler de CORPS exécuté AVANT REQUIRE_AUTH)
# devient INEXPRIMABLE sous la forme imposée ici : `server->on(chemin, méthode,
# UN SEUL lambda)` à TROIS arguments — aucun handler de corps, aucun `onBody`,
# aucun `onUpload`. Un corps qui n'est jamais parsé ne peut jamais s'exécuter
# avant la garde.
#
# ⚠️ CE QUE `grep -c REQUIRE_AUTH == grep -c 'server->on('` NE PROUVE PAS À LUI
# SEUL : cette égalité compte une PRÉSENCE (autant de REQUIRE_AUTH que de
# routes), pas une PLACE. REQUIRE_AUTH pourrait être présent mais posé APRÈS
# la logique métier — l'égalité resterait vraie. Ce script fait donc DEUX
# contrôles, le second seul étant suffisant, le premier restant fait par
# discipline (comme dans le design revu par security-reviewer) :
#   1. PRÉSENCE : autant de REQUIRE_AUTH que de server->on(.
#   2. PLACE : pour CHAQUE route, les 5 lignes qui suivent IMMÉDIATEMENT la
#      déclaration `server->on(...)` sont EXACTEMENT la garde à deux temps
#      exigée par la condition S3 (403 sans log si authentification désactivée
#      ou premier démarrage détecté, PUIS REQUIRE_AUTH) :
#        if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {
#          sendJsonError(req, 403, "forbidden");
#          return;
#        }
#        REQUIRE_AUTH(req, RouteProtection::CRITICAL);
#      Appliquée UNIFORMÉMENT aux quatre routes (R2 et R3) : S3 la rend
#      EXIGÉE pour R3 et seulement RECOMMANDÉE pour R2 — l'uniformité évite
#      deux patrons de garde différents dans le même fichier, chacun avec sa
#      propre marge d'erreur.
#   3. FORME À TROIS ARGUMENTS : la ligne `server->on(...)` elle-même doit
#      matcher EXACTEMENT `server->on("chemin", HTTP_X, [](AsyncWebServerRequest* req) {`
#      — un 4ᵉ argument (handler de corps, upload) fait échouer le patron.
#      Et globalement : zéro `.onBody(`/`->onBody(`, zéro `.onUpload(`/`->onUpload(`
#      dans tout le fichier.
#
# ⛔ CE QUE CE SCRIPT NE PEUT PAS FERMER (à savoir plutôt qu'à croire couvert,
# même discipline que check_fopen_guard.sh) : il compare des lignes TEXTUELLES
# exactes après avoir retiré les espaces de bord — un reformatage manuel
# (retour à la ligne différent, espace supplémentaire À L'INTÉRIEUR de la
# ligne) ferait échouer le script même si la garde reste sémantiquement
# correcte. C'est un compromis délibéré : un texte figé et vérifiable plutôt
# qu'un analyseur syntaxique C++, absent de la boîte à outils du projet.
#
# Usage : ./tools/check_heap_routes_guard.sh
# Sortie : 0 si conforme, 1 sinon (liste des violations sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

FILE="src/web_routes_heap.cpp"

if [ ! -f "$FILE" ]; then
  echo "ERREUR : $FILE introuvable — rien à vérifier." >&2
  exit 1
fi

trim() {
  # Retire les espaces de bord (tête/queue) uniquement — pas les espaces internes.
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "$s"
}

fail=0
errors=()

# ---------------------------------------------------------------------------
# 1) PRÉSENCE : autant de REQUIRE_AUTH( que de server->on( — nécessaire, PAS
#    suffisant seul (voir tête de fichier).
# ---------------------------------------------------------------------------
on_count=$(grep -c 'server->on(' "$FILE")
require_count=$(grep -c 'REQUIRE_AUTH(' "$FILE")

if [ "$on_count" -eq 0 ]; then
  errors+=("Aucun server->on( trouvé dans $FILE — structure du fichier inattendue.")
  fail=1
fi
if [ "$on_count" -ne "$require_count" ]; then
  errors+=("Invariant de PRÉSENCE violé : $on_count server->on( pour $require_count REQUIRE_AUTH(.")
  fail=1
fi

# ---------------------------------------------------------------------------
# 2) FORME GLOBALE : aucun handler de corps / upload dans tout le fichier.
# ---------------------------------------------------------------------------
body_handlers=$(grep -cE '(\.|->)[[:space:]]*onBody[[:space:]]*\(' "$FILE")
upload_handlers=$(grep -cE '(\.|->)[[:space:]]*onUpload[[:space:]]*\(' "$FILE")
if [ "$body_handlers" -ne 0 ] || [ "$upload_handlers" -ne 0 ]; then
  errors+=("Handler de corps/upload détecté ($body_handlers onBody, $upload_handlers onUpload) — interdit (S6, forme à trois arguments seulement).")
  fail=1
fi

# ---------------------------------------------------------------------------
# 3) PLACE : pour chaque route, la déclaration ET les 5 lignes de garde.
# ---------------------------------------------------------------------------
readonly EXPECTED=(
  'if (!authManager.isEnabled() || authManager.isFirstBootDetected()) {'
  'sendJsonError(req, 403, "forbidden");'
  'return;'
  '}'
  'REQUIRE_AUTH(req, RouteProtection::CRITICAL);'
)

strict_re='^[[:space:]]*server->on\("[^"]+",[[:space:]]*HTTP_(GET|POST),[[:space:]]*\[\]\(AsyncWebServerRequest\* req\)[[:space:]]*\{$'

on_lines=$(grep -n 'server->on(' "$FILE" | cut -d: -f1)
total_lines=$(wc -l < "$FILE")

for L in $on_lines; do
  decl=$(sed -n "${L}p" "$FILE")
  if ! [[ "$decl" =~ $strict_re ]]; then
    errors+=("L$L : la déclaration ne correspond pas au patron à TROIS arguments (chemin, méthode, UN SEUL lambda) : « $decl »")
    fail=1
    continue
  fi

  route_ok=1
  for i in "${!EXPECTED[@]}"; do
    idx=$((i + 1))
    ln=$((L + idx))
    if [ "$ln" -gt "$total_lines" ]; then
      errors+=("L$L : fichier trop court pour porter les 5 lignes de garde attendues.")
      route_ok=0
      break
    fi
    got_raw=$(sed -n "${ln}p" "$FILE")
    got=$(trim "$got_raw")
    exp="${EXPECTED[$i]}"
    if [ "$got" != "$exp" ]; then
      errors+=("L$L (garde, ligne $ln) : attendu « $exp », trouvé « $got » — la garde n'est plus la PREMIÈRE instruction.")
      route_ok=0
    fi
  done
  [ "$route_ok" -eq 1 ] || fail=1
done

if [ "$fail" -ne 0 ]; then
  {
    echo "ERREUR S6 : au moins une route de $FILE viole la convention « garde en"
    echo "première instruction, forme à trois arguments seulement »."
    echo
    for e in "${errors[@]}"; do
      echo "  - $e"
    done
  } >&2
  exit 1
fi

echo "OK — $on_count route(s) dans $FILE, garde en première instruction, forme à trois arguments."
