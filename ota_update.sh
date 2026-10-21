#!/bin/bash
# ota_update.sh - Mise à jour rapide sans recompilation
# Usage: ./ota_update.sh [firmware|filesystem|both] [hostname] [password]
#
# Le paramètre [hostname] accepte :
#   - Un nom mDNS : poolcontroller.local  (nécessite que le mDNS fonctionne sur le réseau)
#   - Une adresse IP : 192.168.1.42       (fonctionne sans mDNS, utile si le réseau ne le supporte pas)
# Par défaut : poolcontroller.local
#
# Authentification (par ordre de priorité) :
#   1. Argument $3 : ./ota_update.sh both poolcontroller.local monmotdepasse
#   2. Variable d'environnement POOL_PASSWORD : POOL_PASSWORD=monmotdepasse ./ota_update.sh
#   3. Variable d'environnement POOL_TOKEN    : POOL_TOKEN=<api_token> ./ota_update.sh
#   4. Saisie interactive si aucun des précédents n'est défini
#
# Signature OTA (feature-151 D1) :
#   Chaque envoi est signé (RSA-3072/PKCS#1 v1.5/SHA-256) avec la clé privée
#   locale — jamais versionnée. Emplacement : $OTA_SIGNING_KEY (défaut
#   ~/.pool-ota/signing_key.pem, 0600). ⛔ FAIL-CLOSED : ce script REFUSE
#   d'envoyer quoi que ce soit si openssl est absent OU si la clé est
#   introuvable (condition 11 du verdict security-reviewer P1, feature-151).
#   ⚠️ La phrase de passe de la clé (chiffrée AES-256-CBC) est demandée par
#   openssl lui-même, de façon INTERACTIVE — jamais en argument, jamais en
#   variable d'environnement (historique du shell, /proc), jamais écrite sur
#   disque. Décision utilisateur du 2026-10-18 (feature-151, "option B") :
#   le vrai secret est cette phrase de passe, gardée dans la tête de
#   l'utilisateur — la clé chiffrée seule, si elle fuitait, est inexploitable.

set -e

TYPE="${1:-both}"
ESP32_HOST="${2:-poolcontroller.local}"
UPDATE_URL="http://$ESP32_HOST/update"

# feature-151 D1 : clé privée de signature — HORS DÉPÔT, chiffrée, 0600.
OTA_SIGNING_KEY="${OTA_SIGNING_KEY:-$HOME/.pool-ota/signing_key.pem}"

# Environnement déployé : esp32dev_core3, seul env matériel depuis v2.42.1
# (ADR-0069 ; core 3 par défaut depuis v2.20.4, ADR-0042). Surchargeable par
#   PIO_ENV=<env> ./ota_update.sh firmware <hôte>
# Chemins surchargeables individuellement si besoin (FIRMWARE_PATH / FILESYSTEM_PATH).
#
# RAPPEL UTILE : l'OTA n'écrit QUE l'application, jamais le bootloader.
# Corollaire : tout changement de bootloader exige un flash USB (./deploy.sh),
# l'OTA n'y suffit pas. Le filesystem, lui, est indépendant du bootloader.
PIO_ENV="${PIO_ENV:-esp32dev_core3}"
FIRMWARE_PATH="${FIRMWARE_PATH:-.pio/build/$PIO_ENV/firmware.bin}"
FILESYSTEM_PATH="${FILESYSTEM_PATH:-.pio/build/$PIO_ENV/littlefs.bin}"

# Couleurs
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

# feature-151 D1, condition 11 : REFUSE d'envoyer sans clé privée ET sans
# openssl — vérifié ICI, AVANT toute invite de mot de passe admin ou tout
# appel réseau : inutile de faire saisir un mot de passe pour une mise à jour
# qui ne pourra de toute façon pas être signée.
require_signing_tooling() {
    if ! command -v openssl >/dev/null 2>&1; then
        echo -e "${RED}❌ openssl introuvable — impossible de signer le manifeste OTA${NC}"
        echo -e "${RED}   Installez openssl (coreutils de la plupart des distributions / macOS) — envoi refusé (fail-closed)${NC}"
        return 1
    fi
    if [ ! -f "$OTA_SIGNING_KEY" ]; then
        echo -e "${RED}❌ Clé privée de signature introuvable : $OTA_SIGNING_KEY${NC}"
        echo -e "${RED}   Définissez OTA_SIGNING_KEY=<chemin> ou placez la clé à cet emplacement — envoi refusé (fail-closed)${NC}"
        return 1
    fi
    return 0
}
if ! require_signing_tooling; then
    exit 1
fi

# feature-151 D1 : version déclarée dans le manifeste. ⚠️ (revue de sécurité,
# 2026-10-18) LUE DANS src/version.h DE L'ARBRE DE TRAVAIL, PAS dans le binaire
# qui va être envoyé ($FIRMWARE_PATH/$FILESYSTEM_PATH) : rien ici ne garantit
# que ce dernier a été compilé APRÈS ce commentaire de version.h — un `pio
# run` oublié après une édition de src/version.h, ou un FIRMWARE_PATH pointant
# vers une archive d'un build antérieur, ferait signer une version déclarée
# qui NE CORRESPOND PAS à l'image réellement flashée. Mitigation : toujours
# `pio run` juste avant d'envoyer (cf. docs/BUILD.md) — ce script ne le
# vérifie pas lui-même (horodatage de build_src vs .bin, hors périmètre D1).
# Hissée ICI (AVANT la résolution des credentials, M5 — revue du 2026-10-18) :
# avec require_signing_tooling, c'est l'un des DEUX contrôles qui peuvent
# refuser l'envoi avant toute saisie — inutile de faire taper un mot de passe
# pour une version qu'on ne pourra de toute façon pas lire.
FIRMWARE_MANIFEST_VERSION=$(sed -n 's/.*FIRMWARE_VERSION "\(.*\)".*/\1/p' src/version.h | head -1)
if [ -z "$FIRMWARE_MANIFEST_VERSION" ]; then
    echo -e "${RED}❌ Impossible de lire FIRMWARE_VERSION dans src/version.h — envoi refusé (fail-closed)${NC}"
    exit 1
fi

# feature-151 D1, M3 (revue de sécurité, 2026-10-18) : `both` envoie deux
# fichiers ⇒ signe deux manifestes ⇒ openssl redemande la phrase de passe de
# la clé DEUX FOIS (une par appel à `sign_manifest`, cf. ci-dessous) — aucune
# mise en cache n'est faite ICI (la clé privée déchiffrée ne doit jamais
# rester en mémoire plus longtemps que le strict appel à openssl). Annoncé
# AVANT la première invite : la spec prévenait qu'« une friction pareille finit
# par être contournée » si elle surprend sans prévenir.
if [ "$TYPE" = "both" ]; then
    echo -e "${BLUE}ℹ️  Envoi firmware + filesystem ⇒ 2 signatures ⇒ la phrase de passe de la clé sera demandée 2 fois${NC}"
fi

# Résoudre les credentials
AUTH_ARGS=()
if [ -n "${3}" ]; then
    AUTH_ARGS=(-u "admin:${3}")
elif [ -n "${POOL_PASSWORD}" ]; then
    AUTH_ARGS=(-u "admin:${POOL_PASSWORD}")
elif [ -n "${POOL_TOKEN}" ]; then
    AUTH_ARGS=(-H "X-Auth-Token: ${POOL_TOKEN}")
else
    read -rsp "Mot de passe admin (poolcontroller.local) : " password
    echo
    AUTH_ARGS=(-u "admin:${password}")
fi

# feature-118 [O1] : empreinte SHA-256 du fichier, format "sha256:<64 hex
# minuscule>" EXIGÉ par /update depuis le lot B (empreinte obligatoire — une
# requête sans ce champ est refusée AVANT tout flash, cf.
# kOtaSha256MissingMessage, src/constants.h). Portabilité : `sha256sum`
# (coreutils GNU, présent sur Linux) est tenté en premier ; `shasum -a 256`
# (Perl, c'est l'outil natif sur macOS — `sha256sum` n'y existe PAS par
# défaut) en repli. Si NI L'UN NI L'AUTRE n'est trouvé : FAIL-CLOSED — le
# script refuse l'envoi lui-même, AVANT curl (cf. update_file ci-dessous).
# Le repli « envoi sans empreinte » du lot A a disparu : un appareil en lot B
# refuserait cet envoi de toute façon, un script qui l'enverrait quand même
# ferait croire à une tentative alors qu'elle est vouée à l'échec.
compute_sha256() {
    local file=$1
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$file" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$file" | awk '{print $1}'
    else
        return 1
    fi
}

# feature-151 D1 : construit le manifeste CANONIQUE (grammaire stricte,
# src/ota_manifest_logic.h) — "pool-ota/1 <kind> <version> sha256:<hex>" — et
# le signe (RSA-3072/PKCS#1 v1.5/SHA-256). Résultat dans $MANIFEST_RESULT et
# $SIGNATURE_RESULT (base64 standard, 512 caractères sans padding).
# ⚠️ `printf '%s'`, JAMAIS `echo` : le piège du LF final est CONNU (D0) —
# l'appareil re-sérialise le manifeste depuis les champs qu'il a parsés et
# vérifie la signature sur CES octets ; un seul octet de différence (un LF en
# trop côté signataire) romprait la vérification malgré une clé correcte.
# ⚠️ Phrase de passe : demandée par `openssl` LUI-MÊME (clé chiffrée),
# jamais interceptée ni stockée par ce script.
sign_manifest() {
    local kind=$1       # "firmware" ou "filesystem"
    local version=$2
    local digest=$3     # hex SANS préfixe "sha256:" (sortie de compute_sha256)
    local manifest="pool-ota/1 ${kind} ${version} sha256:${digest}"
    local sig_file err_file
    sig_file=$(mktemp) || return 1
    err_file=$(mktemp) || { rm -f "$sig_file"; return 1; }
    if ! printf '%s' "$manifest" | openssl dgst -sha256 -sign "$OTA_SIGNING_KEY" -out "$sig_file" 2>"$err_file"; then
        echo -e "${RED}❌ Échec de signature (phrase de passe incorrecte, ou clé invalide) :${NC}"
        cat "$err_file" >&2
        rm -f "$sig_file" "$err_file"
        return 1
    fi
    rm -f "$err_file"
    SIGNATURE_RESULT=$(openssl base64 -A -in "$sig_file")
    rm -f "$sig_file"
    MANIFEST_RESULT="$manifest"
    return 0
}

update_file() {
    local file=$1
    local name=$2
    local type=${3:-firmware}  # "firmware" ou "filesystem"

    if [ ! -f "$file" ]; then
        echo -e "${RED}❌ Fichier introuvable : $file${NC}"
        return 1
    fi

    # [O1] Champ "sha256" calculé AVANT l'envoi et placé AVANT "update" dans
    # l'ordre des arguments curl (le serveur lit les champs non-fichier du
    # multipart dans l'ordre d'arrivée — un sha256 envoyé APRÈS le fichier
    # arriverait trop tard pour être vu à index==0).
    # FAIL-CLOSED (feature-118 lot B) : sans outil de hachage disponible, on
    # refuse l'envoi ICI, AVANT curl — un /update sans empreinte serait de
    # toute façon rejeté par le contrôleur (400, kOtaSha256MissingMessage) ;
    # échouer tôt évite une tentative réseau pour rien et un message serveur
    # moins actionnable que celui-ci.
    local digest
    if ! digest=$(compute_sha256 "$file"); then
        echo -e "${RED}❌ Ni sha256sum ni shasum disponibles — impossible de calculer l'empreinte SHA-256${NC}"
        echo -e "${RED}   Installez coreutils (sha256sum) ou Perl (shasum -a 256) — envoi de $name refusé (fail-closed)${NC}"
        return 1
    fi
    local sha256_args=(-F "sha256=sha256:${digest}")

    # feature-151 D1 : manifeste + signature — AVANT curl, donc avant toute
    # tentative réseau. `require_signing_tooling` a déjà validé openssl/clé en
    # tête de script ; un échec ICI (phrase de passe incorrecte) reste
    # possible et doit encore refuser l'envoi (fail-closed).
    if ! sign_manifest "$type" "$FIRMWARE_MANIFEST_VERSION" "$digest"; then
        echo -e "${RED}❌ Signature du manifeste échouée — envoi de $name refusé (fail-closed)${NC}"
        return 1
    fi
    local signing_args=(-F "manifest=${MANIFEST_RESULT}" -F "signature=${SIGNATURE_RESULT}")

    echo -e "${BLUE}📤 Envoi de $name...${NC}"
    if ! response=$(curl -s --show-error --connect-timeout 10 --max-time 120 \
            -w "\n%{http_code}" -X POST "${AUTH_ARGS[@]}" \
            -F "update_type=$type" "${sha256_args[@]}" "${signing_args[@]}" -F "update=@$file" "$UPDATE_URL" 2>&1); then
        echo -e "${RED}❌ curl a échoué pour $name :${NC}"
        echo "$response"
        return 1
    fi
    http_code=$(echo "$response" | tail -1)
    body=$(echo "$response" | sed '$d')

    # Test STRICT : HTTP 200 ET corps exactement "OK".
    # L'ancien `grep -q "OK"` n'était pas ancré : depuis 2.20.1 le corps d'échec
    # est une phrase française "FAIL: <cause>" qui peut contenir "OK" (ex. le
    # nom du fichier envoyé) → faux succès silencieux.
    body_trimmed=$(printf '%s' "$body" | tr -d '[:space:]')

    if [ "$http_code" = "200" ] && [ "$body_trimmed" = "OK" ]; then
        echo -e "${GREEN}✅ $name mis à jour${NC}"
        # Marque l'archive correspondante comme DÉPLOYÉE : tools/archive_build.py
        # ne purgera plus cet ELF, seul décodeur possible d'un futur coredump
        # (esp-coredump vérifie le SHA256 de l'image applicative). Sans ce
        # marqueur, cinq builds de la même version évincent l'archive du firmware
        # en production — c'est arrivé le 2026-09-13 sur la 2.51.0, et l'ELF du
        # 12/09 a été définitivement perdu. Jamais bloquant : un déploiement
        # réussi ne doit pas échouer sur un archivage.
        if [ "$type" = "firmware" ] && [ -f tools/mark_deployed.py ]; then
            python3 tools/mark_deployed.py "$file" || true
        fi
        return 0
    elif [ "$http_code" = "401" ]; then
        echo -e "${RED}❌ Authentification refusée (mot de passe incorrect ?)${NC}"
        return 1
    else
        echo -e "${RED}❌ Échec de la mise à jour de $name (HTTP $http_code)${NC}"
        if [ -n "$body" ]; then
            echo -e "${RED}   Réponse du contrôleur : $body${NC}"
        else
            echo -e "${RED}   Réponse vide — consulter le Journal du contrôleur${NC}"
        fi
        # [O3] Indice ciblé quand le refus porte sur l'empreinte (feature-118) :
        # le message brut du contrôleur ne précise pas que l'ORDRE des champs
        # multipart est en cause si le sha256 a été envoyé mais arrive après
        # le fichier (ce script l'envoie toujours AVANT — cf. [O1] ci-dessus).
        if [ "$http_code" = "400" ] && printf '%s' "$body" | grep -qi "sha256\|empreinte"; then
            echo -e "${RED}   → Vérifiez l'empreinte SHA-256 (sha256sum/shasum) et son format \"sha256:<64 hex>\"${NC}"
        fi
        return 1
    fi
}

case "$TYPE" in
    firmware)
        update_file "$FIRMWARE_PATH" "Firmware" "firmware" || exit 1
        ;;
    filesystem|fs)
        update_file "$FILESYSTEM_PATH" "Filesystem" "filesystem" || exit 1
        ;;
    both)
        update_file "$FIRMWARE_PATH" "Firmware" "firmware" || exit 1
        sleep 30
        update_file "$FILESYSTEM_PATH" "Filesystem" "filesystem" || exit 1
        ;;
    *)
        echo "Usage: $0 [firmware|filesystem|both] [hostname] [password]"
        exit 1
        ;;
esac

echo -e "${GREEN}✅ Terminé !${NC}"
