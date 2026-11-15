#!/usr/bin/env bash
# =============================================================================
# check_fs_probe_paths.sh — le SONDEUR et le PRODUCTEUR s'accordent (feature-151 D3-b1)
# =============================================================================
# Défaut trouvé AVANT tout code, pendant la conception de D3-b1 : minify.js
# n'écrit QUE les variantes `.gz` (feature-048) — `data-build/` ne contient aucun
# `.html`/`.js`/`.css` nu. Une sonde `exists("/index.html")` aurait rendu FAUX sur
# un appareil SAIN, armé la page de secours en permanence et masqué l'interface
# partout. Un test natif ne l'aurait pas vu : le module pur reçoit `indexPresent`
# en booléen, c'est l'APPELANT qui mentait.
#
# CE SCRIPT LIE le sondeur au producteur. Il lance minify.js (dans un répertoire
# TEMPORAIRE : ni data-build/ ni littlefs.bin du dépôt ne sont touchés), puis
# vérifie que CHAQUE chemin que le code sonde ou sert depuis le filesystem existe
# dans la sortie, en variante nue OU `.gz` (miroir de AsyncWebServerRequest::send).
# Il attrape aussi la rupture inverse : un minify.js qui changerait de format.
#
# Chemins vérifiés (web_server.cpp ET web_routes_recovery.cpp, route /ui) :
#   - kOtaRecoveryProbePath (src/constants.h) — la sonde d'index ;
#   - chaque `req->send(LittleFS, "/…")` de src/web_server.cpp — les routes de page.
#
# Usage : ./tools/check_fs_probe_paths.sh   (exige node et node_modules/ — `npm ci`)
# Sortie : 0 si conforme, 1 sinon.
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

if ! command -v node >/dev/null 2>&1; then
  echo "ERREUR : node introuvable sur le PATH" >&2
  exit 1
fi
if [ ! -d node_modules ]; then
  echo "ERREUR : node_modules/ absent — lancer 'npm ci' (minify.js en dépend)" >&2
  exit 1
fi

root=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp minify.js package.json "$tmp"/
cp -r data "$tmp"/data
ln -s "$root/node_modules" "$tmp/node_modules"

if ! (cd "$tmp" && node minify.js >/dev/null 2>"$tmp/minify.err"); then
  echo "ERREUR : minify.js a échoué :" >&2
  cat "$tmp/minify.err" >&2
  exit 1
fi

probed=$(
  {
    grep -hE 'kOtaRecoveryProbePath[[:space:]]*=' src/constants.h | grep -oE '"/[^"]+"'
    grep -hoE 'send\(LittleFS,[[:space:]]*"/[^"]+"' src/web_server.cpp src/web_routes_recovery.cpp | grep -oE '"/[^"]+"'
  } | tr -d '"' | sort -u
)

if [ -z "$probed" ]; then
  echo "ERREUR : aucun chemin sondé trouvé — le contrôle serait vide (motifs périmés ?)" >&2
  exit 1
fi

# Routes MORTES préexistantes, constatées au passage de D3-b1 : /config et
# /config.html servent un fichier que data/ ne contient plus (supprimé par la
# refonte de l'interface) ⇒ elles rendent 501. Hors périmètre du lot, SIGNALÉ ;
# nommées ici une par une pour que toute AUTRE absence reste rouge.
KNOWN_ABSENT=" /config.html "

status=0
for p in $probed; do
  rel="${p#/}"
  case "$KNOWN_ABSENT" in *" $p "*) echo "SKIP $p (route morte préexistante connue)"; continue ;; esac
  if [ -f "$tmp/data-build/$rel" ] || [ -f "$tmp/data-build/$rel.gz" ]; then
    echo "OK  $p"
  else
    echo "ERREUR : '$p' est sondé/servi par le firmware mais minify.js ne produit ni '$rel' ni '$rel.gz'" >&2
    status=1
  fi
done

# Garde du miroir : la sonde doit tester .gz (cf. fs_guard.cpp). Si quelqu'un
# retire le .gz de la sonde, ce contrôle rougit.
if ! grep -q '"%s.gz"' src/fs_guard.cpp; then
  echo "ERREUR : fsPathResolvableIfHeapAllows() ne sonde plus la variante .gz" >&2
  status=1
fi

exit $status
