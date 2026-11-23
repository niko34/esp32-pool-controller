#!/usr/bin/env bash
# Vecteurs du troisieme etat de capture : « Connexion… » (feature-213).
#
# ⭐ Le script injecte n'est PAS recopie dans le harnais : il est DEMANDE a
# `tools/ui_shot_server.py`, qui est le fichier qui le sert reellement. Si la
# pose change la-bas, ce controle execute la nouvelle. Esprit d'ADR-0099.
# ⛔ Aucun appareil, aucun navigateur, aucune compilation : ce controle tourne
# en CI.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ⚠️ `-I` : le dossier du script n'est PAS ajoute au chemin d'import, on ajoute
# `tools/` explicitement. Le module ne demarre aucun serveur a l'import.
UI_SHOT_MID=1 python3 -I -c "
import sys
sys.path.insert(0, 'tools')
import ui_shot_server
sys.stdout.buffer.write(ui_shot_server.Handler._mid_script())
" > "$TMP/mid_stub.html"

if [[ ! -s "$TMP/mid_stub.html" ]]; then
  echo "ERREUR : ui_shot_server.py n'a servi AUCUN script pour l'etat « Connexion… »." >&2
  echo "  La capture -m ne figerait alors plus rien, en silence." >&2
  exit 1
fi

# Temoin : sans UI_SHOT_MID, la pose doit etre VIDE — sinon l'option -m serait
# active en permanence et les captures « En ligne » / « Deconnecte » seraient
# toutes fausses.
VIDE="$(python3 -I -c "
import sys
sys.path.insert(0, 'tools')
import ui_shot_server
sys.stdout.write(str(len(ui_shot_server.Handler._mid_script())))
")"
if [[ "$VIDE" != "0" ]]; then
  echo "ERREUR : la pose « Connexion… » s'injecte SANS l'option -m ($VIDE octets)." >&2
  echo "  Toutes les autres captures seraient figees sur le mauvais etat." >&2
  exit 1
fi

# Exclusion mutuelle -n / -m. ⚠️ Elle doit trancher AVANT toute lecture de
# l'appareil : en CI il n'y a ni identifiants ni appareil, donc si la garde
# disparaissait, le script partirait sur le reseau et sortirait en 1 (mot de
# passe absent) ou serait tue par `timeout` (124). ⇒ Exiger EXACTEMENT 2.
set +e
timeout 5 ./tools/ui_shot.sh -n -m >/dev/null 2>&1
CODE=$?
set -e
if [[ "$CODE" != "2" ]]; then
  echo "ERREUR : ./tools/ui_shot.sh -n -m devait refuser (code 2), code obtenu : $CODE." >&2
  echo "  Les deux options posent un faux WebSocket contradictoire : l'une ouvre" >&2
  echo "  le socket, l'autre le laisse en cours d'ouverture." >&2
  exit 1
fi

node test/mid_stub_vectors.js "$TMP/mid_stub.html"

# Cablage de bout en bout : le VRAI ui_shot.sh, avec un faux appareil et un faux
# navigateur. Sans lui, remplacer `export UI_SHOT_MID="$MID_STUB"` par
# `export UI_SHOT_MID=0` laisse tout le reste au vert -- mesure le 2026-11-23.
python3 test/mid_stub_plumbing.py
