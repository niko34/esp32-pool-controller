#!/bin/bash
# ============================================================================
# Génération des PDF utilisateur (docs/user/) depuis les sources HTML.
#
# - Les captures d'écran sont REDIMENSIONNÉES à la résolution d'impression
#   (1200 px de large ≈ 180 dpi sur la largeur utile A4) dans un dossier
#   temporaire — les originaux de screenshots/ restent intacts.
# - La structure relative est reproduite dans le temporaire pour que les
#   chemins ../../../screenshots/ des HTML résolvent sans modification.
# - Sortie : docs/user/guide-*-v<version>.pdf
#
# Usage : ./tools/build_user_pdfs.sh [version]   (défaut : lit src/version.h)
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Moteur de rendu. Surchargeable par l'appelant (scripts/build-docs.sh sonde
# weasyprint puis les différentes installations de Chrome/Chromium) ; le chemin
# macOS reste le défaut pour un appel direct.
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
LARGEUR_CIBLE=1200   # px — largeur utile A4 ≈ 172 mm : ~180 dpi, net à l'impression
QUALITE_JPEG=75      # normalFormatOptions sips (0-100)

VERSION="${1:-$(sed -n 's/.*FIRMWARE_VERSION "\([0-9]*\.[0-9]*\)\..*/\1/p' "$ROOT/src/version.h")}"
if [ -z "$VERSION" ]; then echo "❌ version introuvable"; exit 1; fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/docs/user/src" "$TMP/screenshots"

# 1. Sources HTML + style — la version affichée (couverture, pied de page,
#    colophon) est injectée depuis version.h. Motifs CIBLÉS uniquement :
#    les mentions historiques du contenu (« depuis la v2.32 ») sont préservées.
cp "$ROOT/docs/user/src/"*.css "$TMP/docs/user/src/"
for html in "$ROOT/docs/user/src/"*.html; do
  # ( |&nbsp;) : la typographie française met des espaces insécables
  sed -E "s/(firmware|Firmware|version)( |\&nbsp;)2\.[0-9]+(\.[0-9]+)?/\1\2${VERSION}/g" \
      "$html" > "$TMP/docs/user/src/$(basename "$html")"
done

# 2. Captures redimensionnées (jamais AGRANDIES : on borne au maximum)
#
# Trois moteurs possibles, dans cet ordre : sips (livré avec macOS),
# ImageMagick, puis Pillow. Le script tournait auparavant sur `sips` seul — donc
# uniquement sur macOS, alors que tout le reste de la chaîne (tests, build,
# gerbers) est portable. Un clone sur Linux échouait ICI, après avoir détecté un
# Chromium parfaitement utilisable.
#
# ⚠️ L'étape n'est pas optionnelle : sans elle les PDF passent de 4 à 26 Mo.
if command -v sips >/dev/null 2>&1; then          MOTEUR_IMG="sips"
elif command -v magick >/dev/null 2>&1; then      MOTEUR_IMG="magick"
elif command -v convert >/dev/null 2>&1; then     MOTEUR_IMG="convert"
elif python3 -c "import PIL" 2>/dev/null; then    MOTEUR_IMG="pillow"
else
  echo "❌ Aucun outil de redimensionnement d'image disponible." >&2
  echo "   Installez l'un des suivants :" >&2
  echo "     Debian/Ubuntu : sudo apt install imagemagick" >&2
  echo "                     (ou : pip install Pillow)" >&2
  echo "     macOS         : sips est fourni avec le système" >&2
  echo "" >&2
  echo "   Cette étape n'est pas facultative : sans redimensionnement, les PDF" >&2
  echo "   pèsent 26 Mo au lieu de 4." >&2
  exit 1
fi

echo "→ Redimensionnement des captures (${LARGEUR_CIBLE}px, JPEG q${QUALITE_JPEG}, via ${MOTEUR_IMG})…"
for img in "$ROOT/screenshots/"*.jpg; do
  base="$(basename "$img")"
  case "$MOTEUR_IMG" in
    sips)
      sips -Z "$LARGEUR_CIBLE" -s format jpeg -s formatOptions "$QUALITE_JPEG" \
           "$img" --out "$TMP/screenshots/$base" >/dev/null
      ;;
    magick|convert)
      # « > » suffixé : ne redimensionne QUE si l'image dépasse la cible,
      # équivalent du bornage de `sips -Z`.
      "$MOTEUR_IMG" "$img" -resize "${LARGEUR_CIBLE}x${LARGEUR_CIBLE}>" \
           -quality "$QUALITE_JPEG" "$TMP/screenshots/$base"
      ;;
    pillow)
      python3 - "$img" "$TMP/screenshots/$base" "$LARGEUR_CIBLE" "$QUALITE_JPEG" <<'PY'
import sys
from PIL import Image
src, dst, largeur, qualite = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
im = Image.open(src)
im.thumbnail((largeur, largeur), Image.LANCZOS)   # thumbnail n'agrandit jamais
im.convert("RGB").save(dst, "JPEG", quality=qualite)
PY
      ;;
  esac
done

# 3. Génération des PDF
for guide in guide-utilisation guide-regulation; do
  case "$guide" in
    guide-utilisation) out="guide-d-utilisation-v${VERSION}.pdf" ;;
    guide-regulation)  out="guide-regulation-v${VERSION}.pdf" ;;
  esac
  echo "→ $out"
  "$CHROME" --headless --no-pdf-header-footer \
    --print-to-pdf="$ROOT/docs/user/$out" \
    "$TMP/docs/user/src/$guide.html" 2>/dev/null
done

echo "✅ Terminé :"
ls -lh "$ROOT/docs/user/"*.pdf | awk '{print "   " $9 " — " $5}'
