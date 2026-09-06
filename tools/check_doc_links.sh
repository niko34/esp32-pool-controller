#!/usr/bin/env bash
# ============================================================================
# Contrôle des liens relatifs de la documentation : toute cible doit exister.
#
# POURQUOI CE SCRIPT : le chemin d'un lien vers une spec encode son ÉTAT dans le
# cycle (`specs/features/doing/…`). Or cet état change par conception —
# todo → doing → done. Chaque promotion casse donc, SILENCIEUSEMENT, tous les
# liens qui citaient l'ancien emplacement. Constat du 2026-09-06 : 27 liens morts
# sur 94 vers des specs (29 %), tous pointant vers `todo/` ou `doing/` pour des
# specs passées en `done/`. Ce n'étaient pas des fautes : c'étaient des liens
# JUSTES le jour où ils ont été écrits.
#
# `check_specs.sh` vérifie qu'une spec ne vit pas dans deux dossiers ; il ne dit
# rien des liens qui la citent. Le registre écrit doit rester univoque ET
# navigable.
#
# TROIS EXCLUSIONS, sans lesquelles le contrôle serait bruyant donc inutilisé :
#   1. les blocs de code clôturés (```) — `](AsyncWebServerRequest*)` en C++
#      n'est pas un lien ;
#   2. les segments entre backticks — `docs/README.md` documente la convention
#      avec `[Label](chemin/fichier.md)`, qui est un exemple, pas une cible ;
#   3. le suffixe `:123` (et `:12-34`) — c'est un NUMÉRO DE LIGNE, convention du
#      dépôt pour citer du code, pas une partie du chemin.
#
# USAGE :
#     ./tools/check_doc_links.sh        # sortie 0 si tout résout, 1 sinon
#
# Portable bash 3.2 (macOS) : ni `declare -A`, ni `mapfile`, ni `realpath`.
# ============================================================================
set -uo pipefail

RACINE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$RACINE"

INVENTAIRE="$(mktemp "${TMPDIR:-/tmp}/check_doc_links.XXXXXX")" || {
  echo "❌ mktemp a échoué — le contrôle ne peut pas conclure." >&2
  exit 1
}
trap 'rm -f "$INVENTAIRE"' EXIT

# Fichiers examinés : toute la doc, plus les .md de la racine.
find docs -name '*.md' -type f 2>/dev/null > "$INVENTAIRE.f" || true
find . -maxdepth 1 -name '*.md' -type f >> "$INVENTAIRE.f"

while IFS= read -r fichier; do
  [ -z "$fichier" ] && continue
  # awk : retire les blocs clôturés et les segments entre backticks, puis
  # extrait chaque cible de lien markdown.
  awk -v F="$fichier" '
    /^[[:space:]]*```/ { fence = !fence; next }
    fence { next }
    {
      ligne = $0
      gsub(/`[^`]*`/, "", ligne)          # segments entre backticks
      while (match(ligne, /\]\([^)[:space:]]+\)/)) {
        cible = substr(ligne, RSTART + 2, RLENGTH - 3)
        print F "\t" cible
        ligne = substr(ligne, RSTART + RLENGTH)
      }
    }
  ' "$fichier" >> "$INVENTAIRE"
done < "$INVENTAIRE.f"
rm -f "$INVENTAIRE.f"

nb=0
while IFS="$(printf '\t')" read -r fichier cible; do
  [ -z "${cible:-}" ] && continue
  case "$cible" in
    http://*|https://*|mailto:*|\#*) continue ;;
  esac
  chemin="${cible%%#*}"                    # retire l'ancre
  chemin="$(printf '%s' "$chemin" | sed -E 's/:[0-9]+(-[0-9]+)?$//')"   # retire :ligne
  [ -z "$chemin" ] && continue
  if [ ! -e "$(dirname "$fichier")/$chemin" ]; then
    [ "$nb" -eq 0 ] && { echo "❌ Lien(s) de documentation vers une cible inexistante :" >&2; echo "" >&2; }
    nb=$((nb + 1))
    printf '   %s\n       → %s\n' "$fichier" "$cible" >&2
  fi
done < "$INVENTAIRE"

if [ "$nb" -eq 0 ]; then
  echo "✅ $(wc -l < "$INVENTAIRE" | tr -d ' ') lien(s) relatif(s), toutes les cibles existent."
  exit 0
fi

cat >&2 <<'AIDE'

Cause la plus fréquente : une spec a été PROMUE (todo → doing → done) et les liens
qui la citaient pointent encore vers son ancien dossier. Ils étaient justes le jour
où ils ont été écrits.

Correctif : mettre le chemin à jour vers l'emplacement réel. Ne jamais recréer un
fichier à l'ancien emplacement pour faire taire le contrôle.
AIDE
echo "" >&2
echo "$nb lien(s) mort(s)." >&2
exit 1
