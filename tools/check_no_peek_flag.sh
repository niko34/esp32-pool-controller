#!/usr/bin/env bash
# =============================================================================
# check_no_peek_flag.sh — POOL_HEAP_PEEK absent de tout fichier versionné (S2)
# =============================================================================
# feature-128 (R3, ADR-0081) : la lecture de contenu du tas n'est PAS livrée en
# production parce qu'elle n'existe QUE si la macro POOL_HEAP_PEEK est définie
# au build (`PLATFORMIO_BUILD_FLAGS=-DPOOL_HEAP_PEEK=1 pio run ...`) — et cette
# variable d'environnement n'est écrite dans AUCUN fichier versionné. Ce script
# échoue si quelqu'un la fige quand même dans un fichier du dépôt.
#
# ⚠️ CE QUE CE SCRIPT NE PROUVE PAS (à savoir plutôt qu'à croire couvert) :
#   1. L'ARTEFACT compilé (le .bin/.elf réellement publié) — c'est le rôle de
#      S1 (grep -a -c '/debug/heap_peek' sur le binaire), câblé dans
#      .claude/commands/release.md, PAS dans ce script. `/release` compile
#      LOCALEMENT, dans l'environnement de l'appelant : une variable
#      PLATFORMIO_BUILD_FLAGS résiduelle du shell échapperait entièrement à ce
#      contrôle, qui ne regarde QUE des fichiers versionnés.
#   2. L'INTENTION : quiconque peut committer peut modifier CE script dans le
#      même commit qui introduit la définition qu'il est censé bloquer. Il
#      protège de l'INADVERTANCE, pas d'un contournement délibéré — même
#      discipline que tools/check_fopen_guard.sh (ADR-0070).
#
# PÉRIMÈTRE : platformio.ini, src/, tools/, scripts/, deploy.sh, .github/,
# .claude/ — À L'EXCLUSION de docs/, specs/ et de tout fichier *.md (la
# documentation DOIT pouvoir écrire la commande de build exacte en clair).
#
# MOTIFS RECHERCHÉS (une définition, jamais une simple mention) :
#   - `-DPOOL_HEAP_PEEK` (espace(s) tolérés entre `-D` et le nom, GCC accepte
#     LES DEUX formes `-DPOOL_HEAP_PEEK` et `-D POOL_HEAP_PEEK`, cette dernière
#     définissant la macro à 1 — trou trouvé et corrigé en revue, G3 : un motif
#     sans `[[:space:]]*` laissait passer `build_flags = -D POOL_HEAP_PEEK`)
#   - `#define POOL_HEAP_PEEK` : définition directe dans un en-tête
#   - `POOL_HEAP_PEEK=`        : forme d'affectation (variable d'env figée en dur)
# Ces trois motifs NE matchent PAS une mention en commentaire ou un usage —
# `#if defined(POOL_HEAP_PEEK)`, `POOL_HEAP_PEEK == 1`, `#endif // POOL_HEAP_PEEK`
# (heap_census.h/.cpp) et les commentaires de constants.h:600,656 et
# main.cpp:49 sont donc LÉGITIMES et ne doivent PAS faire échouer ce script.
#
# Usage : ./tools/check_no_peek_flag.sh
# Sortie : 0 si conforme, 1 sinon (liste des sites fautifs sur stderr).
# =============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."

# Ce script lui-même vit sous tools/ (donc dans le périmètre scanné) et
# CONTIENT nécessairement les trois motifs en toutes lettres dans ce
# commentaire — il doit s'exclure lui-même pour ne pas se signaler.
SELF="tools/check_no_peek_flag.sh"

MOTIF='(-D[[:space:]]*POOL_HEAP_PEEK|#[[:space:]]*define[[:space:]]+POOL_HEAP_PEEK|POOL_HEAP_PEEK=)'

# ARBRE DE TRAVAIL, pas seulement `git ls-files` (même choix que
# check_fopen_guard.sh) : un fichier neuf pas encore ajouté à l'index doit être
# détecté AVANT le premier commit qui l'introduirait, pas seulement en CI après
# coup. Périmètre listé explicitement plutôt qu'un `find .` large, pour ne
# jamais dériver vers node_modules/, .pio/, data-build/ ou autre artefact de
# build — aucun de ces répertoires n'est sous les chemins listés ci-dessous.
targets="platformio.ini deploy.sh src tools scripts .github .claude"
files=$(find $targets -type f 2>/dev/null | sort -u)

found=""
while IFS= read -r f; do
  [ -z "$f" ] && continue
  [ "$f" = "$SELF" ] && continue           # ce script lui-même
  case "$f" in
    *.md) continue ;;                       # toute doc Markdown, où qu'elle vive
    docs/*) continue ;;
    specs/*) continue ;;
  esac
  [ -f "$f" ] || continue                   # fichier supprimé de l'arbre de travail
  hit=$(grep -nE "$MOTIF" -- "$f" 2>/dev/null || true)
  if [ -n "$hit" ]; then
    found="${found}${f}:
${hit}
"
  fi
done <<< "$files"

if [ -n "$found" ]; then
  {
    echo "ERREUR S2/ADR-0081 : POOL_HEAP_PEEK défini dans un fichier versionné."
    echo "R3 (lecture de contenu du tas) doit rester absente de tout binaire de"
    echo "release : cette macro ne doit JAMAIS être définie ailleurs que par la"
    echo "variable d'environnement PLATFORMIO_BUILD_FLAGS au moment du build de"
    echo "diagnostic (voir docs/adr/0081-*.md pour la commande exacte)."
    echo
    echo "$found"
  } >&2
  exit 1
fi

echo "OK — POOL_HEAP_PEEK n'est défini dans aucun fichier versionné du périmètre."
