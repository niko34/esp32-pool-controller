#!/bin/bash
# ============================================================================
# Dossier de fabrication JLCPCB d'une carte du PCB v3 : gravure (gerbers +
# perçage, 4 couches), liste des pièces (BOM) et positions de pose (CPL).
#
# Les sorties vont dans kicad/<carte>/fabrication/, IGNORÉ par git : elles se
# régénèrent depuis les sources KiCad.
#
# Usage : ./scripts/build-jlcpcb.sh <carte>
#         ex. ./scripts/build-jlcpcb.sh v3-principale
#             ./scripts/build-jlcpcb.sh v3-mesure-std
#
# ⚠️ Le script corrige la position et l'angle des boîtiers connus (voir
#    scripts/jlcpcb_bom_cpl.py), mais JLCPCB ne compte pas les angles comme
#    KiCad : la vérification à l'écran de pose de JLCPCB, au moment de la
#    commande, reste une étape humaine obligatoire.
# ============================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
CARTE="${1:?usage : $0 <carte>   (ex. v3-principale)}"
DIR="kicad/$CARTE"; PCB="$DIR/$CARTE.kicad_pcb"; SCH="$DIR/$CARTE.kicad_sch"
[ -f "$PCB" ] && [ -f "$SCH" ] || { echo "❌ $PCB ou $SCH introuvable" >&2; exit 1; }
command -v kicad-cli >/dev/null || { echo "❌ kicad-cli introuvable (KiCad 9)" >&2; exit 1; }
OUT="$DIR/fabrication"; G="$OUT/gerbers"
rm -rf "$OUT"; mkdir -p "$G"

# 1. Le contrôle des règles doit être propre AVANT de fabriquer.
kicad-cli pcb drc --severity-error --schematic-parity -o "$OUT/drc.rpt" "$PCB" >/dev/null || true
if grep -q "^\[" "$OUT/drc.rpt"; then
  echo "❌ Le contrôle des règles signale des erreurs : $OUT/drc.rpt" >&2; exit 1
fi

# 1 bis. Les règles de l'étude CEM et fiabilité (kicad/REGLES-CEM-ET-FIABILITE.md) doivent tenir, elles aussi.
#        Une règle non tenue arrête ici : on ne fabrique pas une carte qui a perdu une protection.
python3 scripts/verifier_regles_cartes.py "$CARTE" 2>/dev/null | tee "$OUT/regles.txt" | grep -v "^✅" || true
if grep -q "^❌" "$OUT/regles.txt"; then
  echo "❌ Règle non tenue — détail ci-dessus, raison de chaque règle dans kicad/REGLES-CEM-ET-FIABILITE.md" >&2; exit 1
fi
grep -q "toutes les règles vérifiables par calcul sont tenues" "$OUT/regles.txt" || { echo "❌ scripts/verifier_regles_cartes.py n'est pas allé au bout" >&2; exit 1; }

# 2. Gravure : cuivre (4 couches), vernis, pâte, sérigraphie, contour.
kicad-cli pcb export gerbers --output "$G/" --no-x2 --subtract-soldermask \
  --layers "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts" "$PCB" >/dev/null
kicad-cli pcb export drill --output "$G/" --format excellon --excellon-units mm \
  --excellon-zeros-format decimal --excellon-separate-th "$PCB" >/dev/null
rm -f "$G"/*.gbrjob
(cd "$G" && zip -q -r "../gerbers-$CARTE.zip" .)

# 3. Pièces et positions, au format demandé par JLCPCB.
kicad-cli sch export bom --output "$OUT/bom-brut.csv" \
  --fields 'Reference,Value,Footprint,LCSC,${DNP}' --labels 'Reference,Value,Footprint,LCSC,DNP' "$SCH" >/dev/null
kicad-cli pcb export pos --output "$OUT/pos-brut.csv" --format csv --units mm --side both "$PCB" >/dev/null
python3 scripts/jlcpcb_bom_cpl.py "$OUT/bom-brut.csv" "$OUT/pos-brut.csv" "$OUT/bom-$CARTE.csv" "$OUT/cpl-$CARTE.csv" "$PCB" 2>/dev/null
rm -f "$OUT/bom-brut.csv" "$OUT/pos-brut.csv" "$OUT/drc.rpt" "$OUT/regles.txt"
echo ""
echo "✅ $OUT/"
ls -1 "$OUT" | sed 's/^/     /'
