#!/usr/bin/env bash
# ============================================================================
# Contrôle d'hygiène des specs : un nom de spec ne doit vivre que dans UN seul
# des trois dossiers todo/ · doing/ · done/.
#
# POURQUOI CE SCRIPT : le cycle todo → doing → done suppose un DÉPLACEMENT.
# Quand une spec est copiée au lieu d'être déplacée, elle existe en deux
# exemplaires qui divergent, et la question « où en est cette feature ? » n'a
# plus de réponse lisible dans les fichiers. Constat du 2026-09-06 : trois specs
# vivaient dans todo/ ET done/ avec des statuts contradictoires — « todo » d'un
# côté, « livré en 2.20.x, vérifié » de l'autre. Une règle que rien ne vérifie
# n'est pas une règle (feature-103).
#
# DEUX règles, de sévérité différente :
#   1. BLOQUANTE — un nom de fichier ne vit que dans un seul dossier.
#   2. AVERTISSEMENT — un même numéro (feature-NNN / bug-NNN) n'est pas porté par
#      deux fichiers de slugs différents. Constat du 2026-09-06 : feature-067
#      désigne à la fois « etude-portage-esp32-s3 » (todo) et « offset-mesure-ph »
#      (done) — une référence « feature-067 » dans un commit ou un ADR est donc
#      ambiguë. Non bloquant à dessein : corriger exigerait de renuméroter une spec
#      déjà close, donc de réécrire de l'historique. On le signale, on ne l'impose pas.
#
# La cohérence du champ « Statut » avec le dossier a été écartée — ce champ est en
# texte libre dans le dépôt et son analyse produirait des faux positifs.
#
# USAGE :
#     ./tools/check_specs.sh        # sortie 0 si tout va bien, 1 sinon
#
# Écrit pour tourner à l'identique en local et en CI, et pour pouvoir servir de
# hook pre-commit. Pas de `declare -A` ni de `mapfile` : le bash de macOS est en
# 3.2 et ne les connaît pas.
# ============================================================================
set -uo pipefail

RACINE="$(cd "$(dirname "$0")/.." && pwd)"
BASE="$RACINE/specs/features"
DOSSIERS="todo doing done"

# CHOIX ASSUMÉ : l'absence du répertoire de specs est une ERREUR (sortie 1), pas un
# dépôt sain. Un dossier de specs vide, en revanche, est légitime (sortie 0 plus bas).
# Un dépôt cassé ne doit pas produire un contrôle vert.
if [ ! -d "$BASE" ]; then
  echo "❌ Répertoire introuvable : $BASE" >&2
  echo "   Le contrôle ne peut pas conclure — dépôt incomplet ?" >&2
  exit 1
fi

# Template explicite : le mktemp de BSD/macOS l'EXIGE, `mktemp` nu y échoue. Et la
# garde `||` n'est pas décorative — sans elle, un mktemp en échec laissait $INVENTAIRE
# vide, tous les accès fichier échouaient, la liste des doublons ressortait vide et le
# script annonçait « aucun doublon » en sortant 0. Une garde qui se neutralise en
# silence est pire que pas de garde (défaut relevé en revue, feature-103).
INVENTAIRE="$(mktemp "${TMPDIR:-/tmp}/check_specs.XXXXXX")" || {
  echo "❌ mktemp a échoué — impossible de constituer l'inventaire des specs." >&2
  echo "   Le contrôle ne peut PAS conclure : sortie en erreur plutôt qu'en succès." >&2
  exit 1
}
trap 'rm -f "$INVENTAIRE"' EXIT

absents=""
for d in $DOSSIERS; do
  if [ ! -d "$BASE/$d" ]; then
    absents="$absents $d"
    continue
  fi
  # -maxdepth 1 : les sous-dossiers éventuels ne participent pas au cycle.
  find "$BASE/$d" -maxdepth 1 -type f -name '*.md' | while IFS= read -r fichier; do
    printf '%s\t%s\n' "$(basename "$fichier")" "$d"
  done >> "$INVENTAIRE"
done

[ -n "$absents" ] && echo "ℹ️  Dossier(s) absent(s), ignoré(s) :$absents"

total="$(wc -l < "$INVENTAIRE" | tr -d ' ')"
if [ "$total" -eq 0 ]; then
  echo "ℹ️  Aucune spec trouvée dans $BASE — rien à vérifier."
  exit 0
fi

# ---------------------------------------------------------------------------
# Règle 2 (AVERTISSEMENT) : un numéro porté par deux slugs différents.
# ---------------------------------------------------------------------------
numeros_ambigus="$(cut -f1 "$INVENTAIRE" \
  | sed -nE 's/^((feature|bug|refactor)-[0-9]+)-.*/\1/p' \
  | sort -u \
  | while IFS= read -r num; do
      distincts="$(cut -f1 "$INVENTAIRE" | grep -E "^${num}-" | sort -u | wc -l | tr -d ' ')"
      [ "$distincts" -gt 1 ] && echo "$num"
    done)"

if [ -n "$numeros_ambigus" ]; then
  echo "⚠️  Numéro(s) de spec porté(s) par plusieurs slugs — toute référence y est ambiguë :"
  while IFS= read -r num; do
    [ -z "$num" ] && continue
    printf '   %s\n' "$num"
    awk -F'\t' -v n="$num" 'index($1, n "-") == 1 { print $2 "/" $1 }' "$INVENTAIRE" \
      | sort -u | while IFS= read -r ligne; do printf '       %s\n' "$ligne"; done
  done <<< "$numeros_ambigus"
  echo "   (non bloquant : renuméroter une spec close réécrirait de l'historique)"
  echo ""
fi

# ---------------------------------------------------------------------------
# Règle 1 (BLOQUANTE) : un nom présent dans plusieurs dossiers.
# ---------------------------------------------------------------------------
doublons="$(cut -f1 "$INVENTAIRE" | sort | uniq -d)"

if [ -z "$doublons" ]; then
  echo "✅ $total spec(s), aucun doublon entre todo/ · doing/ · done/."
  exit 0
fi

nb=0
echo "❌ Spec(s) présente(s) dans plusieurs dossiers à la fois :" >&2
echo "" >&2
while IFS= read -r nom; do
  [ -z "$nom" ] && continue
  nb=$((nb + 1))
  # Comparaison de champ EXACTE : un `grep` sur « nom<TAB> » n'est pas ancré et
  # attribuerait à « feature-202-a.md » les dossiers de « pre-feature-202-a.md ».
  emplacements="$(awk -F'\t' -v n="$nom" '$1 == n { print $2 }' "$INVENTAIRE" | sort | tr '\n' ' ')"
  printf '   %s\n       → %s\n' "$nom" "$emplacements" >&2
done <<< "$doublons"

cat >&2 <<'AIDE'

Le cycle todo → doing → done procède par DÉPLACEMENT (`git mv`), jamais par copie.
Une spec dans deux dossiers, ce sont deux versions qui divergent et deux statuts
qui se contredisent.

Correctif : ne garder qu'un exemplaire — celui du dossier qui reflète l'état réel,
généralement le plus avancé et le plus riche — après avoir vérifié qu'aucun contenu
ne vit UNIQUEMENT dans la copie supprimée.
AIDE

echo "" >&2
echo "$nb doublon(s) — voir specs/features/_template.md et CLAUDE.md § « Source de vérité »." >&2
exit 1
