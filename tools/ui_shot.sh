#!/usr/bin/env bash
# Photographie l'interface web SANS passer par l'appareil.
#
# Affiche `data/` dans un navigateur sans interface, avec les données réelles
# recopiées depuis l'appareil, et enregistre une capture PNG.
# Né du défaut du 2026-10-14 : les cartes pH/ORP ne ressemblaient pas à la
# carte Filtration, et personne ne les avait comparées RENDUES.
# Voir docs/DESIGN_SYSTEM.md §9.
#
# Usage :
#   ./tools/ui_shot.sh                          # tableau de bord, 390 px
#   ./tools/ui_shot.sh -w 1024                  # largeur desktop
#   ./tools/ui_shot.sh -r '#/ph' -o /tmp/ph.png # une autre page
#   ./tools/ui_shot.sh -f cal_switch_active=true    # forcer un champ de /data
#   ./tools/ui_shot.sh -c lighting_mode=scheduled   # forcer un champ de /get-config
#
# ⚠️ Les données recopiées contiennent le SSID Wi-Fi et l'identifiant MQTT.
#    Elles vont dans un dossier TEMPORAIRE, jamais dans le dépôt.
set -euo pipefail

WIDTH=390
HEIGHT=2600
ROUTE=""
OUT=""
declare -a FORCE=()
declare -a FORCE_CFG=()

while getopts "w:h:r:o:f:c:" opt; do
  case "$opt" in
    w) WIDTH="$OPTARG" ;;
    h) HEIGHT="$OPTARG" ;;
    r) ROUTE="$OPTARG" ;;
    o) OUT="$OPTARG" ;;
    f) FORCE+=("$OPTARG") ;;
    c) FORCE_CFG+=("$OPTARG") ;;
    *) grep '^#' "$0" | sed 's/^# \?//' >&2; exit 2 ;;
  esac
done

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$REPO/ui-shot-${WIDTH}px.png}"
HOST="${OTA_HOST:-${POOL_HOST:-192.168.1.48}}"

# Identifiants hors dépôt — jamais affichés.
if [[ -z "${POOL_PASSWORD:-}" && -r /root/.pool-credentials ]]; then
  set -a; . /root/.pool-credentials; set +a
fi
if [[ -z "${POOL_PASSWORD:-}" ]]; then
  echo "ERREUR : POOL_PASSWORD absent (ni variable, ni /root/.pool-credentials)." >&2
  exit 1
fi

CHROME="$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)"
if [[ -z "$CHROME" ]]; then
  echo "ERREUR : aucun navigateur Chromium trouvé." >&2
  exit 1
fi

TMP="$(mktemp -d)"
PORT=0
SRV=""
cleanup() {
  [[ -n "$SRV" ]] && kill "$SRV" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT

echo "→ Lecture de l'appareil ($HOST)…"
TOKEN="$(curl -s -m 10 -X POST "http://$HOST/auth/login" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"admin\",\"password\":\"$POOL_PASSWORD\"}" \
  | grep -oE '"token"[^,]*' | cut -d'"' -f4)"
if [[ -z "$TOKEN" ]]; then
  echo "ERREUR : authentification refusée par $HOST." >&2
  exit 1
fi

mkdir -p "$TMP/api"
for ep in data get-config get-system-info; do
  curl -s -m 15 "http://$HOST/$ep" -H "X-Auth-Token: $TOKEN" -o "$TMP/api/$ep.json"
  [[ -s "$TMP/api/$ep.json" ]] || { echo "ERREUR : /$ep vide." >&2; exit 1; }
done

# Forçage d'état : -f sur /data (mesures), -c sur /get-config (réglages).
force_field() {  # $1 = fichier, $2 = cle=valeur
  local file="$1" key="${2%%=*}" val="${2#*=}"
  python3 - "$file" "$key" "$val" <<'PY'
import json, sys
path, key, val = sys.argv[1], sys.argv[2], sys.argv[3]
doc = json.load(open(path))
if val in ('true', 'false'):
    doc[key] = (val == 'true')
else:
    try:
        doc[key] = int(val)
    except ValueError:
        doc[key] = val
json.dump(doc, open(path, 'w'))
PY
}

for f in "${FORCE[@]:-}"; do
  [[ -z "$f" ]] && continue
  force_field "$TMP/api/data.json" "$f"
  echo "   /data forcé : $f"
done
for f in "${FORCE_CFG[@]:-}"; do
  [[ -z "$f" ]] && continue
  force_field "$TMP/api/get-config.json" "$f"
  echo "   /get-config forcé : $f"
done

# Port libre.
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
python3 "$REPO/tools/ui_shot_server.py" "$PORT" "$REPO/data" "$TMP/api" &
SRV=$!
for _ in $(seq 1 40); do
  curl -s -o /dev/null -m 1 "http://127.0.0.1:$PORT/__boot.html" && break
  python3 -c 'import time;time.sleep(0.1)'
done

echo "→ Capture ${WIDTH}×${HEIGHT}${ROUTE:+ (route $ROUTE)}…"
# ⚠️ LIMITE CONNUE DE CET OUTIL, mesurée le 2026-10-14 :
#    les champs `<input type="time">` apparaissent au format AMÉRICAIN
#    (« 08:00 PM ») dans les captures. Ce N'EST PAS un défaut de l'interface :
#    ce type de champ est rendu par le navigateur selon SA locale, et le
#    navigateur sans interface l'ignore ici — testé avec --lang=fr-FR,
#    --accept-lang, LANG/LC_ALL, et la locale fr_FR.UTF-8 effectivement
#    générée : aucun effet. Sur un téléphone configuré en français, l'heure
#    s'affiche « 20:00 ».
# ⛔ NE PAS « corriger » l'interface sur la foi d'une capture à cause de ça.
LANG=fr_FR.UTF-8 LC_ALL=fr_FR.UTF-8 LANGUAGE=fr \
"$CHROME" --headless --no-sandbox --disable-gpu --hide-scrollbars \
  --lang=fr-FR --accept-lang=fr-FR,fr \
  --window-size="$WIDTH,$HEIGHT" --virtual-time-budget=10000 \
  --screenshot="$OUT" "http://127.0.0.1:$PORT/__boot.html$ROUTE" 2>/dev/null

[[ -s "$OUT" ]] || { echo "ERREUR : capture vide." >&2; exit 1; }
echo "✅ $OUT ($(wc -c < "$OUT") octets)"
