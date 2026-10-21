#!/usr/bin/env bash
# =============================================================================
# check_release_artefacts.sh — R3 absente de L'ARTEFACT de release (S1)
# =============================================================================
# feature-128, ADR-0081 : « non livrée » signifie ABSENTE du binaire publié,
# pas seulement absente de l'arbre (c'est le rôle de check_no_peek_flag.sh,
# qui ne prouve QUE l'arbre — une variable PLATFORMIO_BUILD_FLAGS résiduelle
# du shell qui lance `/release` échapperait entièrement à ce contrôle-là).
#
# Appelé par .claude/commands/release.md entre l'étape 2 (compilation) et
# l'étape 6 (publication). Revue code-reviewer (Request changes) : une
# instruction « ARRÊTE » en prose dans un fichier de commande interprétée
# N'EST PAS une garantie — rien ne force l'exécutant à s'arrêter. Un script à
# CODE DE SORTIE en est une.
#
# CE QUE CE SCRIPT VÉRIFIE, DANS L'ORDRE :
#   1. `/debug/heap_peek` absente du .bin ET de l'.elf qui viennent d'être
#      compilés par `pio run -e esp32dev_core3` (`.pio/build/esp32dev_core3/`).
#      ⚠️ `nm firmware.elf | grep heap_peek` NE SUFFIT PAS : sous `-Os` le
#      lambda de la route s'inline et son nom devient manglé — un `nm` vide
#      peut être un FAUX NÉGATIF. La chaîne de chemin `/debug/heap_peek`, elle,
#      DOIT exister en `.rodata` pour que `server->on("/debug/heap_peek", ...)`
#      enregistre la route : ni l'inlining ni le renommage ne la font
#      disparaître de l'image.
#   2. Que l'ELF qui sera RÉELLEMENT attaché à l'étape 6 (celui archivé sous
#      builds/<VERSION>/esp32dev_core3/, copié par tools/archive_build.py) est
#      bien une copie BINAIRE IDENTIQUE de l'ELF vérifié au point 1 — pas un
#      ELF plus ancien qui traînerait.
#
# ⚠️ CORRECTIF DE REVUE (S1, code-reviewer) — erreurs de la première version :
#   - `builds/` est un chemin DÉRIVÉ, comparé au `.pio/build/` qui en est la
#     SOURCE (`shutil.copy2` dans archive_build.py) : on ne teste plus « la
#     copie » comme si c'était l'original, on teste l'ORIGINAL puis on prouve
#     que la copie qui sera publiée lui est IDENTIQUE (`cmp`).
#   - Sélection de l'archive par NOM, JAMAIS par date de modification :
#     `archive_build.py:69-73` documente que `shutil.copy2` recopie la mtime
#     de la SOURCE — un rebuild d'une révision ancienne peut ainsi porter une
#     mtime plus récente que l'archive courante. `ls -t` reproduirait ce
#     défaut. L'horodatage est en tête du nom (AAAAMMJJ_HHMMSS) : l'ordre
#     LEXICOGRAPHIQUE est l'ordre chronologique — `sort -r` dessus, jamais
#     `ls -t`.
#   - `builds/` est ignoré par git (.gitignore:23) : sur un dossier vide ou
#     absent, ce script échoue avec un message EXPLICITE plutôt que de laisser
#     `grep`/`cmp` sortir en erreur muette sur une chaîne vide.
#
# Usage : ./tools/check_release_artefacts.sh
# Sortie : 0 si conforme, 1 sinon (raison explicite sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

ENV_DIR=".pio/build/esp32dev_core3"
BIN="${ENV_DIR}/firmware.bin"
ELF="${ENV_DIR}/firmware.elf"

if [ ! -f "$BIN" ] || [ ! -f "$ELF" ]; then
  echo "ERREUR : $BIN et/ou $ELF introuvables." >&2
  echo "Lance 'pio run -e esp32dev_core3' avant ce contrôle (étape 2 de /release)." >&2
  exit 1
fi

bin_count=$(grep -a -c '/debug/heap_peek' "$BIN" || true)
elf_count=$(grep -a -c '/debug/heap_peek' "$ELF" || true)
bin_count=${bin_count:-0}
elf_count=${elf_count:-0}

echo "grep -a -c '/debug/heap_peek' $BIN -> $bin_count"
echo "grep -a -c '/debug/heap_peek' $ELF -> $elf_count"

if [ "$bin_count" -ge 1 ] || [ "$elf_count" -ge 1 ]; then
  {
    echo
    echo "ERREUR S1 : /debug/heap_peek présente dans un artefact fraîchement compilé."
    echo "R3 (lecture de contenu du tas) ne doit JAMAIS être publiée."
    echo "PLATFORMIO_BUILD_FLAGS actuel : '${PLATFORMIO_BUILD_FLAGS:-}'"
    echo "Corrige (nouveau shell, ou 'unset PLATFORMIO_BUILD_FLAGS') puis relance"
    echo "'pio run -e esp32dev_core3' avant de relancer ce contrôle."
  } >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# L'ELF vérifié ci-dessus est-il bien celui que l'étape 6 attachera ?
# ---------------------------------------------------------------------------
version=$(sed -nE 's/.*FIRMWARE_VERSION[[:space:]]+"([^"]+)".*/\1/p' src/version.h | head -1)
if [ -z "$version" ]; then
  echo "ERREUR : impossible de lire FIRMWARE_VERSION dans src/version.h." >&2
  exit 1
fi

archive_dir="builds/${version}/esp32dev_core3"
if [ ! -d "$archive_dir" ]; then
  {
    echo "ERREUR : $archive_dir introuvable."
    echo "Le hook d'archivage post-build (tools/archive_build.py, branché en"
    echo "extra_scripts = post: sur platformio.ini) ne s'est pas exécuté — vérifie"
    echo "que l'étape 2 a bien lancé 'pio run' (pas 'pio run -t nobuild')."
  } >&2
  exit 1
fi

# Tri par NOM (AAAAMMJJ_HHMMSS en tête), PAS par mtime — voir l'en-tête.
# `! -name '*-diag.elf'` : les builds NON STANDARD (PLATFORMIO_BUILD_FLAGS non
# vide) sont archivés avec le suffixe `-diag` par tools/archive_build.py
# (condition R3-2). Ils embarquent potentiellement `/debug/heap_peek` — une route
# qui lit la mémoire du processus — et ne doivent JAMAIS être candidats à une
# release. Sans cette exclusion, le suffixe ne sert à rien : c'est précisément ce
# qui s'est produit le 2026-09-14, ce contrôle sélectionnant l'archive `-diag`
# qu'il venait lui-même de faire créer.
latest_elf=$(find "$archive_dir" -maxdepth 1 -type f -name 'firmware_*.elf' \
  ! -name '*-diag.elf' 2>/dev/null | sort -r | head -1)
if [ -z "$latest_elf" ]; then
  echo "ERREUR : aucun firmware_*.elf archivé sous $archive_dir." >&2
  exit 1
fi

if ! cmp -s "$ELF" "$latest_elf"; then
  {
    echo
    echo "ERREUR S1 : $latest_elf (celui que l'étape 6 attacherait) DIFFÈRE de"
    echo "$ELF (celui vérifié ci-dessus, absence de R3 confirmée)."
    echo "L'archive est probablement PÉRIMÉE (build antérieur) — relance"
    echo "'pio run -e esp32dev_core3' puis ce contrôle."
  } >&2
  exit 1
fi

echo "OK — $latest_elf (attaché à l'étape 6) est une copie binaire identique de"
echo "$ELF, tous deux vérifiés SANS /debug/heap_peek."

# =============================================================================
# S2 — SIGNATURES : une release signée, ou pas de release (feature-151, M1)
# =============================================================================
# ⚠️ POURQUOI CE CONTRÔLE EXISTE : `/check-update` cherche des assets
# `firmware.bin.sig` et `littlefs.bin.sig`. Si la release n'en porte pas, les
# champs `firmware_sig_url`/`filesystem_sig_url` restent VIDES et l'appareil
# n'a rien à vérifier — la fonctionnalité meurt EN SILENCE. C'est exactement
# ce qui s'est produit six semaines durant avec `littlefs.bin` (bug-170) :
# un asset manquant ne fait échouer personne, il rend seulement la chose
# inopérante sans un mot.
#
# ⭐ ET SURTOUT : on ne vérifie pas que les .sig EXISTENT, on vérifie qu'ils
# sont VALIDES **POUR LA CLÉ QUE CE FIRMWARE EMBARQUE**. Le DER est extrait
# de `src/constants.h` — donc du binaire qu'on s'apprête à publier, pas d'un
# fichier à côté. Signer avec une autre clé (clé de test, clé périmée,
# mauvaise machine) est ainsi arrêté AVANT publication, et non découvert par
# l'appareil après coup.
# ⛔ Un contrôle qui se contenterait de `test -f` ne protégerait de rien.
# =============================================================================

sig_tmp="$(mktemp -d)"
trap 'rm -rf "$sig_tmp"' EXIT

# DER de la clé publique, extrait du code qui part en flash.
python3 - "$sig_tmp/pub.der" <<'PYEOF'
import re, sys, pathlib
src = pathlib.Path("src/constants.h").read_text()
i = src.index("kOtaSigningPublicKeyDer[kOtaSigningPublicKeyDerLen] = {")
j = src.index("};", i)
data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", src[i:j]))
if len(data) != 422:
    sys.exit("ERREUR : DER embarque de taille inattendue (%d o)" % len(data))
pathlib.Path(sys.argv[1]).write_bytes(data)
PYEOF
if [ ! -s "$sig_tmp/pub.der" ]; then
  echo "ERREUR S2 : clé publique introuvable dans src/constants.h." >&2
  exit 1
fi
openssl pkey -pubin -inform DER -in "$sig_tmp/pub.der" -out "$sig_tmp/pub.pem" 2>/dev/null || {
  echo "ERREUR S2 : le DER embarqué dans src/constants.h n'est pas une clé publique lisible." >&2
  exit 1
}

build_dir="$(dirname "$ELF")"
sig_errors=0
for genre in firmware filesystem; do
  case "$genre" in
    firmware)   bin="$build_dir/firmware.bin" ;;
    filesystem) bin="$build_dir/littlefs.bin" ;;
  esac
  sig="$bin.sig"

  if [ ! -f "$bin" ]; then
    echo "ERREUR S2 : $bin absent — lancer 'pio run' et './build_fs.sh'." >&2
    sig_errors=1
    continue
  fi
  if [ ! -f "$sig" ]; then
    {
      echo "ERREUR S2 : $sig ABSENT."
      echo "  → Sans lui, /check-update servira une URL de signature VIDE et"
      echo "    l'appareil n'aura rien à vérifier : la fonctionnalité meurt en silence."
      echo "  → Produire la signature avec la clé privée (hors dépôt) avant de publier."
    } >&2
    sig_errors=1
    continue
  fi

  version=$(sed -n 's/.*FIRMWARE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' src/version.h)
  digest=$( { sha256sum "$bin" 2>/dev/null || shasum -a 256 "$bin"; } | awk '{print $1}' )
  manifest="$sig_tmp/manifest_$genre.txt"
  printf '%s' "pool-ota/1 $genre $version sha256:$digest" > "$manifest"

  # ⚠️ FORMAT À 2 LIGNES (feature-151 D1-bis) : ligne 1 = manifeste canonique,
  # ligne 2 = signature base64. ⭐ RAISON : la page web ne peut pas CONSTRUIRE
  # le manifeste — il contient la version, et le descripteur d'application du
  # binaire porte un hash git, pas la version SemVer. Elle ne peut que le
  # TRANSPORTER. ⛔ Aucun changement cryptographique : la signature porte
  # toujours sur les octets canoniques re-sérialisés par l'appareil.
  # 🔴 GRAMMAIRE STRICTE — MÊME exigence que `parseSigFile()` de `data/app.js`
  # (revue de sécurité P2, 2026-10-21, condition 1). ⚠️ Le défaut mesuré :
  # `sed -n '1p'`/`'2p'` IGNORENT tout ce qui dépasse, donc un `.sig` de 3
  # lignes (un éditeur qui ajoute un saut de ligne de courtoisie) PASSAIT ce
  # contrôle et était REFUSÉ par la page, avec un message qui accuse le
  # fichier de l'opérateur. ⛔ Un artefact publié avec le feu vert du gate ne
  # doit pas être rejeté chez celui qui l'installe.
  # ⇒ Le gate est délibérément AU MOINS aussi strict que le lecteur.
  if LC_ALL=C grep -q $'\r' "$sig"; then
    echo "ERREUR S2 : $sig contient un retour chariot (\\r) — fichier à fins de ligne Windows." >&2
    sig_errors=1
    continue
  fi
  sig_line_count=$(wc -l < "$sig" | tr -d ' ')
  if [ "$sig_line_count" != "2" ]; then
    {
      echo "ERREUR S2 : $sig n'a pas EXACTEMENT 2 lignes (vu : $sig_line_count)."
      echo "  Ligne 1 : le manifeste canonique · Ligne 2 : la signature base64."
      echo "  ⛔ Chacune se termine par UN saut de ligne, et rien ne suit la ligne 2."
    } >&2
    sig_errors=1
    continue
  fi
  sig_manifest="$(sed -n '1p' "$sig")"
  sig_b64="$(sed -n '2p' "$sig")"
  if [ -z "$sig_manifest" ] || [ -z "$sig_b64" ]; then
    {
      echo "ERREUR S2 : $sig a une ligne vide."
      echo "  Ligne 1 : le manifeste canonique · Ligne 2 : la signature base64."
    } >&2
    sig_errors=1
    continue
  fi
  # RSA-3072 ⇒ 384 octets ⇒ EXACTEMENT 512 caractères base64 SANS padding
  # (384 est multiple de 3), alphabet standard RFC 4648. Miroir du contrôle
  # de `parseSigFile()`.
  if ! printf '%s' "$sig_b64" | LC_ALL=C grep -Eq '^[A-Za-z0-9+/]{512}$'; then
    {
      echo "ERREUR S2 : la ligne 2 de $sig n'est pas une signature RSA-3072 en base64."
      echo "  Attendu : 512 caractères de l'alphabet [A-Za-z0-9+/], sans '=' de remplissage."
      echo "  Vu      : ${#sig_b64} caractères."
      echo "  → 'openssl base64' SANS '-A' replie la sortie tous les 64 caractères : utiliser '-A'."
    } >&2
    sig_errors=1
    continue
  fi
  # ⚠️ La ligne 1 du fichier doit être EXACTEMENT le manifeste attendu : si elle
  # diverge, la signature pourrait être valide pour un AUTRE binaire — c'est le
  # mélange de versions, l'erreur la plus probable.
  if [ "$sig_manifest" != "pool-ota/1 $genre $version sha256:$digest" ]; then
    {
      echo "ERREUR S2 : le manifeste de $sig ne décrit PAS ce binaire."
      echo "  Dans le fichier : $sig_manifest"
      echo "  Attendu         : pool-ota/1 $genre $version sha256:$digest"
      echo "  → Signature d'une AUTRE version ou d'un AUTRE binaire."
    } >&2
    sig_errors=1
    continue
  fi
  if ! printf '%s' "$sig_b64" | openssl base64 -d -A > "$sig_tmp/sig_$genre.bin" 2>/dev/null \
     || [ ! -s "$sig_tmp/sig_$genre.bin" ]; then
    # ⚠️ Message DISTINCT du refus de clé ci-dessous : confondre « je n'ai pas
    # su décoder » et « la clé ne correspond pas » enverrait le signataire
    # soupçonner sa clé pour un défaut de transport.
    echo "ERREUR S2 : la ligne 2 de $sig n'a pas pu être DÉCODÉE (base64 invalide) — ce n'est pas un problème de clé." >&2
    sig_errors=1
    continue
  fi

  if ! openssl dgst -sha256 -verify "$sig_tmp/pub.pem" \
        -signature "$sig_tmp/sig_$genre.bin" "$manifest" >/dev/null 2>&1; then
    {
      echo "ERREUR S2 : la signature $sig NE CORRESPOND PAS à la clé embarquée."
      echo "  Manifeste attendu (octets exacts, SANS saut de ligne final) :"
      echo "    pool-ota/1 $genre $version sha256:$digest"
      echo "  Causes les plus probables, dans l'ordre :"
      echo "    1. signature produite avec une AUTRE clé privée que celle de src/constants.h ;"
      echo "    2. 'echo' utilisé au lieu de 'printf %s' (saut de ligne final) ;"
      echo "    3. binaire recompilé APRÈS la signature (l'empreinte a changé) ;"
      echo "    4. src/version.h bumpé après la signature."
    } >&2
    sig_errors=1
  fi
done

if [ "$sig_errors" -ne 0 ]; then
  echo "⛔ Release REFUSÉE : signatures manquantes ou invalides." >&2
  exit 1
fi

echo "OK — firmware.bin.sig et littlefs.bin.sig vérifiés contre la clé"
echo "EMBARQUÉE dans src/constants.h (celle que l'appareil utilisera)."
