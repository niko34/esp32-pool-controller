#!/usr/bin/env bash
# =============================================================================
# check_ota_signature_switch.sh — gardes OUTILLÉES de l'interrupteur de signature OTA
# (feature-151 D3-b2, conditions C2, C5, C6, C12 du verdict security-reviewer P1)
# =============================================================================
#   C5  aucun `= true` littéral sur nvsReadable / entryPresent : ces booléens ne viennent QUE
#       d'une lecture positive (readNvs / mot du miroir). L'appelant peut mentir (leçon de la
#       sonde /index.html de D3-b1). Un seul constructeur d'OtaSignatureSwitchInputs.
#   C6  Peek : seul mqtt_manager.cpp l'appelle ; Arm : un seul appelant (la route d'armement) ;
#       le miroir est FERMÉ par défaut.
#   C2  les 3 routes en AsyncURIMatcher::exact (sinon /ota-signature-switch capte /arm).
#   C12 web_routes_ota_switch.cpp : pas de Update./LittleFS/onBody/onUpload/hasParam,
#       REQUIRE_AUTH en première instruction de chaque lambda, server->on à 3 arguments ;
#       <!--@@VERSION@@--> exactement UNE fois dans la page de secours.
# Les commentaires // sont retirés avant le grep. Fichier absent => rouge (pas de faux vert).
# Usage : ./tools/check_ota_signature_switch.sh [racine]   Sortie : 0 propre, 1 sinon.
# =============================================================================
set -u   # pas de pipefail : `grep -q` ferme le tube tôt (SIGPIPE) et ferait rater une détection
cd "${1:-$(dirname "$0")/..}"
rc=0
fail() { echo "ERREUR : $*" >&2; rc=1; }
strip() { sed -e 's|//.*$||' "$1"; }

sw=src/ota_signature_switch.cpp
swh=src/ota_signature_switch.h
lg=src/ota_signature_switch_logic.cpp
rt=src/web_routes_ota_switch.cpp
for f in "$sw" "$swh" "$lg" "$rt" src/mqtt_manager.cpp src/ota_recovery_page.html.in; do
  [ -f "$f" ] || { fail "$f introuvable"; }
done
[ $rc -eq 0 ] || exit 1

# --- C5 ----------------------------------------------------------------------
for f in src/*.cpp src/*.h; do
  strip "$f" | grep -nE '(nvsReadable|entryPresent)[[:space:]]*(=|\{)[[:space:]]*\(?[[:space:]]*true\b' \
    | sed "s|^|$f:|" | while read -r l; do echo "$l"; done
done > /tmp/_c5_hits.$$ 2>/dev/null
if [ -s /tmp/_c5_hits.$$ ]; then fail "C5 : booléen littéral sur nvsReadable/entryPresent :"; cat /tmp/_c5_hits.$$ >&2; fi
rm -f /tmp/_c5_hits.$$
# agrégat {true, true, ...} construisant des OtaSignatureSwitchInputs hors tests
for f in src/*.cpp src/*.h; do
  strip "$f" | grep -qE 'OtaSignatureSwitchInputs[[:space:]]+[A-Za-z_]+[[:space:]]*\{[[:space:]]*(true|false)' && fail "C5 : $f construit OtaSignatureSwitchInputs par agrégat littéral"
done
# un seul constructeur d'entrées de décision (hors définition du type et de la décision)
n=$(grep -lE 'OtaSignatureSwitchInputs' src/*.cpp src/*.h | grep -vE 'ota_signature_switch(_logic)?\.(cpp|h)$' | wc -l)
[ "$n" -eq 0 ] || fail "C5 : OtaSignatureSwitchInputs utilisé hors ota_signature_switch*.{cpp,h}"
# `present = true` / `readable = true` : UNIQUEMENT dans readNvs (1 occurrence chacun)
for v in readable present; do
  c=$(strip "$sw" | grep -cE "\*$v[[:space:]]*=[[:space:]]*true")
  [ "$c" -eq 1 ] || fail "C5 : *$v = true doit apparaître exactement 1 fois (readNvs), trouvé $c"
done
strip "$sw" | grep -qE 'prefs\.getType\([^)]*\)[[:space:]]*==[[:space:]]*PT_U32' || fail "C5 : readNvs ne contrôle plus le type PT_U32"
strip "$sw" | grep -qE 'prefs\.isKey\(' || fail "C5 : readNvs ne contrôle plus isKey"

# --- C6 ----------------------------------------------------------------------
peekers=$(for f in src/*.cpp src/*.h; do case "$f" in src/ota_signature_switch.cpp|src/ota_signature_switch.h) continue;; esac; [ "$(strip "$f" | grep -c 'otaSignatureSwitchPeek')" -gt 0 ] && echo -n "$f "; done)
[ "$peekers" = "src/mqtt_manager.cpp " ] || fail "C6 : otaSignatureSwitchPeek doit n'être appelé que par mqtt_manager.cpp (trouvé : $peekers)"
arm_calls=$(for f in src/*.cpp; do [ "$f" = "$sw" ] && continue; strip "$f" | grep -cE 'otaSignatureSwitchArm\(' | sed "s|^|$f |"; done | awk '$2>0{n+=$2; f=f" "$1} END{print n+0 f}')
[ "${arm_calls%% *}" = "1" ] || fail "C6 : otaSignatureSwitchArm doit avoir UN seul appelant (trouvé : $arm_calls)"
echo "$arm_calls" | grep -q "web_routes_ota_switch.cpp" || fail "C6 : l'unique appelant d'Arm doit être web_routes_ota_switch.cpp ($arm_calls)"
grep -qE 'constexpr uint32_t kWordClosed = 0u;' "$sw" || fail "C6 : kWordClosed doit valoir 0"
grep -qE 'std::atomic<uint32_t> sWord\{kWordClosed\};' "$sw" || fail "C6 : le miroir doit être initialisé FERMÉ (kWordClosed)"

# --- C2 ----------------------------------------------------------------------
tot=$(strip "$rt" | grep -c 'server->on(')
ex=$(strip "$rt" | grep -c 'server->on(AsyncURIMatcher::exact(')
[ "$tot" -eq 3 ] && [ "$ex" -eq 3 ] || fail "C2 : 3 routes attendues, toutes en AsyncURIMatcher::exact (total=$tot exact=$ex)"
for p in kOtaSignatureSwitchArmRoutePath kOtaSignatureSwitchRoutePath; do
  grep -q "AsyncURIMatcher::exact($p)" "$rt" || fail "C2 : route $p non enregistrée en exact"
done

# --- C12 ---------------------------------------------------------------------
for w in 'Update\.' 'LittleFS' 'onBody' 'onUpload' 'hasParam' 'getParam'; do
  strip "$rt" | grep -qE "$w" && fail "C12 : $rt contient « $w » (surface d'acceptation d'image interdite)"
done
nl=$(strip "$rt" | grep -c '\[\](AsyncWebServerRequest\* req) {')
na=$(strip "$rt" | grep -c 'REQUIRE_AUTH(req, RouteProtection::CRITICAL);')
[ "$nl" -eq 3 ] && [ "$na" -eq 3 ] || fail "C12 : 3 lambdas et 3 REQUIRE_AUTH CRITICAL attendus (lambdas=$nl auth=$na)"
# REQUIRE_AUTH = première instruction : la ligne non vide qui suit chaque ouverture de lambda
bad=$(strip "$rt" | awk '/\[\]\(AsyncWebServerRequest\* req\) \{/{getline nx; if (nx !~ /REQUIRE_AUTH\(req, RouteProtection::CRITICAL\);/) print nx}')
[ -z "$bad" ] || fail "C12 : REQUIRE_AUTH n'est pas la première instruction d'un lambda ($bad)"
# server->on à 3 arguments : (matcher, méthode, lambda) — pas de variante à 4 (upload handler)
strip "$rt" | tr '\n' ' ' | tr -s ' ' | grep -oE 'server->on\([^{]*\{' | while read -r s; do
  echo "$s" | grep -qE 'server->on\(AsyncURIMatcher::exact\([A-Za-z]+\), HTTP_(GET|POST), \[\]\(AsyncWebServerRequest\* req\) \{' \
    || echo "$s"
done > /tmp/_c12_on.$$
[ -s /tmp/_c12_on.$$ ] && { fail "C12 : server->on hors forme (matcher, méthode, lambda) :"; cat /tmp/_c12_on.$$ >&2; }
rm -f /tmp/_c12_on.$$
v=$(grep -o '<!--@@VERSION@@-->' src/ota_recovery_page.html.in | wc -l)
[ "$v" -eq 1 ] || fail "C12 : <!--@@VERSION@@--> doit apparaître exactement 1 fois dans ota_recovery_page.html.in (trouvé $v)"

# --- R2 / R8 : ce que ni les tests natifs ni le compilateur ne voient (mqtt_manager / route JSON) ---
# R8 : l'état `locked` n'est ON que si un résidu NVS (non nul ou inconnu) subsiste.
strip src/mqtt_manager.cpp | tr -s ' ' | grep -qF 'const bool on = armed || (locked && residual != 0);' \
  || fail "R8 : mqtt_manager.cpp — l'état locked doit n'être ON qu'avec un résidu non nul ou inconnu"
# R2 : résidu INCONNU = null (jamais 0) ; rearms_on_reboot publié.
strip "$rt" | grep -qF 'nvs_residual_until\":null' || fail "R2 : résidu inconnu doit se publier null dans web_routes_ota_switch.cpp"
strip "$rt" | grep -qF 'rearms_on_reboot' || fail "R2 : champ rearms_on_reboot absent de web_routes_ota_switch.cpp"

[ $rc -eq 0 ] && echo "OK — interrupteur de signature : C2, C5, C6, C12 conformes."
exit $rc
