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
