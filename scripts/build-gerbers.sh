#!/bin/bash
# ============================================================================
# Génère les fichiers de fabrication du PCB (gerbers + perçage) depuis les
# sources KiCad, et les zippe pour envoi en fabrication (profil JLCPCB).
#
# Les sorties sont VOLONTAIREMENT ignorées par git (cf. .gitignore) : elles se
# régénèrent depuis le .kicad_pcb, et c'est le zip qui devient l'asset d'une
# release pcb-vX (/release-pcb).
#
# Usage : ./scripts/build-gerbers.sh [tag] [carte]
#         tag   : nom de version pour le zip (défaut : pcb-dev)
#         carte : dossier de la carte sous kicad/ (ex. esp32-pool-controller).
#                 Obligatoire dès qu'il y a plusieurs cartes sous kicad/.
#               ex. ./scripts/build-gerbers.sh pcb-v2.1 esp32-pool-controller
#                   ->  kicad/gerbers-pcb-v2.1-esp32-pool-controller.zip
#
# ⚠️ Les cartes du v3 (4 couches, posées par l'atelier) se préparent avec
#    ./scripts/build-jlcpcb.sh <carte>, qui ajoute la liste des pièces et les
#    positions de pose. Ce script-ci ne sort que la gravure.
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TAG="${1:-pcb-dev}"
CARTE="${2:-}"

# ---------------------------------------------------------------------------
# 1. kicad-cli : PATH d'abord, puis les emplacements d'installation connus.
# ---------------------------------------------------------------------------
KICAD_CLI=""
for candidat in \
  "${KICAD_CLI:-}" \
  "$(command -v kicad-cli 2>/dev/null || true)" \
  "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli" \
  "/usr/bin/kicad-cli" \
  "/usr/local/bin/kicad-cli"
do
  if [ -n "$candidat" ] && [ -x "$candidat" ]; then
    KICAD_CLI="$candidat"
    break
  fi
done

if [ -z "$KICAD_CLI" ]; then
  echo "❌ kicad-cli introuvable." >&2
  echo "" >&2
  echo "   Installez KiCad 7 ou supérieur :" >&2
  echo "     macOS  : brew install --cask kicad" >&2
  echo "     Debian : sudo apt install kicad" >&2
  echo "" >&2
  echo "   Sur macOS, kicad-cli n'est pas ajouté au PATH par l'installateur ;" >&2
  echo "   ce script sonde /Applications/KiCad/KiCad.app/Contents/MacOS/." >&2
  echo "   Vous pouvez aussi le pointer explicitement :" >&2
  echo "     KICAD_CLI=/chemin/vers/kicad-cli ./scripts/build-gerbers.sh" >&2
  exit 1
fi

echo "→ kicad-cli : $KICAD_CLI"

# ---------------------------------------------------------------------------
# 2. Localisation du .kicad_pcb (un seul attendu sous kicad/)
# ---------------------------------------------------------------------------
# Boucle `while read` plutôt que `mapfile` : le bash livré par macOS est en 3.2
# et ne connaît pas mapfile.
PCBS=()
while IFS= read -r ligne; do
  [ -n "$ligne" ] && PCBS+=("$ligne")
done < <(find kicad -name '*.kicad_pcb' -not -name '*-save.kicad_pcb' 2>/dev/null | sort)

if [ -n "$CARTE" ]; then
  if [ ! -f "kicad/$CARTE/$CARTE.kicad_pcb" ]; then
    echo "❌ kicad/$CARTE/$CARTE.kicad_pcb introuvable." >&2
    exit 1
  fi
  PCBS=("kicad/$CARTE/$CARTE.kicad_pcb")
fi

if [ "${#PCBS[@]}" -eq 0 ]; then
  echo "❌ Aucun fichier .kicad_pcb trouvé sous kicad/." >&2
  exit 1
fi
if [ "${#PCBS[@]}" -gt 1 ]; then
  echo "❌ Plusieurs cartes sous kicad/ — laquelle fabriquer ?" >&2
  printf '     %s\n' "${PCBS[@]}" >&2
  echo "   Donnez son dossier en second argument :" >&2
  echo "     ./scripts/build-gerbers.sh $TAG esp32-pool-controller" >&2
  echo "   Pour une carte du v3 : ./scripts/build-jlcpcb.sh <carte>" >&2
  exit 1
fi

PCB="${PCBS[0]}"
NOM="$(basename "$PCB" .kicad_pcb)"
SUFFIXE="$TAG"
[ -n "$CARTE" ] && SUFFIXE="$TAG-$CARTE"
SORTIE="kicad/gerbers-$SUFFIXE"
ZIP="kicad/gerbers-$SUFFIXE.zip"

# Couches de cuivre internes : présentes sur une carte à 4 couches.
COUCHES="F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts"
if grep -q '"In1.Cu"' "$PCB"; then
  COUCHES="F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts"
fi

echo "→ PCB       : $PCB"
echo "→ Sortie    : $SORTIE/"

rm -rf "$SORTIE" "$ZIP"
mkdir -p "$SORTIE"

# ---------------------------------------------------------------------------
# 3. Gerbers — jeu de couches JLCPCB (2 ou 4 couches de cuivre selon la carte).
# ---------------------------------------------------------------------------
# --no-x2 : JLCPCB attend du Gerber X1 classique (les attributs X2 sont tolérés
#           mais inutiles ici et brouillent certains visualiseurs).
# --subtract-soldermask : le masque est soustrait de la sérigraphie, sinon la
#           sérigraphie déborde sur les pastilles.
echo "→ Génération des gerbers…"
"$KICAD_CLI" pcb export gerbers \
  --output "$SORTIE/" \
  --layers "$COUCHES" \
  --no-x2 \
  --subtract-soldermask \
  --use-drill-file-origin \
  "$PCB"

# ---------------------------------------------------------------------------
# 4. Perçage — Excellon, unités mm, un seul fichier PTH+NPTH fusionné.
# ---------------------------------------------------------------------------
echo "→ Génération du perçage…"
"$KICAD_CLI" pcb export drill \
  --output "$SORTIE/" \
  --format excellon \
  --drill-origin plot \
  --excellon-units mm \
  --excellon-zeros-format decimal \
  --generate-map \
  --map-format gerberx2 \
  "$PCB"

# ---------------------------------------------------------------------------
# 5. Archive
# ---------------------------------------------------------------------------
echo "→ Archive…"
(cd "$SORTIE" && zip -q -r "../../$ZIP" .)

NB="$(find "$SORTIE" -type f | wc -l | tr -d ' ')"
TAILLE="$(du -h "$ZIP" | cut -f1)"

echo ""
echo "✅ $ZIP ($TAILLE, $NB fichiers)"
echo ""
echo "   Contenu :"
(cd "$SORTIE" && ls -1) | sed 's/^/     /'
echo ""
echo "   Vérifiez le rendu avant fabrication (visualiseur JLCPCB ou gerbv)."
