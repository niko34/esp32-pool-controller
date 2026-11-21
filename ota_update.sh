#!/bin/bash
# ota_update.sh - Mise à jour rapide sans recompilation
# Usage: ./ota_update.sh [firmware|filesystem|both] [hostname] [password]
#        ./ota_update.sh --sign-only [firmware|filesystem|both]
#
# --sign-only (feature-151 D1-bis) : produit le(s) fichier(s) ".sig" à côté
# du/des binaire(s), SANS AUCUN envoi réseau et SANS mot de passe admin —
# c'est le geste de développement qui alimente la carte "Mise à jour
# manuelle" (celle-ci ne peut pas CONSTRUIRE le manifeste, voir
# specs/features/done/feature-151-signature-de-l-image-ota.md, section
# D1-bis). Réutilise exactement `sign_manifest()`/`compute_sha256()` —
# même format de fichier que `.claude/commands/release.md` (étape 2ter).
#
# Le paramètre [hostname] accepte :
#   - Un nom mDNS : poolcontroller.local  (nécessite que le mDNS fonctionne sur le réseau)
#   - Une adresse IP : 192.168.1.42       (fonctionne sans mDNS, utile si le réseau ne le supporte pas)
# Par défaut : poolcontroller.local
#
# Authentification (bug-184) : UNE seule preuve, le jeton X-Auth-Token. L'appareil
# n'accepte plus HTTP Basic. Ordre de priorité :
#   1. Variable d'environnement POOL_TOKEN    : POOL_TOKEN=<api_token> ./ota_update.sh
#   2. Argument $3 : ./ota_update.sh both poolcontroller.local monmotdepasse
#      ⚠️ DÉCONSEILLÉ : un argument reste visible dans /proc et dans l'historique
#      du shell. Préférer POOL_TOKEN, POOL_PASSWORD ou la saisie interactive.
#   3. Variable d'environnement POOL_PASSWORD : POOL_PASSWORD=monmotdepasse ./ota_update.sh
#   4. Saisie interactive si aucun des précédents n'est défini
# Un mot de passe est échangé contre un jeton par POST /auth/login (existe aussi
# sur les anciens firmwares : ce script marche sur ancien et neuf). Erreurs :
#   401 -> mot de passe incorrect
#   403 -> assistant de configuration non terminé : terminez-le dans l'interface web
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

# feature-151 D1-bis : `--sign-only` EN PREMIER ARGUMENT bascule tout le
# script en mode local (aucune requête réseau, aucun mot de passe admin) —
# shift pour que le genre (firmware|filesystem|both) retombe en $1, comme
# en mode normal.
SIGN_ONLY=0
if [ "${1:-}" = "--sign-only" ]; then
    SIGN_ONLY=1
    shift
fi

# feature-151 D2 : `--use-sig` RELIT le fichier `.sig` déjà produit au lieu de
# re-signer. ⭐ RAISON MESURÉE le 2026-11-14 : le mode d'envoi re-signait le
# manifeste à chaque fois, donc réclamait la phrase de passe — alors que le
# `.sig` validé était déjà là, à côté du binaire. La campagne de preuve
# terrain (D2) en devenait impraticable sans un humain à chaque envoi.
# ⛔ C'est exactement ce que fait la PAGE WEB : elle transporte les deux
# lignes, elle ne signe jamais (ADR-0121).
#
#   --use-sig              → lit <binaire>.sig, et REFUSE si son empreinte ne
#                            décrit pas le binaire envoyé (fail-closed).
#   --use-sig-raw <fichier> → lit CE fichier SANS contrôle d'empreinte.
#                            ⚠️ Réservé aux ESSAIS NÉGATIFS de D2 (mélange de
#                            fichiers, signature altérée) : c'est le seul
#                            moyen de faire parler la garde de l'appareil.
USE_SIG=0
SIG_PATH=""
SIG_UNCHECKED=0
if [ "${1:-}" = "--use-sig" ]; then
    USE_SIG=1
    shift
elif [ "${1:-}" = "--use-sig-raw" ]; then
    USE_SIG=1
    SIG_UNCHECKED=1
    SIG_PATH="${2:-}"
    if [ -z "$SIG_PATH" ]; then
        echo "--use-sig-raw exige un chemin de fichier .sig" >&2
        exit 1
    fi
    shift 2
fi

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
# ⚠️ En mode `--use-sig`, la clé PRIVÉE n'est pas nécessaire : on ne signe
# rien, on relit une signature déjà produite. Exiger la clé ici refuserait un
# envoi parfaitement légitime depuis une machine qui ne la détient pas — et
# c'est précisément la situation d'un poste de test.
if [ "$USE_SIG" = "0" ] && ! require_signing_tooling; then
    exit 1
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
# ⚠️ Déplacée ICI (feature-151 D1-bis) : `sign_only_file` (mode --sign-only,
# ci-dessous) l'utilise AVANT la section "Résoudre les credentials" — cette
# fonction et `sign_manifest` doivent donc être DÉFINIES avant ce point.
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
if [ "$TYPE" = "both" ] && [ "$USE_SIG" = "0" ]; then
    echo -e "${BLUE}ℹ️  Envoi firmware + filesystem ⇒ 2 signatures ⇒ la phrase de passe de la clé sera demandée 2 fois${NC}"
fi

# =============================================================================
# FRAÎCHEUR DU BINAIRE (2026-11-17) — le garde-fou qui remplace une discipline.
#
# POURQUOI : le manifeste signé déclare un numéro de version lu dans
# `src/version.h` de l'ARBRE DE TRAVAIL (l. ~200), pas dans le binaire envoyé.
# Un `pio run` oublié après un bump produit donc une signature PARFAITEMENT
# VALIDE portant une étiquette FAUSSE : l'empreinte décrit bien le fichier,
# mais le numéro décrit un autre build. L'utilisateur lit ce numéro sur la
# carte de confirmation (`data/app.js`, ligne de récapitulatif) avant de
# cliquer — c'est la seule chose qu'il ait pour savoir ce qu'il installe.
#
# ⛔ CE N'EST PAS UNE FAILLE : l'étiquette est produite par la clé privée de
# l'utilisateur, un tiers ne peut pas la fabriquer, et AUCUNE décision de
# l'appareil n'en dépend (le plancher de version est figé à 0.0.0, et D5 — le
# lot qui devait le relever — a été ANNULÉ le 2026-11-17 : décision produit
# « AUCUN PLANCHER, NULLE PART », feature-151). C'est de la comptabilité :
# savoir ce qui tourne réellement.
#
# LA FORMULATION JUSTE DU DÉFAUT n'est pas « le numéro vient du code source »
# mais « le binaire peut être PLUS VIEUX que le code source ». Ainsi posé, le
# contrôle est le même pour les deux genres — et il n'a pas besoin de savoir
# lire une version à l'intérieur d'un binaire (⚠️ impossible pour
# `littlefs.bin` : les pages y sont gzippées, mesuré le 2026-11-17).
#
# ⚠️ POURQUOI UN SCRIPT ET PAS UNE CONSIGNE : la consigne existait déjà, dans
# `docs/BUILD.md` ET dans le commentaire de FIRMWARE_MANIFEST_VERSION. Le
# 2026-11-14 la règle jumelle (« ne recompilez rien entre la signature et les
# tests ») était écrite dans la spec depuis quelques heures : un sous-agent a
# relancé `build_fs.sh`, la signature est devenue caduque et la réparation d'un
# essai destructif a été REFUSÉE. ⇒ Une règle qu'aucune machine ne vérifie
# finit par être violée.
#
# ⛔ PAS DE DÉROGATION, pas de variable d'échappement : un interrupteur
# « sauter le contrôle » serait mis une fois pour débloquer, puis oublié.
# `--use-sig-raw` (essais négatifs) n'est PAS concerné — il joue sur le couple
# manifeste/signature, pas sur la fraîcheur du binaire.
# =============================================================================
require_fresh_binary() {
    local bin=$1 genre=$2 src_dir

    case "$genre" in
        firmware)   src_dir="src"  ;;
        filesystem) src_dir="data" ;;
        *) echo -e "${RED}❌ require_fresh_binary : genre inconnu « $genre »${NC}"; return 1 ;;
    esac

    [ -d "$src_dir" ] || return 0   # hors dépôt (binaire fourni) : rien à comparer

    # `find -newer` compare les mtime. ⚠️ Pour le firmware on ajoute
    # `src/version.h` explicitement : c'est LUI qui fournit l'étiquette, et un
    # montage réseau à granularité de mtime grossière peut le faire manquer à
    # `find`. `sort -u` dédoublonne — sans lui le fichier sortait DEUX fois.
    # On n'affiche que 5 coupables : la liste complète d'un `git checkout`
    # serait illisible et noierait le message.
    local recents
    recents=$( { find "$src_dir" -type f -newer "$bin" 2>/dev/null
                 if [ "$genre" = "firmware" ] && [ src/version.h -nt "$bin" ]; then
                     echo "src/version.h"
                 fi
               } | sort -u | head -5 )

    [ -z "$recents" ] && return 0

    echo -e "${RED}❌ $bin est PLUS ANCIEN que ses sources — envoi refusé (fail-closed)${NC}" >&2
    echo    "   Modifié après la construction du binaire :" >&2
    # ⛔ IFS=newline, pas de word-splitting : un nom de fichier peut contenir
    # des espaces, et une liste mal découpée rendrait le message trompeur.
    printf '     %s\n' "$recents" >&2
    if [ "$genre" = "firmware" ]; then
        echo "   ⇒ Le manifeste déclarerait la version de src/version.h (${FIRMWARE_MANIFEST_VERSION:-?})," >&2
        echo "     alors que le binaire envoyé vient d'une construction antérieure." >&2
        echo "   Correctif : « pio run », puis re-signer (⚠️ signer est le DERNIER geste)." >&2
    else
        echo "   Correctif : « ./build_fs.sh », puis re-signer (⚠️ signer est le DERNIER geste)." >&2
    fi
    return 1
}

# =============================================================================
# --sign-only (feature-151 D1-bis) : produit le(s) ".sig" et S'ARRÊTE LÀ.
# ⛔ Aucune requête réseau, ⛔ aucun mot de passe admin requis dans ce mode —
# c'est pourquoi ce bloc est placé AVANT "Résoudre les credentials" : inutile
# de faire saisir un mot de passe admin pour une opération qui ne contactera
# jamais l'appareil.
# =============================================================================
sign_only_file() {
    local file=$1
    local genre=$2  # "firmware" ou "filesystem"

    if [ ! -f "$file" ]; then
        echo -e "${RED}❌ Fichier introuvable : $file${NC}"
        return 1
    fi

    # ⭐ AVANT de signer : c'est ICI que l'étiquette de version est frappée.
    require_fresh_binary "$file" "$genre" || return 1

    local digest
    if ! digest=$(compute_sha256 "$file"); then
        echo -e "${RED}❌ Ni sha256sum ni shasum disponibles — impossible de calculer l'empreinte SHA-256${NC}"
        return 1
    fi

    if ! sign_manifest "$genre" "$FIRMWARE_MANIFEST_VERSION" "$digest"; then
        echo -e "${RED}❌ Signature du manifeste échouée — $file.sig NON produit${NC}"
        return 1
    fi

    # ⚠️ Format 2 lignes STRICT — BIT POUR BIT identique au bloc produit par
    # .claude/commands/release.md (étape 2ter) : deux producteurs du même
    # format ne doivent jamais diverger. Ligne 1 = manifeste canonique (le
    # \n qui suit est un séparateur de FICHIER, jamais signé — le message
    # réellement signé est sans saut de ligne final, cf. sign_manifest) ;
    # ligne 2 = signature base64 (openssl base64 -A, jamais repliée).
    { printf '%s\n' "$MANIFEST_RESULT"; printf '%s\n' "$SIGNATURE_RESULT"; } > "$file.sig"
    echo -e "${GREEN}✅ Signature produite : $file.sig${NC}"
    return 0
}

if [ "$SIGN_ONLY" = "1" ]; then
    case "$TYPE" in
        firmware)
            sign_only_file "$FIRMWARE_PATH" "firmware" || exit 1
            ;;
        filesystem|fs)
            sign_only_file "$FILESYSTEM_PATH" "filesystem" || exit 1
            ;;
        both)
            sign_only_file "$FIRMWARE_PATH" "firmware" || exit 1
            sign_only_file "$FILESYSTEM_PATH" "filesystem" || exit 1
            ;;
        *)
            echo "Usage: $0 --sign-only [firmware|filesystem|both]"
            exit 1
            ;;
    esac
    echo -e "${GREEN}✅ Terminé (signature seule, aucun envoi réseau) !${NC}"
    exit 0
fi

# Résoudre les credentials (bug-184) : toujours un jeton X-Auth-Token.
# Mot de passe -> POST /auth/login -> jeton.
#
# ⚠️ NI le mot de passe NI le jeton ne doivent apparaître dans la ligne de
# commande de curl : /proc/<pid>/cmdline est lisible par TOUT utilisateur de la
# machine, alors que /proc/<pid>/environ ne l'est que par son propriétaire.
# ⇒ le mot de passe part dans un fichier de corps (`--data @fichier`, mode 600)
#   et le jeton dans un fichier de configuration curl (`--config`, mode 600).
# ⛔ Ne jamais revenir à `-H "X-Auth-Token: $jeton"` : c'est un argument.
AUTH_ARGS=()
AUTH_CONF=""
auth_conf_from_token() {
    # $1 = jeton. Écrit un fichier de configuration curl illisible par les autres
    # et renseigne AUTH_ARGS. Le jeton ne transite jamais par argv.
    #
    # ⛔ VALIDATION OBLIGATOIRE AVANT ÉCRITURE. Un fichier `--config` porte UNE
    # OPTION PAR LIGNE : un saut de ligne, un guillemet ou un antislash dans le
    # jeton refermerait l'en-tête et ouvrirait des lignes d'options arbitraires
    # (destination, fichier de sortie, fichier à téléverser) — c'est-à-dire la
    # prise de contrôle de CETTE machine, celle qui détient la clé de signature
    # OTA. Or le jeton vient du RÉSEAU en clair (réponse de POST /auth/login) :
    # un faux appareil sur le LAN le choisit. Relevé par security-reviewer (P2).
    # Le firmware en génère 32 caractères hexadécimaux (AuthManager::generateRandomToken).
    # L'alphabet accepté est un peu plus large pour survivre à un futur format,
    # mais ⛔ sans AUCUN caractère de structure.
    case "$1" in
        *[!A-Za-z0-9._-]*|"")
            echo -e "${RED}❌ Jeton refusé : caractère interdit${NC}" >&2
            echo -e "   Attendu : lettres, chiffres, '.', '_' ou '-' uniquement." >&2
            echo -e "   ⛔ Aucun fichier de configuration n'a été écrit." >&2
            exit 1
            ;;
    esac
    AUTH_CONF=$(mktemp)
    chmod 600 "$AUTH_CONF"
    printf 'header = "X-Auth-Token: %s"\n' "$1" > "$AUTH_CONF"
    AUTH_ARGS=(--config "$AUTH_CONF")
}
cleanup_auth_files() { rm -f "$AUTH_CONF" "$LOGIN_BODY" "$LOGIN_RESP"; }
LOGIN_BODY=""
LOGIN_RESP=""
trap cleanup_auth_files EXIT

if [ -n "${POOL_TOKEN}" ]; then
    auth_conf_from_token "${POOL_TOKEN}"
else
    if [ -n "${3}" ]; then
        password="${3}"
    elif [ -n "${POOL_PASSWORD}" ]; then
        password="${POOL_PASSWORD}"
    else
        read -rsp "Mot de passe admin ($ESP32_HOST) : " password
        echo
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        # Même exigence déclarée que pour openssl et sha256sum plus haut : sans
        # ce contrôle, `set -e` tuerait le script sur un « command not found »
        # nu, sans dire ce qui manque.
        echo -e "${RED}❌ python3 est requis pour échanger le mot de passe contre un jeton${NC}" >&2
        echo -e "   Contournement sans python3 : fournir POOL_TOKEN=<jeton> directement." >&2
        exit 1
    fi
    LOGIN_BODY=$(mktemp)
    LOGIN_RESP=$(mktemp)
    # mktemp crée DÉJÀ en 0600 ; ce chmod est une ceinture redondante, pas la
    # protection elle-même (⚠️ ne pas lire l'inverse : il n'y a pas de fenêtre
    # de course à refermer ici).
    chmod 600 "$LOGIN_BODY" "$LOGIN_RESP"
    # Le mot de passe passe par l'environnement du python (lisible du seul
    # propriétaire), jamais par son argv.
    PW="$password" python3 -c 'import json,os;print(json.dumps({"username":"admin","password":os.environ["PW"]}))' > "$LOGIN_BODY"
    unset password
    login_code=$(curl -s --connect-timeout 10 --max-time 20 -o "$LOGIN_RESP" -w "%{http_code}" \
        -X POST -H "Content-Type: application/json" --data @"$LOGIN_BODY" \
        "http://$ESP32_HOST/auth/login") || login_code="000"
    rm -f "$LOGIN_BODY"; LOGIN_BODY=""
    case "$login_code" in
        200)
            # `|| token_obtained=""` : un corps qui n'est pas du JSON ferait lever
            # python et `set -e` tuerait le script sur une trace d'erreur illisible.
            # On retombe sur le message maison juste en dessous.
            token_obtained=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("token",""))' "$LOGIN_RESP" 2>/dev/null) || token_obtained=""
            if [ -z "$token_obtained" ]; then
                echo -e "${RED}❌ Connexion acceptée mais aucun jeton reçu${NC}"; exit 1
            fi
            auth_conf_from_token "$token_obtained"
            unset token_obtained
            rm -f "$LOGIN_RESP"; LOGIN_RESP=""   # le jeton n'a plus à traîner sur le disque
            ;;
        401) echo -e "${RED}❌ Mot de passe incorrect${NC}"; exit 1 ;;
        403)
            echo -e "${RED}❌ Premier démarrage : l'assistant de configuration n'est pas terminé.${NC}"
            echo -e "   Terminez-le dans l'interface web, puis relancez."
            echo -e "   ⚠️ Si l'interface est HORS SERVICE (c'est souvent pourquoi on envoie une mise à jour) :"
            echo -e "   la page de secours http://$ESP32_HOST/recovery permet d'obtenir un jeton ;"
            echo -e "   relancez ensuite avec POOL_TOKEN=<jeton> $0 ..."
            exit 1 ;;
        429) echo -e "${RED}❌ Trop de requêtes : patientez une minute${NC}"; exit 1 ;;
        *)   echo -e "${RED}❌ Connexion impossible (HTTP $login_code)${NC}"; exit 1 ;;
    esac
fi

# feature-151 D2 : relit un `.sig` à DEUX LIGNES (ADR-0121) et le valide avec
# la MÊME grammaire que `parseSigFile()` de `data/app.js` — exactement 2
# lignes, pas de \r, ligne 2 = 512 caractères base64 sans remplissage.
# ⛔ Un gate plus permissif que le lecteur produirait un artefact accepté ici
# et refusé chez l'opérateur : c'est le défaut déjà corrigé dans
# tools/check_release_artefacts.sh.
# Résultat dans $MANIFEST_RESULT / $SIGNATURE_RESULT, comme sign_manifest().
read_sig_file() {
    local sig=$1 kind=$2 digest=$3
    if [ ! -f "$sig" ]; then
        echo -e "${RED}❌ Fichier de signature introuvable : $sig${NC}"
        echo -e "${RED}   Produisez-le avec « ./ota_update.sh --sign-only $kind » — envoi refusé (fail-closed)${NC}"
        return 1
    fi
    if LC_ALL=C grep -q $'\r' "$sig"; then
        echo -e "${RED}❌ $sig contient un retour chariot (fins de ligne Windows) — envoi refusé${NC}"
        return 1
    fi
    local n
    n=$(wc -l < "$sig" | tr -d ' ')
    if [ "$n" != "2" ]; then
        echo -e "${RED}❌ $sig n'a pas EXACTEMENT 2 lignes (vu : $n) — envoi refusé${NC}"
        return 1
    fi
    local m b
    m="$(sed -n '1p' "$sig")"
    b="$(sed -n '2p' "$sig")"
    if ! printf '%s' "$b" | LC_ALL=C grep -Eq '^[A-Za-z0-9+/]{512}$'; then
        echo -e "${RED}❌ La ligne 2 de $sig n'est pas une signature RSA-3072 base64 (512 caractères) — envoi refusé${NC}"
        return 1
    fi
    if [ "$SIG_UNCHECKED" = "1" ]; then
        # ⚠️ Volontairement NON vérifié : c'est l'objet de l'essai.
        echo -e "${RED}⚠️  --use-sig-raw : empreinte NON vérifiée, envoi d'un couple délibérément incohérent${NC}"
        echo -e "${RED}    manifeste : $m${NC}"
    elif [ "$m" != "pool-ota/1 $kind $FIRMWARE_MANIFEST_VERSION sha256:$digest" ]; then
        echo -e "${RED}❌ Le manifeste de $sig ne décrit PAS ce binaire — envoi refusé (fail-closed)${NC}"
        echo -e "${RED}   Dans le fichier : $m${NC}"
        echo -e "${RED}   Attendu         : pool-ota/1 $kind $FIRMWARE_MANIFEST_VERSION sha256:$digest${NC}"
        echo -e "${RED}   → signature d'une AUTRE version, d'un AUTRE binaire, ou binaire recompilé depuis.${NC}"
        return 1
    fi
    MANIFEST_RESULT="$m"
    SIGNATURE_RESULT="$b"
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

    # ⭐ Même contrôle à l'envoi qu'à la signature : avec `--use-sig` le `.sig`
    # a pu être produit sur une machine ou à un moment où l'arbre était sain.
    require_fresh_binary "$file" "$type" || return 1

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
    if [ "$USE_SIG" = "1" ]; then
        local sig_file="${SIG_PATH:-$file.sig}"
        if ! read_sig_file "$sig_file" "$type" "$digest"; then
            echo -e "${RED}❌ Signature inutilisable — envoi de $name refusé (fail-closed)${NC}"
            return 1
        fi
        echo -e "${BLUE}🔏 Signature relue depuis $sig_file (aucune re-signature)${NC}"
    elif ! sign_manifest "$type" "$FIRMWARE_MANIFEST_VERSION" "$digest"; then
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
        # bug-184 : ici le mot de passe a DÉJÀ été validé par /auth/login — un 401
        # à ce stade ne peut donc PAS être « mot de passe incorrect ». C'est le
        # JETON qui est refusé : régénéré entre-temps, ou POOL_TOKEN périmé.
        echo -e "${RED}❌ Jeton refusé par l'appareil${NC}"
        echo -e "   Il a pu être régénéré depuis. Relancez SANS POOL_TOKEN pour en obtenir un neuf."
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
        echo "Usage: $0 [--use-sig|--use-sig-raw <f.sig>] [firmware|filesystem|both] [hostname] [password]"
        echo "       $0 --sign-only [firmware|filesystem|both]"
        exit 1
        ;;
esac

echo -e "${GREEN}✅ Terminé !${NC}"
