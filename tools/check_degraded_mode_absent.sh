#!/usr/bin/env bash
# =============================================================================
# check_degraded_mode_absent.sh — le secours n'est PAS un « mode » (feature-151 D3-b1)
# =============================================================================
# Réserve de fond du lot : « le mode dégradé ne doit pas être un MODE ». Sans
# filesystem le serveur démarre quand même (A1), avec la MÊME table de routes
# (A2) ; aucune décision de sécurité ne dépend de l'état de secours (A3).
#
# ⚠️ CONDITION S2 — le contrôle porte sur les SYMBOLES, pas sur les fichiers :
# les quatre sites du marqueur vivent DANS web_routes_ota.cpp, le fichier même
# qui porte otaUploadAuthorized et le prédicat de signature. « Aucune mention
# dans web_routes_ota.cpp » serait faux d'emblée. Ce qui compte : ce fichier
# n'a qu'une interface d'ÉCRITURE du marqueur, et le nombre de LECTEURS de l'état
# est FIGÉ.
#
# Vérifie, dans l'ordre :
#   1. `otaRecoveryActive` : 0 occurrence dans auth.* ;
#   2. web_routes_ota.cpp : seuls otaRecoveryOnUpdatePhase / otaRecoveryNoteTransition
#      (écriture) y figurent — aucun lecteur (Active, Reason, Drain) ;
#   3. lecteurs de l'état (otaRecoveryActive / otaRecoveryReason) hors module
#      marqueur : EXACTEMENT 1, dans web_routes_recovery.cpp ;
#   4. aucun AUTRE fichier de src/ ne mentionne les symboles du secours (liste
#      blanche) — ota_signature.*, ota_*_logic.*, auth.*, REQUIRE_AUTH… exclus ;
#   5. A1 : aucun `return` entre le montage de LittleFS et setupRoutes() dans
#      WebServerManager::begin() ;
#   6. A2 : exactement UN `if` sur LittleFS dans web_server.cpp (celui du
#      CRITICAL) — aucun enregistrement de route conditionné au montage ;
#   7. A3 : aucun identifiant « dégradé » (degraded…) dans auth/OTA/serveur/fs_guard ;
#   8. A7 : aucun `LittleFS.begin(true…)` dans src/.
#
# ⛔ CE QUE CE SCRIPT NE FERME PAS : grep est ligne à ligne ; un lecteur qui
# passerait par un autre symbole (ex. relire directement la NVS) échapperait aux
# points 1-4. La revue P2 reste le dernier rempart.
#
# Usage : ./tools/check_degraded_mode_absent.sh
# Sortie : 0 si conforme, 1 sinon.
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

status=0
fail() { echo "ERREUR D3-b1 : $*" >&2; status=1; }

# Code seul : sans lignes de commentaire (`//`, `*`, `/*` en tête).
code_of() { grep -vE '^[[:space:]]*(//|\*|/\*)' "$1"; }

for f in src/web_routes_ota.cpp src/web_server.cpp src/web_routes_recovery.cpp \
         src/ota_recovery_marker.cpp src/ota_recovery_marker.h; do
  [ -f "$f" ] || { fail "fichier attendu absent : $f"; }
done

# 1 -------------------------------------------------------------------------
for f in src/auth.cpp src/auth.h; do
  n=$(code_of "$f" | grep -c 'otaRecoveryActive' || true)
  [ "$n" -eq 0 ] || fail "otaRecoveryActive lu dans $f ($n occurrence(s)) : le secours deviendrait un mode d'authentification"
done

# 2 -------------------------------------------------------------------------
bad=$(code_of src/web_routes_ota.cpp | grep -oE 'otaRecovery[A-Za-z]*' | sort -u \
      | grep -vxE 'otaRecoveryOnUpdatePhase|otaRecoveryNoteTransition' || true)
[ -z "$bad" ] || fail "web_routes_ota.cpp touche autre chose que l'interface d'ÉCRITURE du marqueur : $(echo $bad)"

# 3 -------------------------------------------------------------------------
readers=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) \
          ! -name 'ota_recovery_marker.cpp' ! -name 'ota_recovery_marker.h' -print0 \
          | xargs -0 grep -HnE 'otaRecovery(Active|Reason)[[:space:]]*\(' \
          | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' || true)
n=$(printf '%s' "$readers" | grep -c . || true)
if [ "$n" -ne 1 ] || ! printf '%s' "$readers" | grep -q '^src/web_routes_recovery.cpp:'; then
  fail "le nombre de lecteurs de l'état de secours doit être EXACTEMENT 1 (web_routes_recovery.cpp) — trouvé $n :
$readers"
fi

# 4 -------------------------------------------------------------------------
others=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
         | xargs -0 grep -lE 'otaRecovery|OtaRecovery' \
         | grep -vxE 'src/ota_recovery_(logic|marker)\.(cpp|h)|src/web_routes_recovery\.(cpp|h)|src/web_routes_ota\.cpp|src/web_server\.cpp|src/constants\.h|src/ota_recovery_page\.(cpp|h)' || true)
[ -z "$others" ] || fail "symboles du secours mentionnés hors de la liste blanche : $(echo $others)"

# 4b ------------------------------------------------------------------------
# feature-151 D3-b2 : ota_signature_switch.cpp n'est PLUS dans la liste blanche (R3) et ne cite
# aucun symbole du secours (le namespace partagé s'appelle kOtaNvsNamespace). Le contrôle 4 le
# couvre. Ici : dans ce fichier, tout appel prefs.* ne cite QUE kOtaSignatureSwitchNvsKey
# (ou kOtaNvsNamespace pour begin) — aucune clé littérale ("fsupd"...) dans l'espace partagé.
lit=$(code_of src/ota_signature_switch.cpp | grep -E 'prefs\.' \
      | grep -vE 'prefs\.(end|begin)\(' | grep -E 'prefs\.[A-Za-z]+\(' \
      | grep -vE 'prefs\.[A-Za-z]+\(kOtaSignatureSwitchNvsKey[,)]' || true)
[ -z "$lit" ] || fail "ota_signature_switch.cpp : appel prefs.* qui ne cite pas kOtaSignatureSwitchNvsKey : $(echo $lit)"
beg=$(code_of src/ota_signature_switch.cpp | grep -E 'prefs\.begin\(' | grep -vE 'prefs\.begin\(kOtaNvsNamespace,' || true)
[ -z "$beg" ] || fail "ota_signature_switch.cpp : prefs.begin() hors kOtaNvsNamespace : $(echo $beg)"

# 5 -------------------------------------------------------------------------
between=$(awk '/if \(!LittleFS\.begin\(\)\)/{f=1} f{print} /setupRoutes\(\);/{if(f)exit}' src/web_server.cpp \
          | grep -vE '^[[:space:]]*//' || true)
if [ -z "$between" ]; then
  fail "motif LittleFS.begin() … setupRoutes() introuvable dans web_server.cpp (contrôle périmé ?)"
elif printf '%s' "$between" | grep -qE '\breturn\b'; then
  fail "un return s'est glissé entre le montage et setupRoutes() (A1) — le serveur ne démarrerait plus sans filesystem"
fi

# 6 -------------------------------------------------------------------------
n=$(code_of src/web_server.cpp | grep -cE '\bif[[:space:]]*\(.*LittleFS' || true)
[ "$n" -eq 1 ] || fail "web_server.cpp : $n test(s) sur LittleFS (attendu : 1, celui du CRITICAL) — route conditionnée au montage ? (A2)"

# 7 -------------------------------------------------------------------------
# Périmètre : les fichiers où un « mode » influencerait une décision de sécurité
# ou le service des pages. (sensors.* a ses propres « I2cDegradedLogged »,
# sans rapport — un drapeau de journal de bus, pas un mode du serveur.)
n=$(find src -type f \( -name 'auth.*' -o -name 'web_server.*' -o -name 'web_routes_ota.*' \
        -o -name 'web_routes_recovery.*' -o -name 'ota_*' -o -name 'fs_guard.*' \) -print0 \
    | xargs -0 grep -HniE '[a-z_]*(degraded|degrade|dégradé)[a-z_]*' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' | grep -vE '//.*(degraded|degrade|dégradé)' \
    | grep -c . || true)
[ "$n" -eq 0 ] || fail "identifiant « dégradé » dans le code d'auth/OTA/serveur ($n) : aucune variable d'état de mode (A3)"

# 8 -------------------------------------------------------------------------
n=$(find src -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
    | xargs -0 grep -HnE 'LittleFS\.begin\([[:space:]]*true' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' | grep -c . || true)
[ "$n" -eq 0 ] || fail "LittleFS.begin(true…) dans src/ ($n) : un formatage détruirait les données (A7)"

[ $status -eq 0 ] && echo "OK — le secours n'est pas un mode (symboles, lecteurs, A1/A2/A3/A7)."
exit $status
